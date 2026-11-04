#!/usr/bin/env python3
# Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
"""Verify one SHA-256/RSA Authenticode signature on a PE/COFF EFI image.

Designed for Jsign 7.x compatibility where the Jsign CLI can sign/extract but
has no native `verify` command. Uses only Python stdlib plus OpenSSL.
"""
from __future__ import annotations

import hashlib
import os
from datetime import datetime, timezone
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

OID_PKCS7_SIGNED_DATA = "1.2.840.113549.1.7.2"
OID_SPC_INDIRECT_DATA = "1.3.6.1.4.1.311.2.1.4"
OID_SHA256 = "2.16.840.1.101.3.4.2.1"
OID_CONTENT_TYPE = "1.2.840.113549.1.9.3"
OID_MESSAGE_DIGEST = "1.2.840.113549.1.9.4"
OID_RSA_ENCRYPTION = "1.2.840.113549.1.1.1"
OID_SHA256_WITH_RSA = "1.2.840.113549.1.1.11"
OID_BASIC_CONSTRAINTS = "2.5.29.19"
OID_KEY_USAGE = "2.5.29.15"
OID_EXTENDED_KEY_USAGE = "2.5.29.37"
OID_CODE_SIGNING_EKU = "1.3.6.1.5.5.7.3.3"


class VerifyError(Exception):
    pass


@dataclass(frozen=True)
class Node:
    data: bytes
    start: int
    tag_end: int
    content_start: int
    end: int
    tag_class: int
    constructed: bool
    tag_number: int

    @property
    def raw(self) -> bytes:
        return self.data[self.start:self.end]

    @property
    def content(self) -> bytes:
        return self.data[self.content_start:self.end]

    def children(self) -> list["Node"]:
        if not self.constructed:
            raise VerifyError("ASN.1 primitive cannot have children")
        out: list[Node] = []
        pos = self.content_start
        while pos < self.end:
            child = parse_node(self.data, pos, self.end)
            out.append(child)
            pos = child.end
        if pos != self.end:
            raise VerifyError("ASN.1 child boundary mismatch")
        return out


def parse_node(data: bytes, start: int = 0, limit: int | None = None) -> Node:
    if limit is None:
        limit = len(data)
    if start < 0 or start >= limit or limit > len(data):
        raise VerifyError("ASN.1 offset out of range")
    pos = start
    first = data[pos]
    pos += 1
    tag_class = first >> 6
    constructed = bool(first & 0x20)
    tag_number = first & 0x1F
    if tag_number == 0x1F:
        tag_number = 0
        seen = 0
        while True:
            if pos >= limit or seen >= 6:
                raise VerifyError("invalid high-tag-number ASN.1 value")
            b = data[pos]
            pos += 1
            seen += 1
            if seen == 1 and (b & 0x7F) == 0:
                raise VerifyError("non-canonical ASN.1 tag")
            tag_number = (tag_number << 7) | (b & 0x7F)
            if not (b & 0x80):
                break
    tag_end = pos
    if pos >= limit:
        raise VerifyError("truncated ASN.1 length")
    first_len = data[pos]
    pos += 1
    if first_len < 0x80:
        length = first_len
    else:
        count = first_len & 0x7F
        if count == 0:
            raise VerifyError("indefinite ASN.1 length is not accepted")
        if count > 8 or pos + count > limit:
            raise VerifyError("invalid ASN.1 length")
        if data[pos] == 0:
            raise VerifyError("non-canonical ASN.1 length")
        length = int.from_bytes(data[pos:pos+count], "big")
        if length < 0x80:
            raise VerifyError("non-canonical ASN.1 long length")
        pos += count
    end = pos + length
    if end > limit:
        raise VerifyError("truncated ASN.1 value")
    return Node(data, start, tag_end, pos, end, tag_class, constructed, tag_number)


def require_universal(node: Node, tag: int, label: str) -> None:
    if node.tag_class != 0 or node.tag_number != tag:
        raise VerifyError(f"expected {label}")


