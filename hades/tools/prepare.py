#!/usr/bin/env python3
# Hades 的隔离本机准备链：NSTool 解包、元数据验证、依赖清单、ABI 改写与验证。
# Isolated local Hades preparation: NSTool extraction, metadata checks, dependency inventory and verified ABI rewriting.
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

from inspect_target import FormatError, inspect_pfs0

REPO = Path(__file__).resolve().parents[2]
TITLE_ID = "0100535012974000"


def outside_git(path):
    resolved = path.expanduser().resolve()
    if any((parent / ".git").exists() for parent in (resolved, *resolved.parents)):
        raise FormatError("Game files and generated reports must stay outside all git checkouts")
    return resolved


def sha256(path):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_cnmt(data):
    if len(data) < 32:
        raise FormatError("Truncated CNMT header")
    title, version, content_type = struct.unpack_from("<QIB", data)
    extended_size, count, meta_count = struct.unpack_from("<HHH", data, 14)
    start = 32 + extended_size
    if start + count * 56 + meta_count * 16 > len(data) or not count:
        raise FormatError("Truncated CNMT content table")
    contents = []
    names = set()
    for index in range(count):
        record = data[start + index * 56:start + (index + 1) * 56]
        name = record[32:48].hex() + ".nca"
        if name in names:
            raise FormatError("Duplicate CNMT content ID")
        names.add(name)
        contents.append({"file": name, "sha256": record[:32].hex(),
                         "size": int.from_bytes(record[48:54], "little"),
                         "type": record[54], "id_offset": record[55]})
    return {"title_id": f"{title:016x}", "version": version,
            "type": content_type, "contents": contents}


def read_nacp(data):
    if len(data) < 0x3070:
        raise FormatError("Truncated NACP")
    return {"title": data[:512].split(b"\0")[0].decode("utf-8"),
            "publisher": data[512:768].split(b"\0")[0].decode("utf-8"),
            "display_version": data[0x3060:0x3070].split(b"\0")[0].decode("utf-8")}


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def nstool(tool, keys, arguments, warnings):
    result = subprocess.run([str(tool), "-k", str(keys), *map(str, arguments)],
                            capture_output=True, text=True, cwd=keys.parent,
                            env=dict(os.environ, HOME=str(tool.parent), USERPROFILE=str(tool.parent)))
    output = result.stdout + result.stderr
    if "is fake-signed" in output:
        warnings.add("Package ticket is fake-signed; authenticity is not established")
    if 'Failed to load "title.keys"' in output:
        warnings.add("No title.keys loaded; title-key source is the package ticket")
    if result.returncode or "[ERROR]" in output or "[FAIL]" in output:
        raise FormatError("NSTool failed; no key-bearing tool output is echoed or saved")
    return output


