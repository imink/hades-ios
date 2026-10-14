#!/usr/bin/env python3
# 根据实际 EffectsForgeSwitch SPIR-V 生成 iOS MSL 与绑定反射；不使用 D2R 着色器规则。
# Generate iOS MSL and binding reflection from actual EffectsForgeSwitch SPIR-V; no D2R shader rules.
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

from inspect_target import FormatError
from prepare import outside_git, sha256, write_json


def inspect_spirv(data, stage):
    if len(data) < 20 or len(data) % 4:
        raise FormatError("Truncated SPIR-V")
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    magic, version, generator, bound, schema = words[:5]
    if magic != 0x07230203 or version >> 16 != 1 or version & 0xFF or (version >> 8) & 0xFF > 6:
        raise FormatError("Unsupported SPIR-V header")
    if schema or not 0 < bound <= 0x100000:
        raise FormatError("Invalid SPIR-V ID bound or schema")
    entries = []
    offset = 5
    instructions = 0
    while offset < len(words):
        size, opcode = words[offset] >> 16, words[offset] & 0xFFFF
        if not size or offset + size > len(words):
            raise FormatError("Invalid SPIR-V instruction size")
        if opcode == 15:
            if size < 4:
                raise FormatError("Truncated SPIR-V entry point")
            entries.append(words[offset + 1])
        offset += size
        instructions += 1
    if entries != [{"vert": 0, "frag": 4}[stage]]:
        raise FormatError("SPIR-V entry-point stage disagrees with its filename")
    return {"version": version, "generator": generator, "id_bound": bound,
            "instruction_count": instructions, "stage": stage}


def run_logged(command, log):
    result = subprocess.run(command, capture_output=True, text=True)
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise FormatError(f"Shader tool failed; diagnostic log: {log}")
    return result


def translate(source, output, tool, compile_libraries=False):
    output = outside_git(output)
    tool = tool.expanduser().resolve(strict=True)
    if not os.access(tool, os.X_OK) or not source.is_dir():
        raise FormatError("Require executable SPIRV-Cross and EffectsForgeSwitch directory")
    if source.name != "EffectsForgeSwitch":
        raise FormatError("Refuse non-Switch shader resources")
    if output.exists():
        raise FormatError("Shader output must be a new directory")
    files = sorted([*source.glob("*.vert"), *source.glob("*.frag")])
    if not files:
        raise FormatError("No Switch shader stages found")
    inspected = [(path, inspect_spirv(path.read_bytes(), path.suffix[1:]), sha256(path)) for path in files]
    if compile_libraries:
        compiler = subprocess.run(["xcrun", "--sdk", "iphoneos", "metal", "--version"], capture_output=True, text=True)
        if compiler.returncode:
            raise FormatError("Xcode Metal Toolchain missing; cannot compile or validate metallib")
    output.mkdir(parents=True, mode=0o700)
    records = []
    for path, inventory, input_digest in inspected:
        metal = output / (path.name + ".metal")
        run_logged([str(tool), str(path), "--msl", "--msl-ios", "--msl-version", "20100",
                    "--output", str(metal)], output / (path.name + ".translate.log"))
        reflection = run_logged([str(tool), str(path), "--reflect"], output / (path.name + ".reflect.log"))
        bindings = json.loads(reflection.stdout)
        write_json(output / (path.name + ".reflection.json"), bindings)
        if not metal.is_file() or not metal.stat().st_size or not bindings.get("entryPoints"):
            raise FormatError("Missing generated shader or entry-point reflection")
        if sha256(path) != input_digest:
            raise FormatError("SPIR-V input changed during translation")
        record = {"file": path.name, "input_sha256": input_digest, "msl_sha256": sha256(metal),
                  "spirv": inventory, "metallib_compiled": False}
        if compile_libraries:
            air, library = output / (path.name + ".air"), output / (path.name + ".metallib")
            run_logged(["xcrun", "--sdk", "iphoneos", "metal", "-std=ios-metal2.1",
                        "-c", str(metal), "-o", str(air)], output / (path.name + ".compile.log"))
            run_logged(["xcrun", "--sdk", "iphoneos", "metallib",
                        str(air), "-o", str(library)], output / (path.name + ".link.log"))
            record["metallib_compiled"] = True
            record["metallib_sha256"] = sha256(library)
        records.append(record)
    report = {"status": "metallib-only" if compile_libraries else "msl-only",
              "tool_sha256": sha256(tool), "shader_count": len(records), "shaders": records,
              "binding_runtime_verified": False, "rendering_verified": False}
    write_json(output / "shaders.json", report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description="Translate local Hades SPIR-V stages, not a rendering implementation")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--spirv-cross", required=True, type=Path)
    parser.add_argument("--compile", action="store_true")
    args = parser.parse_args(argv)
    try:
        report = translate(args.source, args.out, args.spirv_cross, args.compile)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Hades shader preparation failed: {error}", file=sys.stderr)
        return 2
    print(f"Hades shaders: {report['shader_count']} {report['status']}; rendering remains unverified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
