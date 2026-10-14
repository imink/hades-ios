# 用合成 PFS0/NSO0 验证只读检查；不使用游戏内容或密钥。
# Validate read-only inspection with synthetic PFS0/NSO0 data; no game content or keys.
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "inspect_target", Path(__file__).resolve().parents[1] / "tools/inspect_target.py")
inspect = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inspect)


def pfs(names=("test.nca",), payload=b"ABCD"):
    strings = b""
    table = b""
    for index, name in enumerate(names):
        table += struct.pack("<QQII", index * len(payload), len(payload), len(strings), 0)
        strings += name.encode() + b"\0"
    return b"PFS0" + struct.pack("<III", len(names), len(strings), 0) + table + strings + payload * len(names)


def nso():
    header = bytearray(256)
    header[:4] = b"NSO0"
    header[0x40:0x60] = bytes(range(32))
    for index in range(3):
        struct.pack_into("<III", header, 0x10 + index * 16, 256 + index * 4, index * 0x1000, 4)
    return header + b"\0" * 12


class InspectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, data, name="fixture"):
        path = self.root / name
        path.write_bytes(data)
        return path

    def test_package_read_only_and_unverified(self):
        path = self.write(pfs())
        before = path.read_bytes()
        result = inspect.inspect_pfs0(path)
        self.assertEqual(result["entries"][0]["size"], 4)
        self.assertFalse(result["title_id_verified"])
        self.assertFalse(result["version_verified"])
        self.assertEqual(path.read_bytes(), before)

    def test_invalid_packages(self):
        for data in (b"", b"PFS0", b"BAD!" + bytes(12),
                     b"PFS0" + struct.pack("<III", 5000, 1, 0),
                     pfs()[:-1], pfs(("same", "same")), pfs(("../escape",)),
                     pfs(("/absolute",)), pfs(("a\\b",)), pfs(("",))):
            with self.subTest(data=data[:24]), self.assertRaises(inspect.FormatError):
                inspect.inspect_pfs0(self.write(data))

    def test_unterminated_name(self):
        data = bytearray(pfs())
        data[48] = ord("x")
        with self.assertRaises(inspect.FormatError):
            inspect.inspect_pfs0(self.write(data))

    def test_overlapping_package_entries(self):
        data = bytearray(pfs(("one", "two")))
        struct.pack_into("<Q", data, 40, 0)
        with self.assertRaises(inspect.FormatError):
            inspect.inspect_pfs0(self.write(data))

    def test_nso_inventory(self):
        result = inspect.inspect_nso(self.write(nso(), "main"))
        self.assertEqual(result["build_id"], bytes(range(32)).hex())
        self.assertEqual([s["name"] for s in result["segments"]], ["text", "ro", "data"])
        self.assertFalse(result["segment_hashes_verified"])
        self.assertFalse(result["rewrite_verified"])

    def test_invalid_nso(self):
        for field, value in ((4, 1), (12, 0x80), (16, 240), (16, 500),
                             (0x20, 256), (0x24, 0)):
            data = nso()
            struct.pack_into("<I", data, field, value)
            with self.subTest(field=field, value=value), self.assertRaises(inspect.FormatError):
                inspect.inspect_nso(self.write(data))

    def test_nso_truncation(self):
        with self.assertRaises(inspect.FormatError):
            inspect.inspect_nso(self.write(nso()[:128]))

    def test_compressed_header_without_decompression(self):
        data = nso()
        struct.pack_into("<I", data, 12, 1)
        struct.pack_into("<I", data, 0x60, 4)
        result = inspect.inspect_nso(self.write(data))
        self.assertTrue(result["segments"][0]["compressed"])
        self.assertEqual(result["instruction_analysis"], "not performed")

    def test_discover_modules_not_d2r_list(self):
        self.write(nso(), "main")
        self.write(nso(), "subsdk7")
        self.write(b"metadata", "main.npdm")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = inspect.main(["nso", "--exefs", str(self.root)])
        result = json.loads(output.getvalue())
        self.assertEqual(code, 0)
        self.assertEqual([m["name"] for m in result["modules"]], ["main", "subsdk7"])
        self.assertFalse(result["target"]["d2r_patches_allowed"])
        self.assertEqual(result["target"]["verified_build_ids"], [])

    def test_missing_main_is_error(self):
        self.write(nso(), "sdk")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(inspect.main(["nso", "--exefs", str(self.root)]), 2)

    def test_preflight_is_blocked_even_when_tools_exist(self):
        package = self.write(pfs(), "game.nsp")
        keys = self.write(b"not a real key", "prod.keys")
        output = io.StringIO()
        with patch.object(inspect.shutil, "which", return_value="/fake/tool"), \
                patch.object(inspect, "probe", return_value={"available": True, "output": "1 valid identities found"}), \
                contextlib.redirect_stdout(output):
            code = inspect.main(["check", "--package", str(package), "--keys", str(keys)])
        result = json.loads(output.getvalue())
        self.assertEqual(code, 1)
        self.assertTrue(result["keys_path_exists"])
        self.assertEqual(result["status"], "blocked")
        self.assertEqual(len(result["blockers"]), 1)
        self.assertNotIn("not a real key", output.getvalue())


if __name__ == "__main__":
    unittest.main()
