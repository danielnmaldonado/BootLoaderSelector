#!/usr/bin/env python3
"""Positive certificate-policy coverage without malformed certificate fixtures."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


VERIFIER_PATH = Path(__file__).resolve().parents[1] / "AuditTools" / "Verify-PeCoff-Authenticode.py"
SPEC = importlib.util.spec_from_file_location("authenticode_positive_verifier", VERIFIER_PATH)
assert SPEC is not None and SPEC.loader is not None
VERIFIER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFIER
SPEC.loader.exec_module(VERIFIER)


class SelfSignedLeafCertificateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.directory = tempfile.TemporaryDirectory(prefix="BootLoaderSelector.CertificatePositive.")
        cls.addClassCleanup(cls.directory.cleanup)
        temporary = Path(cls.directory.name)
        cls.certificate = temporary / "certificate.pem"
        subprocess.run([
            "openssl", "req", "-new", "-x509", "-newkey", "rsa:3072", "-sha256", "-nodes", "-days", "1",
            "-subj", "/CN=BootLoaderSelector Positive Test",
            "-addext", "basicConstraints=critical,CA:FALSE",
            "-addext", "keyUsage=critical,digitalSignature",
            "-addext", "extendedKeyUsage=codeSigning",
            "-keyout", str(temporary / "private.pem"), "-out", str(cls.certificate),
        ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_self_signed_leaf_without_ca_signing_usage(self) -> None:
        VERIFIER.verify_certificate_policy(self.certificate)

    def test_self_signed_leaf_with_supplied_der(self) -> None:
        certificate_der = VERIFIER.openssl_cert_der(self.certificate)
        VERIFIER.verify_certificate_policy(self.certificate, certificate_der)


if __name__ == "__main__":
    unittest.main()
