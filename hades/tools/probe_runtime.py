#!/usr/bin/env python3
"""Persist native core/GPU evidence separately from the deliberately incomplete boot probe."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

from inspect_target import FormatError
from analyze_modules import load_nso, dynamic_symbols
from link_modules import generate_probe
from prepare import outside_git, REPO, sha256, write_json
from reflect_bindings import catalog


def logged(command, log, timeout=120):
    try:
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        partial = (error.stdout or b"") + (error.stderr or b"")
        log.write_text(partial.decode(errors="replace") if isinstance(partial, bytes) else partial)
        raise FormatError(f"Probe timed out: {log}")
    log.write_text(result.stdout + result.stderr)
    return result


def probe(root, output, cross):
    output = outside_git(output)
    if output.exists():
        raise FormatError("Runtime probe output must be new")
    output.mkdir(parents=True, mode=0o700)
    sources = (
        REPO / "hades/runtime/core.c", REPO / "hades/runtime/core.h",
        REPO / "hades/runtime/callout.c", REPO / "hades/runtime/callout.h",
        REPO / "hades/runtime/memory.c", REPO / "hades/runtime/memory.h",
        REPO / "hades/runtime/ipc.c", REPO / "hades/runtime/ipc.h",
        REPO / "hades/runtime/audio.c", REPO / "hades/runtime/audio.h",
        REPO / "hades/runtime/trace.h",
        REPO / "hades/runtime/diagnostics.h",
        REPO / "hades/tests/callout_probe.c",
        REPO / "hades/tests/audio_probe.c",
        REPO / "hades/tests/trace_probe.c",
        REPO / "hades/tests/ios_policy_probe.c", REPO / "hades/ios/policy.h",
        REPO / "hades/tests/diagnostics_probe.c",
        REPO / "hades/runtime/threads.c", REPO / "hades/runtime/threads.h",
        REPO / "hades/runtime/probe.c", REPO / "hades/runtime/render_probe.m",
        REPO / "hades/runtime/probes.h",
        REPO / "hades/runtime/host.c", REPO / "hades/runtime/host.h",
        REPO / "hades/runtime/save.c", REPO / "hades/runtime/save.h",
        REPO / "hades/runtime/host_entry.S", REPO / "hades/runtime/host_bindings.def",
        REPO / "dm/host/entry.S", REPO / "dm/gfx/nvn/nvn_spirv_bind.c",
        REPO / "dm/gfx/nvn/nvn_spirv_bind.h",
        REPO / "dm/gfx/nvn/nvn_obj.h", REPO / "dm/gfx/nvn/nvn_tex.h",
        REPO / "dm/gfx/nvn/include/nvn.h",
        REPO / "hades/tools/probe_runtime.py", REPO / "hades/tools/link_modules.py",
        REPO / "hades/tools/nro.py", REPO / "hades/tools/reflect_bindings.py",
        REPO / "hades/tests/page_geometry_probe.c",
        REPO / "hades/tests/test_core_runtime.py",
        REPO / "hades/tests/thread_probe.c", REPO / "hades/tests/memory_probe.c",
        REPO / "hades/tests/ipc_probe.c",
        REPO / "hades/tests/host_probe.c", REPO / "hades/tools/analyze_modules.py",
        REPO / "hades/tests/save_probe.c", REPO / "hades/tests/transfer_probe.c",
        REPO / "hades/runtime/vulkan.m", REPO / "hades/runtime/vulkan.h",
        REPO / "hades/runtime/window_probe.m", REPO / "hades/runtime/vulkan_optional.def",
        REPO / "hades/runtime/vulkan_bindings.def", REPO / "hades/tools/vulkan_dispatch.py",
        REPO / "hades/tests/vulkan_probe.c", REPO / "hades/tests/test_vulkan_runtime.py",
        REPO / "hades/tests/gate_stack_probe.c", REPO / "hades/tests/gate_stack_probe.S")
    source_hashes = {str(path.relative_to(REPO)): sha256(path) for path in sources}
    core = output / "core"
    generate_probe(root, core)
    core_tests = logged([sys.executable, "-m", "unittest", "discover", "-s",
                         REPO / "hades/tests", "-p", "test_core_runtime.py"], core / "core-tests.log")
    if core_tests.returncode:
        raise FormatError("Native core/memory/IPC/pthread/host-gate tests failed")
    executable = core / "hades-core"
    signature = logged(["codesign", "--verify", "--strict", executable], core / "signature.log")
    if signature.returncode:
        raise FormatError("Native AOT executable signature verification failed")
    arguments = [executable, root / "data/RomFS/.nrr/game.nrr", root / "data/RomFS/nro/EOSSDK-Switch-Shipping.nro"]
    run = logged(arguments, core / "run.log")
    if run.returncode or "CORE_OK:" not in run.stdout:
        raise FormatError("Native AOT module/timer probe failed")
    version = re.search(r"EOS_GetVersion native result: ([^\n]+)", run.stdout)
    if not version:
        raise FormatError("Native version result missing")
    bootstrap = logged([*arguments, "--sdk-init"], core / "sdk-init.log")
    completed_exports = re.findall(r"SDK bootstrap completed (\S+)", bootstrap.stderr)
    required_port = re.search(r"required named port: ([A-Za-z0-9:_]+);", bootstrap.stderr)
    required_service = re.search(r"required service: (\S+); backend is unimplemented", bootstrap.stderr)
    unsupported_svc = re.search(r"unsupported SVC (0x[0-9a-f]+) at ([^;\s]+);", bootstrap.stderr)
    granularity = re.search(r"native page granularity cannot represent requested alias: host=(\d+) size=(\d+) destination-offset=(\d+) source-offset=(\d+)", bootstrap.stderr)
    geometry = core / "page-geometry"
    geometry_build = logged(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                             REPO / "hades/tests/page_geometry_probe.c", "-o", geometry],
                            core / "page-geometry-build.log")
    if geometry_build.returncode:
        raise FormatError("Page geometry probe compilation failed")
    geometry_run = logged([geometry], core / "page-geometry.log")
    geometry_result = re.search(r"PAGE_GEOMETRY host=(\d+) guest=(\d+) remap_result=(\d+) exact_unaligned_alias=(\d+) adjacent_guest_page_denied=(\d+)", geometry_run.stdout)
    if geometry_run.returncode or not geometry_result:
        raise FormatError("Native page geometry/alias precision probe failed")
    if bootstrap.returncode == 3 and "unsupported SVC " in bootstrap.stderr:
        bootstrap_status = ("blocked-at-native-subpage-alias-incompatibility" if granularity
                            else "blocked-at-explicitly-unsupported-kernel-service")
    elif not bootstrap.returncode:
        bootstrap_status = "sdk-bootstrap-only"
    else:
        raise FormatError("SDK diagnostic probe crashed or failed unexpectedly; inspect sdk-init.log")
    host_core = output / "host-core"
    generate_probe(root, host_core, use_host_services=True)
    host_arguments = [host_core / "hades-core", *arguments[1:]]
    user_storage = output / "local-user"
    user_storage.mkdir(mode=0o700)
    host_sdk = logged([*host_arguments, "--sdk-host", "512", "--storage", user_storage],
                      host_core / "sdk-host.log")
    host_exports = re.findall(r"SDK bootstrap completed (\S+)", host_sdk.stderr)
    if host_sdk.returncode or len(host_exports) != 6 or "HOST_BOOTSTRAP_RETURN" not in host_sdk.stderr:
        raise FormatError("Explicit host-service SDK bootstrap did not complete")
    host_games = []
    frame_symbols = {}
    for index in range(3):
        log = host_core / f"game-host-{index}.log"
        game = logged([*host_arguments, "--game-host", "512", "--storage", user_storage], log)
        constructors = re.findall(r"resident DT_INIT completed (\S+)", game.stderr)
        service = re.search(r"required service: (\S+); backend is unimplemented", game.stderr)
        if (game.returncode != 3 or constructors != ["main", "subsdk0", "subsdk1", "sdk"]
                or "entering actual Hades nnMain" not in game.stderr or not service
                or "actual persistent local save namespace ensured" not in game.stderr):
            raise FormatError("Game probe did not reach an explicit required-service boundary")
        frames = []
        for module, offset_text in re.findall(r"guest frame \d+ (\w+)\+(0x[0-9a-f]+)", game.stderr):
            if module not in frame_symbols:
                path = root / "data/ExeFS" / module
                image, _ = load_nso(path)
                symbols, _, _ = dynamic_symbols(image, path.read_bytes()[:256])
                frame_symbols[module] = symbols
            offset = int(offset_text, 16)
            enclosing = [symbol["name"] for symbol in frame_symbols[module] if symbol["defined"] and symbol["type"] == 2
                         and symbol["value"] <= offset < symbol["value"] + symbol["size"]]
            frames.append({"module": module, "offset": offset, "symbols": enclosing})
        host_games.append({"exit_code": game.returncode, "resident_initializers": constructors,
                           "game_entry_executed": True, "required_service": service[1],
                           "native_frames": frames, "log_sha256": sha256(log)})
    binding_directory = output / "bindings"
    binding_report = catalog(root, root / "build/shaders-msl", cross, binding_directory)
    render = binding_directory / "render-probe"
    compilation = logged(["xcrun", "clang", "-fobjc-arc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                          "-Wno-deprecated-declarations", "-I", REPO / "dm/gfx/nvn/include",
                          "-framework", "Foundation", "-framework", "Metal",
                          REPO / "hades/runtime/render_probe.m", REPO / "dm/gfx/nvn/nvn_spirv_bind.c",
                          "-o", render], binding_directory / "build.log")
    if compilation.returncode:
        raise FormatError("GPU binding probe compilation failed")
    rendering = logged([render, root / "build/shaders-msl", binding_directory / "bindings.json"],
                        binding_directory / "run.log")
    frames = re.findall(r"FRAME_OK (\d+) colored=(\d+) total=1024", rendering.stdout)
    stages = re.search(r"MSL_COMPILE_OK stages=(\d+)", rendering.stdout)
    if (rendering.returncode or "RENDER_OK:" not in rendering.stdout or len(frames) != 3
            or not stages or int(stages[1]) != binding_report["shader_count"]):
        raise FormatError("Actual shader GPU binding/readback probe failed")
    if any(sha256(path) != source_hashes[str(path.relative_to(REPO))] for path in sources):
        raise FormatError("Runtime probe source changed during verification")
    report = {"status": "macos-native-core-and-offscreen-render-probes-only",
              "native_eos_version": version[1], "native_core_binary_sha256": sha256(executable),
              "native_core_log_sha256": sha256(core / "run.log"),
              "macos_linker_adhoc_signature_verified": True,
              "relocations": json.loads((core / "plan.json").read_text())["relocation_count"],
              "native_timer_threads": 8, "native_timer_reads_per_thread": 100,
              "sdk_bootstrap": {"status": bootstrap_status, "exit_code": bootstrap.returncode,
                                "npdm_thread_policy": json.loads((core / "plan.json").read_text())["npdm_thread_policy"],
                                "completed_exports": completed_exports,
                                "required_named_port": required_port[1] if required_port else None,
                                "required_service": required_service[1] if required_service else None,
                                "unsupported_svc": unsupported_svc[1] if unsupported_svc else None,
                                "unsupported_svc_location": unsupported_svc[2] if unsupported_svc else None,
                                "native_alias_request": {
                                    "host_page": int(granularity[1]), "requested_size": int(granularity[2]),
                                    "destination_offset": int(granularity[3]), "source_offset": int(granularity[4])
                                } if granularity else None,
                                "sm_registration_observed": "sm registered kernel process " in bootstrap.stderr,
                                "bound_services": sorted(set(re.findall(r"service (\S+) bound to host endpoint", bootstrap.stderr))),
                                "domain_conversions_observed": bootstrap.stderr.count("session converted to domain root="),
                                "copy_borrows_observed": bootstrap.stderr.count("D2R-style copy borrow:"),
                                "guest_pthreads_created": re.findall(r"created stopped pthread handle=(0x[0-9a-f]+)", bootstrap.stderr),
                                "guest_pthreads_started": re.findall(r"started handle=(0x[0-9a-f]+) on SDK supplied stack", bootstrap.stderr),
                                "host_gate_stack_bytes": 65536,
                                "memory_policy": {
                                    "source": "NPDM 39-bit bounds with explicit host probe reservation policy",
                                    "alias_bytes": 256 << 20, "stack_bytes": 128 << 20,
                                    "heap_capacity_bytes": 512 << 20,
                                    "subpage_policy": "D2R-style copy-move, logical source revocation; no live alias or physical 4KiB isolation",
                                    "not_measured_switch_memory_budget": True},
                                "log_sha256": sha256(core / "sdk-init.log")},
              "core_tests_log_sha256": sha256(core / "core-tests.log"),
              "host_services": {"status": "native-nnMain-entered-save-mounted-native-gpu-service-blocked-not-playable",
                                "plan_sha256": sha256(host_core / "plan.json"),
                                "binary_sha256": sha256(host_core / "hades-core"),
                                "sdk_completed_exports": host_exports,
                                "sdk_log_sha256": sha256(host_core / "sdk-host.log"),
                                "game_runs": host_games,
                                "local_user_persistence": "application-local offline ID, not Nintendo authentication",
                                "readonly_romfs_cache": "actual guest-owned ROM/global read windows",
                                "save_transactions": "working/committed/pending snapshots, atomic swap and process-interruption recovery tests"},
              "synthetic_thread_mutex_condition_handoffs": {"iterations_per_run": 64, "runs": 8, "optimizations": ["O0", "O2"]},
              "host_gate_stack_probe": {"guest_stack_bytes": 4096, "host_c_frame_bytes": 24576,
                                        "gp_simd_flags_preserved_and_outputs_modified": True},
              "msl_shader_interfaces": binding_report["shader_count"],
              "page_geometry": {
                  "host_page": int(geometry_result[1]), "guest_page": int(geometry_result[2]),
                  "mach_remap_return": int(geometry_result[3]),
                  "exact_unaligned_alias": bool(int(geometry_result[4])),
                  "adjacent_guest_page_denied": bool(int(geometry_result[5])),
                  "log_sha256": sha256(core / "page-geometry.log")},
              "macos_runtime_compiled_msl_stages": int(stages[1]),
              "render_frames": [{"frame": int(frame), "colored_pixels": int(count)} for frame, count in frames],
              "render_probe_binary_sha256": sha256(render),
              "render_log_sha256": sha256(binding_directory / "run.log"),
              "source_sha256": source_hashes,
              "limitations": ["Decoded ldr:ro core is not yet connected to guest HIPC",
                              "HIPC/CMIF subset and partial lm/apm/appletOE/aoc:u endpoints, not complete Horizon services",
                              "Guest pthread/mutex/condition subset lacks priority inheritance, address arbitration, mixed event/thread waits and complete guest-exclusive validation",
                              "Joined pthread stacks cannot share one borrowed mapping; live/stopped pthreads retain guest regions until shutdown/join",
                              "Foreground-only bootstrap: policies stored, required host guest-thread suspension transitions explicitly rejected",
                              "Lifecycle/GPU notification source APIs tested but not wired to iOS/guest Metal backend",
                              "Explicit subpage copy compatibility depends on no guest access to borrowed source; not live aliases or physical guest-page isolation",
                              "Memory reservations/budget are explicit host policy, not measured Switch limits",
                              "Memory mapping, permissions/attributes and guest scheduling remain incomplete",
                              "Explicit test-client binding map, not observed guest GL/NVN program identity",
                              "Mac OS runtime MSL compilation, not an iPhoneOS metallib",
                              "Timer/TSD and bounded mutex-condition pthread tests, not complete guest exclusive/concurrency correctness"],
              "game_entry_executed": True, "onscreen_present_verified": False, "ios_runtime_verified": False}
    write_json(output / "runtime.json", report)
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--spirv-cross", type=Path, required=True)
    args = parser.parse_args()
    try:
        probe(outside_git(args.work), args.out, args.spirv_cross)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"Hades runtime probe failed: {error}", file=sys.stderr)
        return 2
    print("Native module/GPU probe evidence saved; kernel boot, guest graphics and iOS gameplay remain unverified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
