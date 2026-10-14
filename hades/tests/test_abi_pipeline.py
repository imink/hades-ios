# 用合成 NSO 验证段哈希、导入解析、D2R 隔离、长跳转表与 x18 栈切换。
# Verify hashes, imports, D2R isolation, long jump-table blocks and x18 stack switching using synthetic NSOs.
import importlib.util
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from fixtures import synthetic_nso

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from inspect_target import FormatError
import analyze_modules

REPO = Path(__file__).resolve().parents[2]
HAVE_DEPENDENCIES = all(importlib.util.find_spec(name) for name in ("capstone", "lz4", "unicorn"))
LLVM = Path("/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin")


@unittest.skipUnless(HAVE_DEPENDENCIES, "Run with the isolated Hades requirements environment")
class AbiPipelineTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.nso = self.root / "main"
        self.nso.write_bytes(synthetic_nso(bytes.fromhex("db0507f36b84200ac05f75d76ed5026f") + bytes(16)))

    def test_hashes_and_dynamic_symbols(self):
        image, inventory = analyze_modules.load_nso(self.nso)
        self.assertTrue(inventory["segment_hashes_verified"])
        symbols, relocs, needed = analyze_modules.dynamic_symbols(image, self.nso.read_bytes()[:256])
        self.assertEqual([s["name"] for s in symbols], ["", "provider", "external"])
        self.assertEqual(relocs[0]["symbol"], 2)
        self.assertEqual(needed, ["libgame.so"])
        report = analyze_modules.analyze(self.root)
        self.assertEqual(report["modules"][0]["unresolved_imports"], ["external"])
        changed = bytearray(self.nso.read_bytes())
        changed[-1] ^= 1
        self.nso.write_bytes(changed)
        with self.assertRaisesRegex(FormatError, "hash mismatch"):
            analyze_modules.load_nso(self.nso)

    def test_dynamic_bounds_and_header_disagreement(self):
        image, _ = analyze_modules.load_nso(self.nso)
        malformed = bytearray(image)
        struct.pack_into("<I", malformed, 0x12B0, 0xFFFFFF)
        with self.assertRaises(FormatError):
            analyze_modules.dynamic_symbols(malformed, self.nso.read_bytes()[:256])
        header = bytearray(self.nso.read_bytes()[:256])
        struct.pack_into("<I", header, 0x9C, 24)
        with self.assertRaisesRegex(FormatError, "disagree"):
            analyze_modules.dynamic_symbols(image, header)

    def test_portable_profile_and_equivalence(self):
        output = self.root / "abi"
        environment = dict(os.environ, DM_RULESET="touch-run-v4", DM_PRIVATE_GATES="1",
                           LLVM_BIN=str(LLVM), DM_ROOT=str(self.root),
                           DM_VCACHE=str(self.root / "cache"))
        patched = subprocess.run([sys.executable, str(REPO / "dm/tools/patch_nso.py"),
                                  str(self.nso), str(output), "main", "--profile", "portable-abi"],
                                 capture_output=True, text=True, env=environment)
        self.assertEqual(patched.returncode, 0, patched.stdout + patched.stderr)
        self.assertIn("'stack_switch': 1", patched.stdout)
        self.assertIn("'br': 1", patched.stdout)
        self.assertFalse((output / "main.rules.tsv").exists())
        for extension in ("cuts.tsv", "obs.tsv", "params.tsv"):
            self.assertTrue(all(line.startswith("#") for line in (output / ("main." + extension)).read_text().splitlines()))
        equivalence = subprocess.run([sys.executable, str(REPO / "dm/verify/equiv.py"),
                                      str(output), "main", str(self.nso), "--jobs", "1", "--mutate", "1"],
                                     capture_output=True, text=True, env=environment)
        self.assertEqual(equivalence.returncode, 0, equivalence.stdout + equivalence.stderr)
        if (LLVM / "llvm-objdump").is_file():
            structure = subprocess.run([sys.executable, str(REPO / "dm/verify/structck.py"),
                                        str(output), "main", str(self.nso)],
                                       capture_output=True, text=True, env=environment)
            self.assertEqual(structure.returncode, 0, structure.stdout + structure.stderr)
            corrupted = bytearray((output / "main.patched.bin").read_bytes())
            headers = dict(line[2:].split() for line in (output / "main.fixups.tsv").read_text().splitlines())
            patch_offset = int(headers["PATCH_OFF"], 0)
            branch, = struct.unpack_from("<I", corrupted, 0x20)
            target = 0x20 + 4 * (((branch & 0x3FFFFFF) ^ 0x2000000) - 0x2000000)
            self.assertGreaterEqual(target, patch_offset)
            struct.pack_into("<I", corrupted, target + 40, 0xD503201F)
            (output / "main.patched.bin").write_bytes(corrupted)
            bad = subprocess.run([sys.executable, str(REPO / "dm/verify/structck.py"),
                                  str(output), "main", str(self.nso)], capture_output=True, text=True, env=environment)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("旧栈恢复模板不符", bad.stdout)

    def test_default_d2r_still_rejects_wrong_original_bytes(self):
        result = subprocess.run([sys.executable, str(REPO / "dm/tools/patch_nso.py"),
                                 str(self.nso), str(self.root / "d2r"), "main"],
                                capture_output=True, text=True,
                                env=dict(os.environ, DM_RULESET="native"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("剪口原字节不符", result.stderr)

    def test_portable_biased_keys_gates_and_equivalence(self):
        environment=dict(os.environ,LLVM_BIN=str(LLVM),DM_ROOT=str(self.root))
        for bias in (4096,8192,12288):
            output=self.root/str(bias)
            result=subprocess.run([sys.executable,str(REPO/"dm/tools/patch_nso.py"),
                str(self.nso),str(output),"main","--profile","portable-abi","--load-bias",str(bias)],
                capture_output=True,text=True,env=environment)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            header={k:int(v,0) for k,v in (line[2:].split() for line in (output/"main.fixups.tsv").read_text().splitlines())}
            self.assertEqual(header["LOAD_BIAS"],bias)
            self.assertEqual((header["KEY_PAGE_OFF"]+bias)%16384,0)
            self.assertEqual((header["PATCH_OFF"]+bias)%65536,0)
            result=subprocess.run([sys.executable,str(REPO/"dm/verify/equiv.py"),
                str(output),"main",str(self.nso),"--jobs","1","--mutate","1"],
                capture_output=True,text=True,env=environment)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            if (LLVM/"llvm-objdump").is_file():
                result=subprocess.run([sys.executable,str(REPO/"dm/verify/structck.py"),
                    str(output),"main",str(self.nso)],capture_output=True,text=True,env=environment)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        for arguments in (["--load-bias","1"],["--load-bias","4096","--profile","d2r"]):
            result=subprocess.run([sys.executable,str(REPO/"dm/tools/patch_nso.py"),
                str(self.nso),str(self.root/"invalid"),"main",*arguments],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)

    def test_signed_offset_table(self):
        data = bytearray(self.nso.read_bytes())
        words = [0xB000000B, 0x91000000 | (0x500 << 10) | (11 << 5) | 11,
                 0x7100001F | (3 << 10) | (18 << 5),
                 0x54000008 | (((0x600 - 0x50C) // 4) << 5),
                 0xB8B27972, 0x8B0B0252, 0xD61F0240]
        for index, word in enumerate(words):
            struct.pack_into("<I", data, 256 + 0x500 + index * 4, word)
        for index in range(4):
            struct.pack_into("<i", data, 256 + 0x1500 + index * 4, 0x80 + index * 4 - 0x1500)
        data[0xA0:0xC0] = hashlib.sha256(data[256:256 + 4096]).digest()
        data[0xC0:0xE0] = hashlib.sha256(data[256 + 4096:256 + 8192]).digest()
        self.nso.write_bytes(data)
        output = self.root / "signed"
        result = subprocess.run([sys.executable, str(REPO / "dm/tools/patch_nso.py"),
                                 str(self.nso), str(output), "main", "--profile", "portable-abi"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run([sys.executable, str(REPO / "dm/verify/equiv.py"),
                                 str(output), "main", str(self.nso), "--mutate", "4"],
                                capture_output=True, text=True,
                                env=dict(os.environ, DM_ROOT=str(self.root)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # A write to the address register invalidates the table proof.
        struct.pack_into("<I", data, 256 + 0x504, 0xAA0103EB)
        data[0xA0:0xC0] = hashlib.sha256(data[256:256 + 4096]).digest()
        self.nso.write_bytes(data)
        result = subprocess.run([sys.executable, str(REPO / "dm/tools/patch_nso.py"),
                                 str(self.nso), str(self.root / "bad"), "main", "--profile", "portable-abi"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("signed table base", result.stderr)

    def test_jump_table_clobber_is_rejected(self):
        data = bytearray(self.nso.read_bytes())
        struct.pack_into("<I", data, 256 + 0x30, 0xAA0103F2)
        data[0xA0:0xC0] = hashlib.sha256(data[256:256 + 0x1000]).digest()
        self.nso.write_bytes(data)
        result = subprocess.run([sys.executable, str(REPO / "dm/tools/patch_nso.py"),
                                 str(self.nso), str(self.root / "abi"), "main", "--profile", "portable-abi"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("jump table x18 clobber", result.stderr)

    def test_dynamic_nro_inventory_is_not_runtime_verification(self):
        raw = bytearray(synthetic_nso()[256:])
        raw[0x10:0x14] = b"NRO0"
        struct.pack_into("<III", raw, 0x14, 0, len(raw), 0)
        for index, (offset, size) in enumerate(((0, 0x1000), (0x1000, 0x1000), (0x2000, 16))):
            struct.pack_into("<II", raw, 0x20 + index * 8, offset, size)
        struct.pack_into("<I", raw, 0x38, 0)
        struct.pack_into("<II", raw, 0x78, 0x280, 72)
        path = self.root / "module.nro"
        path.write_bytes(raw)
        image, inventory = analyze_modules.load_nro(path)
        self.assertFalse(inventory["rewrite_verified"])
        symbols, _, _ = analyze_modules.dynamic_symbols(image, raw[:256])
        self.assertEqual(symbols[1]["name"], "provider")
        malformed = bytearray(raw)
        struct.pack_into("<I", malformed, 0x18, len(raw) + 1)
        path.write_bytes(malformed)
        with self.assertRaises(FormatError):
            analyze_modules.load_nro(path)


if __name__ == "__main__":
    unittest.main()
