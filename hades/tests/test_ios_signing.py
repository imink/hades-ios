import copy
import datetime
import hashlib
import io
import json
from pathlib import Path
import sys
import plistlib
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from inspect_target import FormatError
from sign_ios import MEMORY_ENTITLEMENTS, sign, signing_entitlements, verify_archive


class IOSSigningTests(unittest.TestCase):
    def setUp(self):
        self.certificate = b"synthetic-test-certificate-not-an-Apple-credential"
        self.identity = hashlib.sha1(self.certificate).hexdigest().upper()
        self.now = datetime.datetime(2026, 10, 13, tzinfo=datetime.timezone.utc)
        self.profile = {
            "ExpirationDate": self.now + datetime.timedelta(days=7), "Platform": ["iOS"],
            "ProvisionedDevices": ["synthetic-device"], "TeamIdentifier": ["TESTTEAM01"],
            "ApplicationIdentifierPrefix": ["TESTPREFIX"],
            "DeveloperCertificates": [self.certificate],
            "Entitlements": {"application-identifier": "TESTPREFIX.local.hades.*",
                             "com.apple.developer.team-identifier": "TESTTEAM01", "get-task-allow": True,
                             **{key: True for key in MEMORY_ENTITLEMENTS}},
        }

    def authorize(self, **changes):
        arguments = dict(profile=self.profile, bundle="local.hades.test", device="synthetic-device",
                         fingerprint=self.identity, now=self.now)
        arguments.update(changes)
        return signing_entitlements(**arguments)

    def test_authorized_entitlements_and_distinct_app_prefix(self):
        result, omitted = self.authorize()
        self.assertEqual(result["application-identifier"], "TESTPREFIX.local.hades.test")
        self.assertEqual(result["com.apple.developer.team-identifier"], "TESTTEAM01")
        self.assertTrue(result["get-task-allow"])
        self.assertEqual(omitted, [])
        self.assertTrue(all(result[key] is True for key in MEMORY_ENTITLEMENTS))
        exact = copy.deepcopy(self.profile)
        exact["Entitlements"]["application-identifier"] = result["application-identifier"]
        self.assertEqual(self.authorize(profile=exact), (result, []))
        naive = copy.deepcopy(self.profile)
        naive["ExpirationDate"] = naive["ExpirationDate"].replace(tzinfo=None)
        self.assertEqual(self.authorize(profile=naive), (result, []))

    def test_no_silent_memory_permission_stripping(self):
        for key in MEMORY_ENTITLEMENTS:
            reduced = copy.deepcopy(self.profile)
            del reduced["Entitlements"][key]
            with self.subTest(key=key), self.assertRaisesRegex(FormatError, "no silent"):
                self.authorize(profile=reduced)
            result, omitted = self.authorize(profile=reduced, diagnostic=True)
            self.assertEqual(omitted, [key])
            self.assertNotIn(key, result)

    def test_identity_device_bundle_and_profile_failures(self):
        for changes in ({"fingerprint": "-"}, {"fingerprint": "F" * 40},
                        {"device": "other-device"}, {"bundle": "other.hades.test"},
                        {"bundle": "x..invalid"}):
            with self.subTest(changes=changes), self.assertRaises(FormatError):
                self.authorize(**changes)
        for field, value in (("ExpirationDate", self.now), ("ExpirationDate", "tomorrow"),
                             ("Platform", ["macOS"]), ("Platform", "iOS"),
                             ("TeamIdentifier", []), ("ApplicationIdentifierPrefix", ["A", "B"]),
                             ("DeveloperCertificates", ["not-DER"]), ("ProvisionedDevices", [])):
            malformed = copy.deepcopy(self.profile)
            malformed[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(FormatError):
                self.authorize(profile=malformed)
        for field, value in (("application-identifier", "*PREFIX.local.hades.*"),
                             ("com.apple.developer.team-identifier", "OTHERTEAM"),
                             ("get-task-allow", "true"), (MEMORY_ENTITLEMENTS[0], 1)):
            malformed = copy.deepcopy(self.profile)
            malformed["Entitlements"][field] = value
            with self.subTest(field=field), self.assertRaises(FormatError):
                self.authorize(profile=malformed, diagnostic=True)

    def archive(self, names):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            for name in names:
                archive.writestr(name, b"synthetic")
        buffer.seek(0)
        return zipfile.ZipFile(buffer)

    def test_private_archive_inventory_and_bytes(self):
        name = "Payload/HadesPortTest.app/Info.plist"
        expected = {name: hashlib.sha256(b"synthetic").hexdigest()}
        with self.archive([name]) as archive:
            verify_archive(archive, expected)
            with self.assertRaisesRegex(FormatError, "bytes differ"):
                verify_archive(archive, {name: "0" * 64})
            with self.assertRaisesRegex(FormatError, "inventory differs"):
                verify_archive(archive, {"another": expected[name]})
        for name in ("../escape", "Payload/HadesPortTest.app/../escape",
                     "Payload/HadesPortTest.app//escape", "Payload/Other.app/file",
                     "Payload/HadesPortTest.app/sub\\escape"):
            with self.subTest(name=name), self.archive([name]) as archive:
                with self.assertRaisesRegex(FormatError, "Unsafe"):
                    verify_archive(archive, {name: hashlib.sha256(b"synthetic").hexdigest()})

    def test_missing_identity_does_not_copy_games_or_create_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            profile = root / "synthetic.mobileprovision"
            profile.write_bytes(b"not-a-real-provisioning-profile")
            output = root / "signing-output"
            with patch("sign_ios.subprocess.run", return_value=SimpleNamespace(returncode=0, stdout="", stderr="")):
                with self.assertRaisesRegex(FormatError, "unavailable locally"):
                    sign(root / "missing-package", root / "work", output, profile,
                         self.identity, "synthetic-device", "local.hades.test")
            self.assertFalse(output.exists())
            self.assertEqual(profile.read_bytes(), b"not-a-real-provisioning-profile")

    def test_local_signing_pipeline_and_signed_entitlement_mismatch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            package = root / "package"
            package.mkdir()
            profile_path = root / "synthetic.mobileprovision"
            profile_path.write_bytes(b"synthetic-CMS-fixture")
            ipa = package / "HadesPortTest-resign-required.ipa"
            members = {
                "Payload/HadesPortTest.app/Info.plist": plistlib.dumps({
                    "CFBundleExecutable": "HadesPortTest", "CFBundleIdentifier": "local.hades.original"}),
                "Payload/HadesPortTest.app/HadesPortTest": b"synthetic-Mach-O-fixture",
            }
            with zipfile.ZipFile(ipa, "w") as archive:
                for name, contents in members.items():
                    archive.writestr(name, contents)
            receipt = {"local_adhoc_signature_verified": True,
                       "ipa_sha256": hashlib.sha256(ipa.read_bytes()).hexdigest(),
                       "ipa_entries_sha256": {name: hashlib.sha256(contents).hexdigest()
                                              for name, contents in members.items()}}
            (package / "package.json").write_text(json.dumps(receipt))
            self.profile["ExpirationDate"] = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None) + datetime.timedelta(days=7)
            authorized, _ = signing_entitlements(self.profile, "local.hades.test", "synthetic-device", self.identity)
            displayed = authorized

            def command(arguments, **keywords):
                if arguments[0] == "security" and arguments[1] == "find-identity":
                    self.assertTrue(keywords["text"])
                    return SimpleNamespace(returncode=0, stdout=f'  1) {self.identity} "Synthetic test identity"\n', stderr="")
                if arguments[0] == "security":
                    return SimpleNamespace(returncode=0, stdout=plistlib.dumps(self.profile), stderr=b"")
                self.assertEqual(arguments[0], "codesign")
                stdout = plistlib.dumps(displayed) if "--display" in arguments else b""
                return SimpleNamespace(returncode=0, stdout=stdout, stderr=b"")

            with patch("sign_ios.subprocess.run", side_effect=command), patch("sign_ios.verify_object", return_value={"sections": 15}) as verify:
                output = root / "signed"
                report = sign(package, root / "work", output, profile_path,
                              self.identity, "synthetic-device", "local.hades.test")
                self.assertFalse(report["device_installation_verified"])
                self.assertFalse(report["device_runtime_verified"])
                self.assertEqual(report["omitted_memory_entitlements"], [])
                verify.assert_called_once_with(output / "Payload/HadesPortTest.app/HadesPortTest",
                                               root / "work/build/abi", ["main", "subsdk0", "subsdk1", "sdk", "eos"], linked=True)
                with zipfile.ZipFile(output / "HadesPortTest-local-apple-signed.ipa") as archive:
                    info = plistlib.loads(archive.read("Payload/HadesPortTest.app/Info.plist"))
                    self.assertEqual(info["CFBundleIdentifier"], "local.hades.test")
                    self.assertEqual(archive.read("Payload/HadesPortTest.app/HadesPortTest"), members["Payload/HadesPortTest.app/HadesPortTest"])
                displayed = {}
                failed = root / "mismatched"
                with self.assertRaisesRegex(FormatError, "signed entitlements differ"):
                    sign(package, root / "work", failed, profile_path,
                         self.identity, "synthetic-device", "local.hades.test")
                self.assertFalse((failed / "signing.json").exists())
                self.assertFalse((failed / "HadesPortTest-local-apple-signed.ipa").exists())
            self.assertEqual(hashlib.sha256(ipa.read_bytes()).hexdigest(), receipt["ipa_sha256"])
