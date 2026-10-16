from pathlib import Path
from contextlib import redirect_stdout
import io
import plistlib
import json
import hashlib
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from inspect_target import FormatError
import link_modules
import package_ios
from package_ios import archive_bundle, archive_romfs, build_receipt, copy_resources, create_app_icons, info_plist, verify_catalog, verify_romfs_archive
from verify_embed import verify_object


class IOSPackageTests(unittest.TestCase):
    def test_build_receipt_tracks_code_images_without_output_path_dependence(self):
        framework = {"platform": "ios", "version": "synthetic", "archive_sha256": "a",
                     "members_sha256": {"header": "b"}, "dispatch": {"output": "/first/path"}}
        first = build_receipt({"source": "s"}, "bindings", {"image": "i"}, framework)
        framework["dispatch"]["output"] = "/second/path"
        self.assertEqual(first, build_receipt({"source": "s"}, "bindings", {"image": "i"}, framework))
        self.assertNotEqual(first, build_receipt({"source": "changed"}, "bindings", {"image": "i"}, framework))
        self.assertNotEqual(first, build_receipt({"source": "s"}, "bindings", {"image": "changed"}, framework))
        self.assertNotEqual(first, build_receipt({"source": "s"}, "other-bindings", {"image": "i"}, framework))
        self.assertEqual(len(first), 64)

    def test_romfs_is_separate_with_complete_inventory_and_no_online_delivery(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "RomFS"
            (source / "Content").mkdir(parents=True)
            (source / "Content/test.bin").write_bytes(b"synthetic test asset")
            destination = root / "resources.zip"
            records = archive_romfs(source, destination)
            self.assertEqual(verify_romfs_archive(source, destination), records)
            self.assertEqual(set(records), {"RomFS/Content/test.bin"})
            with zipfile.ZipFile(destination) as archive:
                self.assertEqual(json.loads(archive.read("RomFS-files.json")), records)
                self.assertEqual(archive.read("RomFS/Content/test.bin"), b"synthetic test asset")
            with self.assertRaises(FormatError):
                archive_romfs(source, destination)
            (source / "Content/test.bin").write_bytes(b"changed")
            with self.assertRaisesRegex(FormatError, "inventory differs"):
                verify_romfs_archive(source, destination)

    def test_macos_probe_keeps_generated_assembly_path_after_refactor(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            with patch.object(link_modules, "generate_artifacts", return_value=["-Wl,-example"]), \
                    patch.object(link_modules.subprocess, "run",
                                 return_value=SimpleNamespace(returncode=0, stdout="", stderr="")) as compiler:
                link_modules.generate_probe(output, output)
            arguments = compiler.call_args.args[0]
            self.assertIn(str(output / "manifest.c"), arguments)
            self.assertIn(str(output / "images.S"), arguments)
            self.assertIn("-Wl,-example", arguments)

    def test_incomplete_or_unsafe_shader_catalog_is_not_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bindings = root / "bindings.json"
            bindings.write_text(json.dumps({"status": "direct-msl-interface-verified",
                                           "shader_count": 72, "shaders": []}))
            with self.assertRaisesRegex(FormatError, "complete verified"):
                verify_catalog(root, bindings)
            for name in ("../escape.frag", "sub/file.frag", "sub\\file.frag", ".."):
                bindings.write_text(json.dumps({"status": "direct-msl-interface-verified",
                                               "shader_count": 72,
                                               "shaders": [{"file": name, "stage": "fragment"}] * 72}))
                with self.subTest(name=name), self.assertRaisesRegex(FormatError, "Invalid or duplicate"):
                    verify_catalog(root, bindings)

    def test_bundle_identity_and_scene_are_hades_specific(self):
        metadata = info_plist("local.hades.porttest")
        self.assertEqual(metadata["CFBundleExecutable"], "HadesPortTest")
        self.assertEqual(metadata["CFBundleSupportedPlatforms"], ["iPhoneOS"])
        self.assertEqual(metadata["MinimumOSVersion"], "26.0")
        self.assertEqual(metadata["CFBundleVersion"], "37")
        self.assertEqual(metadata["CFBundleName"], "Hades")
        self.assertEqual(metadata["CFBundleDisplayName"], "Hades")
        self.assertEqual(metadata["CFBundleIdentifier"], "local.hades.porttest")
        self.assertIs(metadata["HadesSafePauseResume"], True)
        self.assertIs(metadata["HadesAutoStartGame"], True)
        self.assertEqual(metadata["HadesRenderResolution"], "720p")
        self.assertEqual(info_plist("local.hades.porttest", "1080p")["HadesRenderResolution"], "1080p")
        self.assertEqual(info_plist("local.hades.porttest", "wide-720p")["HadesRenderResolution"], "wide-720p")
        self.assertIs(metadata["HadesDirectoryOrderProbe"], True)
        self.assertIs(metadata["HadesVulkanAttachmentLoadProbe"], False)
        self.assertIs(metadata["HadesVulkanPassCaptureProbe"], False)
        self.assertEqual(metadata["HadesVulkanBindingMode"], "argument-buffers")
        self.assertFalse(metadata["HadesVulkanQueueCompletionProbe"])
        self.assertFalse(metadata["HadesVulkanPixelCaptureProbe"])
        self.assertTrue(metadata["UIRequiresFullScreen"])
        self.assertNotIn("BAAppGroupID", metadata)
        scenes = metadata["UIApplicationSceneManifest"]["UISceneConfigurations"]
        self.assertEqual(scenes["UIWindowSceneSessionRoleApplication"][0]["UISceneDelegateClassName"],
                         "HadesSceneDelegate")
        self.assertEqual(plistlib.loads(plistlib.dumps(metadata)), metadata)
        for name in ("bad", "x/y.bundle", "x.bundle;cmd", "x..bundle"):
            with self.subTest(name=name), self.assertRaises(FormatError):
                info_plist(name)

    def test_gpu_readback_requires_explicit_opt_in_for_every_render_profile(self):
        for profile in ("720p", "1080p", "wide-720p"):
            with self.subTest(profile=profile):
                normal = info_plist("local.hades.porttest", profile)
                for key in ("HadesVulkanPixelCaptureProbe", "HadesVulkanPassCaptureProbe"):
                    self.assertIs(normal[key], False)
                capture = info_plist("local.hades.porttest", profile, gpu_capture_probe=True)
                self.assertEqual({key for key in normal if normal[key] != capture[key]},
                                 {"HadesVulkanPixelCaptureProbe", "HadesVulkanPassCaptureProbe"})
                for key in ("HadesVulkanPixelCaptureProbe", "HadesVulkanPassCaptureProbe"):
                    self.assertIs(capture[key], True)
                self.assertIs(normal["HadesDirectoryOrderProbe"], True)
                self.assertEqual(normal["HadesVulkanBindingMode"], "argument-buffers")
                self.assertEqual(plistlib.loads(plistlib.dumps(capture)), capture)
        for invalid in (None, 1, "true"):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(FormatError, "explicit boolean"):
                info_plist("local.hades.porttest", gpu_capture_probe=invalid)

    def test_build_receipt_distinguishes_launch_configuration(self):
        normal = info_plist("local.hades.porttest", "wide-720p")
        capture = info_plist("local.hades.porttest", "wide-720p", gpu_capture_probe=True)
        first = build_receipt({"source": "s"}, "bindings", {"image": "i"}, None, normal)
        self.assertEqual(first, build_receipt({"source": "s"}, "bindings", {"image": "i"},
                                             None, dict(reversed(list(normal.items())))))
        diagnostics = dict(normal, HadesAutoStartGame=False)
        branded = dict(normal, HadesAppIconSHA256="synthetic-icon-hash")
        renamed = dict(normal, CFBundleDisplayName="Other")
        for metadata in (capture, info_plist("local.hades.porttest", "720p"), diagnostics, branded, renamed):
            with self.subTest(metadata=metadata):
                self.assertNotEqual(first, build_receipt({"source": "s"}, "bindings", {"image": "i"},
                                                         None, metadata))

    def test_cli_defaults_to_no_gpu_capture_and_forwards_explicit_opt_in(self):
        arguments = ["package_ios.py", "--work", "private-work", "--out", "private-out",
                     "--bindings", "private-bindings", "--moltenvk", "private-framework"]
        for flags, enabled in (([], False), (["--gpu-capture-probe"], True)):
            with self.subTest(enabled=enabled), patch.object(sys, "argv", arguments + flags), \
                    patch.object(package_ios, "build") as builder, redirect_stdout(io.StringIO()):
                self.assertEqual(package_ios.main(), 0)
                self.assertIs(builder.call_args.kwargs["gpu_capture_probe"], enabled)

    def test_gpu_capture_rejects_non_vulkan_build_before_creating_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "package"
            with self.assertRaisesRegex(FormatError, "requires the Vulkan profile"):
                package_ios.build(Path(temporary), output, Path(temporary) / "bindings",
                                  gpu_capture_probe=True)
            self.assertFalse(output.exists())

    def test_local_control_icon_generates_all_sizes_without_modifying_source(self):
        def chunk(kind, body):
            return (struct.pack(">I", len(body)) + kind + body
                    + struct.pack(">I", zlib.crc32(kind + body)))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fixture = root / "synthetic.png"
            fixture.write_bytes(b"\x89PNG\r\n\x1a\n"
                                + chunk(b"IHDR", struct.pack(">IIBBBBB", 256, 256, 8, 2, 0, 0, 0))
                                + chunk(b"IDAT", zlib.compress((b"\0" + b"\x24\x56\x78" * 256) * 256))
                                + chunk(b"IEND", b""))
            source = root / "icon_AmericanEnglish.dat"
            subprocess.run(["sips", "-s", "format", "jpeg", str(fixture), "--out", str(source)],
                           check=True, capture_output=True)
            original = source.read_bytes()
            assets = root / "icons/AppIcon.appiconset"
            report = create_app_icons(source, assets)
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(report["source_sha256"], hashlib.sha256(original).hexdigest())
            images = json.loads((assets / "Contents.json").read_text())["images"]
            self.assertEqual(len(images), 18)
            self.assertEqual(set(report["assets_sha256"]), {image["filename"] for image in images})
            for image in images:
                data = (assets / image["filename"]).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), report["assets_sha256"][image["filename"]])
                self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
                size = int(float(image["size"].split("x")[0]) * int(image["scale"][:-1]))
                self.assertEqual(struct.unpack_from(">II", data, 16), (size, size))
                self.assertEqual(data[25], 2)  # Opaque RGB, not RGBA.
            with self.assertRaisesRegex(FormatError, "opaque 256x256 JPEG"):
                create_app_icons(fixture, root / "invalid/AppIcon.appiconset")
            with self.assertRaisesRegex(FormatError, "Missing regular"):
                create_app_icons(root / "missing.dat", root / "missing-icons")
            link = root / "linked.dat"
            link.symlink_to(source)
            with self.assertRaisesRegex(FormatError, "Missing regular"):
                create_app_icons(link, root / "linked-icons")

    def test_private_resource_copy_hashes_and_ipa_layout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            (source / ".nrr").mkdir(parents=True)
            (source / ".nrr/test.nrr").write_bytes(b"synthetic registration")
            app = root / "Payload/HadesPortTest.app"
            resources = app / "HadesData/RomFS"
            inventory = copy_resources(source, resources)
            self.assertEqual(set(inventory), {".nrr/test.nrr"})
            (app / "Info.plist").write_bytes(plistlib.dumps(info_plist("local.hades.porttest")))
            binary = app / "HadesPortTest"
            binary.write_bytes(b"synthetic executable")
            binary.chmod(0o755)
            archive = root / "test.ipa"
            records = archive_bundle(app, archive)
            self.assertEqual(len(records), 3)
            with zipfile.ZipFile(archive) as bundle:
                self.assertEqual(bundle.read("Payload/HadesPortTest.app/HadesData/RomFS/.nrr/test.nrr"),
                                 b"synthetic registration")
                self.assertEqual((bundle.getinfo("Payload/HadesPortTest.app/HadesPortTest").external_attr >> 16) & 0o777,
                                 0o755)
            with self.assertRaises(FormatError):
                archive_bundle(app, archive)
            with self.assertRaises(FormatError):
                copy_resources(source, resources)

    def test_symlink_resources_and_empty_directories_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            with self.assertRaises(FormatError):
                copy_resources(source, root / "empty")
            target = root / "target"
            target.write_bytes(b"not a bundled regular file")
            (source / "link").symlink_to(target)
            with self.assertRaises(FormatError):
                copy_resources(source, root / "symlink")

    def test_linked_macho_requires_device_platform_pie_and_single_rx_segment(self):
        def image(platform=2, flags=0x200000, permissions=5, segment="__TEXT"):
            command = struct.pack("<II16sQQQQiiII", 0x19, 72, segment.encode(),
                                  0x100000000, 0x4000, 0, 128, permissions, permissions, 0, 0)
            command += struct.pack("<6I", 0x32, 24, platform, 0x110000, 0x1B0000, 0)
            return struct.pack("<IiiIIIII", 0xFEEDFACF, 0x0100000C, 0, 2, 2, len(command), flags, 0) + command
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "linked"
            binary.write_bytes(image())
            report = verify_object(binary, root, [], linked=True)
            self.assertTrue(report["linked_permissions_verified"])
            self.assertFalse(report["codesign_verified"])
            for values in ({"platform": 7}, {"flags": 0}, {"permissions": 7}, {"segment": "__OTHER"}):
                binary.write_bytes(image(**values))
                with self.subTest(values=values), self.assertRaises(FormatError):
                    verify_object(binary, root, [], linked=True)


if __name__ == "__main__":
    unittest.main()
