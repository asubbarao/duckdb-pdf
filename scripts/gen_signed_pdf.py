#!/usr/bin/env python3
"""Generate reproducibly signed PDF fixtures for pdf_signatures tests.

Creates:
  - test/data/signed.pdf            — adbe.pkcs7.detached over the whole file
  - test/data/signed_tampered.pdf   — same file with trailing bytes appended
                                      (covers_whole_file → false)
  - test/data/signed_fixture_key.pem / signed_fixture_cert.pem  — self-signed
    material used to produce the CMS blob (committed so rebuilds are identical)

Requires: python3, openssl on PATH, cryptography (for key/cert generation only).
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "test" / "data"
SIGNED = OUT_DIR / "signed.pdf"
TAMPERED = OUT_DIR / "signed_tampered.pdf"
KEY_PATH = OUT_DIR / "signed_fixture_key.pem"
CERT_PATH = OUT_DIR / "signed_fixture_cert.pem"

# Hex digit budget for /Contents (must be even). 8192 hex = 4096 DER bytes.
CONTENTS_HEX_LEN = 8192


def ensure_key_and_cert() -> None:
    if KEY_PATH.exists() and CERT_PATH.exists():
        return
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = issuer = x509.Name(
        [
            x509.NameAttribute(NameOID.COUNTRY_NAME, "US"),
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, "DuckDB PDF Test"),
            x509.NameAttribute(NameOID.COMMON_NAME, "Test Signer"),
        ]
    )
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(0xD0CDB51C)
        .not_valid_before(datetime(2024, 1, 1, tzinfo=timezone.utc))
        .not_valid_after(datetime(2044, 1, 1, tzinfo=timezone.utc))
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )
    KEY_PATH.write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    CERT_PATH.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    print(f"generated {KEY_PATH.name} and {CERT_PATH.name}")


def pdf_date(dt: datetime) -> str:
    dt = dt.astimezone(timezone.utc)
    return dt.strftime("D:%Y%m%d%H%M%S+00'00'")


def assemble(objs: dict[int, bytes]) -> bytes:
    header = b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"
    body = bytearray(header)
    offsets = {0: 0}
    for i in sorted(objs):
        offsets[i] = len(body)
        body += f"{i} 0 obj\n".encode("ascii")
        body += objs[i]
        body += b"\nendobj\n"
    xref_pos = len(body)
    max_id = max(objs)
    body += f"xref\n0 {max_id + 1}\n".encode("ascii")
    body += b"0000000000 65535 f \n"
    for i in range(1, max_id + 1):
        body += f"{offsets[i]:010d} 00000 n \n".encode("ascii")
    body += (
        f"trailer\n<< /Size {max_id + 1} /Root 1 0 R >>\n"
        f"startxref\n{xref_pos}\n%%EOF\n"
    ).encode("ascii")
    return bytes(body)


def cryptography_detached_sign(data: bytes) -> bytes:
    """Detached PKCS#7 via cryptography (true eContent ABSENT)."""
    from cryptography.hazmat.primitives.serialization import pkcs7

    key = serialization.load_pem_private_key(KEY_PATH.read_bytes(), password=None)
    cert = x509.load_pem_x509_certificate(CERT_PATH.read_bytes())
    # Binary is required: without it cryptography canonicalizes the payload
    # (text-mode) and the digest won't match raw PDF bytes.
    return (
        pkcs7.PKCS7SignatureBuilder()
        .set_data(data)
        .add_signer(cert, key, hashes.SHA256())  # type: ignore[arg-type]
        .sign(
            serialization.Encoding.DER,
            [pkcs7.PKCS7Options.DetachedSignature, pkcs7.PKCS7Options.Binary],
        )
    )


