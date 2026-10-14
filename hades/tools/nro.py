#!/usr/bin/env python3
"""Validate local NRR membership and normalize NRO segments without changing bytes."""
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

from analyze_modules import bounded, dynamic_symbols, load_nro, load_nso, unpack
from inspect_target import FormatError
from prepare import TITLE_ID


def inspect_nrr(data, title_id=int(TITLE_ID, 16)):
    if len(data) < 0x350 or data[:4] != b"NRR0":
        raise FormatError("Truncated or invalid NRR")
    program, size = unpack("<QI", data, 0x330)
    offset, count = unpack("<II", data, 0x340)
    mask, pattern = unpack("<QQ", data, 0x10)
    if (size != len(data) or size % 4096 or program != title_id
            or program & mask != pattern or data[0x33C] != 0
            or offset != 0x350 or not count):
        raise FormatError("NRR program, certification, size or kind mismatch")
    hashes = [bytes(bounded(data, offset + index * 32, 32)) for index in range(count)]
    if hashes != sorted(set(hashes)):
        raise FormatError("NRR hash list must be strictly sorted")
    return {"program_id": f"{program:016x}", "size": size,
            "hashes": [digest.hex() for digest in hashes],
            "certificate_root_authenticated": False}


def der_item(tag, body):
    size = len(body)
    length = bytes([size]) if size < 128 else size.to_bytes((size.bit_length() + 7) // 8, "big")
    if size >= 128:
        length = bytes([0x80 | len(length)]) + length
    return bytes([tag]) + length + body


def verify_nrr_signature(data):
    """Check the NRR signature, not the certification chain's Nintendo trust root."""
    inspect_nrr(data)
    modulus = bounded(data, 0x30, 256).lstrip(b"\0")
    if not modulus:
        raise FormatError("Missing NRR certification public key")
    if modulus[0] & 0x80:
        modulus = b"\0" + modulus
    key = der_item(0x30, der_item(2, modulus) + der_item(2, b"\x01\x00\x01"))
    with tempfile.TemporaryDirectory(prefix="hades-nrr-") as directory:
        root = Path(directory)
        (root / "public.der").write_bytes(key)
        (root / "signature").write_bytes(bounded(data, 0x230, 256))
        conversion = subprocess.run(
            ["openssl", "rsa", "-RSAPublicKey_in", "-inform", "DER",
             "-in", str(root / "public.der"), "-pubout", "-out", str(root / "public.pem")],
            capture_output=True)
        if conversion.returncode:
            raise FormatError("Cannot decode NRR certification key with local OpenSSL")
        verification = subprocess.run(
            ["openssl", "dgst", "-sha256", "-verify", str(root / "public.pem"),
             "-signature", str(root / "signature"), "-sigopt", "rsa_padding_mode:pss",
             "-sigopt", "rsa_pss_saltlen:32"], input=data[0x330:], capture_output=True)
        if verification.returncode:
            raise FormatError("NRR payload signature verification failed")
    return True


def authorize_nro(nro_path, nrr_path):
    data = nrr_path.read_bytes()
    report = inspect_nrr(data)
    report["payload_signature_verified"] = verify_nrr_signature(data)
    digest = hashlib.sha256(nro_path.read_bytes()).hexdigest()
    if digest not in report["hashes"]:
        raise FormatError("Original NRO hash is not authorized by the local NRR")
    report.update(nrr_sha256=hashlib.sha256(data).hexdigest(), nro_sha256=digest)
    return report


def normalize_nro(source, destination):
    image, inventory = load_nro(source)
    raw = source.read_bytes()
    symbols, relocations, needed = dynamic_symbols(image, raw[:256])
    header = bytearray(256)
    header[:4] = b"NSO0"
    struct.pack_into("<I", header, 12, 0x38)
    struct.pack_into("<I", header, 0x3C, inventory["bss_size"])
    header[0x40:0x60] = raw[0x40:0x60]
    header[0x88:0xA0] = raw[0x68:0x80]
    segments = []
    offset = 256
    for index, segment in enumerate(inventory["segments"]):
        content = bounded(image, segment["memory_offset"], segment["size"])
        struct.pack_into("<III", header, 0x10 + index * 16,
                         offset, segment["memory_offset"], len(content))
        struct.pack_into("<I", header, 0x60 + index * 4, len(content))
        header[0xA0 + 32 * index:0xC0 + 32 * index] = hashlib.sha256(content).digest()
        segments.append(content)
        offset += len(content)
    with destination.open("xb") as output:
        output.write(header)
        for content in segments:
            output.write(content)
    reconstructed, _ = load_nso(destination)
    if reconstructed != image:
        raise FormatError("NRO normalization changed its loaded bytes")
    if dynamic_symbols(reconstructed, header) != (symbols, relocations, needed):
        raise FormatError("NRO normalization changed its dynamic metadata")
    return inventory