def extract(args, root):
    keys = args.keys.expanduser().resolve(strict=True)
    tool = args.nstool.expanduser().resolve(strict=True)
    package = args.package.expanduser().resolve(strict=True)
    if keys.name != "prod.keys" or not keys.is_file() or not os.access(tool, os.X_OK):
        raise FormatError("Require a local prod.keys file and executable NSTool")
    if keys.parent == root or keys.parent in root.parents or root in keys.parents:
        raise FormatError("Output workspace must be separate from the key directory")
    container = inspect_pfs0(package)
    if root.exists():
        raise FormatError("Extraction requires a new workspace; existing files are never overwritten")
    package_hash = sha256(package)
    root.mkdir(parents=True, mode=0o700)
    data = root / "data"
    data.mkdir(mode=0o700)
    warnings = set()
    nstool(tool, keys, ["-t", "nsp", "-x", data / "nca", package], warnings)
    nca_dir = data / "nca"
    meta_candidates = []
    for path in sorted(nca_dir.glob("*.nca")):
        info = nstool(tool, keys, ["-t", "nca", path], warnings)
        if re.search(r"Content Type:\s+Meta\b", info) and re.search(r"ProgID:\s+0x" + TITLE_ID, info):
            meta_candidates.append(path)
    if len(meta_candidates) != 1:
        raise FormatError("Expected exactly one Hades metadata NCA")
    nstool(tool, keys, ["-t", "nca", "-x", "/0", data / "meta", meta_candidates[0]], warnings)
    cnmts = list((data / "meta").glob("*.cnmt"))
    if len(cnmts) != 1:
        raise FormatError("Expected exactly one CNMT")
    metadata = read_cnmt(cnmts[0].read_bytes())
    if metadata["title_id"] != TITLE_ID or metadata["type"] != 0x80 or metadata["version"] != 0:
        raise FormatError("This extraction target supports only the verified Hades base application (version 0)")
    for content in metadata["contents"]:
        path = nca_dir / content["file"]
        if not path.is_file() or path.stat().st_size != content["size"] or sha256(path) != content["sha256"]:
            raise FormatError("CNMT content hash or size mismatch")
    programs = [c for c in metadata["contents"] if c["type"] == 1 and c["id_offset"] == 0]
    controls = [c for c in metadata["contents"] if c["type"] == 3 and c["id_offset"] == 0]
    if len(programs) != 1 or len(controls) != 1:
        raise FormatError("Ambiguous program or control content")
    program = nca_dir / programs[0]["file"]
    info = nstool(tool, keys, ["-t", "nca", program], warnings)
    rights = re.search(r"RightsId:\s+([0-9a-fA-F]{32})", info)
    ticket_arguments = []
    if rights and int(rights[1], 16):
        candidates = [p for p in nca_dir.glob("*.tik") if p.stem.lower() == rights[1].lower()]
        if len(candidates) != 1:
            raise FormatError("Matching package ticket missing; this target does not accept command-line key values")
        ticket_arguments = ["--tik", candidates[0]]
    for partition, name in (("/0", "ExeFS"), ("/1", "RomFS")):
        nstool(tool, keys, ["-t", "nca", *ticket_arguments, "-x", partition, data / name, program], warnings)
    nstool(tool, keys, ["-t", "nca", "-x", "/0", data / "control",
                        nca_dir / controls[0]["file"]], warnings)
    control = read_nacp((data / "control/control.nacp").read_bytes())
    if control["title"] != "Hades":
        raise FormatError("NACP title mismatch")
    npdm = (data / "ExeFS/main.npdm").read_bytes()
    if len(npdm) < 0x80 or npdm[:4] != b"META":
        raise FormatError("Invalid NPDM")
    aci_offset, aci_size = struct.unpack_from("<II", npdm, 0x70)
    if aci_size < 0x20 or aci_offset + aci_size > len(npdm) or npdm[aci_offset:aci_offset + 4] != b"ACI0":
        raise FormatError("Invalid NPDM ACI0")
    npdm_title, = struct.unpack_from("<Q", npdm, aci_offset + 0x10)
    if f"{npdm_title:016x}" != TITLE_ID:
        raise FormatError("NPDM program ID mismatch")
    if sha256(package) != package_hash:
        raise FormatError("Original NSP changed during extraction")
    from analyze_modules import analyze
    dependencies = analyze(data / "ExeFS", data / "RomFS")
    write_json(root / "dependencies.json", dependencies)
    write_json(root / "extraction.json", {"status": "verified-local-extraction",
               "package_sha256": package_hash, "container_size": container["size"],
               "cnmt": metadata, "nacp": control, "npdm_title_id": TITLE_ID,
               "warnings": sorted(warnings), "content_hashes_verified": True,
               "runtime_verified": False})


