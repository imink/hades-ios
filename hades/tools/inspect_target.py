#!/usr/bin/env python3
# 只读检查 Hades 的容器与 NSO 头；依据 PFS0/NSO0 格式，不解密、不改写。
# Inspect Hades containers and NSO headers read-only using PFS0/NSO0 formats; no decryption or rewriting.
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import sys


class FormatError(ValueError):
    pass


def inspect_pfs0(path):
    size = path.stat().st_size
    with path.open("rb") as stream:
        header = stream.read(16)
        if len(header) != 16 or header[:4] != b"PFS0":
            raise FormatError("Expected a complete PFS0 header")
        count, strings_size, reserved = struct.unpack_from("<III", header, 4)
        if reserved or not 0 < count <= 4096 or strings_size > 1024 * 1024:
            raise FormatError("Invalid PFS0 entry count or string table")
        data_offset = 16 + count * 24 + strings_size
        if data_offset > size:
            raise FormatError("PFS0 table extends beyond the file")
        table = stream.read(count * 24)
        strings = stream.read(strings_size)
    entries, names, ranges = [], set(), []
    for index in range(count):
        offset, length, name_offset, reserved = struct.unpack_from("<QQII", table, index * 24)
        if reserved or name_offset >= strings_size:
            raise FormatError("Invalid PFS0 entry")
        end = strings.find(b"\0", name_offset)
        if end < 0:
            raise FormatError("Unterminated PFS0 filename")
        try:
            name = strings[name_offset:end].decode("utf-8")
        except UnicodeDecodeError as error:
            raise FormatError("Invalid UTF-8 PFS0 filename") from error
        if not name or "\\" in name or PurePosixPath(name).name != name or name in (".", ".."):
            raise FormatError("Unsafe PFS0 filename")
        if name in names:
            raise FormatError("Duplicate PFS0 filename")
        names.add(name)
        if data_offset + offset + length > size:
            raise FormatError("PFS0 entry extends beyond the file")
        if length:
            ranges.append((offset, offset + length))
        entries.append({"name": name, "size": length, "offset": data_offset + offset})
    ranges.sort()
    if any(left[1] > right[0] for left, right in zip(ranges, ranges[1:])):
        raise FormatError("Overlapping PFS0 entries")
    return {"format": "PFS0", "size": size, "entries": entries,
            "title_id_verified": False, "version_verified": False}


def inspect_nso(path):
    size = path.stat().st_size
    with path.open("rb") as stream:
        header = stream.read(256)
        if len(header) != 256 or header[:4] != b"NSO0":
            raise FormatError(f"{path.name}: expected a complete NSO0 header")
        version, reserved, flags = struct.unpack_from("<III", header, 4)
        if version or reserved or flags & ~0x3F:
            raise FormatError(f"{path.name}: unsupported NSO header")
        compressed_sizes = struct.unpack_from("<III", header, 0x60)
        segments, file_ranges, memory_ranges = [], [], []
        for index, name in enumerate(("text", "ro", "data")):
            offset, memory_offset, length = struct.unpack_from("<III", header, 0x10 + index * 16)
            compressed = bool(flags & (1 << index))
            stored_size = compressed_sizes[index] if compressed else length
            if length:
                if offset < 256 or not stored_size or offset + stored_size > size:
                    raise FormatError(f"{path.name}: invalid {name} file range")
                if memory_offset + length > 0x100000000:
                    raise FormatError(f"{path.name}: invalid {name} memory range")
                file_ranges.append((offset, offset + stored_size))
                memory_ranges.append((memory_offset, memory_offset + length))
            segments.append({"name": name, "file_offset": offset,
                             "memory_offset": memory_offset, "size": length,
                             "stored_size": stored_size, "compressed": compressed})
        for ranges in (file_ranges, memory_ranges):
            ranges.sort()
            if any(left[1] > right[0] for left, right in zip(ranges, ranges[1:])):
                raise FormatError(f"{path.name}: overlapping NSO segments")
        digest = hashlib.sha256()
        stream.seek(0)
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"name": path.name, "sha256": digest.hexdigest(),
            "build_id": header[0x40:0x60].hex(),
            "bss_size": struct.unpack_from("<I", header, 0x3C)[0],
            "segments": segments, "segment_hashes_verified": False,
            "instruction_analysis": "not performed", "rewrite_verified": False}


