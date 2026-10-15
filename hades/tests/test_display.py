from pathlib import Path
import hashlib
import struct
import sys
import unittest
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from display import (MAIN_BUILD_ID, WINDOW_INSTRUCTIONS, WIDE_WORDS, resized_image,
                     display_images, centered_gui, ui_overlays, widened_vignette)
from inspect_target import FormatError


class DisplayTests(unittest.TestCase):
    def test_wide_vignette_scales_only_its_horizontal_coverage(self):
        anchor = b'SetAnimation({ Name = "VignetteOverlay", DestinationId = ScreenAnchors.Vignette })'
        source = b"function CreateVignette()\r\n\t" + anchor + b"\r\nend\r\n"
        with patch("display.ROOM_MANAGER_SHA256", hashlib.sha256(source).hexdigest()):
            changed = widened_vignette(source)
            with self.assertRaises(FormatError):
                widened_vignette(source + b"changed")
        added = b"\r\n\tSetScaleX({ Id = ScreenAnchors.Vignette, Fraction = ScreenWidth / 1920 })"
        self.assertEqual(changed.replace(added, b""), source)
        self.assertNotIn(b"SetScaleY", changed)
        self.assertNotIn(b"SetAlpha", changed)
        self.assertEqual(2340 / 1920, 1.21875)
        for bad in (b"unidentified", anchor + anchor):
            with patch("display.ROOM_MANAGER_SHA256", hashlib.sha256(bad).hexdigest()):
                with self.assertRaises(FormatError):
                    widened_vignette(bad)

    def test_native_menu_absolute_and_relative_layouts_are_separate(self):
        source = ('{ Screen = { Back = { Width = 1920 X = 0 Height = 1080 }\n'
                  ' Center = { X = 960 Y = 540 }\n'
                  ' Attached = { AttachTo = "Center" X = 20 OffsetX = 30 }\n'
                  ' Inherited = { InheritFrom = "Attached" X = 25 }\n'
                  ' FullCenter = { Width = 1920 X = 960 }\n'
                  ' /* X = 20 */ Label = { Text = "X = 960 { }" } } }')
        changed = centered_gui(source)
        self.assertIn("Width = 2340 X = 0 Height = 1080", changed)
        self.assertIn("X = 1170 Y = 540", changed)
        self.assertIn('AttachTo = "Center" X = 20 OffsetX = 30', changed)
        self.assertIn('InheritFrom = "Attached" X = 25', changed)
        self.assertIn("Width = 2340 X = 1170", changed)
        self.assertIn('/* X = 20 */ Label = { Text = "X = 960 { }" }', changed)
        for bad in ('{ X = 1 X = 2 }', '{ X = "bad" }', '{', '}',
                    '{ A = { InheritFrom = "B" X = 5 } B = { InheritFrom = "A" } }'):
            with self.subTest(source=bad), self.assertRaises(FormatError):
                centered_gui(bad)
        self.assertIn('X = 1170', centered_gui('{ A = { InheritFrom = "A" X = 960 } }'))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.assertEqual(ui_overlays(root, "720p"), [])
            self.assertEqual(ui_overlays(root, "1080p"), [])
            script = root / "data/RomFS/Content/Scripts/UIData.lua"
            script.parent.mkdir(parents=True)
            script.write_bytes(b"unknown source")
            with self.assertRaises(FormatError):
                ui_overlays(root, "wide-720p")
            source = b"ScreenCenterX = 1920/2\nScreenWidth = 1920\n"
            script.write_bytes(source)
            room_manager = script.parent / "RoomManager.lua"
            room_source = b'SetAnimation({ Name = "VignetteOverlay", DestinationId = ScreenAnchors.Vignette })'
            room_manager.write_bytes(room_source)
            gui = root / "data/RomFS/Content/Game/GUI"
            gui.mkdir(parents=True)
            for index in range(33):
                (gui / f"{index}.sjson").write_text('{ Center = { X = 960 } }')
            with patch("display.UI_DATA_SHA256", hashlib.sha256(source).hexdigest()), \
                    patch("display.ROOM_MANAGER_SHA256", hashlib.sha256(room_source).hexdigest()):
                overlays = ui_overlays(root, "wide-720p")
            self.assertEqual(len(overlays), 35)
            self.assertEqual(overlays[0]["source_size"], len(source))
            self.assertIn(b"ScreenCenterX = 2340/2", overlays[0]["data"])
            self.assertIn(b"SaveIgnores.ScreenCenterX = true", overlays[0]["data"])
            self.assertIn(b"SaveIgnores.ScreenWidth = true", overlays[0]["data"])
            self.assertEqual(script.read_bytes(), source)
            self.assertEqual(room_manager.read_bytes(), room_source)
            self.assertEqual(overlays[1]["path"], "Content/Scripts/RoomManager.lua")
            self.assertIn(b"Fraction = ScreenWidth / 1920", overlays[1]["data"])

    def test_baseline_is_default_and_experimental_images_are_private_copies(self):
        image = bytearray(max(WIDE_WORDS) + 8)
        for offset, word in (WINDOW_INSTRUCTIONS | WIDE_WORDS).items():
            struct.pack_into("<I", image, offset, word)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline = root / "build/abi"
            baseline.mkdir(parents=True)
            output = root / "output"
            output.mkdir()
            for name, data in (("main", bytes(image)), ("sdk", b"synthetic unchanged sdk")):
                (baseline / (name + ".patched.bin")).write_bytes(data)
                (baseline / (name + ".fixups.tsv")).write_text("# synthetic metadata\n")
            images, receipt = display_images(root, output, "720p", ("main", "sdk"))
            self.assertEqual(images, baseline)
            self.assertEqual(receipt["intentional_guest_instruction_changes"], 0)
            self.assertFalse((output / "display-images").exists())
            with patch("display.load_nso", return_value=(bytes(image), {"build_id": MAIN_BUILD_ID})):
                images, receipt = display_images(root, output, "1080p", ("main", "sdk"))
            self.assertEqual((baseline / "main.patched.bin").read_bytes(), bytes(image))
            self.assertEqual((images / "main.patched.bin").read_bytes(), resized_image(image, MAIN_BUILD_ID, "1080p"))
            self.assertEqual((images / "sdk.patched.bin").read_bytes(), b"synthetic unchanged sdk")
            self.assertEqual((images / "main.fixups.tsv").read_bytes(), (baseline / "main.fixups.tsv").read_bytes())
            self.assertEqual(receipt["intentional_guest_instruction_changes"], 2)
            self.assertFalse(receipt["full_game_equivalence_claimed"])
            with self.assertRaises(FormatError):
                display_images(root, output, "4k", ("main", "sdk"))

    def test_only_two_bootstrap_constants_change_and_source_is_immutable(self):
        image = bytearray(max(WIDE_WORDS) + 8)
        for offset, word in (WINDOW_INSTRUCTIONS | WIDE_WORDS).items():
            struct.pack_into("<I", image, offset, word)
        before = bytes(image)
        changed = resized_image(image, MAIN_BUILD_ID, "1080p")
        self.assertEqual(bytes(image), before)
        self.assertEqual(struct.unpack_from("<I", changed, 0x248238)[0], 0xD2800009 | (1920 << 5))
        self.assertEqual(struct.unpack_from("<I", changed, 0x248240)[0], 0xF2C00009 | (1080 << 5))
        restored = bytearray(changed)
        for offset in (0x248238, 0x248240):
            restored[offset:offset + 4] = before[offset:offset + 4]
        self.assertEqual(restored, image)
        for offset in WINDOW_INSTRUCTIONS:
            wrong = bytearray(image)
            wrong[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(FormatError):
                resized_image(wrong, MAIN_BUILD_ID, "1080p")
        with self.assertRaises(FormatError):
            resized_image(image, "unknown build", "1080p")
        with self.assertRaises(FormatError):
            resized_image(b"", MAIN_BUILD_ID, "1080p")
        wide = resized_image(image, MAIN_BUILD_ID, "wide-720p")
        restored = bytearray(wide)
        self.assertEqual(struct.unpack_from("<I", wide, 0x248238)[0], 0xD2800009 | (1560 << 5))
        self.assertEqual(struct.unpack_from("<I", wide, 0x61D738)[0], 0xD2C00008 | (2340 << 5))
        self.assertEqual(struct.unpack_from("<f", wide, 0x6CEF78)[0], 2340)
        for offset in (0x248238, 0x61D738, 0x6CEF78):
            restored[offset:offset + 4] = before[offset:offset + 4]
        self.assertEqual(restored, before)
        for offset in WIDE_WORDS:
            wrong = bytearray(image)
            wrong[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(FormatError):
                resized_image(wrong, MAIN_BUILD_ID, "wide-720p")
        for bad in ("720p", "4k", ""):
            with self.subTest(preset=bad), self.assertRaises(FormatError):
                resized_image(image, MAIN_BUILD_ID, bad)
