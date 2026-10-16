from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
class LifecycleTests(unittest.TestCase):
    def test_cooperative_execution_waits_callbacks_clocks_and_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "lifecycle")
            for optimization in ("-O0", "-O2"):
                subprocess.run(["xcrun", "clang", "-std=c11", optimization,
                                "-Wall", "-Wextra", "-Werror",
                                str(REPO / "hades/tests/lifecycle_probe.c"),
                                str(REPO / "hades/runtime/lifecycle.c"), "-o", executable],
                               check=True, capture_output=True, timeout=30)
                for case, marker in (("", "LIFECYCLE_REPEATED_PAUSE_WAIT_CALLBACK_TIMER_OK"),
                                     ("timeout", "LIFECYCLE_TIMEOUT_OK"),
                                     ("failure", "LIFECYCLE_HOOK_FAILURE_OK"),
                                     ("resume-failure", "LIFECYCLE_RESUME_FAILURE_PARKED_OK")):
                    run = subprocess.run([executable] + ([case] if case else []),
                                         capture_output=True, text=True, timeout=15)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn(marker, run.stdout)


if __name__ == "__main__":
    unittest.main()
