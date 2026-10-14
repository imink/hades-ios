import hashlib
import json
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from inspect_target import FormatError
from package_ios import info_plist
from prepare import REPO
from sign_ios import MEMORY_ENTITLEMENTS
from xcode_project import generate


class XcodeProjectTests(unittest.TestCase):
    def fixture(self, root):
        package = root / "package"
        app = package / "Payload/HadesPortTest.app"
        (app / "HadesData").mkdir(parents=True)
        (app / "HadesData/synthetic.txt").write_bytes(b"synthetic-only")
        (app / "Info.plist").write_bytes(plistlib.dumps(info_plist("local.hades.porttest")))
        (app / "HadesPortTest").write_bytes(b"synthetic-not-executable")
        (app / "Assets.car").write_bytes(b"synthetic")
        (app / "SyntheticIcon.png").write_bytes(b"synthetic-png-only")
        (app / "PkgInfo").write_bytes(b"APPL????")
        (package / "generated").mkdir()
        for name in ("manifest.c", "images.S", "vulkan_dispatch.inc", "relocations.bin"):
            (package / "generated" / name).write_text("")
        (package / "generated/plan.json").write_text(json.dumps({"modules": ["main", "subsdk0", "subsdk1", "sdk", "eos"]}))
        source = "hades/runtime/core.c"
        receipt = {"vulkan_profile": True, "local_adhoc_signature_verified": True,
                   "ipa_sha256": "synthetic-receipt-not-an-IPA",
                   "source_sha256": {source: hashlib.sha256((REPO / source).read_bytes()).hexdigest()},
                   "ipa_entries_sha256": {path.relative_to(package).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                                          for path in app.rglob("*") if path.is_file()}}
        (package / "package.json").write_text(json.dumps(receipt))
        include, library = root / "include", root / "framework/libMoltenVK.a"
        return package, receipt, include, library

    def test_native_target_scheme_resources_link_search_and_memory_policy(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            package, _, include, library = self.fixture(root)
            for reduced in (False, True):
                output = root / str(reduced)
                with patch("xcode_project.framework", return_value=(include, library)):
                    report = generate(package, output, root / "framework", "local.hades.test", reduced)
                model = plistlib.loads((output / "HadesPortTest.xcodeproj/project.pbxproj").read_bytes())
                objects = model["objects"]
                targets = [value for value in objects.values() if value["isa"] == "PBXNativeTarget"]
                self.assertEqual(len(targets), 1)
                self.assertEqual(targets[0]["productType"], "com.apple.product-type.application")
                configs = [value["buildSettings"] for value in objects.values()
                           if value["isa"] == "XCBuildConfiguration" and value["buildSettings"]]
                for config in configs:
                    self.assertEqual(config["ARCHS"], "arm64")
                    self.assertEqual(config["SUPPORTED_PLATFORMS"], "iphoneos")
                    self.assertEqual(config["CODE_SIGN_STYLE"], "Automatic")
                    self.assertNotIn("DEVELOPMENT_TEAM", config)
                    self.assertIn(str(library.parent), config["LIBRARY_SEARCH_PATHS"])
                    self.assertEqual(config["DEAD_CODE_STRIPPING"], "NO")
                    self.assertEqual(config["APPLY_RULES_IN_COPY_FILES"], "NO")
                    self.assertEqual(len(config["OTHER_LDFLAGS"]), 7)
                self.assertFalse(any(value["isa"] == "PBXShellScriptBuildPhase" for value in objects.values()))
                resource_refs = [value["path"] for value in objects.values() if value["isa"] == "PBXFileReference"]
                self.assertIn(str(package / "Payload/HadesPortTest.app/HadesData"), resource_refs)
                self.assertNotIn(str(package / "Payload/HadesPortTest.app/PkgInfo"), resource_refs)
                copy_phases = [value for value in objects.values() if value["isa"] == "PBXCopyFilesBuildPhase"]
                self.assertEqual(len(copy_phases), 1)
                self.assertEqual(copy_phases[0]["dstSubfolderSpec"], 7)
                self.assertEqual(len(copy_phases[0]["files"]), 1)
                memory = plistlib.loads((output / "Hades.entitlements").read_bytes())
                self.assertEqual(memory, {} if reduced else {key: True for key in MEMORY_ENTITLEMENTS})
                metadata = plistlib.loads((output / "Info.plist").read_bytes())
                self.assertEqual(metadata["CFBundleIdentifier"], "$(PRODUCT_BUNDLE_IDENTIFIER)")
                self.assertNotIn("UIDeviceFamily", metadata)
                scheme = ET.parse(output / "HadesPortTest.xcodeproj/xcshareddata/xcschemes/HadesPortTest.xcscheme")
                self.assertEqual(scheme.find(".//BuildableReference").get("BlueprintIdentifier"),
                                 next(key for key, value in objects.items() if value["isa"] == "PBXNativeTarget"))
                self.assertFalse(report["device_runtime_verified"])
                frozen=output / "Sources/hades/runtime/core.c"
                self.assertEqual(frozen.read_bytes(),(REPO / "hades/runtime/core.c").read_bytes())
                self.assertFalse(frozen.stat().st_mode & 0o222)
                self.assertTrue(any(value.get("path")==str(frozen) for value in objects.values()))
                self.assertTrue(all(str(output / "Sources/hades/runtime") in config["HEADER_SEARCH_PATHS"] for config in configs))

    def test_stale_source_or_private_resource_is_rejected_before_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            package, receipt, include, library = self.fixture(root)
            with patch("xcode_project.framework", return_value=(include, library)):
                receipt["source_sha256"]["hades/runtime/core.c"] = "0" * 64
                (package / "package.json").write_text(json.dumps(receipt))
                with self.assertRaisesRegex(FormatError, "source changed"):
                    generate(package, root / "stale", root / "framework", "local.hades.test")
                self.assertFalse((root / "stale").exists())
                receipt["source_sha256"]["hades/runtime/core.c"] = hashlib.sha256((REPO / "hades/runtime/core.c").read_bytes()).hexdigest()
                (package / "package.json").write_text(json.dumps(receipt))
                (package / "Payload/HadesPortTest.app/HadesData/synthetic.txt").write_bytes(b"changed")
                with self.assertRaisesRegex(FormatError, "resource differs"):
                    generate(package, root / "changed", root / "framework", "local.hades.test")
                self.assertFalse((root / "changed").exists())
