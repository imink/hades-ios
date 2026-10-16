#!/usr/bin/env python3
"""Build a private, re-signable iPhoneOS AOT diagnostic IPA, not a playable port."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import zipfile

from inspect_target import FormatError
from link_modules import generate_artifacts
from prepare import REPO, outside_git, sha256, verified_modules, write_json
from verify_embed import verify_object


def info_plist(identifier, render_resolution="720p", *, gpu_capture_probe=False):
    if not re.fullmatch(r"[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+", identifier):
        raise FormatError("Invalid iOS bundle identifier")
    if type(gpu_capture_probe) is not bool:
        raise FormatError("GPU capture probe must be an explicit boolean")
    return {
        "CFBundleIdentifier": identifier, "CFBundleExecutable": "HadesPortTest",
        "CFBundleName": "Hades", "CFBundleDisplayName": "Hades",
        "CFBundlePackageType": "APPL", "CFBundleVersion": "37",
        "HadesSafePauseResume": True,
        "HadesAutoStartGame": True,
        "HadesRenderResolution": render_resolution,
        "HadesDirectoryOrderProbe": True,
        "HadesVulkanBindingMode": "argument-buffers",
        "HadesVulkanQueueCompletionProbe": False,
        "HadesVulkanPixelCaptureProbe": gpu_capture_probe,
        "HadesVulkanAttachmentLoadProbe": False,
        "HadesVulkanPassCaptureProbe": gpu_capture_probe,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleShortVersionString": "0.1", "CFBundleDevelopmentRegion": "en",
        "MinimumOSVersion": "26.0", "LSRequiresIPhoneOS": True, "UIDeviceFamily": [1, 2],
        "CFBundleSupportedPlatforms": ["iPhoneOS"],
        "UIFileSharingEnabled": True, "LSSupportsOpeningDocumentsInPlace": True,
        "UIRequiresFullScreen": True,
        "ITSAppUsesNonExemptEncryption": False, "UILaunchScreen": {},
        "UISupportedInterfaceOrientations": ["UIInterfaceOrientationPortrait",
                                            "UIInterfaceOrientationLandscapeLeft",
                                            "UIInterfaceOrientationLandscapeRight"],
        "UIApplicationSceneManifest": {
            "UIApplicationSupportsMultipleScenes": False,
            "UISceneConfigurations": {"UIWindowSceneSessionRoleApplication": [{
                "UISceneConfigurationName": "Default", "UISceneDelegateClassName": "HadesSceneDelegate"}]}}
    }


def command(arguments, log):
    result = subprocess.run(list(map(str, arguments)), capture_output=True, text=True, timeout=300)
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise FormatError(f"iOS build command failed; inspect {log}")
    return result.stdout.strip()


def create_app_icons(source, assets):
    if not source.is_file() or source.is_symlink():
        raise FormatError("Missing regular local Hades control icon")
    digest = sha256(source)
    assets.mkdir(parents=True)
    properties = command(["sips", "--getProperty", "format", "--getProperty", "pixelWidth",
                          "--getProperty", "pixelHeight", "--getProperty", "hasAlpha", source],
                         assets.parent / "source-icon.log")
    values = dict(re.findall(r"^\s+(format|pixelWidth|pixelHeight|hasAlpha): (\S+)$",
                             properties, re.MULTILINE))
    if (values.get("format") != "jpeg" or values.get("hasAlpha") != "no"
            or values.get("pixelWidth") != "256" or values.get("pixelHeight") != "256"):
        raise FormatError("Hades control icon must be an opaque 256x256 JPEG")
    icons, hashes = [], {}
    for idiom, points, scales in (("iphone", 20, (2, 3)), ("iphone", 29, (2, 3)),
                                  ("iphone", 40, (2, 3)), ("iphone", 60, (2, 3)),
                                  ("ipad", 20, (1, 2)), ("ipad", 29, (1, 2)),
                                  ("ipad", 40, (1, 2)), ("ipad", 76, (1, 2)),
                                  ("ipad", 83.5, (2,)), ("ios-marketing", 1024, (1,))):
        for scale in scales:
            name = f"icon-{idiom}-{points}-{scale}.png"
            size = int(points * scale)
            command(["sips", "--setProperty", "format", "png", "--resampleHeightWidth",
                     size, size, source, "--out", assets / name], assets.parent / (name + ".log"))
            hashes[name] = sha256(assets / name)
            icons.append({"idiom": idiom, "size": f"{points}x{points}", "scale": f"{scale}x", "filename": name})
    write_json(assets / "Contents.json", {"images": icons, "info": {"version": 1, "author": "hades-local-build"}})
    if sha256(source) != digest:
        raise FormatError("Hades control icon changed during conversion")
    return {"source_file": "data/control/icon_AmericanEnglish.dat",
            "source_sha256": digest, "source_dimensions": [256, 256],
            "assets_sha256": hashes}


def copy_resources(source, destination):
    if not source.is_dir() or destination.exists():
        raise FormatError("Missing resource directory or existing copy destination")
    destination.mkdir(parents=True)
    records = {}
    for path in sorted(source.rglob("*")):
        if path.is_symlink():
            raise FormatError("Symlink in private bundle resources")
        relative = path.relative_to(source)
        target = destination / relative
        if path.is_dir():
            target.mkdir(exist_ok=True)
        elif path.is_file():
            digest = sha256(path)
            shutil.copy2(path, target)
            if sha256(target) != digest:
                raise FormatError("Resource changed during bundle copy")
            records[relative.as_posix()] = {"sha256": digest, "bytes": path.stat().st_size}
        else:
            raise FormatError("Non-regular private bundle resource")
    if not records:
        raise FormatError("Empty resource directory")
    return records


def archive_bundle(app, output):
    if output.exists():
        raise FormatError("IPA output must be new")
    records = {}
    with zipfile.ZipFile(output, "x", zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as archive:
        for path in sorted(app.rglob("*")):
            if path.is_symlink():
                raise FormatError("Symlink in app bundle")
            if path.is_file():
                name = "Payload/" + app.name + "/" + path.relative_to(app).as_posix()
                records[name] = sha256(path)
                archive.write(path, name)
    with zipfile.ZipFile(output) as archive:
        if len(archive.namelist()) != len(records) or set(archive.namelist()) != set(records):
            raise FormatError("IPA entry inventory differs from app bundle")
        for name, expected in records.items():
            digest = hashlib.sha256()
            with archive.open(name) as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            if digest.hexdigest() != expected:
                raise FormatError("IPA entry content differs from app bundle")
    return records


def archive_romfs(source, output):
    if output.exists():
        raise FormatError("RomFS archive output must be new")
    records = {}
    with zipfile.ZipFile(output, "x", zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as archive:
        for path in sorted(source.rglob("*")):
            if path.is_symlink():
                raise FormatError("Symlink in RomFS resources")
            if path.is_file():
                name = "RomFS/" + path.relative_to(source).as_posix()
                records[name] = {"sha256": sha256(path), "bytes": path.stat().st_size}
                archive.write(path, name)
            elif not path.is_dir():
                raise FormatError("Non-regular RomFS archive resource")
        if not records:
            raise FormatError("Empty RomFS archive")
        archive.writestr("RomFS-files.json", json.dumps(records, indent=2) + "\n")
    with zipfile.ZipFile(output) as archive:
        if set(archive.namelist()) != {*records, "RomFS-files.json"}:
            raise FormatError("RomFS archive entry inventory changed")
        for name, record in records.items():
            digest = hashlib.sha256()
            with archive.open(name) as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            if digest.hexdigest() != record["sha256"]:
                raise FormatError("RomFS archive bytes differ from source")
    return records


def verify_romfs_archive(source, archive_path):
    records = {}
    for path in sorted(source.rglob("*")):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise FormatError("Unsafe source resource during archive reuse")
        if path.is_file():
            records["RomFS/" + path.relative_to(source).as_posix()] = {
                "sha256": sha256(path), "bytes": path.stat().st_size}
    if not records or archive_path.is_symlink():
        raise FormatError("Empty source inventory or symlink archive")
    with zipfile.ZipFile(archive_path) as archive:
        if (len(archive.namelist()) != len(records) + 1
                or set(archive.namelist()) != {*records, "RomFS-files.json"}
                or json.loads(archive.read("RomFS-files.json")) != records):
            raise FormatError("Reused RomFS archive inventory differs from current source")
        for name, record in records.items():
            digest, size = hashlib.sha256(), 0
            with archive.open(name) as stream:
                for block in iter(lambda: stream.read(1024*1024), b""):
                    size += len(block)
                    digest.update(block)
            if size != record["bytes"] or digest.hexdigest() != record["sha256"]:
                raise FormatError("Reused RomFS archive bytes differ from current source")
    return records


def verify_catalog(root, bindings):
    contents = bindings.read_bytes()
    interfaces = json.loads(contents)
    shaders = interfaces.get("shaders", [])
    if (interfaces.get("status") != "direct-msl-interface-verified"
            or interfaces.get("shader_count") != 72 or len(shaders) != 72):
        raise FormatError("iOS package requires the complete verified 72-stage binding catalog")
    names = set()
    for shader in shaders:
        name = shader.get("file")
        if (not isinstance(name, str) or Path(name).name != name or "\\" in name
                or name in (".", "..") or name in names
                or shader.get("stage") not in ("vertex", "fragment")):
            raise FormatError("Invalid or duplicate shader catalog entry")
        names.add(name)
        if sha256(root / "build/shaders-msl" / (name + ".metal")) != shader.get("msl_sha256"):
            raise FormatError("MSL differs from binding catalog")
    if not {"Default2D.vert", "Default.frag"} <= names:
        raise FormatError("Required real shader pair missing from catalog")
    return hashlib.sha256(contents).hexdigest()


def build_receipt(sources, bindings, images, vulkan, launch_configuration=None):
    inputs = {"sources": sources, "bindings": bindings, "images": images,
              "launch_configuration": launch_configuration,
              "vulkan": {key: vulkan[key] for key in
                         ("platform", "version", "archive_sha256", "members_sha256")} if vulkan else None}
    return hashlib.sha256(json.dumps(inputs, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def build(root, output, bindings, reuse_romfs_archive=None, moltenvk=None, render_resolution="720p",
          *, gpu_capture_probe=False):
    if gpu_capture_probe and not moltenvk:
        raise FormatError("GPU capture probe requires the Vulkan profile")
    root, output = outside_git(root), outside_git(output)
    bindings = outside_git(bindings)
    if output.exists():
        raise FormatError("iOS package output must be a new private directory")
    extraction = json.loads((root / "extraction.json").read_text())
    if extraction["cnmt"]["title_id"] != "0100535012974000" or extraction["cnmt"]["version"] != 0:
        raise FormatError("iOS package requires the verified Hades base version")
    catalog_digest = verify_catalog(root, bindings)
    output.mkdir(mode=0o700, parents=True)
    flags = generate_artifacts(root, output / "generated", use_host_services=True, use_vulkan=moltenvk is not None,
                               render_resolution=render_resolution)
    plan = json.loads((output / "generated/plan.json").read_text())
    images = Path(plan["embedded_images_directory"])
    vulkan_receipt = None
    if moltenvk:
        from vulkan_dispatch import framework, framework_receipt, generate
        moltenvk = outside_git(moltenvk)
        include, library = framework(moltenvk, "ios")
        vulkan_receipt = framework_receipt(moltenvk, "ios")
        vulkan_receipt["dispatch"] = generate(output / "generated/vulkan_dispatch.inc", include)
        flags += ["-DHADES_VULKAN_HOST", "-I", str(include), "-I", str(output / "generated"),
                  str(REPO / "hades/runtime/vulkan.m"), str(library), "-lc++",
                  "-framework", "QuartzCore", "-framework", "IOSurface", "-framework", "CoreGraphics"]
    app = output / "Payload/HadesPortTest.app"
    app.mkdir(parents=True)
    app.chmod(0o700)
    identifier = "local.hades.porttest"
    launch_configuration = info_plist(identifier, render_resolution, gpu_capture_probe=gpu_capture_probe)
    launch_configuration["HadesAutoStartGame"] = moltenvk is not None
    icon_source = root / "data/control/icon_AmericanEnglish.dat"
    assets = output / "icons/Assets.xcassets/AppIcon.appiconset"
    app_icon = create_app_icons(icon_source, assets)
    launch_configuration["HadesAppIconSHA256"] = app_icon["source_sha256"]
    (app / "Info.plist").write_bytes(plistlib.dumps(launch_configuration))
    (app / "PkgInfo").write_bytes(b"APPL????")
    command(["xcrun", "actool", output / "icons/Assets.xcassets", "--compile", app,
             "--platform", "iphoneos", "--minimum-deployment-target", "17.0",
             "--target-device", "iphone", "--target-device", "ipad", "--app-icon", "AppIcon",
             "--output-partial-info-plist", output / "icons/partial.plist"], output / "icons/build.log")
    metadata = plistlib.loads((app / "Info.plist").read_bytes())
    metadata.update(plistlib.loads((output / "icons/partial.plist").read_bytes()))
    (app / "Info.plist").write_bytes(plistlib.dumps(metadata))
    sdk = command(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], output / "sdk.log")
    sdk_version = command(["xcrun", "--sdk", "iphoneos", "--show-sdk-version"], output / "sdk-version.log")
    metadata.update({"DTPlatformName": "iphoneos", "DTPlatformVersion": sdk_version,
                     "DTSDKName": "iphoneos" + sdk_version})
    (app / "Info.plist").write_bytes(plistlib.dumps(metadata))
    sources = [REPO / name for name in ("hades/ios/main.m", "hades/runtime/core.c",
               "hades/runtime/callout.c", "hades/runtime/lifecycle.c", "hades/runtime/audio.c",
               "hades/runtime/memory.c", "hades/runtime/ipc.c", "hades/runtime/threads.c",
               "hades/runtime/host.c", "hades/runtime/save.c", "hades/runtime/host_entry.S",
               "hades/runtime/probe.c", "hades/runtime/render_probe.m", "dm/host/entry.S",
               "dm/gfx/nvn/nvn_spirv_bind.c")]
    hashed = sources + [REPO / name for name in (
        "hades/runtime/core.h", "hades/runtime/callout.h", "hades/runtime/audio.h", "hades/runtime/memory.h", "hades/runtime/ipc.h",
        "hades/runtime/trace.h", "hades/runtime/lifecycle.h",
        "hades/runtime/diagnostics.h",
        "hades/ios/policy.h", "hades/ios/joystick.h", "hades/ios/fps.h",
        "hades/ios/monitor.h", "hades/ios/controls.h", "hades/ios/action_button.h",
        "hades/ios/haptics.h", "hades/runtime/display.h",
        "hades/runtime/threads.h", "hades/runtime/probes.h", "hades/ios/Entitlements.plist",
        "hades/runtime/host.h", "hades/runtime/save.h", "hades/runtime/host_bindings.def",
        "hades/tools/package_ios.py", "hades/tools/link_modules.py", "hades/tools/verify_embed.py", "hades/tools/display.py",
        "dm/ios/tools/gen_embed.py", "dm/gfx/nvn/nvn_spirv_bind.h",
        "dm/gfx/nvn/nvn_obj.h", "dm/gfx/nvn/nvn_tex.h", "dm/gfx/nvn/include/nvn.h")]
    if moltenvk:
        hashed += [REPO / name for name in ("hades/runtime/vulkan.m", "hades/runtime/vulkan.h",
                    "hades/runtime/vulkan_bindings.def", "hades/runtime/vulkan_optional.def",
                    "hades/tools/vulkan_dispatch.py", "hades/toolchain.json")]
    hashes = {str(path.relative_to(REPO)): sha256(path) for path in hashed}
    image_hashes = {name: sha256(images / (name + ".patched.bin"))
                    for name in ("main", "subsdk0", "subsdk1", "sdk", "eos")}
    build_identity = build_receipt(hashes, catalog_digest, image_hashes, vulkan_receipt, launch_configuration)
    metadata["HadesBuildReceipt"] = build_identity
    (app / "Info.plist").write_bytes(plistlib.dumps(metadata))
    executable = app / "HadesPortTest"
    command(["xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios26.0", "-isysroot", sdk,
             "-std=c11", "-O2", "-fobjc-arc", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
             "-DHADES_IOS_APP", "-DHADES_HOST_GATE_STACK", "-I", REPO / "hades/runtime",
             "-I", REPO / "dm/gfx/nvn/include", *sources,
             output / "generated/manifest.c", output / "generated/images.S",
             "-framework", "UIKit", "-framework", "Foundation", "-framework", "Metal",
             "-framework", "AudioToolbox",
             *flags, "-o", executable], output / "build.log")
    verification = verify_object(executable, images, ["main", "subsdk0", "subsdk1", "sdk", "eos"], linked=True)
    resources = app / "HadesData"
    if moltenvk:
        (app / "Licenses").mkdir()
        shutil.copy2(moltenvk / "ios/MoltenVK/LICENSE", app / "Licenses/MoltenVK.txt")
    startup = {}
    for name in (".nrr/game.nrr", "nro/EOSSDK-Switch-Shipping.nro"):
        source = root / "data/RomFS" / name
        target = resources / "RomFS" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        digest = sha256(source)
        shutil.copy2(source, target)
        if sha256(target) != digest:
            raise FormatError("Startup NRR/NRO changed during copy")
        startup[name] = {"sha256": digest, "bytes": source.stat().st_size}
    msl = copy_resources(root / "build/shaders-msl", resources / "MSL")
    shutil.copy2(bindings, resources / "bindings.json")
    if sha256(resources / "bindings.json") != catalog_digest:
        raise FormatError("Binding catalog changed during copy")
    romfs_archive = (outside_git(reuse_romfs_archive) if reuse_romfs_archive
                     else output / "Hades-RomFS-local-only.zip")
    romfs = (verify_romfs_archive(root / "data/RomFS", romfs_archive) if reuse_romfs_archive
             else archive_romfs(root / "data/RomFS", romfs_archive))
    write_json(resources / "RomFS-files.json", romfs)
    command(["codesign", "--force", "--sign", "-", "--entitlements", REPO / "hades/ios/Entitlements.plist",
             "--timestamp=none", app], output / "adhoc-sign.log")
    command(["codesign", "--verify", "--strict", app], output / "signature-check.log")
    verified_modules(root)
    if any(sha256(images / (name + ".patched.bin")) != digest for name, digest in image_hashes.items()):
        raise FormatError("Embedded images changed during packaging")
    verification = verify_object(executable, images, ["main", "subsdk0", "subsdk1", "sdk", "eos"], linked=True)
    ipa = output / "HadesPortTest-resign-required.ipa"
    app_bytes = sum(path.stat().st_size for path in app.rglob("*") if path.is_file())
    if app_bytes > 4_000_000_000:
        raise FormatError("App exceeds the conservative 4 GB uncompressed iOS build limit")
    entries = archive_bundle(app, ipa)
    if any(sha256(path) != hashes[str(path.relative_to(REPO))] for path in hashed):
        raise FormatError("iOS package sources changed during build")
    if sha256(icon_source) != app_icon["source_sha256"]:
        raise FormatError("Hades control icon changed during packaging")
    report = {
        "status": "iphoneos-aot-diagnostic-ipa-requires-apple-resigning-not-playable",
        "build_receipt": build_identity,
        "launch_configuration": launch_configuration,
        "app_icon": app_icon,
        "ipa": ipa.name, "ipa_sha256": sha256(ipa), "ipa_bytes": ipa.stat().st_size,
        "bundle_identifier": identifier, "minimum_ios": "26.0", "architectures": ["arm64"],
        "target_device": "iPhone 15 Pro Max / A17 Pro / iPhone16,2; not yet device-verified",
        "host_services_enabled": True,
        "vulkan_profile": moltenvk is not None, "vulkan_framework": vulkan_receipt,
        "render_resolution": plan["render_resolution"],
        "embedded_images_sha256": image_hashes,
        "host_binding_names": json.loads((output / "generated/plan.json").read_text())["host_binding_names"],
        "iphoneos_sdk": sdk_version, "bindings_sha256": catalog_digest,
        "uncompressed_app_bytes": app_bytes, "uncompressed_app_limit_bytes": 4_000_000_000,
        "linker_verification": verification, "local_adhoc_signature_verified": True,
        "apple_development_or_distribution_signature": False, "device_installable_without_resigning": False,
        "device_runtime_verified": False, "game_entry_executed": False, "game_onscreen_verified": False,
        "guest_heap_policy_mib": 2048,
        "shader_compilation": "OS Metal runtime source compilation on device; no Metal CLI dependency",
        "romfs": {"files": len(romfs), "bytes": sum(value["bytes"] for value in romfs.values()),
                  "archive": str(romfs_archive), "archive_sha256": sha256(romfs_archive),
                  "archive_bytes": romfs_archive.stat().st_size,
                  "delivery": "separate private local archive; USB transfer to Documents/RomFS; full per-file verification before game probe"},
        "resources_sha256": {"startup_NRR_NRO": startup, "MSL": msl},
        "ipa_entries_sha256": entries, "source_sha256": hashes,
        "limitations": ["Host-symbol macOS probe enters nnMain; Vulkan profile reads actual assets and atomically commits local save data",
                        "Required filesystem, RO transport, gameplay graphics correctness and device input behavior incompletely verified",
                        "Core/TSD/signed-page remap and GPU must be verified on a physical device",
                        "Game mode cooperatively parks guest/outer host calls, drains GPU and pauses audio; device suspend/resume remains unverified for this build",
                        "Safe lifecycle transitions freeze guest time; a failed 1.5-second transition retains exit-75 protection; GPU/SDK-only diagnostic modes remain foreground-only",
                        "Suspension is not forced saving or protection against iOS memory-pressure termination; uncommitted progress may be lost",
                        "Self-task resident/peak/physical-footprint/virtual and Metal allocation metrics are separate overlapping accounts, not gameplay or device-memory-limit proof",
                        "Re-sign with a local Apple provisioning profile; requested memory entitlements may require a paid team",
                        "IPA and separate RomFS archive include private game assets; do not upload or share publicly",
                        "Actual ROM cache, local identity and snapshot save transactions supported; process-interruption tests are not power-loss guarantees",
                        "Local-only NIFM/BSD; biased CodeData live alias passes native socket initialization in the Vulkan profile",
                        "Configured keyboard/touch frontend publishes real FullKey/SystemExt shared-memory rings; no physical Switch controller claimed",
                        "Typed allocation/debug callbacks use guarded reentrant stacks; foreign driver threads still lack SDK contexts",
                        "Native macOS game creates shaders and submits draws/presents; this is not pixel correctness or iOS gameplay evidence",
                        "Real AudioQueue PCM output/release events supported; no verified audible game sound or device audio",
                        "2048 MiB heap is an explicit host policy; 512/1024 MiB probes fail during rendering; device RSS/limits unverified",
                        "Native own-swapchain GPU readback and controlled keys verify main menu, options page and return; no complete gameplay/device proof",
                        "Native fresh-save route runs 125 seconds/14546 presents with zero SUBOPTIMAL at fixed 1280x720; late scenes unverified visually",
                        "iOS game probe requests fullscreen landscape 16:9 rendering with 12 touch buttons; geometry and controls need physical device verification",
                        "Default dispatch/IPC traces report first observations and periodic presents; HADES_TRACE=1 restores full dispatch/IPC diagnostics",
                        "Headless/no-input route still stops at SelfController command 9; library-applet backend is not fabricated",
                        "Virtual keyboard/touch has no rumble actuators; actual exported vibration-handle query returns an empty capability set",
                        "Full RomFS is separated to stay below the uncompressed app build size limit; no verified playable session"]}
    write_json(output / "package.json", report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--bindings", required=True, type=Path)
    parser.add_argument("--moltenvk", type=Path, help="Verified private official public-API static framework root")
    parser.add_argument("--render-resolution", choices=("720p", "1080p", "wide-720p"), default="720p",
                        help="Experimental profiles adjust build-identified guest window constants")
    parser.add_argument("--reuse-romfs-archive", type=Path,
                        help="Reverify and reference an existing private local archive without recompressing/copying it")
    parser.add_argument("--gpu-capture-probe", action="store_true",
                        help="Opt in to intrusive GPU pixel/pass readback; changes timing and can terminate at diagnostic limits")
    args = parser.parse_args()
    try:
        build(args.work, args.out, args.bindings, args.reuse_romfs_archive, args.moltenvk, args.render_resolution,
              gpu_capture_probe=args.gpu_capture_probe)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print(f"Hades iOS packaging failed: {error}", file=sys.stderr)
        return 2
    print("Private iPhoneOS diagnostic IPA built; Apple re-signing and physical device verification required; not a playable port")
    return 0


if __name__ == "__main__":
    sys.exit(main())
