#!/usr/bin/env python3
"""Generate self-signed signing fixtures for the pdf_sign tests.

Produces (committed so test runs are hermetic — no openssl needed at test time):
  - test/data/sign_cert.pem           self-signed X.509 certificate (CN=Vori PDF Signer)
  - test/data/sign_key.pem            matching RSA-2048 private key, NO passphrase
  - test/data/sign_key_encrypted.pem  the same key encrypted with passphrase "vori-secret"
                                      (exercises pdf_sign's key_password option)

Uses the openssl CLI only (no Python crypto deps). Deterministic subject/serial
so re-running yields functionally identical material.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

OUT_DIR = Path(__file__).resolve().parent
KEY = OUT_DIR / "sign_key.pem"
KEY_ENC = OUT_DIR / "sign_key_encrypted.pem"
CERT = OUT_DIR / "sign_cert.pem"
KEY_PASSWORD = "vori-secret"
SUBJECT = "/C=US/O=Vori PDF Test/CN=Vori PDF Signer"


def run(cmd: list[str]) -> None:
    proc = subprocess.run(cmd, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(
            f"command failed: {' '.join(cmd)}\n{proc.stderr.decode(errors='replace')}"
        )


def main() -> int:
    # Unencrypted RSA-2048 key.
    run(["openssl", "genrsa", "-out", str(KEY), "2048"])
    # Self-signed cert valid for 20 years.
    run(
        [
            "openssl",
            "req",
            "-new",
            "-x509",
            "-key",
            str(KEY),
            "-out",
            str(CERT),
            "-days",
            "7300",
            "-sha256",
            "-subj",
            SUBJECT,
            "-set_serial",
            "3735928559",
        ]
    )
    # Passphrase-protected copy of the same key (AES-256).
    run(
        [
            "openssl",
            "pkey",
            "-in",
            str(KEY),
            "-out",
            str(KEY_ENC),
            "-aes256",
            "-passout",
            f"pass:{KEY_PASSWORD}",
        ]
    )
    print(f"wrote {CERT.name}, {KEY.name}, {KEY_ENC.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
