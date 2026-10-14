#!/usr/bin/env python3
"""Bind SPIRV-Cross's actual MSL interface to descriptor identities, never NVN guesses."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

from inspect_target import FormatError
from prepare import outside_git, sha256, write_json


def msl_bindings(reflection, msl):
    records = []
    accounted = set()
    for category, attribute in (("ubos", "buffer"), ("separate_images", "texture"), ("separate_samplers", "sampler")):
        for resource in reflection.get(category, []):
            if resource.get("array"):
                raise FormatError("Descriptor arrays require a separate binding implementation")
            if category == "ubos":
                name = reflection["types"][resource["type"]]["name"]
                pattern = r"constant\s+" + re.escape(name) + r"\s*[&*]\s*\w+\s*\[\[buffer\((\d+)\)\]\]"
            else:
                pattern = r"\b" + re.escape(resource["name"]) + r"\s*\[\[" + attribute + r"\((\d+)\)\]\]"
            matches = re.findall(pattern, msl)
            if len(matches) > 1:
                raise FormatError("Ambiguous MSL resource interface")
            index = int(matches[0]) if matches else None
            if index is not None:
                if index >= 31 or (attribute, index) in accounted:
                    raise FormatError("MSL resource index overflow or collision")
                accounted.add((attribute, index))
            records.append({"kind": attribute, "name": resource["name"],
                            "set": resource["set"], "binding": resource["binding"],
                            "msl_index": index, "block_size": resource.get("block_size", 0),
                            "interface_status": "emitted" if index is not None else "not_emitted_by_msl",
                            "nvn_slot": None})
    declared = {(kind, int(index)) for kind, index in re.findall(
        r"\[\[(buffer|texture|sampler)\((\d+)\)\]\]", msl)}
    if accounted != declared:
        raise FormatError("MSL interface has resources not accounted for by SPIR-V reflection")
    if any(reflection.get(kind) for kind in ("ssbos", "textures", "images", "push_constants")):
        raise FormatError("Unsupported resource category in direct MSL binding route")
    entries = re.findall(r"^(vertex|fragment)\s+\w+\s+(\w+)\(", msl, re.MULTILINE)
    if len(entries) != 1:
        raise FormatError("Expected one direct MSL graphics entry point")
    return {"stage": entries[0][0], "entry": entries[0][1], "resources": records}


def catalog(root, msl_directory, tool, output):
    output = outside_git(output)
    if output.exists():
        raise FormatError("Binding catalog output must be new")
    receipt = json.loads((msl_directory / "shaders.json").read_text())
    if sha256(tool) != receipt["tool_sha256"]:
        raise FormatError("SPIRV-Cross changed since translation")
    directory = root / "data/RomFS/Content/EffectsForgeSwitch"
    expected = sorted(path.name for path in directory.iterdir() if path.suffix in (".frag", ".vert"))
    if not expected or sorted(shader["file"] for shader in receipt["shaders"]) != expected:
        raise FormatError("Translation receipt does not cover every Switch shader stage")
    records = []
    for shader in receipt["shaders"]:
        name = shader["file"]
        if Path(name).name != name or Path(name).suffix not in (".frag", ".vert"):
            raise FormatError("Invalid shader filename")
        source = directory / name
        metal = msl_directory / (name + ".metal")
        if sha256(source) != shader["input_sha256"] or sha256(metal) != shader["msl_sha256"]:
            raise FormatError("Shader input or generated MSL changed")
        result = subprocess.run([str(tool), str(source), "--reflect"], capture_output=True, text=True)
        if result.returncode:
            raise FormatError(f"Cannot recheck shader reflection: {name}")
        reflection = json.loads(result.stdout)
        if reflection != json.loads((msl_directory / (name + ".reflection.json")).read_text()):
            raise FormatError("Saved reflection differs from the actual SPIR-V")
        layout = msl_bindings(reflection, metal.read_text())
        if layout["stage"] != {"vert": "vertex", "frag": "fragment"}[source.suffix[1:]]:
            raise FormatError("MSL stage mismatch")
        records.append({"file": name, "input_sha256": shader["input_sha256"],
                        "msl_sha256": shader["msl_sha256"], **layout})
    output.mkdir(parents=True, mode=0o700)
    report = {"status": "direct-msl-interface-verified", "shaders": records,
              "shader_count": len(records), "guest_gl_to_nvn_mapping_verified": False,
              "warning": "Descriptor set/binding are not NVN slots; a guest program binding map is required"}
    write_json(output / "bindings.json", report)
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--msl", type=Path, required=True)
    parser.add_argument("--spirv-cross", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        catalog(outside_git(args.work), outside_git(args.msl), args.spirv_cross, args.out)
    except (OSError, ValueError, KeyError) as error:
        print(f"Hades binding catalog failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
