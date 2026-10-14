# 用合成 SPIR-V/Mach-O 验证阶段、边界、嵌入字节与明确的编译阻塞。
# Test stages, bounds, embedded bytes and explicit compiler blockers using synthetic SPIR-V/Mach-O.
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from inspect_target import FormatError
import translate_shaders
from verify_embed import verify_object


def spirv(stage=4):
    return struct.pack("<10I", 0x07230203, 0x10000, 0, 8, 0, (5 << 16) | 15, stage, 1, 0x6E69616D, 0)


class ShaderTests(unittest.TestCase):
    def test_spirv_stage(self):
        self.assertEqual(translate_shaders.inspect_spirv(spirv(), "frag")["stage"], "frag")
        self.assertEqual(translate_shaders.inspect_spirv(spirv(0), "vert")["instruction_count"], 1)
        with self.assertRaises(FormatError):
            translate_shaders.inspect_spirv(spirv(), "vert")

    def test_spirv_invalid_headers_and_instructions(self):
        variants = [b"", spirv()[:-1]]
        for offset, value in ((0, 1), (4, 0x20000), (12, 0), (16, 1), (20, 0), (20, (100 << 16) | 15)):
            data = bytearray(spirv())
            struct.pack_into("<I", data, offset, value)
            variants.append(data)
        for data in variants:
            with self.subTest(data=data[:24]), self.assertRaises(FormatError):
                translate_shaders.inspect_spirv(data, "frag")

    def test_missing_metal_does_not_produce_success_receipt(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "EffectsForgeSwitch"
            source.mkdir()
            (source / "test.frag").write_bytes(spirv())
            tool = root / "spirv-cross"
            tool.touch()
            tool.chmod(0o700)
            with patch.object(translate_shaders.subprocess, "run",
                              return_value=SimpleNamespace(returncode=1)):
                with self.assertRaisesRegex(FormatError, "Metal Toolchain missing"):
                    translate_shaders.translate(source, root / "out", tool, True)
            self.assertFalse((root / "out").exists())


class EmbedTests(unittest.TestCase):
    def test_macho_bytes_layout_and_corruption(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = bytes(range(256)) * 256 + b"ABCD"
            (root / "main.patched.bin").write_bytes(binary)
            (root / "main.fixups.tsv").write_text("# TEXT_END\t0x4\n# PATCH_OFF\t0x10000\n")
            spans = [("__dmmainx0", "__TEXT", 0, 0x4000),
                     ("__img", "__DMmainW", 0x4000, 0x10000),
                     ("__dmmainx1", "__TEXT", 0x10000, 0x14000)]
            command_size = 72 + 80 * len(spans)
            segment = struct.pack("<II16sQQQQiiII", 0x19, command_size, bytes(16),
                                  0, 0x14000, 0x4000, 0x14000, 7, 7, len(spans), 0)
            bodies = b""
            for name, segment_name, start, end in spans:
                segment += struct.pack("<16s16sQQIIIIIIII", name.encode(), segment_name.encode(),
                                       start, end - start, 0x4000 + start, 14, 0, 0, 0, 0, 0, 0)
                contents = binary[start:min(end, len(binary))]
                bodies += contents + bytes(end - start - len(contents))
            commands = segment + struct.pack("<6I", 0x32, 24, 2, 0xF0000, 0x1B0000, 0)
            header = struct.pack("<IiiIIIII", 0xFEEDFACF, 0x0100000C, 0, 1, 2, len(commands), 0, 0)
            obj = root / "embed.o"
            obj.write_bytes(header + commands + bytes(0x4000 - len(header) - len(commands)) + bodies)
            report = verify_object(obj, root, ["main"])
            self.assertEqual(len(report["byte_verified_sections"]), 3)
            self.assertFalse(report["codesign_verified"])
            corrupted = bytearray(obj.read_bytes())
            corrupted[-1] = 1
            obj.write_bytes(corrupted)
            with self.assertRaisesRegex(FormatError, "bytes differ"):
                verify_object(obj, root, ["main"])

    def test_non_macho_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "bad.o"
            path.write_bytes(bytes(32))
            with self.assertRaises(FormatError):
                verify_object(path, Path(temp), [])


if __name__ == "__main__":
    unittest.main()
