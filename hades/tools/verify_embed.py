#!/usr/bin/env python3
# 逐段核对 ARM64 iOS 对象的嵌入字节与 16K 布局；不将对象文件称为签名 App。
# Check embedded bytes and 16K layout in an ARM64 iOS object; an object is not a signed app.
from pathlib import Path
import struct

from inspect_target import FormatError


def verify_object(path, images, names, *, linked=False):
    file_size = path.stat().st_size
    with path.open("rb") as stream:
        header = stream.read(32)
        if len(header) != 32:
            raise FormatError("Truncated Mach-O header")
        magic, cpu, subtype, filetype, count, size, flags, reserved = struct.unpack("<IiiIIIII", header)
        if magic != 0xFEEDFACF or cpu != 0x0100000C or filetype != (2 if linked else 1) or size > 1024 * 1024:
            raise FormatError("Expected ARM64 Mach-O executable" if linked else "Expected ARM64 Mach-O object")
        if linked and not flags & 0x200000:
            raise FormatError("iPhoneOS executable must preserve PIE/ASLR")
        commands = stream.read(size)
        if len(commands) != size:
            raise FormatError("Truncated Mach-O load commands")
    sections = {}
    offset, platform = 0, None
    executable_segments = []
    code_signature = False
    for _ in range(count):
        if offset + 8 > len(commands):
            raise FormatError("Invalid Mach-O command count")
        command, command_size = struct.unpack_from("<II", commands, offset)
        if command_size < 8 or offset + command_size > len(commands):
            raise FormatError("Invalid Mach-O command size")
        if command == 0x32:
            if command_size < 24:
                raise FormatError("Truncated build version")
            platform, = struct.unpack_from("<I", commands, offset + 8)
        if command == 0x19:
            if command_size < 72:
                raise FormatError("Truncated segment")
            nsections, = struct.unpack_from("<I", commands, offset + 64)
            segment = commands[offset + 8:offset + 24].rstrip(b"\0").decode()
            maximum, initial = struct.unpack_from("<ii", commands, offset + 56)
            if linked:
                if initial & 4:
                    executable_segments.append(segment)
                if initial & 6 == 6 or (segment.startswith("__DM") and (initial != 3 or maximum != 3)):
                    raise FormatError("Invalid executable/writable Mach-O segment permissions")
            if 72 + nsections * 80 > command_size:
                raise FormatError("Truncated section table")
            for index in range(nsections):
                values = struct.unpack_from("<16s16sQQIIIIIIII", commands, offset + 72 + index * 80)
                section_name, segment_name, address, section_size, file_offset, alignment = values[:6]
                key = (segment_name.rstrip(b"\0").decode(), section_name.rstrip(b"\0").decode())
                if key in sections or file_offset + section_size > file_size:
                    raise FormatError("Duplicate or out-of-bounds Mach-O section")
                sections[key] = (address, section_size, file_offset, alignment)
        if command == 0x2C:
            if command_size < 24 or struct.unpack_from("<I", commands, offset + 16)[0]:
                raise FormatError("Encrypted executable is not a local development artifact")
        if command == 0x1D:
            if command_size != 16:
                raise FormatError("Invalid Mach-O code signature command")
            signature_offset, signature_size = struct.unpack_from("<II", commands, offset + 8)
            if not signature_size or signature_offset + signature_size > file_size:
                raise FormatError("Truncated Mach-O code signature")
            code_signature = True
        offset += command_size
    if offset != len(commands) or platform != 2:
        raise FormatError("Expected complete iPhoneOS build metadata")
    if linked and executable_segments != ["__TEXT"]:
        raise FormatError("iPhoneOS AOT requires a single executable __TEXT segment")
    checked = []
    round_page = lambda value: (value + 0x3FFF) & ~0x3FFF
    for name in names:
        headers = dict(line[2:].split() for line in (images / (name + ".fixups.tsv")).read_text().splitlines())
        text_end, patch_offset = int(headers["TEXT_END"], 0), int(headers["PATCH_OFF"], 0)
        bias = int(headers.get("LOAD_BIAS", "0"), 0)
        if bias not in (0,4096,8192,12288):
            raise FormatError("Invalid embedded portable load bias")
        binary = images / (name + ".patched.bin")
        image_size = binary.stat().st_size
        writable_start=round_page(text_end+bias)-bias
        spans = [(("__TEXT", "__dm" + name + "x0"), 0, writable_start),
                 (("__DM" + name + "W", "__img"), writable_start, patch_offset),
                 (("__TEXT", "__dm" + name + "x1"), patch_offset, round_page(image_size+bias)-bias)]
        for key, start, end in spans:
            if key not in sections:
                raise FormatError("Missing embedded section")
            address, section_size, file_offset, alignment = sections[key]
            prefix=bias if start==0 else 0
            if section_size != end - start+prefix or alignment < 14 or address % 0x4000:
                raise FormatError("Embedded section does not preserve its 16K layout")
            with binary.open("rb") as source, path.open("rb") as object_file:
                source.seek(start)
                object_file.seek(file_offset)
                if object_file.read(prefix)!=bytes(prefix):
                    raise FormatError("Biased signed code prefix is not zero padding")
                remaining, available = section_size-prefix, max(0, min(end, image_size) - start)
                while remaining:
                    length = min(1024 * 1024, remaining)
                    from_source = min(length, available)
                    expected = source.read(from_source) + bytes(length - from_source)
                    if object_file.read(length) != expected:
                        raise FormatError("Embedded bytes differ from the verified image")
                    remaining -= length
                    available -= from_source
            checked.append(",".join(key))
    return {"platform": "iPhoneOS", "cpu": "ARM64", "byte_verified_sections": checked,
            "linked_permissions_verified": linked, "codesign_verified": False,
            "code_signature_present": code_signature}
