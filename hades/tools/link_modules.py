#!/usr/bin/env python3
"""Fail-closed AArch64 relocation planning for the verified local AOT images."""
import argparse
from collections import Counter
import hashlib
import json
import re
from pathlib import Path
import struct
import subprocess
import sys

from analyze_modules import dynamic_symbols, dynamic_table, load_nso
from inspect_target import FormatError
from prepare import outside_git, REPO, verified_modules, write_json


def host_bindings(use_vulkan=False):
    lines = (REPO / "hades/runtime/host_bindings.def").read_text().splitlines()
    if use_vulkan:
        lines += (REPO / "hades/runtime/vulkan_bindings.def").read_text().splitlines()
    result = {}
    for line in lines:
        match = re.fullmatch(r'HADES_BINDING\([A-Z0-9_]+, "([^"]+)"\)', line)
        if not match or match[1] in result or len(result) >= 256:
            raise FormatError("Invalid or duplicate static host binding definition")
        result[match[1]] = len(result)
    return result


def resolve_symbol(requester, symbol, modules):
    if symbol["type"] not in (0, 1, 2):
        raise FormatError(f"Unsupported symbol type: {symbol['name']}")
    if symbol["defined"] and (symbol["binding"] == 0 or symbol["visibility"] in (1, 2, 3)):
        return requester, symbol
    candidates = [(index, item) for index, module in enumerate(modules)
                  for item in module["symbols"] if item["name"] == symbol["name"]
                  and item["defined"] and item["binding"] in (1, 2)
                  and item["visibility"] in (0, 3)]
    strong = [item for item in candidates if item[1]["binding"] == 1]
    if strong or candidates:
        return (strong or candidates)[0]
    if symbol["binding"] == 2:
        return -1, {"value": 0, "section": 0xFFF1}
    raise FormatError(f"Unresolved strong import: {symbol['name']}")


def relocation_plan(modules, hosts=None):
    hosts = {} if hosts is None else hosts
    if (any(type(index) is not int or not 0 <= index < 256 for index in hosts.values())
            or len(set(hosts.values())) != len(hosts)):
        raise FormatError("Invalid static host trampoline indices")
    hits = {name: 0 for name in hosts}
    records, counts = [], Counter()
    # The index is shared across all modules; symbol resolution must not be quadratic.
    exports = {}
    for index, module in enumerate(modules):
        for symbol in module["symbols"]:
            if symbol["defined"] and symbol["binding"] in (1, 2) and symbol["visibility"] in (0, 3):
                previous = exports.get(symbol["name"])
                if previous is None or previous[1]["binding"] == 2 and symbol["binding"] == 1:
                    exports[symbol["name"]] = (index, symbol)
    for index, module in enumerate(modules):
        seen = set()
        for relocation in module["relocations"]:
            offset, kind, si, addend = (relocation[key] for key in ("offset", "type", "symbol", "addend"))
            if kind not in (257, 1025, 1026, 1027):
                raise FormatError(f"{module['name']}: unsupported relocation {kind}")
            if offset in seen or offset % 8 or not module["writable_start"] <= offset <= module["original_size"] - 8:
                raise FormatError(f"{module['name']}: duplicate, unaligned or non-writable relocation")
            seen.add(offset)
            target_module, value = index, addend
            if kind == 1027:
                if si:
                    raise FormatError("RELATIVE relocation has a symbol")
            else:
                if not 0 <= si < len(module["symbols"]):
                    raise FormatError("Relocation symbol index out of bounds")
                symbol = module["symbols"][si]
                if symbol["name"] in hosts:
                    provider = exports.get(symbol["name"])
                    if (addend or symbol["binding"] not in (1, 2) or symbol["visibility"] in (1, 2)
                            or symbol["type"] not in (0, 2)
                            or not provider or provider[1]["type"] != 2):
                        raise FormatError(f"Host binding is not a zero-addend exported function: {symbol['name']}")
                    records.append((index, offset, -2 - hosts[symbol["name"]], 0))
                    hits[symbol["name"]] += 1
                    counts[kind] += 1
                    continue
                if symbol["type"] not in (0, 1, 2):
                    raise FormatError(f"Unsupported symbol type: {symbol['name']}")
                if symbol["defined"] and (symbol["binding"] == 0 or symbol["visibility"] in (1, 2, 3)):
                    target_module, target = index, symbol
                elif symbol["name"] in exports:
                    target_module, target = exports[symbol["name"]]
                elif symbol["binding"] == 2:
                    target_module, target = -1, {"value": 0, "section": 0xFFF1}
                else:
                    raise FormatError(f"Unresolved strong import: {symbol['name']}")
                if target.get("type", 0) not in (0, 1, 2) or target["section"] >= 0xFF00 and target["section"] != 0xFFF1:
                    raise FormatError(f"Unsupported resolved symbol: {symbol['name']}")
                if target["section"] == 0xFFF1:
                    target_module = -1
                value = target["value"] + addend
            if target_module >= 0 and not 0 <= value <= modules[target_module]["original_size"]:
                raise FormatError(f"{module['name']}: relocation target outside original image")
            records.append((index, offset, target_module, value & ((1 << 64) - 1)))
            counts[kind] += 1
    if any(not count for count in hits.values()):
        raise FormatError("Configured host binding has no actual symbol relocation reference")
    return records, counts


