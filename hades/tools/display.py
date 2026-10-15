"""Opt-in, build-identified Hades display dimensions; not ABI equivalence."""
import hashlib
import re
import shutil
import struct

from analyze_modules import load_nso
from inspect_target import FormatError

MAIN_BUILD_ID = "c7c02b758c0e18407e87f68971d26c7fa7c39303000000000000000000000000"
WINDOW_INSTRUCTIONS = {
    0x248238: 0xD280A009,  # mov x9, #1280
    0x248240: 0xF2C05A09,  # movk x9, #720, lsl #32
    0x24824C: 0xF9000669,  # str x9, [x19, #8]
    0x24826C: 0xA90EA67F,  # stp xzr, x9, [x19, #0xe8]
}
PRESETS = {"720p": (1280, 720), "1080p": (1920, 1080), "wide-720p": (1560, 720)}
WIDE_WORDS = {
    0x4A80E4: 0xBD4F7908,  # viewport calculation loads the shared virtual width
    0x4A8100: 0xBD4E2D09,  # virtual height remains 1080
    0x61D738: 0xB2590FE8,  # default view descriptor: 1920 in the upper word
    0x61D73C: 0xF9000268,
    0x61D740: 0x52808708,  # default view height: 1080
    0x6CEF78: 0x44F00000,  # shared virtual width: 1920.0
}
WIDE_VIRTUAL_WIDTH = 2340
UI_DATA_SHA256 = "f91dd404c5712a8be17304539bd52b5991bd7d0e109445da237be14e6843a0e1"
ROOM_MANAGER_SHA256 = "de4642b0cbdd8f59c1871073b57da7398eba7afb4d29f48b38a99d815875befb"


def widened_vignette(source):
    if hashlib.sha256(source).hexdigest() != ROOM_MANAGER_SHA256:
        raise FormatError("Wide vignette requires the identified original RoomManager source")
    anchor = b'SetAnimation({ Name = "VignetteOverlay", DestinationId = ScreenAnchors.Vignette })'
    if source.count(anchor) != 1:
        raise FormatError("Wide vignette animation anchor mismatch")
    newline = b"\r\n" if b"\r\n" in source else b"\n"
    return source.replace(anchor, anchor + newline
        + b'\tSetScaleX({ Id = ScreenAnchors.Vignette, Fraction = ScreenWidth / 1920 })')


def centered_gui(source):
    """Shift absolute native-menu positions, preserving attached relative positions."""
    tokens = list(re.finditer(r'//[^\r\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|'
                              r'-?\d+(?:\.\d+)?|[A-Za-z_]\w*|[={}]', source))
    tokens = [t for t in tokens if not t[0].startswith(("//", "/*"))]
    stack, nodes = [], []
    for index, token in enumerate(tokens):
        if token[0] == "{":
            name = tokens[index - 2][0] if index >= 2 and tokens[index - 1][0] == "=" else None
            node = {"name": name, "fields": {}}
            nodes.append(node)
            stack.append(node)
        elif token[0] == "}":
            if not stack:
                raise FormatError("Unbalanced native GUI layout")
            stack.pop()
        elif (stack and index + 2 < len(tokens) and tokens[index + 1][0] == "="
              and re.fullmatch(r"[A-Za-z_]\w*", token[0])
              and tokens[index + 2][0] != "{"):
            fields = stack[-1]["fields"]
            if token[0] in fields:
                raise FormatError("Duplicate native GUI layout property")
            fields[token[0]] = tokens[index + 2]
    if stack:
        raise FormatError("Unbalanced native GUI layout")
    named = {node["name"]: node for node in nodes if node["name"]}

    def attached(node, seen):
        fields = node["fields"]
        if "AttachTo" in fields:
            return True
        inherited = fields.get("InheritFrom")
        if inherited and inherited[0].startswith('"'):
            parent = named.get(inherited[0][1:-1])
            if parent:
                # Platform overrides may inherit a same-named component from the base file.
                if parent is node:
                    return False
                if id(parent) in seen:
                    raise FormatError("Cyclic native GUI layout inheritance")
                return attached(parent, seen | {id(parent)})
        return False

    changes = []
    for node in nodes:
        fields = node["fields"]
        width = fields.get("Width")
        full_width = width is not None and width[0] == "1920"
        if full_width:
            changes.append((width.start(), width.end(), str(WIDE_VIRTUAL_WIDTH)))
        x = fields.get("X")
        if x is not None and not attached(node, {id(node)}):
            if not re.fullmatch(r"-?\d+(?:\.\d+)?", x[0]):
                raise FormatError("Unsupported native GUI absolute X coordinate")
            if full_width and float(x[0]) == 0:
                continue
            shifted = float(x[0]) + (WIDE_VIRTUAL_WIDTH - 1920) / 2
            changes.append((x.start(), x.end(), f"{shifted:g}"))
    result = source
    for start, end, value in sorted(changes, reverse=True):
        result = result[:start] + value + result[end:]
    return result