def decode_oid(node: Node) -> str:
    require_universal(node, 6, "OBJECT IDENTIFIER")
    b = node.content
    if not b:
        raise VerifyError("empty OBJECT IDENTIFIER")
    first = b[0]
    a = min(first // 40, 2)
    parts = [a, first - a * 40]
    value = 0
    active = False
    for x in b[1:]:
        active = True
        value = (value << 7) | (x & 0x7F)
        if not (x & 0x80):
            parts.append(value)
            value = 0
            active = False
    if active:
        raise VerifyError("truncated OBJECT IDENTIFIER")
    return ".".join(str(x) for x in parts)


def encode_length(length: int) -> bytes:
    if length < 0:
        raise VerifyError("negative DER length")
    if length < 0x80:
        return bytes([length])
    raw = length.to_bytes((length.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(raw)]) + raw


def der_set(content: bytes) -> bytes:
    return b"\x31" + encode_length(len(content)) + content


def run(args: list[str], *, input_data: bytes | None = None) -> subprocess.CompletedProcess[bytes]:
    try:
        return subprocess.run(args, input=input_data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    except OSError as exc:
        raise VerifyError(f"unable to execute {args[0]}: {exc}") from exc


def openssl_cert_der(cert: Path) -> bytes:
    cp = run(["openssl", "x509", "-in", str(cert), "-outform", "DER"])
    if cp.returncode != 0 or not cp.stdout:
        raise VerifyError("invalid X.509 certificate")
    return cp.stdout


def _parse_x509_time(node: Node) -> datetime:
    try:
        text = node.content.decode("ascii")
    except UnicodeDecodeError as exc:
        raise VerifyError("X.509 validity contains a non-ASCII time") from exc
    if node.tag_class != 0:
        raise VerifyError("X.509 validity contains an invalid time tag")
    try:
        if node.tag_number == 23 and len(text) == 13 and text.endswith("Z"):
            year2 = int(text[0:2])
            year = 2000 + year2 if year2 < 50 else 1900 + year2
            parts = (year, int(text[2:4]), int(text[4:6]), int(text[6:8]), int(text[8:10]), int(text[10:12]))
        elif node.tag_number == 24 and len(text) == 15 and text.endswith("Z"):
            parts = (int(text[0:4]), int(text[4:6]), int(text[6:8]), int(text[8:10]), int(text[10:12]), int(text[12:14]))
        else:
            raise VerifyError("X.509 validity uses an unsupported time encoding")
        return datetime(*parts, tzinfo=timezone.utc)
    except ValueError as exc:
        raise VerifyError("X.509 validity contains an invalid time") from exc


def _parse_basic_constraints_ca(extn_value: Node) -> bool:
    require_universal(extn_value, 4, "X.509 extnValue OCTET STRING")
    constraints = parse_node(extn_value.content)
    require_universal(constraints, 16, "X.509 Basic Constraints SEQUENCE")
    if constraints.end != len(extn_value.content):
        raise VerifyError("X.509 Basic Constraints has trailing data")
    fields = constraints.children()
    if len(fields) > 2:
        raise VerifyError("X.509 Basic Constraints has an invalid structure")
    is_ca = False
    index = 0
    if fields and fields[0].tag_class == 0 and fields[0].tag_number == 1:
        value = fields[0]
        if value.constructed or len(value.content) != 1 or value.content[0] not in (0x00, 0xFF):
            raise VerifyError("X.509 Basic Constraints has an invalid CA boolean")
        is_ca = value.content[0] != 0
        index = 1
    if index < len(fields):
        path_len = fields[index]
        require_universal(path_len, 2, "X.509 Basic Constraints pathLenConstraint INTEGER")
        if not is_ca:
            raise VerifyError("X.509 Basic Constraints has pathLenConstraint without CA:TRUE")
        index += 1
    if index != len(fields):
        raise VerifyError("X.509 Basic Constraints has an invalid structure")
    return is_ca


def _parse_key_usage_has_digital_signature(extn_value: Node) -> bool:
    require_universal(extn_value, 4, "X.509 extnValue OCTET STRING")
    usage = parse_node(extn_value.content)
    require_universal(usage, 3, "X.509 Key Usage BIT STRING")
    if usage.end != len(extn_value.content) or usage.constructed:
        raise VerifyError("X.509 Key Usage has an invalid structure")
    bits = usage.content
    if len(bits) < 2 or bits[0] > 7:
        raise VerifyError("X.509 Key Usage BIT STRING is invalid")
    unused_bits = bits[0]
    if unused_bits and (bits[-1] & ((1 << unused_bits) - 1)) != 0:
        raise VerifyError("X.509 Key Usage BIT STRING has non-zero padding")
    return bool(bits[1] & 0x80)


def _certificate_policy_fields(cert_der: bytes) -> tuple[datetime, datetime, bool, bool, bool, bool, bool, bool]:
    root = parse_node(cert_der)
    require_universal(root, 16, "X.509 Certificate SEQUENCE")
    if root.end != len(cert_der):
        raise VerifyError("X.509 certificate has trailing data")
    certificate = root.children()
    if len(certificate) != 3:
        raise VerifyError("X.509 certificate has an invalid top-level structure")
    tbs = certificate[0]
    require_universal(tbs, 16, "X.509 TBSCertificate SEQUENCE")
    fields = tbs.children()
    offset = 1 if fields and fields[0].tag_class == 2 and fields[0].tag_number == 0 else 0
    if len(fields) < offset + 6:
        raise VerifyError("X.509 TBSCertificate is truncated")

    validity = fields[offset + 3]
    require_universal(validity, 16, "X.509 Validity SEQUENCE")
    validity_fields = validity.children()
    if len(validity_fields) != 2:
        raise VerifyError("X.509 Validity has an invalid structure")
    not_before = _parse_x509_time(validity_fields[0])
    not_after = _parse_x509_time(validity_fields[1])
    if not_before > not_after:
        raise VerifyError("X.509 validity interval is reversed")

    has_basic_constraints = False
    basic_constraints_is_ca = False
    basic_constraints_is_critical = False
    has_key_usage = False
    key_usage_has_digital_signature = False
    key_usage_is_critical = False
    has_code_signing_eku = False
    extensions_wrapper = next(
        (node for node in fields[offset + 6:] if node.tag_class == 2 and node.tag_number == 3),
        None,
    )
    if extensions_wrapper is not None:
        wrapper_children = extensions_wrapper.children()
        if len(wrapper_children) != 1:
            raise VerifyError("X.509 Extensions wrapper has an invalid structure")
        extensions = wrapper_children[0]
        require_universal(extensions, 16, "X.509 Extensions SEQUENCE")
        for extension in extensions.children():
            require_universal(extension, 16, "X.509 Extension SEQUENCE")
            extension_fields = extension.children()
            if len(extension_fields) not in (2, 3):
                raise VerifyError("X.509 Extension has an invalid structure")
            oid = decode_oid(extension_fields[0])
            critical = False
            if len(extension_fields) == 3:
                critical_node = extension_fields[1]
                require_universal(critical_node, 1, "X.509 Extension critical BOOLEAN")
                if critical_node.constructed or len(critical_node.content) != 1 or critical_node.content[0] not in (0x00, 0xFF):
                    raise VerifyError("X.509 Extension critical flag is invalid")
                critical = critical_node.content[0] != 0
            extn_value = extension_fields[-1]
            if oid == OID_BASIC_CONSTRAINTS:
                if has_basic_constraints:
                    raise VerifyError("X.509 certificate contains duplicate Basic Constraints extensions")
                has_basic_constraints = True
                basic_constraints_is_ca = _parse_basic_constraints_ca(extn_value)
                basic_constraints_is_critical = critical
            elif oid == OID_KEY_USAGE:
                if has_key_usage:
                    raise VerifyError("X.509 certificate contains duplicate Key Usage extensions")
                has_key_usage = True
                key_usage_has_digital_signature = _parse_key_usage_has_digital_signature(extn_value)
                key_usage_is_critical = critical
            elif oid == OID_EXTENDED_KEY_USAGE:
                require_universal(extn_value, 4, "X.509 extnValue OCTET STRING")
                eku_sequence = parse_node(extn_value.content)
                require_universal(eku_sequence, 16, "X.509 Extended Key Usage SEQUENCE")
                if eku_sequence.end != len(extn_value.content):
                    raise VerifyError("X.509 Extended Key Usage has trailing data")
                has_code_signing_eku = any(
                    decode_oid(usage) == OID_CODE_SIGNING_EKU for usage in eku_sequence.children()
                )

    return (
        not_before,
        not_after,
        has_basic_constraints,
        basic_constraints_is_ca,
        basic_constraints_is_critical,
        has_key_usage and key_usage_has_digital_signature,
        key_usage_is_critical,
        has_code_signing_eku,
    )


def _require_rsa_algorithm(node: Node, oid: str, label: str) -> None:
    require_universal(node, 16, label)
    fields = node.children()
    if not fields or len(fields) > 2 or decode_oid(fields[0]) != oid:
        raise VerifyError(f"{label} is not supported")
    if len(fields) == 2:
        require_universal(fields[1], 5, f"{label} NULL parameters")
        if fields[1].constructed or fields[1].content:
            raise VerifyError(f"{label} parameters are invalid")


def _verify_certificate_self_signature(cert_der: bytes) -> None:
    root = parse_node(cert_der)
    require_universal(root, 16, "X.509 Certificate SEQUENCE")
    certificate = root.children()
    if root.end != len(cert_der) or len(certificate) != 3:
        raise VerifyError("X.509 certificate has an invalid top-level structure")
    tbs, algorithm, signature = certificate
    require_universal(tbs, 16, "X.509 TBSCertificate SEQUENCE")
    fields = tbs.children()
    offset = 1 if fields and fields[0].tag_class == 2 and fields[0].tag_number == 0 else 0
    if len(fields) < offset + 6:
        raise VerifyError("X.509 TBSCertificate is truncated")
    _require_rsa_algorithm(algorithm, OID_SHA256_WITH_RSA, "X.509 signature AlgorithmIdentifier")
    if fields[offset + 1].raw != algorithm.raw:
        raise VerifyError("X.509 certificate signature algorithms do not match")
    public_key = fields[offset + 5]
    require_universal(public_key, 16, "X.509 SubjectPublicKeyInfo SEQUENCE")
    public_key_fields = public_key.children()
    if len(public_key_fields) != 2:
        raise VerifyError("X.509 SubjectPublicKeyInfo has an invalid structure")
    _require_rsa_algorithm(public_key_fields[0], OID_RSA_ENCRYPTION, "X.509 public key AlgorithmIdentifier")
    require_universal(signature, 3, "X.509 signature BIT STRING")
    if signature.constructed or len(signature.content) < 2 or signature.content[0] != 0:
        raise VerifyError("X.509 certificate signature BIT STRING is invalid")

    # Verify the exact TBSCertificate DER with the public key carried by that
    # certificate. Trusting the certificate via -CAfile does not check this
    # signature, and -check_ss_sig imposes CA keyCertSign rules on our leaf.
    with tempfile.TemporaryDirectory(prefix="BootLoaderSelector.CertificateVerify.") as td:
        td = Path(td)
        public_key_path = td / "public.der"
        tbs_path = td / "tbs.der"
        signature_path = td / "signature.bin"
        public_key_path.write_bytes(public_key.raw)
        tbs_path.write_bytes(tbs.raw)
        signature_path.write_bytes(signature.content[1:])
        cp = run([
            "openssl", "dgst", "-sha256", "-keyform", "DER", "-verify", str(public_key_path),
            "-signature", str(signature_path), str(tbs_path),
        ])
        if cp.returncode != 0 or b"Verified OK" not in cp.stdout:
            raise VerifyError("ad-hoc signer certificate is not cryptographically self-signed")


def verify_certificate_policy(cert: Path, cert_der: bytes | None = None) -> None:
    if cert_der is None:
        cert_der = openssl_cert_der(cert)
    (
        not_before,
        not_after,
        has_basic_constraints,
        basic_constraints_is_ca,
        basic_constraints_is_critical,
        key_usage_has_digital_signature,
        key_usage_is_critical,
        has_code_signing_eku,
    ) = _certificate_policy_fields(cert_der)
    now = datetime.now(timezone.utc)
    if now < not_before or now > not_after:
        raise VerifyError("signer certificate is expired or not yet valid")

    cp = run(["openssl", "x509", "-in", str(cert), "-noout", "-subject", "-issuer", "-nameopt", "RFC2253"])
    if cp.returncode != 0:
        raise VerifyError("unable to inspect signer certificate")
    lines = cp.stdout.decode("utf-8", "replace").splitlines()
    subject = next((x.partition("=")[2].strip() for x in lines if x.startswith("subject=")), None)
    issuer = next((x.partition("=")[2].strip() for x in lines if x.startswith("issuer=")), None)
    if not subject or subject != issuer:
        raise VerifyError("ad-hoc signer certificate is not self-issued")
    _verify_certificate_self_signature(cert_der)
    # Retain OpenSSL's other certificate checks (including unknown critical
    # extensions); this trust-anchor check is not proof of self-signature.
    profile_verify = run(["openssl", "verify", "-CAfile", str(cert), str(cert)])
    if profile_verify.returncode != 0:
        raise VerifyError("ad-hoc signer certificate failed OpenSSL policy verification")
    if not has_basic_constraints:
        raise VerifyError("signer certificate does not contain Basic Constraints")
    if basic_constraints_is_ca:
        raise VerifyError("signer certificate Basic Constraints must be CA:FALSE")
    if not basic_constraints_is_critical:
        raise VerifyError("signer certificate Basic Constraints must be critical")
    if not key_usage_has_digital_signature:
        raise VerifyError("signer certificate Key Usage does not contain digitalSignature")
    if not key_usage_is_critical:
        raise VerifyError("signer certificate Key Usage must be critical")
    if not has_code_signing_eku:
        raise VerifyError("signer certificate does not contain the codeSigning EKU")


def authenticode_digest_sha256(image: bytes) -> bytes:
    if len(image) < 0x40 or image[:2] != b"MZ":
        raise VerifyError("PE/COFF DOS header is invalid")
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    if pe > len(image) - 24 or image[pe:pe+4] != b"PE\x00\x00":
        raise VerifyError("PE/COFF signature is invalid")
    optional = pe + 24
    if optional + 2 > len(image):
        raise VerifyError("PE/COFF optional header is truncated")
    magic = struct.unpack_from("<H", image, optional)[0]
    if magic == 0x10B:
        checksum = pe + 88
        number_rva = pe + 116
        directories = pe + 120
    elif magic == 0x20B:
        checksum = pe + 88
        number_rva = pe + 132
        directories = pe + 136
    else:
        raise VerifyError("unsupported PE optional-header format")
    if number_rva + 4 > len(image):
        raise VerifyError("PE data-directory count is truncated")
    count = struct.unpack_from("<I", image, number_rva)[0]
    if count <= 4:
        raise VerifyError("PE certificate-table data directory is absent")
    cert_dir = directories + 8 * 4
    if checksum + 4 > len(image) or cert_dir + 8 > len(image) or checksum >= cert_dir:
        raise VerifyError("PE signing fields are outside the image")
    cert_offset, cert_size = struct.unpack_from("<II", image, cert_dir)
    if cert_offset == 0 or cert_size == 0:
        raise VerifyError("PE image has no Authenticode certificate table")
    if cert_offset % 8 != 0 or cert_offset < cert_dir + 8 or cert_offset + cert_size > len(image):
        raise VerifyError("PE certificate table has invalid bounds or alignment")

    digest = hashlib.sha256()
    digest.update(image[:checksum])
    digest.update(image[checksum+4:cert_dir])
    digest.update(image[cert_dir+8:cert_offset])
    digest.update(image[cert_offset+cert_size:])
    return digest.digest()


def parse_signature(signature_der: bytes, expected_cert_der: bytes) -> tuple[bytes, bytes, bytes, bytes]:
    root = parse_node(signature_der)
    if root.end != len(signature_der):
        raise VerifyError("trailing data after CMS ContentInfo")
    require_universal(root, 16, "CMS ContentInfo SEQUENCE")
    root_children = root.children()
    if len(root_children) != 2 or decode_oid(root_children[0]) != OID_PKCS7_SIGNED_DATA:
        raise VerifyError("signature is not PKCS#7 SignedData")
    tagged = root_children[1]
    if tagged.tag_class != 2 or tagged.tag_number != 0 or not tagged.constructed:
        raise VerifyError("CMS SignedData wrapper is invalid")
    tagged_children = tagged.children()
    if len(tagged_children) != 1:
        raise VerifyError("CMS SignedData wrapper is ambiguous")
    signed_data = tagged_children[0]
    require_universal(signed_data, 16, "SignedData SEQUENCE")
    sd = signed_data.children()
    if len(sd) < 5:
        raise VerifyError("SignedData is truncated")

    # Encapsulated Authenticode content is the third SignedData element.
    encap = sd[2]
    require_universal(encap, 16, "encapsulated content SEQUENCE")
    ec = encap.children()
    if len(ec) != 2 or decode_oid(ec[0]) != OID_SPC_INDIRECT_DATA:
        raise VerifyError("unexpected Authenticode content type")
    content_tag = ec[1]
    if content_tag.tag_class != 2 or content_tag.tag_number != 0 or not content_tag.constructed:
        raise VerifyError("Authenticode content wrapper is invalid")
    inner = content_tag.children()
    if len(inner) != 1:
        raise VerifyError("Authenticode content wrapper is ambiguous")
    indirect = inner[0]
    require_universal(indirect, 16, "SpcIndirectDataContent SEQUENCE")

    # Jsign/BC's PKCS7ProcessableObject hashes SEQUENCE contents (without tag/length).
    content_digest = hashlib.sha256(indirect.content).digest()

    # Find the certificate set and signerInfos. signerInfos is the final SET.
    cert_container = next((n for n in sd[3:-1] if n.tag_class == 2 and n.tag_number == 0), None)
    if cert_container is None:
        raise VerifyError("CMS certificate set is missing")
    certs = cert_container.children()
    if sum(1 for n in certs if n.raw == expected_cert_der) != 1:
        raise VerifyError("embedded signer certificate does not match the published certificate")

    signer_infos = sd[-1]
    require_universal(signer_infos, 17, "SignerInfos SET")
    si_items = signer_infos.children()
    if len(si_items) != 1:
        raise VerifyError("expected exactly one CMS signer")
    signer_info = si_items[0]
    require_universal(signer_info, 16, "SignerInfo SEQUENCE")
    si = signer_info.children()
    if len(si) < 6:
        raise VerifyError("SignerInfo is truncated")

    digest_alg = si[2]
    require_universal(digest_alg, 16, "digest AlgorithmIdentifier")
    da = digest_alg.children()
    if not da or decode_oid(da[0]) != OID_SHA256:
        raise VerifyError("CMS signer digest algorithm is not SHA-256")

    attrs = si[3]
    if attrs.tag_class != 2 or attrs.tag_number != 0 or not attrs.constructed:
        raise VerifyError("CMS signed attributes are missing")
    message_digest = None
    content_type = None
    for attr in attrs.children():
        require_universal(attr, 16, "CMS Attribute SEQUENCE")
        ac = attr.children()
        if len(ac) != 2:
            raise VerifyError("malformed CMS attribute")
        oid = decode_oid(ac[0])
        values = ac[1]
        require_universal(values, 17, "CMS Attribute values SET")
        vals = values.children()
        if len(vals) != 1:
            raise VerifyError("ambiguous CMS signed attribute")
        if oid == OID_MESSAGE_DIGEST:
            require_universal(vals[0], 4, "messageDigest OCTET STRING")
            message_digest = vals[0].content
        elif oid == OID_CONTENT_TYPE:
            content_type = decode_oid(vals[0])
    if content_type != OID_SPC_INDIRECT_DATA:
        raise VerifyError("CMS contentType signed attribute is invalid")
    if message_digest != content_digest:
        raise VerifyError("CMS messageDigest does not match Authenticode content")

    sig_alg = si[4]
    require_universal(sig_alg, 16, "signature AlgorithmIdentifier")
    sa = sig_alg.children()
    if not sa or decode_oid(sa[0]) not in (OID_RSA_ENCRYPTION, OID_SHA256_WITH_RSA):
        raise VerifyError("CMS signature algorithm is not RSA/SHA-256 compatible")

    sig_value = si[5]
    require_universal(sig_value, 4, "CMS signature OCTET STRING")
    if len(sig_value.content) != 384:
        raise VerifyError("CMS RSA signature is not 3072 bits")

    # signedAttrs is IMPLICIT [0] SET OF; signature input uses DER SET tag.
    signed_attrs_der = der_set(attrs.content)

    # File digest is the second component of SpcIndirectDataContent.
    ind = indirect.children()
    if len(ind) != 2:
        raise VerifyError("SpcIndirectDataContent is malformed")
    digest_info = ind[1]
    require_universal(digest_info, 16, "DigestInfo SEQUENCE")
    di = digest_info.children()
    if len(di) != 2:
        raise VerifyError("Authenticode DigestInfo is malformed")
    alg = di[0]
    require_universal(alg, 16, "DigestInfo AlgorithmIdentifier")
    ai = alg.children()
    if not ai or decode_oid(ai[0]) != OID_SHA256:
        raise VerifyError("Authenticode file digest algorithm is not SHA-256")
    require_universal(di[1], 4, "Authenticode file digest OCTET STRING")
    file_digest = di[1].content
    if len(file_digest) != 32:
        raise VerifyError("Authenticode SHA-256 digest has invalid size")

    return signed_attrs_der, sig_value.content, file_digest, content_digest


def verify_rsa_signature(cert: Path, signed_attrs_der: bytes, signature: bytes) -> None:
    if not shutil.which("openssl"):
        raise VerifyError("openssl is required")
    with tempfile.TemporaryDirectory(prefix="BootLoaderSelector.AuthenticodeVerify.") as td:
        td = Path(td)
        pub = td / "public.pem"
        attrs = td / "signed-attrs.der"
        sig = td / "signature.bin"
        cp = run(["openssl", "x509", "-in", str(cert), "-pubkey", "-noout"])
        if cp.returncode != 0 or b"BEGIN PUBLIC KEY" not in cp.stdout:
            raise VerifyError("unable to extract signer public key")
        pub.write_bytes(cp.stdout)
        attrs.write_bytes(signed_attrs_der)
        sig.write_bytes(signature)
        cp = run(["openssl", "dgst", "-sha256", "-verify", str(pub), "-signature", str(sig), str(attrs)])
        if cp.returncode != 0 or b"Verified OK" not in cp.stdout:
            raise VerifyError("CMS/RSA signature integrity check failed")


def main() -> int:
    if len(sys.argv) != 4:
        print(f"Usage: {Path(sys.argv[0]).name} SIGNED_EFI SIGNATURE_DER PUBLIC_CERT_PEM", file=sys.stderr)
        return 2
    image_path = Path(sys.argv[1])
    sig_path = Path(sys.argv[2])
    cert_path = Path(sys.argv[3])
    for path, label in ((image_path, "EFI image"), (sig_path, "signature"), (cert_path, "certificate")):
        if not path.is_file() or path.is_symlink():
            raise VerifyError(f"{label} is not a regular non-symlink file: {path}")
    if not shutil.which("openssl"):
        raise VerifyError("openssl is required")

    cert_der = openssl_cert_der(cert_path)
    verify_certificate_policy(cert_path, cert_der)
    signed_attrs, cms_signature, signed_file_digest, _ = parse_signature(sig_path.read_bytes(), cert_der)
    computed_file_digest = authenticode_digest_sha256(image_path.read_bytes())
    if computed_file_digest != signed_file_digest:
        raise VerifyError("PE/COFF Authenticode file digest does not match the signed digest")
    verify_rsa_signature(cert_path, signed_attrs, cms_signature)
    print("Jsign 7.x Authenticode compatibility verification: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except VerifyError as exc:
        print(f"Jsign compatibility verification failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