def build_plan(root, use_host_services=False, use_vulkan=False):
    verified = {module["module"]: module for module in verified_modules(root)}
    if set(verified) != {"main", "sdk", "subsdk0", "subsdk1", "eos"}:
        raise FormatError("Native core probe requires all five verified modules")
    modules = []
    for name in ("main", "subsdk0", "subsdk1", "sdk", "eos"):
        record = verified[name]
        source = root / record.get("normalized_source", "data/ExeFS/" + name)
        image, inventory = load_nso(source)
        symbols, relocations, _ = dynamic_symbols(image, source.read_bytes()[:256])
        tags, _ = dynamic_table(image)
        layout = {key: int(value, 0) for key, value in (
            line[2:].split() for line in (root / "build/abi" / (name + ".fixups.tsv")).read_text().splitlines())}
        bias = layout.get("LOAD_BIAS", 0)
        if bias not in (0, 4096, 8192, 12288):
            raise FormatError("Invalid portable load bias")
        modules.append({"name": name, "symbols": symbols, "relocations": relocations,
                        "original_size": len(image), "writable_start": ((layout["TEXT_END"] + bias + 0x3FFF) & ~0x3FFF) - bias,
                        "layout": layout, "inventory": inventory, "initializer": tags.get(12, 0)})
        if tags.get(12, 0) and (tags[12] >= layout["TEXT_END"] or tags[12] % 4):
            raise FormatError("Module DT_INIT is not inside original executable text")
    records, counts = relocation_plan(modules, host_bindings(use_vulkan) if use_host_services else None)
    return modules, records, counts


def npdm_thread_policy(npdm):
    if len(npdm) < 128 or npdm[:4] != b"META" or npdm[0xE] >= 64 or npdm[0xF] >= 3:
        raise FormatError("Unsupported NPDM main thread scheduling policy")
    if not npdm[0xC] & 1 or (npdm[0xC] >> 1) & 7 != 3:
        raise FormatError("Native memory probe requires NPDM 64-bit / 39-bit address space")
    if struct.unpack_from("<I", npdm, 0x14)[0]:
        raise FormatError("Nonzero NPDM system-resource accounting is not implemented")
    aci, aci_size = struct.unpack_from("<II", npdm, 0x70)
    if aci_size < 0x40 or aci + aci_size > len(npdm) or npdm[aci:aci + 4] != b"ACI0":
        raise FormatError("Invalid NPDM ACI0 capability header")
    offset, size = struct.unpack_from("<II", npdm, aci + 0x30)
    if offset < 0x40 or not size or size % 4 or offset + size > aci_size:
        raise FormatError("Invalid NPDM kernel capabilities extent")
    flags = [value for value, in struct.iter_unpack("<I", npdm[aci + offset:aci + offset + size])
             if value & 15 == 7]
    if len(flags) != 1:
        raise FormatError("NPDM must have exactly one kernel thread capability")
    priority_lo, priority_hi = (flags[0] >> 10) & 63, (flags[0] >> 4) & 63
    core_lo, core_hi = (flags[0] >> 16) & 255, (flags[0] >> 24) & 255
    if not priority_lo <= npdm[0xE] <= priority_hi or not core_lo <= npdm[0xF] <= core_hi < 3:
        raise FormatError("NPDM main thread is outside supported kernel capability ranges")
    return {"priority": npdm[0xE], "core": npdm[0xF],
            "priority_mask": ((1 << (priority_hi - priority_lo + 1)) - 1) << priority_lo,
            "core_mask": ((1 << (core_hi - core_lo + 1)) - 1) << core_lo}


