from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from inspect_target import FormatError
from link_modules import npdm_thread_policy, relocation_plan
from reflect_bindings import msl_bindings

REPO = Path(__file__).resolve().parents[2]


def symbol(name, defined=True, value=24, binding=1, visibility=0, section=1):
    return {"name": name, "defined": defined, "value": value, "binding": binding,
            "visibility": visibility, "section": section, "type": 2}


class CoreRuntimeTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_actual_self_memory_observation(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "diagnostics")
            for optimization in ("-O0", "-O2"):
                subprocess.run(["xcrun", "clang", "-std=c11", optimization, "-Wall", "-Wextra", "-Werror",
                    str(REPO / "hades/tests/diagnostics_probe.c"), "-o", executable], check=True)
                result = subprocess.run([executable], check=True, capture_output=True, text=True, timeout=15)
                self.assertIn("ACTUAL_SELF_MEMORY_ACCOUNTING_OK", result.stdout)
                self.assertIn("overlapping accounts, do not sum", result.stderr)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_ios_foreground_diagnostic_policy(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "ios-policy")
            subprocess.run(["xcrun", "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(REPO / "hades/tests/ios_policy_probe.c"), "-o", executable], check=True)
            result = subprocess.run([executable], check=True, capture_output=True, text=True, timeout=15)
            self.assertIn("IOS_FOREGROUND_DIAGNOSTIC_POLICY_OK", result.stdout)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_trace_policy_concurrent_initialization_and_invalid_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "trace")
            for optimization in ("-O0", "-O2"):
                subprocess.run(["xcrun", "clang", optimization, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(REPO / "hades/tests/trace_probe.c"), "-o", executable], check=True)
                for value, expected in ((None, 0), ("0", 0), ("1", 1), ("yes", 0)):
                    environment = os.environ.copy()
                    environment.pop("HADES_TRACE", None)
                    if value is not None:
                        environment["HADES_TRACE"] = value
                    run = subprocess.run([executable, str(expected)], env=environment,
                                         capture_output=True, text=True, timeout=15)
                    if value == "yes":
                        self.assertEqual(run.returncode, 77, run.stdout + run.stderr)
                        self.assertIn("HADES_TRACE must be 0 or 1", run.stderr)
                        self.assertNotIn("TRACE_POLICY_OK", run.stdout)
                    else:
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                        self.assertIn("TRACE_POLICY_OK", run.stdout)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler/audio output")
    def test_actual_audio_queue_pcm_completion_and_lifecycle(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "audio")
            for optimization in ("-O0", "-O2"):
                subprocess.run(["xcrun", "clang", optimization, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(REPO / "hades/tests/audio_probe.c"), str(REPO / "hades/runtime/audio.c"),
                    "-framework", "AudioToolbox", "-o", executable], check=True)
                subprocess.run([executable], check=True, timeout=15)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_guarded_reentrant_guest_callbacks(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "callout")
            for optimization in ("-O0", "-O2"):
                subprocess.run(["xcrun", "clang", optimization, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(REPO / "hades/tests/callout_probe.c"), str(REPO / "hades/runtime/callout.c"),
                    str(REPO / "hades/runtime/memory.c"), str(REPO / "hades/runtime/host_entry.S"),
                    "-o", executable], check=True)
                subprocess.run([executable], check=True, timeout=30)

    def test_npdm_thread_ranges_and_malformed_capabilities(self):
        npdm = bytearray(196)
        npdm[:4] = b"META"
        npdm[12], npdm[14], npdm[15] = 7, 44, 0
        struct.pack_into("<II", npdm, 0x70, 128, 68)
        npdm[128:132] = b"ACI0"
        struct.pack_into("<II", npdm, 128 + 0x30, 64, 4)
        struct.pack_into("<I", npdm, 192, 7 | (59 << 4) | (28 << 10) | (2 << 24))
        policy = npdm_thread_policy(npdm)
        self.assertEqual(policy, {"priority": 44, "core": 0, "core_mask": 7,
                                 "priority_mask": ((1 << 32) - 1) << 28})
        npdm[14] = 27
        with self.assertRaisesRegex(FormatError, "outside"):
            npdm_thread_policy(npdm)
        npdm[14] = 44
        npdm[12] = 1
        with self.assertRaisesRegex(FormatError, "39-bit"):
            npdm_thread_policy(npdm)
        npdm[12] = 7
        struct.pack_into("<I", npdm, 0x14, 4096)
        with self.assertRaisesRegex(FormatError, "system-resource"):
            npdm_thread_policy(npdm)
        struct.pack_into("<I", npdm, 0x14, 0)
        struct.pack_into("<I", npdm, 192, 0xFFFFFFFF)
        with self.assertRaisesRegex(FormatError, "exactly one"):
            npdm_thread_policy(npdm)
        struct.pack_into("<II", npdm, 128 + 0x30, 64, 8)
        with self.assertRaisesRegex(FormatError, "extent"):
            npdm_thread_policy(npdm)

    def test_relative_external_weak_and_absolute_relocations(self):
        provider = {"name": "provider", "symbols": [symbol("provided")], "relocations": [],
                    "original_size": 64, "writable_start": 8}
        requester = {"name": "requester", "symbols": [symbol("", False),
                     symbol("provided", False), symbol("weak", False, binding=2)],
                     "original_size": 64, "writable_start": 8,
                     "relocations": [{"offset": 8, "type": 1027, "symbol": 0, "addend": 32},
                                     {"offset": 16, "type": 257, "symbol": 1, "addend": 4},
                                     {"offset": 24, "type": 1026, "symbol": 2, "addend": 0}]}
        records, _ = relocation_plan([provider, requester])
        self.assertEqual(records, [(1, 8, 1, 32), (1, 16, 0, 28), (1, 24, -1, 0)])
        provider["symbols"][0]["section"] = 0xFFF1
        self.assertEqual(relocation_plan([provider, requester])[0][1], (1, 16, -1, 28))

    def test_relocation_failures_are_not_import_stubs(self):
        module = {"name": "fixture", "symbols": [symbol("missing", False)],
                  "original_size": 64, "writable_start": 8,
                  "relocations": [{"offset": 8, "type": 1026, "symbol": 0, "addend": 0}]}
        with self.assertRaisesRegex(FormatError, "Unresolved strong"):
            relocation_plan([module])
        module["relocations"][0]["type"] = 999
        with self.assertRaisesRegex(FormatError, "unsupported relocation"):
            relocation_plan([module])
        module["relocations"][0].update(type=1027, offset=0)
        with self.assertRaisesRegex(FormatError, "non-writable"):
            relocation_plan([module])
        module["relocations"][0].update(offset=8, addend=65)
        with self.assertRaisesRegex(FormatError, "outside original"):
            relocation_plan([module])
        module["relocations"][0]["addend"] = 0
        module["relocations"].append(dict(module["relocations"][0]))
        with self.assertRaisesRegex(FormatError, "duplicate"):
            relocation_plan([module])

    def test_descriptor_identity_is_not_nvn_slot(self):
        reflection = {"types": {"_1": {"name": "Uniform"}},
                      "ubos": [{"type": "_1", "name": "Uniform", "set": 1, "binding": 49, "block_size": 32}],
                      "separate_images": [{"name": "Texture", "set": 2, "binding": 7}]}
        msl = ("fragment out main0(constant Uniform& u [[buffer(0)]], "
               "texture2d<float> Texture [[texture(1)]])\n")
        result = msl_bindings(reflection, msl)
        self.assertEqual(result["resources"][0]["binding"], 49)
        self.assertEqual(result["resources"][0]["msl_index"], 0)
        self.assertIsNone(result["resources"][0]["nvn_slot"])
        unused = msl_bindings(reflection, "fragment out main0()\n")
        self.assertTrue(all(r["interface_status"] == "not_emitted_by_msl" for r in unused["resources"]))
        with self.assertRaisesRegex(FormatError, "not accounted"):
            msl_bindings(reflection, msl + "texture2d<float> Unlisted [[texture(2)]]")

    def test_explicit_host_bindings_force_functions_not_private_data(self):
        module = {"name": "sdk", "symbols": [symbol("owned")], "original_size": 64, "writable_start": 8,
                  "relocations": [{"offset": 8, "type": 1026, "symbol": 0, "addend": 0}]}
        self.assertEqual(relocation_plan([module])[0], [(0, 8, 0, 24)])
        self.assertEqual(relocation_plan([module], {"owned": 3})[0], [(0, 8, -5, 0)])
        for alteration in ({"type": 1}, {"binding": 0}, {"visibility": 2}):
            old = module["symbols"][0].copy()
            module["symbols"][0].update(alteration)
            with self.assertRaisesRegex(FormatError, "exported function"):
                relocation_plan([module], {"owned": 3})
            module["symbols"][0] = old
        module["relocations"][0]["addend"] = 4
        with self.assertRaisesRegex(FormatError, "zero-addend"):
            relocation_plan([module], {"owned": 3})
        module["relocations"][0]["addend"] = 0
        with self.assertRaisesRegex(FormatError, "no actual"):
            relocation_plan([module], {"unused": 3})
        for hosts in ({"owned": -1}, {"owned": 256}, {"owned": True}, {"owned": 3, "duplicate": 3}):
            with self.assertRaisesRegex(FormatError, "indices"):
                relocation_plan([module], hosts)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_host_files_clock_cache_and_durable_local_user(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            romfs = root / "romfs"
            romfs.mkdir()
            (romfs / "Hello.txt").write_bytes(b"hello world")
            (romfs / "link").symlink_to(romfs / "Hello.txt")
            ordered = romfs / "Ordered"
            ordered.mkdir()
            for name in ("z.shader", "a.shader", "B.shader", "10.shader"):
                (ordered / name).write_bytes(b"abc")
            (ordered / "folder").mkdir()
            executable = str(root / "host")
            for optimization in ("-O0", "-O2"):
                subprocess.run(["xcrun", "clang", optimization, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                str(REPO / "hades/tests/host_probe.c"),
                                str(REPO / "hades/runtime/host.c"),
                                str(REPO / "hades/runtime/save.c"),
                                str(REPO / "hades/runtime/memory.c"), "-o", executable], check=True)
                for probe in ("0", "1"):
                    storage = root / ("storage" + optimization + probe)
                    storage.mkdir()
                    run = subprocess.run([executable, str(romfs), str(storage)], check=True, timeout=15,
                                         capture_output=True, text=True,
                                         env=dict(os.environ, HADES_FS_DIRECTORY_ORDER_PROBE=probe))
                    self.assertEqual("DIRECTORY_ORDER_PROBE read-only" in run.stderr, probe == "1")
            invalid_order = subprocess.run([executable, str(romfs), str(storage)], capture_output=True, text=True,
                                          env=dict(os.environ, HADES_FS_DIRECTORY_ORDER_PROBE="invalid"))
            self.assertNotEqual(invalid_order.returncode, 0)
            self.assertIn("Invalid HADES_FS_DIRECTORY_ORDER_PROBE", invalid_order.stderr)
            uid = (storage / "local-user.bin").read_bytes()
            self.assertEqual(len(uid), 16)
            self.assertTrue(any(uid))
            self.assertTrue((storage / "saves" / uid.hex()).is_dir())
            (storage / "local-user.bin").write_bytes(b"invalid")
            invalid = subprocess.run([executable, str(romfs), str(storage)], capture_output=True, text=True,
                                     env=dict(os.environ, HADES_FS_DIRECTORY_ORDER_PROBE="0"))
            self.assertNotEqual(invalid.returncode, 0)
            self.assertIn("Cannot create/read durable", invalid.stderr)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_real_save_snapshots_and_abrupt_process_recovery(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for optimization in ("-O0", "-O2"):
                namespace = root / optimization
                namespace.mkdir()
                executable = str(root / "save-test")
                subprocess.run(["xcrun", "clang", optimization, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                str(REPO / "hades/tests/save_probe.c"), str(REPO / "hades/runtime/save.c"),
                                "-o", executable], check=True)
                subprocess.run([executable, str(namespace)], check=True, timeout=30)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_real_transfer_alias_permissions_and_handle_lifetime(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "transfer")
            for optimization in ("-O0", "-O2"):
                subprocess.run(["xcrun", "clang", optimization, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                str(REPO / "hades/tests/transfer_probe.c"),
                                str(REPO / "hades/runtime/ipc.c"),
                                str(REPO / "hades/runtime/audio.c"), "-framework", "AudioToolbox",
                                str(REPO / "hades/runtime/memory.c"), "-o", executable], check=True)
                subprocess.run([executable], check=True, timeout=30)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_static_symbol_host_gate_stack_and_registers(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "host-gate")
            for optimization in ("-O0", "-O2"):
                subprocess.run(["xcrun", "clang", optimization, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                "-DHADES_SYMBOL_GATE_TEST", str(REPO / "hades/tests/gate_stack_probe.c"),
                                str(REPO / "hades/tests/gate_stack_probe.S"),
                                str(REPO / "hades/runtime/host_entry.S"), "-o", executable], check=True)
                subprocess.run([executable], check=True, timeout=15)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_actual_nvn_binding_payloads_and_negative_cases(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "bindings")
            subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(REPO / "dm/gfx/nvn/include"),
                            str(REPO / "hades/tests/binding_probe.c"),
                            str(REPO / "dm/gfx/nvn/nvn_spirv_bind.c"), "-o", executable], check=True)
            subprocess.run([executable], check=True)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_memory_queries_heap_permissions_and_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "memory")
            subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(REPO / "hades/tests/memory_probe.c"),
                            str(REPO / "hades/runtime/memory.c"), "-o", executable], check=True)
            subprocess.run([executable], check=True)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_ipc_wire_sessions_domains_and_kernel_events(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "ipc")
            for optimization in ("-O0", "-O2"):
                subprocess.run(["xcrun", "clang", optimization, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                str(REPO / "hades/tests/ipc_probe.c"),
                                str(REPO / "hades/runtime/ipc.c"),
                                str(REPO / "hades/runtime/audio.c"), "-framework", "AudioToolbox",
                                str(REPO / "hades/runtime/memory.c"), "-o", executable], check=True)
                subprocess.run([executable], check=True, timeout=15)
                subprocess.run([executable, "--frontend"], check=True, timeout=15)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_native_page_geometry_and_alias_precision(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "geometry")
            subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(REPO / "hades/tests/page_geometry_probe.c"), "-o", executable], check=True)
            subprocess.run([executable], check=True, timeout=15)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_guest_pthread_stack_and_mutex_condition_handoffs(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "threads")
            for optimization in ("-O0", "-O2"):
                subprocess.run(["xcrun", "clang", optimization, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                str(REPO / "hades/tests/thread_probe.c"),
                                str(REPO / "hades/runtime/threads.c"),
                                str(REPO / "hades/runtime/memory.c"),
                                str(REPO / "dm/host/entry.S"), "-o", executable], check=True)
                for _ in range(4):
                    subprocess.run([executable], check=True, timeout=15)

    @unittest.skipUnless(shutil.which("xcrun"), "Requires existing Apple C compiler")
    def test_host_gate_stack_and_register_preservation(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = str(Path(temporary) / "gate")
            for optimization in ("-O0", "-O2"):
                subprocess.run(["xcrun", "clang", optimization, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                "-DHADES_HOST_GATE_STACK",
                                str(REPO / "hades/tests/gate_stack_probe.c"),
                                str(REPO / "hades/tests/gate_stack_probe.S"),
                                str(REPO / "dm/host/entry.S"), "-o", executable], check=True)
                subprocess.run([executable], check=True, timeout=15)
            subprocess.run(["xcrun", "clang", "-c", str(REPO / "dm/host/entry.S"),
                            "-o", str(Path(temporary) / "d2r-default.o")], check=True)