def probe(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=30, check=False)
    except FileNotFoundError:
        return {"available": False, "reason": "executable missing"}
    except subprocess.TimeoutExpired:
        return {"available": False, "reason": "probe timed out"}
    return {"available": result.returncode == 0,
            "output": (result.stdout + result.stderr).strip()}


def preflight(args, target):
    package = inspect_pfs0(args.package)
    tools = {name: shutil.which(name) for name in
             ("hactool", "nstool", "xcodegen", "spirv-cross", "dotnet", "xcodebuild")}
    for name, attribute in (("nstool", "nstool"), ("spirv-cross", "spirv_cross")):
        override = getattr(args, attribute, None)
        if override is not None:
            override = override.expanduser().resolve()
            if not override.is_file() or not os.access(override, os.X_OK):
                raise FormatError(f"{name} override is not executable")
            tools[name] = str(override)
    keys_present = args.keys is not None and args.keys.is_file()
    blockers = []
    if not keys_present:
        blockers.append("Provide the path to your own locally exported prod.keys; contents are never read by this check")
    if not (tools["hactool"] or tools["nstool"]):
        blockers.append("No supported local extraction tool found")
    metal = probe(["xcrun", "--sdk", "iphoneos", "metal", "--version"])
    sdk = probe(["xcrun", "--sdk", "iphoneos", "--show-sdk-version"])
    signing = probe(["security", "find-identity", "-v", "-p", "codesigning"])
    if not metal["available"]:
        blockers.append("Metal Toolchain unavailable")
    if not sdk["available"]:
        blockers.append("iPhoneOS SDK unavailable")
    if not signing["available"] or "0 valid identities found" in signing.get("output", ""):
        blockers.append("No verified local codesigning identity")
    for name in ("xcodegen", "spirv-cross", "dotnet"):
        if not tools[name]:
            blockers.append(f"Missing build dependency: {name}")
    blockers.append("This read-only check does not establish a working Hades runtime; preparation receipts are separate")
    return {"target": target, "workspace": str(Path(os.environ.get(
                target["workspace_environment"], target["default_workspace"])).expanduser()),
            "package": package, "keys_path_exists": keys_present, "tools": tools,
            "ios_sdk": sdk, "metal": metal, "signing": signing,
            "status": "blocked", "blockers": blockers}


def main(argv=None):
    parser = argparse.ArgumentParser(description="Read-only Hades inspection; never builds or launches a game")
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check")
    check.add_argument("--package", required=True, type=Path)
    check.add_argument("--keys", type=Path)
    check.add_argument("--nstool", type=Path)
    check.add_argument("--spirv-cross", type=Path)
    nso = commands.add_parser("nso")
    nso.add_argument("--exefs", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        target = json.loads((Path(__file__).resolve().parents[1] / "target.json").read_text())
        if args.command == "check":
            result, code = preflight(args, target), 1
        else:
            modules = []
            for path in sorted(args.exefs.iterdir()):
                if path.is_file():
                    with path.open("rb") as stream:
                        magic = stream.read(4)
                    if magic == b"NSO0":
                        modules.append(inspect_nso(path))
            if not modules or not any(module["name"] == "main" for module in modules):
                raise FormatError("ExeFS must contain a main NSO; module list is discovered, not assumed")
            result, code = {"target": target, "modules": modules,
                            "status": "inventory-only", "title_id_verified": False}, 0
    except (OSError, ValueError) as error:
        print(f"Hades inspection failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, ensure_ascii=True))
    return code


if __name__ == "__main__":
    sys.exit(main())