def rewrite(args, root):
    from analyze_modules import analyze
    rewriter_hash = sha256(REPO / "dm/tools/patch_nso.py")
    extraction = json.loads((root / "extraction.json").read_text())
    if extraction["cnmt"]["title_id"] != TITLE_ID or extraction["cnmt"]["version"] != 0:
        raise FormatError("Missing verified Hades base extraction")
    exefs = root / "data/ExeFS"
    dependencies = analyze(exefs, root / "data/RomFS")
    write_json(root / "dependencies.json", dependencies)
    names = [m["inventory"]["name"] for m in dependencies["modules"] if m["inventory"]["name"] != "rtld"]
    if any(not re.fullmatch(r"main|sdk|subsdk[0-9]+", name) for name in names):
        raise FormatError("Unsupported module name in embedding target")
    biases = {}
    for item in getattr(args, "module_bias", []):
        name, value = item.split(":")
        bias = int(value, 0)
        if name not in names or name in biases or bias not in (0, 4096, 8192, 12288):
            raise FormatError("Invalid or duplicate portable module load bias")
        biases[name] = bias
    build = root / "build"
    build.mkdir(exist_ok=True)
    output = build / "abi"
    if output.exists():
        raise FormatError("Use a fresh ABI output directory; old rewrite products are never accepted")
    environment = dict(os.environ, LLVM_BIN=str(args.llvm_bin),
                       DM_VCACHE=str(build / "verify-cache"), DM_ROOT=str(root))
    results = []
    for name in names:
        commands = [
            ("patch", [REPO / "dm/tools/patch_nso.py", exefs / name, output, name, "--profile", "portable-abi",
                       "--load-bias", str(biases.get(name, 0))]),
            ("struct", [REPO / "dm/verify/structck.py", output, name, exefs / name]),
            ("equiv", [REPO / "dm/verify/equiv.py", output, name, exefs / name,
                       "--jobs", str(args.jobs), "--mutate", "100", "--tsv", build / (name + ".equiv.tsv")])]
        for stage, command in commands:
            result = subprocess.run([sys.executable, *map(str, command)], capture_output=True, text=True, env=environment)
            (build / f"{name}.{stage}.log").write_text(result.stdout + result.stderr)
            if result.returncode:
                raise FormatError(f"{name} {stage} failed; inspect the local build log")
            if stage == "equiv":
                summary = re.search(r"EQUIV " + re.escape(name) + r" OK .*站点=(\d+)", result.stdout)
                total = re.search(r"站点=(\d+)", result.stdout)
                effective = re.search(r"有效≥2站点=(\d+)", result.stdout)
                if not summary or not total or not effective or total[1] != effective[1]:
                    raise FormatError(f"{name}: insufficient effective equivalence trials")
        for extension in ("cuts.tsv", "obs.tsv", "params.tsv"):
            if any(line.strip() and not line.startswith("#") for line in (output / (name + "." + extension)).read_text().splitlines()):
                raise FormatError("Game-specific patch leaked into the ABI-only output")
        results.append({"module": name, "load_bias": biases.get(name, 0), "original_sha256": sha256(exefs / name),
                        "image_sha256": sha256(output / (name + ".patched.bin")),
                        "artifacts": {extension: sha256(output / (name + "." + extension))
                                      for extension in ("fixups.tsv", "gates.tsv", "sites.tsv", "cuts.tsv", "obs.tsv", "params.tsv")},
                        "structural_check": "passed", "equivalence_check": "passed"})
    if sha256(REPO / "dm/tools/patch_nso.py") != rewriter_hash:
        raise FormatError("Rewriter changed during verification; regenerate all modules")
    write_json(build / "rewrite.json", {"status": "verified-abi-only", "modules": results,
               "rewriter_sha256": rewriter_hash,
               "excluded_modules": {"rtld": "requires host loader replacement; not executed or verified",
                   **{m["inventory"]["name"]: "dynamic NRO requires separate rewriting and ldr:ro registration"
                      for m in dependencies["dynamic_modules"]}},
               "limitations": ["Randomized site tests, not full-game equivalence",
                              "Exclusive operations normalized in the existing verifier",
                              "SVC and timer gate semantics require host-runtime verification"],
               "runtime_verified": False})