def generate_artifacts(root, output, use_host_services=False, use_vulkan=False):
    if use_vulkan and not use_host_services:
        raise FormatError("Vulkan profile requires explicit host services")
    modules, records, counts = build_plan(root, use_host_services, use_vulkan)
    policy = npdm_thread_policy((root / "data/ExeFS/main.npdm").read_bytes())
    output = outside_git(output)
    if output.exists():
        raise FormatError("Native probe output must be a new directory")
    output.mkdir(mode=0o700, parents=True)
    relocation_bytes = b"".join(struct.pack("<QQiI", offset, value, target, index)
                                for index, offset, target, value in records)
    (output / "relocations.bin").write_bytes(relocation_bytes)
    dynamic = json.loads((root / "build/rewrite-dynamic.json").read_text())
    nrr_digest = bytes.fromhex(dynamic["nrr"]["nrr_sha256"])
    nro_digest = bytes.fromhex(dynamic["nrr"]["nro_sha256"])
    display_version = json.loads((root / "extraction.json").read_text())["nacp"]["display_version"]
    if (not isinstance(display_version, str) or not display_version
            or any(not 32 <= ord(c) <= 126 for c in display_version) or len(display_version) >= 16):
        raise FormatError("Invalid extracted application display version")
    lines = ['#include "core.h"',
             "const char hades_display_version[16] = " + json.dumps(display_version) + ";",
             "const hades_spec hades_specs[] = {"]
    for module in modules:
        h = module["layout"]
        keys = [h[f"KEY_{key}_IDX"] for key in ("K18", "KTLS", "KTLR", "KGATE")]
        image_size = (root / "build/abi" / (module["name"] + ".patched.bin")).stat().st_size
        bias = h.get("LOAD_BIAS", 0)
        lines.append('{"%s",%d,%d,%d,%d,%d,%d,{%s},%d},' % (
            module["name"], module["original_size"], module["writable_start"], h["PATCH_OFF"],
            h["KEY_PAGE_OFF"], h["KEY_PAGE_SIZE"], ((image_size + bias + 0x3FFF) & ~0x3FFF) - bias,
            ",".join(map(str, keys)), bias))
    lines += ["};", "const unsigned hades_spec_count = 5;",
              f"const int hades_host_services_enabled = {int(use_host_services)};",
              f"const unsigned hades_main_priority = {policy['priority']};",
              f"const unsigned hades_main_core = {policy['core']};",
              f"const uint64_t hades_priority_mask = {policy['priority_mask']}ull;",
              f"const uint64_t hades_core_mask = {policy['core_mask']}ull;",
              "const uint64_t hades_address_space_limit = 1ull << 39;",
              "const uint64_t hades_system_resource_budget = 0;",
              "const unsigned hades_relocation_count = %d;" % len(records),
              "const unsigned char hades_nrr_digest[32] = {%s};" % ",".join(map(str, nrr_digest)),
              "const unsigned char hades_nro_digest[32] = {%s};" % ",".join(map(str, nro_digest)),
              "const size_t hades_eos_bss = %d;" % modules[-1]["inventory"]["bss_size"]]
    for variable, module_index, symbol_name in (
        ("hades_version_offset", 4, "EOS_GetVersion"),
        ("hades_tick_offset", 3, "_ZN2nn2os13GetSystemTickEv"),
        ("hades_frequency_offset", 3, "_ZN2nn2os22GetSystemTickFrequencyEv")):
        symbol = next((s for s in modules[module_index]["symbols"] if s["name"] == symbol_name and s["defined"]), None)
        if symbol is None or symbol["type"] != 2:
            raise FormatError(f"Missing native probe export: {symbol_name}")
        lines.append(f"const size_t {variable} = {symbol['value']};")
    gates = []
    for index, module in enumerate(modules):
        for line in (root / "build/abi" / (module["name"] + ".gates.tsv")).read_text().splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            ret, kind, arg, original = line.split()
            if kind not in ("svc", "cntpct", "cntvct"):
                raise FormatError("Non-portable gate in native probe")
            gates.append("{%d,%d,%d,%d,%d}" % (index, int(ret, 0), int(original, 0),
                         {"svc": 0, "cntpct": 1, "cntvct": 1}[kind], int(arg)))
    lines += ["const hades_gate hades_gates[] = {" + ",".join(gates) + "};",
              f"const unsigned hades_gate_count = {len(gates)};"]
    lines.append("const hades_entry hades_bootstrap[] = {")
    for name in ("_init_libc0", "nnosInitialize", "_init_libc1",
                 "nninitInitializeSdkModule", "nninitStartup", "_init_libc2"):
        index, symbol = resolve_symbol(0, {"name": name, "type": 2, "defined": False,
                                           "binding": 1, "visibility": 0}, modules)
        if symbol["type"] != 2:
            raise FormatError(f"Bootstrap export is not a function: {name}")
        lines.append('{"%s",%d,%d},' % (name, index, symbol["value"]))
    lines.append("};")
    lines.append("const hades_entry hades_initializers[] = {")
    for index, module in enumerate(modules[:4]):
        lines.append('{"%s",%d,%d},' % (module["name"], index, module["initializer"]))
    lines += ["};", "const unsigned hades_initializer_count = 4;"]
    for variable, name in (("hades_game_entry", "nnMain"),
                           ("hades_set_argc", "_ZN2nn2os11SetHostArgcEi"),
                           ("hades_set_argv", "_ZN2nn2os11SetHostArgvEPPc")):
        index, symbol = resolve_symbol(0, {"name": name, "type": 2, "defined": False,
                                           "binding": 1, "visibility": 0}, modules)
        if symbol["type"] != 2:
            raise FormatError("Game bootstrap symbol is not a function")
        lines.append('const hades_entry %s = {"%s",%d,%d};' % (variable, name, index, symbol["value"]))
    (output / "manifest.c").write_text("\n".join(lines) + "\n")
    assembly = output / "images.S"
    generation = subprocess.run([sys.executable, str(REPO / "dm/ios/tools/gen_embed.py"),
                                str(root / "build/abi"), str(assembly), *[m["name"] for m in modules]],
                               capture_output=True, text=True)
    if generation.returncode:
        raise FormatError("Native embedding generation failed")
    with assembly.open("a") as target:
        target.write(f'.section __DATA,__hdrel\n.p2align 3\n.incbin "{output / "relocations.bin"}"\n')
    flags = generation.stdout.split()
    write_json(output / "plan.json", {"status": "strict-relocation-plan", "relocation_count": len(records),
               "types": dict(counts), "relocation_sha256": hashlib.sha256(relocation_bytes).hexdigest(),
               "npdm_thread_policy": policy,
               "host_services": use_host_services,
               "host_binding_names": list(host_bindings(use_vulkan)) if use_host_services else [],
               "vulkan_profile": use_vulkan,
               "host_relocation_count": sum(record[2] < -1 for record in records),
               "modules": [m["name"] for m in modules], "constructors_executed": False,
               "ios_runtime_verified": False})
    return flags


