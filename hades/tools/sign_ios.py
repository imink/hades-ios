#!/usr/bin/env python3
"""Re-sign a verified private Hades IPA using explicitly supplied local Apple credentials."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import stat
import subprocess
import sys
import zipfile

from inspect_target import FormatError
from package_ios import archive_bundle, info_plist
from prepare import outside_git, sha256, write_json
from verify_embed import verify_object

MEMORY_ENTITLEMENTS = (
    "com.apple.developer.kernel.increased-memory-limit",
    "com.apple.developer.kernel.extended-virtual-addressing",
)


def signing_entitlements(profile, bundle, device, fingerprint, diagnostic=False, now=None):
    info_plist(bundle)
    if not re.fullmatch(r"[0-9A-Fa-f]{40}", fingerprint):
        raise FormatError("Supply the SHA-1 fingerprint of a valid local Apple signing identity, not ad-hoc signing")
    now = now or datetime.datetime.now(datetime.timezone.utc)
    expiry = profile.get("ExpirationDate")
    if not isinstance(expiry, datetime.datetime):
        raise FormatError("Provisioning profile has no valid expiration")
    if expiry.tzinfo is None:
        expiry = expiry.replace(tzinfo=datetime.timezone.utc)
    if expiry <= now:
        raise FormatError("Provisioning profile has expired")
    platforms = profile.get("Platform")
    if not isinstance(platforms, list) or "iOS" not in platforms:
        raise FormatError("Provisioning profile does not authorize iOS")
    devices = profile.get("ProvisionedDevices")
    if not device or not isinstance(devices, list) or device not in devices:
        raise FormatError("Target device is not explicitly authorized by this local development/ad-hoc profile")
    teams, prefixes = profile.get("TeamIdentifier"), profile.get("ApplicationIdentifierPrefix")
    if (not isinstance(teams, list) or len(teams) != 1
            or not isinstance(prefixes, list) or len(prefixes) != 1
            or not all(isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9]+", value)
                       for value in (teams[0], prefixes[0]))):
        raise FormatError("Ambiguous or malformed provisioning team/application prefix")
    certificates = profile.get("DeveloperCertificates")
    if (not isinstance(certificates, list) or not certificates
            or not all(isinstance(value, bytes) and value for value in certificates)
            or fingerprint.upper() not in {hashlib.sha1(value).hexdigest().upper() for value in certificates}):
        raise FormatError("Selected local signing identity is not a certificate authorized by the profile")
    grants = profile.get("Entitlements")
    if not isinstance(grants, dict) or grants.get("com.apple.developer.team-identifier") != teams[0]:
        raise FormatError("Profile entitlement team does not match its team identifier")
    identifier = prefixes[0] + "." + bundle
    allowed = grants.get("application-identifier")
    if not isinstance(allowed, str) or not (
            allowed == identifier or
            (allowed.endswith(".*") and "*" not in allowed[:-1] and identifier.startswith(allowed[:-1]))):
        raise FormatError("Requested bundle identifier is not authorized by the provisioning profile")
    debugging = grants.get("get-task-allow", False)
    if not isinstance(debugging, bool):
        raise FormatError("Malformed profile debug entitlement")
    entitlements = {"application-identifier": identifier,
                    "com.apple.developer.team-identifier": teams[0], "get-task-allow": debugging}
    omitted = []
    for key in MEMORY_ENTITLEMENTS:
        value = grants.get(key, False)
        if not isinstance(value, bool):
            raise FormatError(f"Malformed memory authorization: {key}")
        if value:
            entitlements[key] = True
        elif diagnostic:
            omitted.append(key)
        else:
            raise FormatError(f"Profile does not authorize {key}; no silent entitlement stripping")
    return entitlements, omitted


def verify_archive(archive, expected):
    if not isinstance(expected, dict) or not expected:
        raise FormatError("Missing verified package archive inventory")
    entries = archive.infolist()
    if len(entries) != len(expected) or {item.filename for item in entries} != set(expected):
        raise FormatError("IPA inventory differs from the verified package receipt")
    if sum(item.file_size for item in entries) >= 4_000_000_000:
        raise FormatError("IPA exceeds the verified app size limit")
    for item in entries:
        path = PurePosixPath(item.filename)
        if (path.parts[:2] != ("Payload", "HadesPortTest.app") or len(path.parts) < 3
                or any(part in ("", ".", "..") for part in item.filename.split("/"))
                or "\\" in item.filename or item.is_dir() or item.flag_bits & 1
                or stat.S_IFMT(item.external_attr >> 16) not in (0, stat.S_IFREG)):
            raise FormatError("Unsafe, encrypted or non-regular private IPA member")
        with archive.open(item) as stream:
            digest = hashlib.sha256()
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != expected[item.filename]:
            raise FormatError("Private IPA bytes differ from the verified package receipt")


def local_command(arguments, log):
    result = subprocess.run(list(map(str, arguments)), capture_output=True, timeout=300)
    log.write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise FormatError(f"Local Apple signing command failed; private diagnostic: {log}")
    return result.stdout


def sign(package, work, output, profile_path, fingerprint, device, bundle, diagnostic=False):
    package, work, output = outside_git(package), outside_git(work), outside_git(output)
    profile_path = profile_path.expanduser().absolute()
    if profile_path.is_symlink() or not profile_path.is_file():
        raise FormatError("Supply an existing regular local provisioning profile")
    if not re.fullmatch(r"[0-9A-Fa-f]{40}", fingerprint):
        raise FormatError("Supply a local Apple signing identity fingerprint")
    identities = subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"],
                                capture_output=True, text=True, timeout=30)
    if identities.returncode:
        raise FormatError("Cannot inspect valid local signing identities")
    valid = re.findall(r'^\s*\d+\)\s+([0-9A-F]{40})\s+"', identities.stdout, re.MULTILINE)
    if fingerprint.upper() not in valid:
        raise FormatError("Selected valid Apple identity is unavailable locally; no game files copied or signed")
    if output.exists():
        raise FormatError("Signing output must be a new private directory")
    receipt = json.loads((package / "package.json").read_text())
    if receipt.get("local_adhoc_signature_verified") is not True:
        raise FormatError("Input is not a verified Hades diagnostic package")
    profile_bytes = profile_path.read_bytes()
    ipa = package / "HadesPortTest-resign-required.ipa"
    source_digest = sha256(ipa)
    if source_digest != receipt.get("ipa_sha256"):
        raise FormatError("Input IPA hash differs from the verified package receipt")
    output.mkdir(parents=True, mode=0o700)
    embedded = output / "profile.mobileprovision"
    embedded.write_bytes(profile_bytes)
    decoded = local_command(["security", "cms", "-D", "-i", embedded], output / "profile-decode.log")
    entitlements, omitted = signing_entitlements(plistlib.loads(decoded), bundle, device, fingerprint, diagnostic)
    with zipfile.ZipFile(ipa) as archive:
        verify_archive(archive, receipt["ipa_entries_sha256"])
        for item in archive.infolist():
            destination = output.joinpath(*PurePosixPath(item.filename).parts)
            destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            with archive.open(item) as source, destination.open("xb") as target:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    target.write(chunk)
            destination.chmod(0o700 if item.filename.endswith("/HadesPortTest") else 0o600)
    app = output / "Payload/HadesPortTest.app"
    metadata = plistlib.loads((app / "Info.plist").read_bytes())
    if metadata.get("CFBundleExecutable") != "HadesPortTest":
        raise FormatError("Unexpected diagnostic app executable")
    metadata["CFBundleIdentifier"] = bundle
    (app / "Info.plist").write_bytes(plistlib.dumps(metadata))
    (app / "embedded.mobileprovision").write_bytes(profile_bytes)
    entitlement_path = output / "authorized-entitlements.plist"
    entitlement_path.write_bytes(plistlib.dumps(entitlements))
    local_command(["codesign", "--force", "--sign", fingerprint, "--entitlements", entitlement_path,
                   "--generate-entitlement-der", "--timestamp=none", app], output / "sign.log")
    local_command(["codesign", "--verify", "--strict", app], output / "signature-verify.log")
    signed = plistlib.loads(local_command(["codesign", "--display", "--entitlements", ":-",
                                         app], output / "signed-entitlements.log"))
    if signed != entitlements:
        raise FormatError("Actual signed entitlements differ from the authorized request")
    images = work / "build/abi"
    preset = receipt.get("render_resolution", {}).get("preset", "720p")
    if preset not in ("720p", "1080p", "wide-720p"):
        raise FormatError("Unknown embedded render resolution")
    if preset != "720p":
        images = package / "generated/display-images"
        expected = receipt.get("embedded_images_sha256", {})
        if set(expected) != {"main", "subsdk0", "subsdk1", "sdk", "eos"}:
            raise FormatError("Experimental package requires complete embedded image hashes")
        if any(sha256(images / (name + ".patched.bin")) != digest for name, digest in expected.items()):
            raise FormatError("Experimental image hash differs from package receipt")
    verification = verify_object(app / "HadesPortTest", images,
                                 ["main", "subsdk0", "subsdk1", "sdk", "eos"], linked=True)
    result_ipa = output / "HadesPortTest-local-apple-signed.ipa"
    entries = archive_bundle(app, result_ipa)
    if sha256(ipa) != source_digest or profile_path.read_bytes() != profile_bytes:
        raise FormatError("Source IPA or local provisioning profile changed during signing")
    report = {"status": "local-apple-signed-device-runtime-unverified",
              "source_ipa_sha256": source_digest, "ipa_sha256": sha256(result_ipa),
              "profile_sha256": hashlib.sha256(profile_bytes).hexdigest(),
              "source_sha256": sha256(Path(__file__)),
              "omitted_memory_entitlements": omitted,
              "memory_policy": "explicit-reduced-entitlement-diagnostic-only" if omitted else "profile-authorized",
              "linked_sections": verification, "ipa_entries_sha256": entries,
              "device_installation_verified": False, "device_runtime_verified": False,
              "limitations": ["Local code-signing verification is not physical-device installation or gameplay evidence",
                              "Apple provisioning trust/revocation, remap/TSD, A17 Pro GPU and memory limits require device validation",
                              "Do not upload this private IPA or profile; no game/assets/Apple credentials transmitted"]}
    write_json(output / "signing.json", report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True, help="Verified private target workspace containing build/abi images")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--identity", required=True, help="Local valid codesigning certificate SHA-1 fingerprint")
    parser.add_argument("--device", required=True, help="Target device provisioning UDID, not CoreDevice discovery UUID")
    parser.add_argument("--bundle-id", required=True)
    parser.add_argument("--diagnostic-without-memory-entitlements", action="store_true",
                        help="Explicitly permit a reduced-memory-authorization diagnostic; does not establish gameplay")
    args = parser.parse_args()
    previous = os.umask(0o077)
    try:
        report = sign(args.package, args.work, args.out, args.profile, args.identity, args.device,
                      args.bundle_id, args.diagnostic_without_memory_entitlements)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        print(f"Local Hades signing stopped: {error}", file=sys.stderr)
        return 2
    finally:
        os.umask(previous)
    if report["omitted_memory_entitlements"]:
        print("Local reduced-entitlement diagnostic IPA signed; memory authorization omitted explicitly, gameplay unverified")
    else:
        print("Local Apple-signed IPA created; installation and physical-device gameplay remain unverified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
