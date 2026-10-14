"""Generate typed AOT Vulkan forwarding from pinned public headers, never game addresses."""
import hashlib
import json
from pathlib import Path
import re
import tarfile

from inspect_target import FormatError
from prepare import REPO, sha256


def framework_receipt(root, platform):
    pin = json.loads((REPO / "hades/toolchain.json").read_text())["moltenvk"]
    archive = root / pin[platform + "_asset"]
    if sha256(archive) != pin[platform + "_sha256"]:
        raise FormatError("MoltenVK archive differs from pinned official release")
    base = root / platform / "MoltenVK/MoltenVK"
    identifier = "ios-arm64" if platform == "ios" else "macos-arm64_x86_64"
    library = base / "static/MoltenVK.xcframework" / identifier / "libMoltenVK.a"
    header = base / "include/vulkan/vulkan_core.h"
    if library.is_symlink() or not library.is_file() or not header.is_file():
        raise FormatError("Pinned public static MoltenVK slice or Vulkan header missing")
    targets = [library, root / platform / "MoltenVK/LICENSE"]
    targets.extend(path for path in (base / "include").rglob("*") if not path.is_dir())
    hashes = {}
    with tarfile.open(archive, "r:") as package:
        members = {member.name: member for member in package.getmembers()}
        for path in targets:
            relative = path.relative_to(root / platform).as_posix()
            member = members.get(relative)
            if (not member or not member.isfile() or not path.is_file()
                    or any(parent.is_symlink() for parent in (path, *path.parents) if parent != root)):
                raise FormatError("Unsafe/missing pinned framework member: " + relative)
            digest = hashlib.sha256()
            with package.extractfile(member) as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            expected = digest.hexdigest()
            if sha256(path) != expected:
                raise FormatError("Extracted framework member differs from official archive: " + relative)
            hashes[relative] = expected
    return {"platform": platform, "version": pin["tag"], "archive_sha256": sha256(archive),
            "members_sha256": hashes}


def framework(root, platform):
    framework_receipt(root, platform)
    base = root / platform / "MoltenVK/MoltenVK"
    identifier = "ios-arm64" if platform == "ios" else "macos-arm64_x86_64"
    return base / "include", base / "static/MoltenVK.xcframework" / identifier / "libMoltenVK.a"


def generate(output, include):
    lines = (REPO / "hades/runtime/vulkan_bindings.def").read_text().splitlines()
    lines += (REPO / "hades/runtime/vulkan_optional.def").read_text().splitlines()
    header = (include / "vulkan/vulkan_core.h").read_text()
    entries = []
    for line in lines:
        match = re.fullmatch(r'HADES_BINDING\((VULKAN_[A-Z0-9_]+), "(vk\w+)"\)', line)
        if not match:
            continue
        operation, name = match.groups()
        if name in ("vkCreateViSurfaceNN",):
            continue
        prototype = re.search(r"typedef\s+(\w+)\s+\(VKAPI_PTR\s+\*PFN_" + name + r"\)\(([^;]+)\);", header)
        if not prototype:
            raise FormatError("Missing pinned Vulkan function-pointer prototype: " + name)
        result, parameters = prototype.groups()
        arguments, allocator = [], None
        for index, declaration in enumerate(parameters.split(",")):
            argument = re.fullmatch(r"\s*(.+?)\s+(\w+)(?:\[[^\]]+\])?\s*", declaration)
            if not argument or "float" in argument[1] or "double" in argument[1]:
                raise FormatError("Unsupported typed forwarding declaration: " + declaration)
            kind = argument[1]
            value = f"r[{index}]" if index < 8 else f"((const uint64_t *)(r + 100))[{index - 8}]"
            if "VkAllocationCallbacks" in kind:
                allocator = value
                arguments.append("native_allocator")
            else:
                arguments.append(f"({kind})(uintptr_t)({value})")
        entries.append(f"case HADES_HOST_{operation}: {{")
        if allocator:
            entries.append(f"const VkAllocationCallbacks *native_allocator=adapt_allocator((const void *)(uintptr_t)({allocator}));")
            entries.append(f"if ({allocator} && !native_allocator) return -1;")
        invocation = f"((PFN_{name})function)({', '.join(arguments)})"
        if result == "void":
            entries.append(invocation + ";")
        else:
            conversion = "(uint32_t)" if result == "VkResult" else "(uintptr_t)"
            entries.append(f"r[0] = {conversion}{invocation};")
            if result == "VkResult":
                entries.append('static _Atomic uint32_t previous_result;')
                entries.append('uint32_t previous=atomic_exchange_explicit(&previous_result,(uint32_t)r[0],memory_order_relaxed);')
                entries.append(f'if (r[0] && r[0]!=VK_NOT_READY && r[0]!=VK_TIMEOUT && r[0]!=previous) fprintf(stderr,"[hades-vulkan] {name} result=%d\\n",(int32_t)r[0]);')
        entries.append("return 0; }")
    if len(entries) < 100:
        raise FormatError("Incomplete Vulkan forwarding table")
    output.write_text("\n".join(entries) + "\n")
    return {"header_sha256": hashlib.sha256(header.encode()).hexdigest(), "generated_sha256": sha256(output)}
