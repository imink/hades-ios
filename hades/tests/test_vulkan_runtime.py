import os
import itertools
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from prepare import REPO
from vulkan_dispatch import framework, generate


@unittest.skipUnless(os.environ.get("HADES_MOLTENVK_ROOT"), "Requires explicitly selected verified local framework")
class VulkanRuntimeTests(unittest.TestCase):
    def test_real_typed_vulkan_gpu_buffer_copy(self):
        include, library = framework(Path(os.environ["HADES_MOLTENVK_ROOT"]), "macos")
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            generate(output / "vulkan_dispatch.inc", include)
            dispatch = (output / "vulkan_dispatch.inc").read_text()
            self.assertIn("((const uint64_t *)(r + 100))[0]", dispatch)
            self.assertIn("((const uint64_t *)(r + 100))[1]", dispatch)
            for optimization in ("O0", "O2"):
                executable = output / optimization
                subprocess.run(["xcrun", "clang", "-std=c11", "-" + optimization,
                    "-Wall", "-Wextra", "-Werror", "-fobjc-arc", "-DHADES_VULKAN_HOST", "-DHADES_VULKAN_TESTING",
                    "-I", str(include), "-I", str(output),
                    str(REPO / "hades/tests/vulkan_probe.c"), str(REPO / "hades/runtime/vulkan.m"),
                    str(REPO / "hades/runtime/callout.c"), str(REPO / "hades/runtime/host_entry.S"),
                    str(REPO / "hades/runtime/memory.c"), str(REPO / "hades/runtime/ipc.c"),
                    str(REPO / "hades/runtime/audio.c"), "-framework", "AudioToolbox",
                    str(library), "-lc++", "-framework", "Foundation", "-framework", "Metal",
                    "-framework", "QuartzCore", "-framework", "IOSurface", "-framework", "IOKit",
                    "-framework", "CoreGraphics", "-framework", "AppKit", "-o", str(executable)],
                    check=True, capture_output=True, timeout=180)
                for completion_probe, load_probe, pass_probe in itertools.product(("0", "1"), repeat=3):
                    label = optimization + "-" + completion_probe + "-" + load_probe + "-" + pass_probe
                    capture = output / (label + ".ppm")
                    textures = output / (label + "-textures")
                    textures.mkdir()
                    environment = dict(os.environ, HADES_VULKAN_QUEUE_COMPLETION_PROBE=completion_probe,
                                       HADES_VULKAN_ATTACHMENT_LOAD_PROBE=load_probe,
                                       HADES_VULKAN_PASS_CAPTURE_FRAME=pass_probe)
                    run = subprocess.run([executable, capture, textures], capture_output=True, text=True,
                                         timeout=60, env=environment)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertEqual(capture.read_bytes(), b"P6\n16 16\n255\n" + bytes((33,22,11))*256)
                    self.assertIn("VULKAN_BUFFER_GPU_OK:", run.stdout)
                    self.assertIn("VULKAN_DESCRIPTOR_BUDGET_OK:", run.stdout)
                    self.assertIn("VULKAN_TEXTURE_SAMPLE_OK:", run.stdout)
                    self.assertIn("PASS_WINDOW_OK:", run.stdout)
                    self.assertIn("FLOW_STATIC_LOAD_OK:", run.stdout)
                    self.assertIn("ATTACHMENT index=0 format=37 load=1 store=0", run.stderr)
                    self.assertIn("PASS_DEPENDENCY subpass=0->4294967295 stages=0x400->0x80 access=0x100->0x20", run.stderr)
                    self.assertIn("PIPELINE_BARRIER", run.stderr)
                    self.assertIn("layout=7->5 access=0x1000->0x20", run.stderr)
                    for serial, rgb in enumerate(((41, 0, 0), (7, 83, 191),
                                                 (191, 83, 7), (255, 1, 1)), 1):
                        self.assertEqual((textures / f"image-{serial}-frame-1.ppm").read_bytes(),
                                         b"P6\n16 16\n255\n" + bytes(rgb)*256)
                        alpha = (255, 167, 93, 255)[serial - 1]
                        self.assertEqual((textures / f"image-{serial}-frame-1.ppm.alpha.pgm").read_bytes(),
                                         b"P5\n16 16\n255\n" + bytes((alpha,))*256)
                    self.assertEqual(len((textures / "textures-1.tsv").read_text().splitlines()), 5)
                    self.assertEqual((textures / "image-2-frame-3.ppm").read_bytes(),
                                     b"P6\n16 16\n255\n" + bytes((201, 9, 51))*256)
                    self.assertEqual((textures / "image-2-frame-3.ppm.alpha.pgm").read_bytes(),
                                     b"P5\n16 16\n255\n" + bytes((73,))*256)
                    self.assertEqual(len((textures / "textures-2.tsv").read_text().splitlines()), 1)
                    inventory = (textures / "textures-4.tsv").read_text().splitlines()
                    self.assertEqual(len(inventory), 601)
                    self.assertTrue(all(row.endswith("\tunsupported-format-or-usage")
                                        for row in inventory[1:]))
                    self.assertEqual(len((textures / "textures-5.tsv").read_text().splitlines()), 1)
                    self.assertNotIn("additional images not captured", run.stderr)
                    if load_probe == "1":
                        self.assertIn("ATTACHMENT_LOAD_PROBE_CHANGED count=1", run.stderr)
                        self.assertEqual((textures / "image-2-frame-6.ppm").read_bytes(),
                                         b"P6\n16 16\n255\n" + bytes((17, 29, 101))*256)
                        self.assertEqual((textures / "image-2-frame-6.ppm.alpha.pgm").read_bytes(),
                                         b"P5\n16 16\n255\n" + bytes((113,))*256)
                        if pass_probe == "1":
                            self.assertEqual((textures / "pass-frame-1-serial-1.ppm").read_bytes(),
                                             b"P6\n16 16\n255\n" + bytes((201, 9, 51))*256)
                            self.assertEqual((textures / "pass-frame-1-serial-1.ppm.alpha.pgm").read_bytes(),
                                             b"P5\n16 16\n255\n" + bytes((73,))*256)
                    else:
                        self.assertNotIn("ATTACHMENT_LOAD_PROBE_CHANGED", run.stderr)
                    if pass_probe == "1":
                        self.assertIn("PASS_PIXELS_WRITTEN frame=1 serial=1", run.stderr)
                        self.assertIn("PASS_FLOW_CALL frame=1", run.stderr)
                        self.assertIn("width=8 value=0x123456789abcdef0", run.stderr)
                        self.assertIn("descriptor-capacity=1234", run.stderr)
                    else:
                        self.assertNotIn("PASS_PIXELS_WRITTEN", run.stderr)
                        self.assertNotIn("PASS_FLOW_CALL", run.stderr)
                        self.assertNotIn("PASS_FLOW_STATIC_LOAD", run.stderr)
                        self.assertNotIn("PASS_FLOW_FORGE_BATCH", run.stderr)
                        self.assertFalse((textures / "pass-frame-1-serial-1.ppm").exists())
                    self.assertIn("DESCRIPTOR_BATCH_PROVISIONED", run.stderr)
                    if completion_probe == "1":
                        self.assertIn("QUEUE_COMPLETION_PROBE_WAIT_OK submission=1", run.stderr)
                    else:
                        self.assertNotIn("QUEUE_COMPLETION_PROBE_WAIT_OK", run.stderr)