def build_signed_pdf() -> bytes:
    signing_time = datetime(2024, 6, 15, 12, 0, 0, tzinfo=timezone.utc)
    m_str = pdf_date(signing_time)

    br_placeholder = b"[0 0000000000 0000000000 0000000000]"
    contents_placeholder = b"<" + b"0" * CONTENTS_HEX_LEN + b">"

    sig_dict = (
        b"<<\n"
        b"/Type /Sig\n"
        b"/Filter /Adobe.PPKLite\n"
        b"/SubFilter /adbe.pkcs7.detached\n"
        b"/ByteRange " + br_placeholder + b"\n"
        b"/Contents " + contents_placeholder + b"\n"
        b"/M (" + m_str.encode("ascii") + b")\n"
        b"/Name (Test Signer)\n"
        b"/Reason (Fixture signature)\n"
        b"/Location (Test Suite)\n"
        b">>"
    )

    stream = b"BT /F1 24 Tf 72 720 Td (Hello signed) Tj ET\n"
    objects: dict[int, bytes] = {
        1: b"<< /Type /Catalog /Pages 2 0 R /AcroForm 6 0 R >>",
        2: b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        3: (
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]\n"
            b"/Contents 5 0 R /Resources << /Font << /F1 4 0 R >> >>\n"
            b"/Annots [7 0 R] >>"
        ),
        4: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        5: b"<< /Length "
        + str(len(stream)).encode()
        + b" >>\nstream\n"
        + stream
        + b"endstream",
        6: b"<< /Fields [7 0 R] /SigFlags 3 >>",
        7: (
            b"<< /Type /Annot /Subtype /Widget /FT /Sig /T (Signature1)\n"
            b"/F 4 /P 3 0 R /Rect [72 700 300 760] /V 8 0 R >>"
        ),
        8: sig_dict,
    }

    pdf = assemble(objects)

    br_pos = pdf.find(br_placeholder)
    contents_pos = pdf.find(contents_placeholder)
    if br_pos < 0 or contents_pos < 0:
        raise RuntimeError("placeholders missing from assembled PDF")

    contents_start = contents_pos  # '<'
    contents_end = contents_pos + len(contents_placeholder)  # past '>'
    file_len = len(pdf)
    range2_len = file_len - contents_end
    br_final = f"[0 {contents_start} {contents_end} {range2_len}]".encode("ascii")
    if len(br_final) > len(br_placeholder):
        raise RuntimeError(f"ByteRange encoding too long: {br_final!r}")
    br_final = br_final + b" " * (len(br_placeholder) - len(br_final))
    pdf = bytearray(pdf)
    pdf[br_pos : br_pos + len(br_placeholder)] = br_final

    to_sign = bytes(pdf[:contents_start] + pdf[contents_end:])

    # Prefer cryptography for true detached CMS (openssl cms -sign embeds content).
    cms_der = cryptography_detached_sign(to_sign)

    # Self-check with openssl cms -verify -content
    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        (tdp / "cms.der").write_bytes(cms_der)
        (tdp / "data.bin").write_bytes(to_sign)
        r = subprocess.run(
            [
                "openssl",
                "cms",
                "-verify",
                "-inform",
                "DER",
                "-in",
                str(tdp / "cms.der"),
                "-content",
                str(tdp / "data.bin"),
                "-noverify",
                "-binary",
                "-out",
                str(tdp / "out.bin"),
            ],
            capture_output=True,
        )
        if r.returncode != 0:
            # Some openssl builds are picky; try without -binary
            r2 = subprocess.run(
                [
                    "openssl",
                    "cms",
                    "-verify",
                    "-inform",
                    "DER",
                    "-in",
                    str(tdp / "cms.der"),
                    "-content",
                    str(tdp / "data.bin"),
                    "-noverify",
                    "-out",
                    str(tdp / "out.bin"),
                ],
                capture_output=True,
            )
            if r2.returncode != 0:
                raise RuntimeError(
                    "CMS self-check failed:\n"
                    + r.stderr.decode(errors="replace")
                    + "\n"
                    + r2.stderr.decode(errors="replace")
                )

    hex_cms = cms_der.hex().upper().encode("ascii")
    if len(hex_cms) > CONTENTS_HEX_LEN:
        raise RuntimeError(f"CMS too large: {len(hex_cms)} > {CONTENTS_HEX_LEN}")
    # Pad with spaces? PDF hex strings allow whitespace. Using '0' pads as NUL
    # bytes after decode — OpenSSL d2i tolerates trailing garbage sometimes but
    # better to pad with spaces outside the hex? Spec: hex string may contain
    # whitespace which is ignored. So pad with spaces AFTER the hex, still
    # inside < >, so the decoded DER has no trailing NULs.
    pad = CONTENTS_HEX_LEN - len(hex_cms)
    # spaces are ignored in PDF hex strings
    contents_final = b"<" + hex_cms + (b" " * pad) + b">"
    assert len(contents_final) == len(contents_placeholder)
    pdf[contents_start:contents_end] = contents_final
    return bytes(pdf)


def covers_whole(path: Path) -> bool:
    data = path.read_bytes()
    i = data.find(b"/ByteRange")
    lb = data.find(b"[", i)
    rb = data.find(b"]", lb)
    nums = [int(x) for x in data[lb + 1 : rb].split()]
    return nums[0] == 0 and nums[2] + nums[3] == len(data)


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    ensure_key_and_cert()
    pdf = build_signed_pdf()
    SIGNED.write_bytes(pdf)
    print(f"wrote {SIGNED} ({len(pdf)} bytes)")

    TAMPERED.write_bytes(pdf + b"\n%tampered-trailing-bytes\n")
    print(f"wrote {TAMPERED} ({TAMPERED.stat().st_size} bytes)")

    assert covers_whole(SIGNED) is True
    assert covers_whole(TAMPERED) is False
    print("covers_whole_file: signed=True tampered=False OK")

    # External check via openssl if pdfsig is flaky (NSS db)
    data = pdf
    i = data.find(b"/ByteRange")
    lb = data.find(b"[", i)
    rb = data.find(b"]", lb)
    a, b, c, d = [int(x) for x in data[lb + 1 : rb].split()]
    to_sign = data[a : a + b] + data[c : c + d]
    # Contents: find after ByteRange
    ci = data.find(b"/Contents", i)
    lt = data.find(b"<", ci)
    gt = data.find(b">", lt)
    # strip whitespace from hex
    hexdigits = bytes(ch for ch in data[lt + 1 : gt] if ch in b"0123456789abcdefABCDEF")
    cms = bytes.fromhex(hexdigits.decode("ascii"))
    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        (tdp / "cms.der").write_bytes(cms)
        (tdp / "data.bin").write_bytes(to_sign)
        r = subprocess.run(
            [
                "openssl",
                "cms",
                "-verify",
                "-inform",
                "DER",
                "-in",
                str(tdp / "cms.der"),
                "-content",
                str(tdp / "data.bin"),
                "-noverify",
                "-binary",
                "-out",
                str(tdp / "out.bin"),
            ],
            capture_output=True,
        )
        print(
            "openssl cms -verify on written fixture:",
            "OK" if r.returncode == 0 else "FAIL",
        )
        if r.returncode != 0:
            print(r.stderr.decode(errors="replace"))
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