def rewrite_dynamic(args, root):
    from nro import authorize_nro, normalize_nro
    source = root / "data/RomFS/nro/EOSSDK-Switch-Shipping.nro"
    nrr = root / "data/RomFS/.nrr/game.nrr"
    build = root / "build"
    receipt = json.loads((build / "rewrite.json").read_text())
    rewriter_hash = sha256(REPO / "dm/tools/patch_nso.py")
    if receipt["rewriter_sha256"] != rewriter_hash:
        raise FormatError("Static modules require reverification after rewriter changes")
    registration = authorize_nro(source, nrr)
    normalized = build / "nro-input"
    if normalized.exists() or (build / "abi/eos.patched.bin").exists():
        raise FormatError("Dynamic rewrite requires fresh NRO input and EOS products")
    normalized.mkdir(mode=0o700)
    nso = normalized / "eos"
    inventory = normalize_nro(source, nso)
    environment = dict(os.environ, LLVM_BIN=str(args.llvm_bin),
                       DM_VCACHE=str(build / "verify-cache"), DM_ROOT=str(root))
    commands = [
        ("patch", [REPO / "dm/tools/patch_nso.py", nso, build / "abi", "eos", "--profile", "portable-abi"]),
        ("struct", [REPO / "dm/verify/structck.py", build / "abi", "eos", nso]),
        ("equiv", [REPO / "dm/verify/equiv.py", build / "abi", "eos", nso,
                   "--jobs", str(args.jobs), "--mutate", "100", "--tsv", build / "eos.equiv.tsv"])]
    for stage, command in commands:
        result = subprocess.run([sys.executable, *map(str, command)],
                                capture_output=True, text=True, env=environment)
        (build / f"eos.{stage}.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise FormatError(f"EOS {stage} failed; inspect the local build log")
        if stage == "equiv":
            total = re.search(r"站点=(\d+)", result.stdout)
            effective = re.search(r"有效≥2站点=(\d+)", result.stdout)
            if not re.search(r"EQUIV eos OK ", result.stdout) or not total or not effective or total[1] != effective[1]:
                raise FormatError("EOS has insufficient effective equivalence trials")
    artifacts = {}
    for extension in ("fixups.tsv", "gates.tsv", "sites.tsv", "cuts.tsv", "obs.tsv", "params.tsv"):
        path = build / "abi" / ("eos." + extension)
        if extension in ("cuts.tsv", "obs.tsv", "params.tsv") and any(
                line.strip() and not line.startswith("#") for line in path.read_text().splitlines()):
            raise FormatError("Game-specific patch leaked into EOS ABI output")
        artifacts[extension] = sha256(path)
    if sha256(REPO / "dm/tools/patch_nso.py") != rewriter_hash or sha256(source) != registration["nro_sha256"]:
        raise FormatError("Dynamic rewrite inputs changed during verification")
    write_json(build / "rewrite-dynamic.json", {
        "status": "verified-abi-only", "rewriter_sha256": rewriter_hash, "nrr": registration,
        "modules": [{"module": "eos", "source": str(source.relative_to(root)),
                     "normalized_source": str(nso.relative_to(root)),
                     "original_sha256": registration["nro_sha256"], "normalized_sha256": sha256(nso),
                     "build_id": inventory["build_id"], "artifacts": artifacts,
                     "image_sha256": sha256(build / "abi/eos.patched.bin"),
                     "structural_check": "passed", "equivalence_check": "passed"}],
        "runtime_verified": False})


def verified_modules(root):
    build = root / "build"
    receipts = [json.loads((build / "rewrite.json").read_text())]
    dynamic = build / "rewrite-dynamic.json"
    if dynamic.exists():
        receipts.append(json.loads(dynamic.read_text()))
        from nro import authorize_nro
        registration = authorize_nro(root / "data/RomFS/nro/EOSSDK-Switch-Shipping.nro",
                                     root / "data/RomFS/.nrr/game.nrr")
        if registration != receipts[-1]["nrr"]:
            raise FormatError("NRR registration changed since dynamic verification")
    modules = []
    for receipt in receipts:
        if receipt["status"] != "verified-abi-only":
            raise FormatError("Embedding requires a verified ABI receipt")
        if sha256(REPO / "dm/tools/patch_nso.py") != receipt["rewriter_sha256"]:
            raise FormatError("Rewriter changed since verification")
        modules.extend(receipt["modules"])
    names = set()
    for module in modules:
        name = module["module"]
        if not re.fullmatch(r"main|sdk|subsdk[0-9]+|eos", name) or name in names:
            raise FormatError("Invalid embedded module name")
        names.add(name)
        if sha256(build / "abi" / (name + ".patched.bin")) != module["image_sha256"]:
            raise FormatError("Rewritten image changed since verification")
        source = root / module.get("source", "data/ExeFS/" + name)
        if sha256(source) != module["original_sha256"]:
            raise FormatError("Original module changed since verification")
        if "normalized_source" in module and sha256(root / module["normalized_source"]) != module["normalized_sha256"]:
            raise FormatError("Normalized NRO changed since verification")
        for extension, digest in module["artifacts"].items():
            if sha256(build / "abi" / (name + "." + extension)) != digest:
                raise FormatError("Rewrite layout or manifest changed since verification")
    return modules


def embed(args, root):
    build = root / "build"
    names = [module["module"] for module in verified_modules(root)]
    assembly = build / "hades_embed.S"
    result = subprocess.run([sys.executable, str(REPO / "dm/ios/tools/gen_embed.py"),
                             str(build / "abi"), str(assembly), *names], capture_output=True, text=True)
    if result.returncode:
        raise FormatError("Embedding assembly generation failed")
    (build / "embed-link-flags.txt").write_text(result.stdout)
    subprocess.run(["xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios15.0",
                    "-c", str(assembly), "-o", str(build / "hades_embed.o")], check=True)
    from verify_embed import verify_object
    verified_object = verify_object(build / "hades_embed.o", build / "abi", names)
    write_json(build / "embed.json", {"status": "ios-arm64-object-only",
               "object_sha256": sha256(build / "hades_embed.o"),
               "verification": verified_object,
               "modules": names, "pending_dynamic_modules": [] if "eos" in names else [m["inventory"]["name"] for m in
                    json.loads((root / "dependencies.json").read_text()).get("dynamic_modules", [])],
               "dynamic_runtime_registration_verified": False,
               "signed": False, "linked_app": False, "runtime_verified": False})


def main(argv=None):
    parser = argparse.ArgumentParser(description="Local Hades preparation; does not build a working app")
    parser.add_argument("--work", required=True, type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    extraction = commands.add_parser("extract")
    extraction.add_argument("--package", required=True, type=Path)
    extraction.add_argument("--keys", required=True, type=Path)
    extraction.add_argument("--nstool", required=True, type=Path)
    rewriting = commands.add_parser("rewrite")
    rewriting.add_argument("--llvm-bin", required=True, type=Path)
    rewriting.add_argument("--jobs", type=int, default=2)
    rewriting.add_argument("--module-bias", action="append", default=[],
                           help="Explicit portable module:4KiB-granule load bias within a 16KiB host page")
    dynamic = commands.add_parser("rewrite-dynamic")
    dynamic.add_argument("--llvm-bin", required=True, type=Path)
    dynamic.add_argument("--jobs", type=int, default=2)
    commands.add_parser("embed")
    args = parser.parse_args(argv)
    try:
        root = outside_git(args.work)
        if args.command == "extract":
            extract(args, root)
        elif args.command in ("rewrite", "rewrite-dynamic"):
            if args.jobs < 1:
                raise FormatError("--jobs must be positive")
            (rewrite if args.command == "rewrite" else rewrite_dynamic)(args, root)
        else:
            embed(args, root)
    except (OSError, ValueError, ImportError, subprocess.CalledProcessError) as error:
        print(f"Hades preparation failed: {error}", file=sys.stderr)
        return 2
    print(f"Hades {args.command} milestone saved locally; game startup remains unverified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
