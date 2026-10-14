#!/usr/bin/env python3
# 根据本机 NSO/MOD0 动态表生成依赖清单并核对段哈希；不包含游戏代码或 HLE 空桩。
# Inventory dependencies from local NSO/MOD0 dynamic tables and verify segment hashes; no game code or HLE stubs.
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import sys

from inspect_target import FormatError, inspect_nso


def bounded(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise FormatError(f"Out-of-bounds binary range: {offset:#x}+{size:#x}")
    return data[offset:offset + size]


def unpack(fmt, data, offset):
    return struct.unpack(fmt, bounded(data, offset, struct.calcsize(fmt)))


def load_nso(path):
    import lz4.block
    inventory = inspect_nso(path)
    source = path.read_bytes()
    flags, = unpack("<I", source, 12)
    image_size = max(s["memory_offset"] + s["size"] for s in inventory["segments"]) + inventory["bss_size"]
    if image_size > 512 * 1024 * 1024:
        raise FormatError("NSO image exceeds the analysis memory limit")
    image = bytearray(image_size)
    for index, segment in enumerate(inventory["segments"]):
        raw = bounded(source, segment["file_offset"], segment["stored_size"])
        if segment["compressed"]:
            raw = lz4.block.decompress(raw, uncompressed_size=segment["size"])
        if len(raw) != segment["size"]:
            raise FormatError(f"{path.name}: decoded segment size mismatch")
        if not flags & (8 << index):
            raise FormatError(f"{path.name}: missing segment hash; cannot mark verified")
        if hashlib.sha256(raw).digest() != source[0xA0 + 32 * index:0xC0 + 32 * index]:
            raise FormatError(f"{path.name}: segment hash mismatch")
        offset = segment["memory_offset"]
        image[offset:offset + len(raw)] = raw
    inventory["segment_hashes_verified"] = True
    return bytes(image), inventory


def load_nro(path):
    source = path.read_bytes()
    if bounded(source, 0x10, 4) != b"NRO0":
        raise FormatError("Missing NRO0")
    version, size, flags = unpack("<III", source, 0x14)
    bss, = unpack("<I", source, 0x38)
    if version or flags or size > len(source) or size < 128 or size + bss > 512 * 1024 * 1024:
        raise FormatError("Unsupported NRO layout")
    segments, ranges = [], []
    for index, name in enumerate(("text", "ro", "data")):
        offset, length = unpack("<II", source, 0x20 + index * 8)
        bounded(source[:size], offset, length)
        ranges.append((offset, offset + length))
        segments.append({"name": name, "file_offset": offset, "memory_offset": offset,
                         "size": length, "stored_size": length, "compressed": False})
    if ranges[0][0] != 0 or ranges[-1][1] != size or any(left[1] > right[0] for left, right in zip(ranges, ranges[1:])):
        raise FormatError("Overlapping or unsupported NRO segments")
    inventory = {"name": path.name, "format": "NRO0", "sha256": hashlib.sha256(source).hexdigest(),
                 "build_id": source[0x40:0x60].hex(), "bss_size": bss, "segments": segments,
                 "segment_hashes_verified": False, "rewrite_verified": False,
                 "registration_status": "NRR and ldr:ro runtime verification pending"}
    return source[:size] + bytes(bss), inventory


def dynamic_table(image):
    mod0, = unpack("<I", image, 4)
    if bounded(image, mod0, 4) != b"MOD0":
        raise FormatError("Missing MOD0")
    relative, = unpack("<i", image, mod0 + 4)
    offset = mod0 + relative
    tags, needed = {}, []
    for _ in range(4096):
        tag, value = unpack("<qQ", image, offset)
        offset += 16
        if not tag:
            break
        if tag == 1:
            needed.append(value)
        elif tag in tags:
            raise FormatError(f"Duplicate dynamic tag {tag:#x}")
        else:
            tags[tag] = value
    else:
        raise FormatError("Unterminated dynamic table")
    return tags, needed


def dynamic_symbols(image, header):
    tags, needed = dynamic_table(image)
    if not {4, 5, 6, 10, 11} <= tags.keys() or tags[11] != 24:
        raise FormatError("Unsupported dynamic symbol table (requires SysV hash and ELF64 symbols)")
    bucket_count, symbol_count = unpack("<II", image, tags[4])
    bounded(image, tags[4], 8 + 4 * (bucket_count + symbol_count))
    bounded(image, tags[6], symbol_count * 24)
    if header[:4] == b"NSO0":
        declared_offset, declared_size = unpack("<II", header, 0x98)
        ro_offset, = unpack("<I", header, 0x24)
    elif header[0x10:0x14] == b"NRO0":
        declared_offset, declared_size = unpack("<II", header, 0x78)
        ro_offset, = unpack("<I", header, 0x28)
    else:
        raise FormatError("Unknown executable header")
    if declared_size != symbol_count * 24 or ro_offset + declared_offset != tags[6]:
        raise FormatError("NSO and dynamic symbol layouts disagree")
    strings = bounded(image, tags[5], tags[10])

    def string(index):
        if not 0 <= index < len(strings):
            raise FormatError("Invalid symbol string offset")
        end = strings.find(b"\0", index)
        if end < 0:
            raise FormatError("Unterminated dynamic string")
        return strings[index:end].decode("utf-8", "strict")

    symbols = []
    for index in range(symbol_count):
        name, info, visibility, section, value, size = unpack("<IBBHQQ", image, tags[6] + 24 * index)
        symbols.append({"name": string(name), "defined": bool(section),
                        "section": section,
                        "binding": info >> 4, "type": info & 15,
                        "visibility": visibility & 3, "value": value, "size": size})
    relocations = []
    for address_tag, size_tag in ((7, 8), (23, 2)):
        if size_tag not in tags:
            continue
        size = tags[size_tag]
        if size % 24 or (size and address_tag not in tags):
            raise FormatError("Invalid relocation table")
        if not size:
            continue
        raw = bounded(image, tags[address_tag], size)
        for offset in range(0, size, 24):
            destination, info, addend = unpack("<QQq", raw, offset)
            symbol = info >> 32
            if symbol >= len(symbols):
                raise FormatError("Relocation symbol out of bounds")
            bounded(image, destination, 8)
            relocations.append({"offset": destination, "type": info & 0xFFFFFFFF,
                                "symbol": symbol, "addend": addend})
    return symbols, relocations, [string(index) for index in needed]


def analyze(exefs, romfs=None):
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    exports = {}

    def analyze_one(path, loader):
        image, inventory = loader(path)
        symbols, relocations, needed = dynamic_symbols(image, path.read_bytes()[:256])
        defined = [s for s in symbols if s["defined"] and s["binding"] in (1, 2) and s["visibility"] in (0, 3)]
        for symbol in defined:
            exports.setdefault(symbol["name"], []).append(path.name)
        referenced = {r["symbol"] for r in relocations if r["symbol"]}
        imports = sorted({symbols[i]["name"] for i in referenced if not symbols[i]["defined"]})
        text = inventory["segments"][0]
        system_registers, svc = Counter(), Counter()
        for offset in range(text["memory_offset"], text["memory_offset"] + text["size"], 4):
            word, = unpack("<I", image, offset)
            if word & 0xFFE0001F == 0xD4000001:
                svc[f"0x{(word >> 5) & 0xFFFF:02x}"] += 1
            if word & 0xFFF00000 in (0xD5300000, 0xD5100000):
                instruction = next(disassembler.disasm(image[offset:offset + 4], offset), None)
                if instruction is None:
                    raise FormatError("Undecodable system register instruction")
                operands = instruction.op_str.split(",")
                register = operands[-1] if instruction.mnemonic == "mrs" else operands[0]
                system_registers[instruction.mnemonic + " " + register.strip()] += 1
        return {"inventory": inventory, "needed": needed, "imports": imports,
                "import_bindings": {symbols[i]["name"]: symbols[i]["binding"]
                                    for i in referenced if not symbols[i]["defined"]},
                "exports": [s["name"] for s in defined],
                "relocation_types": dict(Counter(str(r["type"]) for r in relocations)),
                "svc": dict(svc), "system_registers": dict(system_registers)}

    modules = []
    for path in sorted(exefs.iterdir()):
        if path.is_file():
            with path.open("rb") as stream:
                magic = stream.read(4)
            if magic == b"NSO0":
                modules.append(analyze_one(path, load_nso))
    if not any(m["inventory"]["name"] == "main" for m in modules):
        raise FormatError("Missing main NSO")
    dynamic_modules = []
    if romfs is not None:
        if not romfs.is_dir():
            raise FormatError("RomFS directory missing")
        for path in sorted(romfs.rglob("*.nro")):
            dynamic_modules.append(analyze_one(path, load_nro))
    for module in modules + dynamic_modules:
        module["import_providers"] = {name: exports.get(name, []) for name in module["imports"]}
        module["unresolved_imports"] = [name for name in module["imports"] if name not in exports]
        module["unresolved_strong_imports"] = [name for name in module["unresolved_imports"]
                                              if module["import_bindings"][name] != 2]
        module["graphics_imports"] = [name for name in module["imports"]
                                     if name.startswith(("gl", "_gl", "egl", "nvn", "NvGl", "NvEgl"))]
    assets = None
    if romfs is not None:
        if not romfs.is_dir():
            raise FormatError("RomFS directory missing")
        files = sorted(path for path in romfs.rglob("*") if path.is_file())
        assets = {"file_count": len(files),
                  "extensions": dict(Counter(p.suffix.lower() for p in files)),
                  "shader_files": [str(p.relative_to(romfs)) for p in files
                                   if p.suffix.lower() in (".shader", ".vert", ".frag")],
                  "translation_status": "not implemented"}
    return {"status": "dependency-inventory", "modules": modules, "dynamic_modules": dynamic_modules,
            "import_resolution": "static symbol availability only; dynamic modules are not loaded",
            "assets": assets,
            "hle_verified": False, "graphics_verified": False,
            "warning": "Static symbols and NPDM permissions do not prove runtime service usage"}


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--exefs", required=True, type=Path)
    parser.add_argument("--romfs", type=Path)
    args = parser.parse_args(argv)
    try:
        report = analyze(args.exefs, args.romfs)
    except (OSError, ValueError, ImportError, struct.error) as error:
        print(f"Hades module analysis failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
