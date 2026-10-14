import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

from fixtures import synthetic_nso

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analyze_modules import load_nro
from inspect_target import FormatError
from nro import authorize_nro, inspect_nrr, normalize_nro
from prepare import TITLE_ID


def synthetic_nro():
    data = bytearray(synthetic_nso()[256:])
    data[0x10:0x14] = b"NRO0"
    struct.pack_into("<III", data, 0x14, 0, len(data), 0)
    for index, (offset, size) in enumerate(((0, 4096), (4096, 4096), (8192, 16))):
        struct.pack_into("<II", data, 0x20 + index * 8, offset, size)
    struct.pack_into("<I", data, 0x38, 4096)
    struct.pack_into("<II", data, 0x78, 0x280, 72)
    return data


def synthetic_nrr(digest):
    data = bytearray(4096)
    data[:4] = b"NRR0"
    struct.pack_into("<QQ", data, 0x10, 0xFFFFFFFFFFFFFFFF, int(TITLE_ID, 16))
    struct.pack_into("<QI", data, 0x330, int(TITLE_ID, 16), len(data))
    struct.pack_into("<II", data, 0x340, 0x350, 1)
    data[0x350:0x370] = digest
    return data


class NroTests(unittest.TestCase):
    def test_normalization_preserves_entire_image(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, destination = root / "input.nro", root / "normalized.nso"
            source.write_bytes(synthetic_nro())
            inventory = normalize_nro(source, destination)
            self.assertEqual(inventory["bss_size"], 4096)
            from analyze_modules import load_nso
            self.assertEqual(load_nro(source)[0], load_nso(destination)[0])
            with self.assertRaises(FileExistsError):
                normalize_nro(source, destination)

    def test_nrr_program_bounds_kind_and_duplicates(self):
        data = synthetic_nrr(bytes(32))
        self.assertFalse(inspect_nrr(data)["certificate_root_authenticated"])
        for offset, value in ((0x330, 1), (0x338, 1), (0x33C, 1), (0x340, 1)):
            changed = bytearray(data)
            changed[offset] ^= value
            with self.assertRaises(FormatError):
                inspect_nrr(changed)
        changed = bytearray(data)
        struct.pack_into("<I", changed, 0x344, 2)
        with self.assertRaisesRegex(FormatError, "strictly sorted"):
            inspect_nrr(changed)

    def test_membership_and_signature_failure_are_not_success(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, registration = root / "input.nro", root / "input.nrr"
            source.write_bytes(synthetic_nro())
            registration.write_bytes(synthetic_nrr(hashlib.sha256(source.read_bytes()).digest()))
            with patch("nro.verify_nrr_signature", return_value=True):
                self.assertTrue(authorize_nro(source, registration)["payload_signature_verified"])
                source.write_bytes(source.read_bytes() + b"changed")
                with self.assertRaisesRegex(FormatError, "not authorized"):
                    authorize_nro(source, registration)
            with patch("nro.verify_nrr_signature", side_effect=FormatError("invalid signature")):
                with self.assertRaisesRegex(FormatError, "invalid signature"):
                    authorize_nro(source, registration)
