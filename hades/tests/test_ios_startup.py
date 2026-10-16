from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("xcrun") and
    Path("/System/iOSSupport/System/Library/Frameworks/UIKit.framework").is_dir(),
    "Requires existing Mac Catalyst UIKit runtime")
class IOSStartupTests(unittest.TestCase):
    def test_actual_controller_autostart_monitor_log_loading_and_resource_checks(self):
        sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "HadesStartupProbe.app"
            executable = app / "Contents/MacOS/HadesStartupProbe"
            executable.parent.mkdir(parents=True)
            metadata = {
                "CFBundleIdentifier": "local.hades.startupprobe",
                "CFBundleExecutable": "HadesStartupProbe", "CFBundleName": "HadesStartupProbe",
                "CFBundlePackageType": "APPL", "LSUIElement": True, "LSRequiresIPhoneOS": True,
                "UIDeviceFamily": [2], "CFBundleSupportedPlatforms": ["MacOSX"],
                "LSMinimumSystemVersion": "13.0",
                "UIApplicationSceneManifest": {"UIApplicationSupportsMultipleScenes": False,
                    "UISceneConfigurations": {"UIWindowSceneSessionRoleApplication": [{
                        "UISceneClassName": "UIWindowScene", "UISceneConfigurationName": "StartupProbe",
                        "UISceneDelegateClassName": "HadesStartupProbeScene"}]}}}
            (app / "Contents/Info.plist").write_bytes(plistlib.dumps(metadata))
            frameworks = sdk + "/System/iOSSupport/System/Library/Frameworks"
            for optimization in ("-O0", "-O2"):
                build = subprocess.run(["xcrun", "--sdk", "macosx", "clang",
                    "-target", "arm64-apple-ios26.0-macabi", "-isysroot", sdk,
                    "-iframework", frameworks, "-F", frameworks, "-std=c11", "-fobjc-arc",
                    "-DHADES_VULKAN_HOST", optimization, "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-deprecated-declarations",
                    str(REPO / "hades/tests/ios_startup_probe.m"),
                    str(REPO / "hades/runtime/lifecycle.c"),
                    "-framework", "UIKit", "-framework", "Foundation", "-framework", "QuartzCore",
                    "-framework", "Metal", "-framework", "CoreGraphics", "-o", str(executable)],
                    capture_output=True, text=True, timeout=60)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                subprocess.run(["codesign", "--force", "--sign", "-", str(app)],
                    check=True, capture_output=True)
                run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("UIKIT_AUTOSTART_MONITOR_LOG_AND_LOADING_OK", run.stdout)


if __name__ == "__main__":
    unittest.main()
