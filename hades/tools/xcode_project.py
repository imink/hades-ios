#!/usr/bin/env python3
"""Generate a private native Xcode target from an already verified Hades iOS package."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import sys
import xml.etree.ElementTree as ET

from inspect_target import FormatError
from package_ios import info_plist
from prepare import outside_git, REPO, sha256, write_json
from sign_ios import MEMORY_ENTITLEMENTS
from vulkan_dispatch import framework


class Project:
    def __init__(self):
        self.objects = {}

    def add(self, kind, key, **values):
        identifier = hashlib.sha256((kind + ":" + key).encode()).hexdigest()[:24].upper()
        if identifier in self.objects:
            raise FormatError("Duplicate Xcode project object")
        self.objects[identifier] = {"isa": kind, **values}
        return identifier

    def file(self, path, filetype):
        path = Path(path)
        return self.add("PBXFileReference", str(path), path=str(path),
                        sourceTree="<absolute>", lastKnownFileType=filetype)

    def phase(self, name, kind, references):
        files = [self.add("PBXBuildFile", name + ":" + reference, fileRef=reference)
                 for reference in references]
        return self.add(kind, name, buildActionMask=2147483647, files=files,
                        runOnlyForDeploymentPostprocessing=0)

    def configurations(self, name, settings):
        configurations = [self.add("XCBuildConfiguration", name + configuration,
                                  name=configuration, buildSettings=settings)
                          for configuration in ("Debug", "Release")]
        return self.add("XCConfigurationList", name, buildConfigurations=configurations,
                        defaultConfigurationIsVisible=0, defaultConfigurationName="Release")


def generate(package, output, moltenvk, bundle, reduced=False):
    package, output, moltenvk = map(outside_git, (package, output, moltenvk))
    info_plist(bundle)
    if output.exists():
        raise FormatError("Xcode project output must be a new private directory")
    receipt = json.loads((package / "package.json").read_text())
    if not receipt.get("vulkan_profile") or not receipt.get("local_adhoc_signature_verified"):
        raise FormatError("Xcode handoff requires a verified Hades Vulkan iOS package")
    include, library = framework(moltenvk, "ios")
    for name, digest in receipt["source_sha256"].items():
        if sha256(REPO / name) != digest:
            raise FormatError("Package source changed; rebuild the private package before Xcode handoff")
    original = package / "Payload/HadesPortTest.app"
    for name, digest in receipt["ipa_entries_sha256"].items():
        relative = Path(name)
        if relative.parts[:2] != ("Payload", "HadesPortTest.app") or ".." in relative.parts:
            raise FormatError("Unexpected verified resource path")
        path = package / relative
        if path.is_symlink() or not path.is_file() or sha256(path) != digest:
            raise FormatError("Private package resource differs from its verified receipt")
    plan = json.loads((package / "generated/plan.json").read_text())
    if plan.get("modules") != ["main", "subsdk0", "subsdk1", "sdk", "eos"]:
        raise FormatError("Unexpected Hades native embedding plan")
    output.mkdir(parents=True, mode=0o700)
    snapshot=output / "Sources"
    for name,digest in receipt["source_sha256"].items():
        relative=Path(name)
        if relative.is_absolute() or ".." in relative.parts:
            raise FormatError("Unsafe source snapshot path")
        contents=(REPO / relative).read_bytes()
        if hashlib.sha256(contents).hexdigest()!=digest:
            raise FormatError("Source changed while freezing Xcode handoff")
        destination=snapshot / relative
        destination.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
        destination.write_bytes(contents)
        destination.chmod(0o400)
    generated=output / "Generated"
    generated.mkdir()
    generated_hashes={}
    for name in ("manifest.c","images.S","vulkan_dispatch.inc","relocations.bin","plan.json"):
        contents=(package / "generated" / name).read_bytes()
        (generated / name).write_bytes(contents)
        (generated / name).chmod(0o400)
        generated_hashes[name]=hashlib.sha256(contents).hexdigest()
    project_directory = output / "HadesPortTest.xcodeproj"
    project_directory.mkdir()
    metadata = plistlib.loads((original / "Info.plist").read_bytes())
    metadata["CFBundleIdentifier"] = "$(PRODUCT_BUNDLE_IDENTIFIER)"
    metadata["CFBundleExecutable"] = "$(EXECUTABLE_NAME)"
    metadata.pop("UIDeviceFamily", None)
    (output / "Info.plist").write_bytes(plistlib.dumps(metadata))
    entitlements = {} if reduced else {key: True for key in MEMORY_ENTITLEMENTS}
    (output / "Hades.entitlements").write_bytes(plistlib.dumps(entitlements))
    model = Project()
    source_paths = [snapshot / name for name in receipt["source_sha256"]
                    if Path(name).suffix in (".c", ".m", ".S")]
    source_paths.extend(generated / name for name in ("manifest.c", "images.S"))
    sources = [model.file(path, {".c": "sourcecode.c.c", ".m": "sourcecode.c.objc",
                               ".S": "sourcecode.asm"}[path.suffix]) for path in source_paths]
    framework_names = ("UIKit", "Foundation", "Metal", "AudioToolbox", "QuartzCore", "IOSurface", "CoreGraphics")
    frameworks = [model.add("PBXFileReference", name, path=f"System/Library/Frameworks/{name}.framework",
                            sourceTree="SDKROOT", lastKnownFileType="wrapper.framework")
                  for name in framework_names]
    frameworks.append(model.file(library, "archive.ar"))
    resource_paths = [original / "HadesData"]
    resource_paths.extend(path for path in sorted(original.iterdir())
                          if path.is_file() and path.suffix != ".png"
                          and path.name not in ("HadesPortTest", "Info.plist", "PkgInfo", "embedded.mobileprovision"))
    resources = [model.file(path, "folder" if path.is_dir() else "file") for path in resource_paths]
    icons = [model.file(path, "file") for path in sorted(original.glob("*.png")) if path.is_file()]
    icon_phase = model.phase("verified-icons", "PBXCopyFilesBuildPhase", icons)
    model.objects[icon_phase].update(dstPath="", dstSubfolderSpec=7, name="Preserve verified icon bytes")
    product = model.add("PBXFileReference", "product", explicitFileType="wrapper.application",
                        path="HadesPortTest.app", sourceTree="BUILT_PRODUCTS_DIR")
    product_group = model.add("PBXGroup", "products", name="Products", children=[product], sourceTree="<group>")
    groups = [model.add("PBXGroup", name, name=name, children=references, sourceTree="<group>")
              for name, references in (("Sources", sources), ("Frameworks", frameworks), ("Resources", resources + icons))]
    main_group = model.add("PBXGroup", "main", children=groups + [product_group], sourceTree="<group>")
    settings = {
        "SDKROOT": "iphoneos", "SUPPORTED_PLATFORMS": "iphoneos", "ARCHS": "arm64",
        "IPHONEOS_DEPLOYMENT_TARGET": "26.0", "TARGETED_DEVICE_FAMILY": "1,2",
        "PRODUCT_NAME": "HadesPortTest", "PRODUCT_BUNDLE_IDENTIFIER": bundle,
        "INFOPLIST_FILE": str(output / "Info.plist"), "GENERATE_INFOPLIST_FILE": "NO",
        "CODE_SIGN_STYLE": "Automatic", "CODE_SIGN_ENTITLEMENTS": str(output / "Hades.entitlements"),
        "CLANG_ENABLE_OBJC_ARC": "YES", "GCC_C_LANGUAGE_STANDARD": "c11",
        "GCC_OPTIMIZATION_LEVEL": "2", "GCC_TREAT_WARNINGS_AS_ERRORS": "YES",
        "GCC_PREPROCESSOR_DEFINITIONS": ["$(inherited)", "HADES_IOS_APP=1", "HADES_HOST_GATE_STACK=1", "HADES_VULKAN_HOST=1"],
        "HEADER_SEARCH_PATHS": [str(snapshot / "hades/runtime"), str(snapshot / "dm/gfx/nvn/include"),
                                str(include), str(generated)],
        "LIBRARY_SEARCH_PATHS": ["$(inherited)", str(library.parent)],
        "OTHER_CFLAGS": ["$(inherited)", "-Wall", "-Wextra", "-Wno-deprecated-declarations"],
        "OTHER_LDFLAGS": ["$(inherited)", "-lc++"] + [f"-Wl,-segprot,__DM{name}W,rw,rw" for name in plan["modules"]],
        "DEAD_CODE_STRIPPING": "NO", "COPY_PHASE_STRIP": "NO", "STRIP_INSTALLED_PRODUCT": "NO",
        "APPLY_RULES_IN_COPY_FILES": "NO",
        "ENABLE_BITCODE": "NO", "ENABLE_USER_SCRIPT_SANDBOXING": "YES",
        "ALWAYS_SEARCH_USER_PATHS": "NO",
    }
    target = model.add("PBXNativeTarget", "HadesPortTest", name="HadesPortTest", productName="HadesPortTest",
                       productReference=product, productType="com.apple.product-type.application",
                       buildConfigurationList=model.configurations("target", settings),
                       buildPhases=[model.phase("sources", "PBXSourcesBuildPhase", sources),
                                    model.phase("frameworks", "PBXFrameworksBuildPhase", frameworks),
                                    model.phase("resources", "PBXResourcesBuildPhase", resources), icon_phase],
                       buildRules=[], dependencies=[])
    root = model.add("PBXProject", "project", attributes={"LastUpgradeCheck": "2700"},
                     buildConfigurationList=model.configurations("project", {}),
                     compatibilityVersion="Xcode 14.0", developmentRegion="en", knownRegions=["en", "Base"],
                     mainGroup=main_group, productRefGroup=product_group, projectDirPath="", projectRoot="",
                     targets=[target])
    (project_directory / "project.pbxproj").write_bytes(plistlib.dumps(
        {"archiveVersion": "1", "objectVersion": "56", "classes": {}, "objects": model.objects, "rootObject": root}))
    scheme = ET.Element("Scheme", LastUpgradeVersion="2700", version="1.3")
    reference = {"BuildableIdentifier": "primary", "BlueprintIdentifier": target,
                 "BuildableName": "HadesPortTest.app", "BlueprintName": "HadesPortTest",
                 "ReferencedContainer": "container:HadesPortTest.xcodeproj"}
    build = ET.SubElement(scheme, "BuildAction", parallelizeBuildables="YES", buildImplicitDependencies="YES")
    entry = ET.SubElement(ET.SubElement(build, "BuildActionEntries"), "BuildActionEntry",
                          **{name: "YES" for name in ("buildForTesting", "buildForRunning", "buildForProfiling", "buildForArchiving", "buildForAnalyzing")})
    ET.SubElement(entry, "BuildableReference", **reference)
    launch = ET.SubElement(scheme, "LaunchAction", buildConfiguration="Release",
                           selectedDebuggerIdentifier="Xcode.DebuggerFoundation.Debugger.LLDB",
                           selectedLauncherIdentifier="Xcode.IDEFoundation.Launcher.LLDB",
                           launchStyle="0", useCustomWorkingDirectory="NO", ignoresPersistentStateOnLaunch="NO",
                           debugDocumentVersioning="YES", debugServiceExtension="internal", allowLocationSimulation="NO")
    ET.SubElement(ET.SubElement(launch, "BuildableProductRunnable", runnableDebuggingMode="0"),
                  "BuildableReference", **reference)
    ET.SubElement(scheme, "ArchiveAction", buildConfiguration="Release", revealArchiveInOrganizer="YES")
    scheme_directory = project_directory / "xcshareddata/xcschemes"
    scheme_directory.mkdir(parents=True)
    ET.ElementTree(scheme).write(scheme_directory / "HadesPortTest.xcscheme", encoding="UTF-8", xml_declaration=True)
    report = {"status": "native-xcode-target-generated-not-device-verified", "source_package_sha256": receipt["ipa_sha256"],
              "source_sha256": receipt["source_sha256"], "generator_sha256": sha256(Path(__file__)),
              "source_policy": "read-only source snapshot; subsequent worktree changes cannot alter this handoff",
              "generated_source_sha256": generated_hashes,
              "memory_authorization": "explicit-reduced-diagnostic" if reduced else "requires-profile-memory-grants",
              "source_files": len(sources), "device_runtime_verified": False,
              "limitations": ["User must select their own local Apple signing team and trusted physical device in Xcode",
                              "No accounts, devices or capabilities registered; no provisioning updates or uploads performed",
                              "Do not use the simulator or claim device gameplay from an unsigned build",
                              "Source/package/framework paths must remain available; full RomFS still requires local USB transfer"]}
    write_json(output / "xcode-project.json", report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--moltenvk", required=True, type=Path)
    parser.add_argument("--bundle-id", default="local.hades.porttest")
    parser.add_argument("--diagnostic-without-memory-entitlements", action="store_true")
    args = parser.parse_args()
    previous = os.umask(0o077)
    try:
        generate(args.package, args.out, args.moltenvk, args.bundle_id, args.diagnostic_without_memory_entitlements)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Local Hades Xcode handoff stopped: {error}", file=sys.stderr)
        return 2
    finally:
        os.umask(previous)
    print("Private native Xcode target created; local Apple team/device selection and gameplay verification required")
    return 0


if __name__ == "__main__":
    sys.exit(main())