def generate_probe(root, output, use_host_services=False, moltenvk=None):
    flags = generate_artifacts(root, output, use_host_services, moltenvk is not None)
    if moltenvk:
        from vulkan_dispatch import framework, generate
        include, library = framework(outside_git(moltenvk), "macos")
        generate(output / "vulkan_dispatch.inc", include)
        flags += ["-DHADES_VULKAN_HOST", "-fobjc-arc", "-I", str(include), "-I", str(output),
                  str(REPO / "hades/runtime/vulkan.m"), str(library), "-lc++",
                  str(REPO / "hades/runtime/window_probe.m"),
                  "-framework", "Foundation", "-framework", "Metal", "-framework", "QuartzCore",
                  "-framework", "IOSurface", "-framework", "IOKit", "-framework", "CoreGraphics",
                  "-framework", "AppKit"]
    build = subprocess.run(["xcrun", "clang", "-arch", "arm64", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-DHADES_HOST_GATE_STACK",
                            "-Wno-deprecated-declarations", "-I", str(REPO / "hades/runtime"),
                            str(REPO / "hades/runtime/core.c"), str(REPO / "hades/runtime/memory.c"),
                            str(REPO / "hades/runtime/callout.c"),
                            str(REPO / "hades/runtime/ipc.c"),
                            str(REPO / "hades/runtime/audio.c"), "-framework", "AudioToolbox",
                            str(REPO / "hades/runtime/threads.c"),
                            str(REPO / "hades/runtime/host.c"), str(REPO / "hades/runtime/save.c"),
                            str(REPO / "hades/runtime/host_entry.S"),
                            str(REPO / "hades/runtime/probe.c"),
                            str(REPO / "dm/host/entry.S"), str(output / "manifest.c"), str(output / "images.S"),
                            *flags, "-o", str(output / "hades-core")], capture_output=True, text=True)
    (output / "build.log").write_text(build.stdout + build.stderr)
    if build.returncode:
        raise FormatError("Native probe compilation failed; inspect build.log")


def main():
    parser = argparse.ArgumentParser(description="Build local macOS AOT module/timer probe, not the game")
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--host-services", action="store_true")
    parser.add_argument("--moltenvk", type=Path, help="Explicit verified local Vulkan/Metal static-framework root")
    args = parser.parse_args()
    try:
        generate_probe(outside_git(args.work), args.out, args.host_services, args.moltenvk)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Hades native core failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
