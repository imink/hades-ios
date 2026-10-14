# 用合成元数据验证隔离工作区和失败语义；密钥只用虚拟测试路径。
# Test isolated workspaces and failure semantics using synthetic metadata and dummy key paths.
import contextlib
import io
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import prepare
from inspect_target import FormatError


class PreparationTests(unittest.TestCase):
    def test_cnmt_base(self):
        data = bytearray(32 + 16 + 56)
        struct.pack_into("<QIB", data, 0, int(prepare.TITLE_ID, 16), 0, 0x80)
        struct.pack_into("<HHH", data, 14, 16, 1, 0)
        data[48:80] = bytes(range(32))
        data[80:96] = bytes(range(16))
        data[96:102] = (123).to_bytes(6, "little")
        data[102] = 1
        result = prepare.read_cnmt(data)
        self.assertEqual(result["title_id"], prepare.TITLE_ID)
        self.assertEqual(result["contents"][0]["size"], 123)
        self.assertEqual(result["contents"][0]["type"], 1)
        with self.assertRaises(FormatError):
            prepare.read_cnmt(data[:-1])

    def test_cnmt_duplicates(self):
        data = bytearray(32 + 112)
        struct.pack_into("<HHH", data, 14, 0, 2, 0)
        with self.assertRaises(FormatError):
            prepare.read_cnmt(data)

    def test_short_cnmt(self):
        with self.assertRaises(FormatError):
            prepare.read_cnmt(b"\0")

    def test_nacp(self):
        data = bytearray(0x4000)
        data[:6] = b"Hades\0"
        data[512:516] = b"SGG\0"
        data[0x3060:0x3064] = b"1.0\0"
        self.assertEqual(prepare.read_nacp(data), {"title": "Hades", "publisher": "SGG", "display_version": "1.0"})
        with self.assertRaises(FormatError):
            prepare.read_nacp(data[:0x1000])

    def test_git_workspace_rejected(self):
        with self.assertRaises(FormatError):
            prepare.outside_git(prepare.REPO / "hades/build")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / ".git").mkdir()
            with self.assertRaises(FormatError):
                prepare.outside_git(root / "data")

    def test_tool_does_not_echo_key_output(self):
        output = io.StringIO()
        result = SimpleNamespace(returncode=1, stdout="dummy sensitive output", stderr="")
        with patch.object(prepare.subprocess, "run", return_value=result), contextlib.redirect_stdout(output):
            with self.assertRaisesRegex(FormatError, "no key-bearing"):
                prepare.nstool(Path("/local/tool"), Path("/local/keys/prod.keys"), [], set())
        self.assertEqual(output.getvalue(), "")

    def test_key_paths_only_and_ticket_warning(self):
        warnings = set()
        result = SimpleNamespace(returncode=0, stdout="Ticket is fake-signed", stderr="")
        with patch.object(prepare.subprocess, "run", return_value=result) as run:
            prepare.nstool(Path("/local/tool"), Path("/local/keys/prod.keys"), ["-t", "nca"], warnings)
        self.assertEqual(run.call_args.kwargs["cwd"], Path("/local/keys"))
        self.assertIn("/local/keys/prod.keys", run.call_args.args[0])
        self.assertNotIn("--showkeys", run.call_args.args[0])
        self.assertTrue(warnings)


if __name__ == "__main__":
    unittest.main()