def ui_overlays(root, preset):
    if preset != "wide-720p":
        return []
    romfs = root / "data/RomFS"
    script = romfs / "Content/Scripts/UIData.lua"
    source = script.read_bytes()
    if hashlib.sha256(source).hexdigest() != UI_DATA_SHA256:
        raise FormatError("Wide UIData requires the identified original source")
    changed = source
    for before, after in ((b"ScreenCenterX = 1920/2", b"ScreenCenterX = 2340/2"),
                          (b"ScreenWidth = 1920", b"ScreenWidth = 2340")):
        if changed.count(before) != 1:
            raise FormatError("Wide UIData canvas anchor mismatch")
        changed = changed.replace(before, after)
    # Main.Load restores saved globals after UIData; display geometry must remain runtime-only.
    changed += b"\nSaveIgnores.ScreenCenterX = true\nSaveIgnores.ScreenWidth = true\n"
    variants = [(script, source, changed)]
    room_manager = romfs / "Content/Scripts/RoomManager.lua"
    source = room_manager.read_bytes()
    variants.append((room_manager, source, widened_vignette(source)))
    gui = romfs / "Content/Game/GUI"
    layouts = sorted(gui.rglob("*.sjson"))
    if len(layouts) != 33:
        raise FormatError("Native GUI inventory differs from the identified Switch build")
    for path in layouts:
        if path.is_symlink():
            raise FormatError("Symlink in native GUI layout inventory")
        source = path.read_bytes()
        try:
            changed = centered_gui(source.decode("utf-8")).encode("utf-8")
        except FormatError as error:
            raise FormatError(f"Native GUI {path.relative_to(gui)}: {error}") from error
        if source != changed:
            variants.append((path, source, changed))
    return [{"path": path.relative_to(romfs).as_posix(), "source_size": len(source),
             "source_sha256": hashlib.sha256(source).hexdigest(), "data": changed,
             "sha256": hashlib.sha256(changed).hexdigest()} for path, source, changed in variants]


def resized_image(image, build_id, preset):
    if preset not in PRESETS or preset == "720p":
        raise FormatError("Bootstrap resizing requires an explicit experimental preset")
    if build_id != MAIN_BUILD_ID:
        raise FormatError("Window resizing requires the identified Hades main build")
    expected_words = dict(WINDOW_INSTRUCTIONS)
    if preset == "wide-720p":
        expected_words.update(WIDE_WORDS)
    if len(image) < max(expected_words) + 4:
        raise FormatError("Truncated Hades bootstrap image")
    for offset, expected in expected_words.items():
        if struct.unpack_from("<I", image, offset)[0] != expected:
            raise FormatError(f"Hades display word mismatch at {offset:#x}")
    result = bytearray(image)
    width, height = PRESETS[preset]
    struct.pack_into("<I", result, 0x248238, 0xD2800009 | (width << 5))
    struct.pack_into("<I", result, 0x248240, 0xF2C00009 | (height << 5))
    if preset == "wide-720p":
        struct.pack_into("<I", result, 0x61D738, 0xD2C00008 | (WIDE_VIRTUAL_WIDTH << 5))
        struct.pack_into("<f", result, 0x6CEF78, WIDE_VIRTUAL_WIDTH)
    return bytes(result)


def display_images(root, output, preset, names):
    if preset not in PRESETS:
        raise FormatError("Unknown render resolution preset")
    baseline = root / "build/abi"
    width, height = PRESETS[preset]
    report = {"preset": preset, "width": width, "height": height, "intentional_guest_instruction_changes": 0,
              "full_game_equivalence_claimed": False}
    if preset == "720p":
        return baseline, report
    original, inventory = load_nso(root / "data/ExeFS/main")
    resized_image(original, inventory["build_id"], preset)
    source = (baseline / "main.patched.bin").read_bytes()
    changed = resized_image(source, inventory["build_id"], preset)
    images = output / "display-images"
    images.mkdir(mode=0o700)
    for name in names:
        shutil.copyfile(baseline / (name + ".fixups.tsv"), images / (name + ".fixups.tsv"))
        if name == "main":
            (images / "main.patched.bin").write_bytes(changed)
        else:
            shutil.copyfile(baseline / (name + ".patched.bin"), images / (name + ".patched.bin"))
    report.update({"main_build_id": inventory["build_id"],
                   "baseline_main_sha256": hashlib.sha256(source).hexdigest(),
                   "embedded_main_sha256": hashlib.sha256(changed).hexdigest(),
                   "intentional_guest_instruction_changes": 2,
                   "instruction_offsets": (["0x248238", "0x61d738"] if preset == "wide-720p"
                                           else ["0x248238", "0x248240"]),
                   "data_offsets": ["0x6cef78"] if preset == "wide-720p" else [],
                   "virtual_extent": [WIDE_VIRTUAL_WIDTH, 1080] if preset == "wide-720p" else [1920, 1080],
                   "ui_read_overlay": preset == "wide-720p",
                   "purpose": ("Window, shared virtual width and default view descriptor; SHA-256-guarded UI canvas reads"
                               if preset == "wide-720p" else
                               "Initial and current window descriptor dimensions; no mode/clock or driver extent override")})
    return images, report
