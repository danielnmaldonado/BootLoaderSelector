#!/bin/bash
set -euo pipefail

# Credits: ©DNMTechLabs / dnmtechlabs@gmail.com

cd "$(dirname "$0")/.."
PROJECT_ROOT="$PWD"
VERIFIER="$PROJECT_ROOT/AuditTools/Verify-PeCoff-Authenticode.py"

command -v openssl >/dev/null 2>&1 || { echo "openssl is required" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 2; }

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/BootLoaderSelector.AuthenticodePolicyTest.XXXXXX")"
cleanup() { rm -rf "$TMP_ROOT"; }
trap cleanup EXIT

check_policy() {
  local mode="$1"
  local cert="$2"
  PYTHONDONTWRITEBYTECODE=1 python3 - "$VERIFIER" "$cert" "$mode" <<'PY'
import importlib.util
import pathlib
import sys

verifier_path = pathlib.Path(sys.argv[1])
cert_path = pathlib.Path(sys.argv[2])
mode = sys.argv[3]
spec = importlib.util.spec_from_file_location("bls_authenticode_verifier", verifier_path)
if spec is None or spec.loader is None:
    raise SystemExit("unable to load verifier module")
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)

try:
    module.verify_certificate_policy(cert_path)
except module.VerifyError as exc:
    if mode == "reject":
        print(f"policy rejection: {exc}")
        raise SystemExit(0)
    print(f"unexpected policy rejection: {exc}", file=sys.stderr)
    raise SystemExit(1)
else:
    if mode == "accept":
        print("policy acceptance: PASS")
        raise SystemExit(0)
    print("invalid certificate policy was accepted", file=sys.stderr)
    raise SystemExit(1)
PY
}

# Positive control: the exact policy expected from the release signer must pass.
openssl req -new -x509 -newkey rsa:3072 -sha256 -nodes -days 1 \
  -subj '/CN=DNMTechLabs Policy Positive/O=DNMTechLabs' \
  -addext 'basicConstraints=critical,CA:FALSE' \
  -addext 'keyUsage=critical,digitalSignature' \
  -addext 'extendedKeyUsage=codeSigning' \
  -keyout "$TMP_ROOT/positive-key.pem" -out "$TMP_ROOT/positive-cert.pem" >/dev/null 2>&1
check_policy accept "$TMP_ROOT/positive-cert.pem"

# Regression: the signer emits Basic Constraints as critical. A non-critical
# CA:FALSE extension is semantically weaker than the release certificate
# profile and must not be accepted as an exact policy match.
openssl req -new -x509 -newkey rsa:3072 -sha256 -nodes -days 1 \
  -subj '/CN=DNMTechLabs Noncritical BC Policy Negative/O=DNMTechLabs' \
  -addext 'basicConstraints=CA:FALSE' \
  -addext 'keyUsage=critical,digitalSignature' \
  -addext 'extendedKeyUsage=codeSigning' \
  -keyout "$TMP_ROOT/noncritical-bc-key.pem" -out "$TMP_ROOT/noncritical-bc-cert.pem" >/dev/null 2>&1
check_policy reject "$TMP_ROOT/noncritical-bc-cert.pem"

# Regression: Key Usage is also critical in the signer profile. Preserve that
# invariant so verification detects certificate-profile drift instead of merely
# checking that digitalSignature happens to be present.
openssl req -new -x509 -newkey rsa:3072 -sha256 -nodes -days 1 \
  -subj '/CN=DNMTechLabs Noncritical KU Policy Negative/O=DNMTechLabs' \
  -addext 'basicConstraints=critical,CA:FALSE' \
  -addext 'keyUsage=digitalSignature' \
  -addext 'extendedKeyUsage=codeSigning' \
  -keyout "$TMP_ROOT/noncritical-ku-key.pem" -out "$TMP_ROOT/noncritical-ku-cert.pem" >/dev/null 2>&1
check_policy reject "$TMP_ROOT/noncritical-ku-cert.pem"

# Regression: the old verifier searched all of `openssl x509 -text` for the
# words "Code Signing". A certificate with that text only in its subject could
# therefore bypass the EKU requirement.
openssl req -new -x509 -newkey rsa:3072 -sha256 -nodes -days 1 \
  -subj '/CN=Code Signing/O=DNMTechLabs Policy Negative' \
  -addext 'basicConstraints=critical,CA:FALSE' \
  -addext 'keyUsage=critical,digitalSignature' \
  -keyout "$TMP_ROOT/no-eku-key.pem" -out "$TMP_ROOT/no-eku-cert.pem" >/dev/null 2>&1
check_policy reject "$TMP_ROOT/no-eku-cert.pem"

# Regression: the release signer creates a leaf-only ad-hoc certificate. A
# self-signed CA certificate with codeSigning EKU must not satisfy that policy.
openssl req -new -x509 -newkey rsa:3072 -sha256 -nodes -days 1 \
  -subj '/CN=DNMTechLabs CA Policy Negative/O=DNMTechLabs' \
  -addext 'basicConstraints=critical,CA:TRUE' \
  -addext 'keyUsage=critical,digitalSignature,keyCertSign' \
  -addext 'extendedKeyUsage=codeSigning' \
  -keyout "$TMP_ROOT/ca-key.pem" -out "$TMP_ROOT/ca-cert.pem" >/dev/null 2>&1
check_policy reject "$TMP_ROOT/ca-cert.pem"

# Regression: codeSigning EKU alone is not sufficient. The release signer
# explicitly restricts Key Usage to signing, so digitalSignature is mandatory.
openssl req -new -x509 -newkey rsa:3072 -sha256 -nodes -days 1 \
  -subj '/CN=DNMTechLabs KeyUsage Policy Negative/O=DNMTechLabs' \
  -addext 'basicConstraints=critical,CA:FALSE' \
  -addext 'keyUsage=critical,keyEncipherment' \
  -addext 'extendedKeyUsage=codeSigning' \
  -keyout "$TMP_ROOT/no-dsig-key.pem" -out "$TMP_ROOT/no-dsig-cert.pem" >/dev/null 2>&1
check_policy reject "$TMP_ROOT/no-dsig-cert.pem"

# Regression: Basic Constraints is part of the release certificate contract;
# absence must not be treated as the default leaf policy.
openssl req -new -newkey rsa:3072 -sha256 -nodes \
  -subj '/CN=DNMTechLabs Missing BC Policy Negative/O=DNMTechLabs' \
  -keyout "$TMP_ROOT/no-bc-key.pem" -out "$TMP_ROOT/no-bc.csr" >/dev/null 2>&1
cat > "$TMP_ROOT/no-bc-ext.cnf" <<'EOF'
keyUsage=critical,digitalSignature
extendedKeyUsage=codeSigning
EOF
openssl x509 -req -in "$TMP_ROOT/no-bc.csr" -signkey "$TMP_ROOT/no-bc-key.pem" \
  -days 1 -sha256 -extfile "$TMP_ROOT/no-bc-ext.cnf" -out "$TMP_ROOT/no-bc-cert.pem" >/dev/null 2>&1
check_policy reject "$TMP_ROOT/no-bc-cert.pem"

# Regression: Key Usage is also explicit in the release certificate contract.
openssl req -new -x509 -newkey rsa:3072 -sha256 -nodes -days 1 \
  -subj '/CN=DNMTechLabs Missing KU Policy Negative/O=DNMTechLabs' \
  -addext 'basicConstraints=critical,CA:FALSE' \
  -addext 'extendedKeyUsage=codeSigning' \
  -keyout "$TMP_ROOT/no-ku-key.pem" -out "$TMP_ROOT/no-ku-cert.pem" >/dev/null 2>&1
check_policy reject "$TMP_ROOT/no-ku-cert.pem"

# Regression: `openssl x509 -checkend 0` checks notAfter only. Build a
# self-issued code-signing certificate whose notBefore is in the future; the
# policy verifier must reject it.
mkdir -p "$TMP_ROOT/future-ca"
cd "$TMP_ROOT/future-ca"
touch index.txt
printf '1000\n' > serial
cat > openssl.cnf <<'EOF'
[ ca ]
default_ca = CA_default

[ CA_default ]
database = index.txt
serial = serial
new_certs_dir = .
default_md = sha256
policy = policy_any
copy_extensions = copy

[ policy_any ]
commonName = supplied
organizationName = optional

[ req ]
distinguished_name = dn
prompt = no

[ dn ]
CN = DNMTechLabs Future Policy Test
O = DNMTechLabs

[ code_signing ]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = codeSigning
EOF
openssl req -new -newkey rsa:3072 -sha256 -nodes \
  -keyout future-key.pem -out future.csr -config openssl.cnf >/dev/null 2>&1
FUTURE_START="$(date -u -d '+2 days' '+%Y%m%d%H%M%SZ' 2>/dev/null || date -u -v+2d '+%Y%m%d%H%M%SZ')"
FUTURE_END="$(date -u -d '+4 days' '+%Y%m%d%H%M%SZ' 2>/dev/null || date -u -v+4d '+%Y%m%d%H%M%SZ')"
openssl ca -batch -selfsign -config openssl.cnf -keyfile future-key.pem \
  -in future.csr -startdate "$FUTURE_START" -enddate "$FUTURE_END" \
  -extensions code_signing -out future-cert.pem >/dev/null 2>&1
cd "$PROJECT_ROOT"
check_policy reject "$TMP_ROOT/future-ca/future-cert.pem"

# Regression: matching Subject/Issuer DNs prove only that a certificate is
# self-issued. Build a code-signing leaf signed by a different key whose issuer
# has the same DN. The release policy requires the ad-hoc signer to be genuinely
# self-signed, so this certificate must be rejected.
SAME_DN='/CN=DNMTechLabs Same-DN Policy Test/O=DNMTechLabs'
openssl req -new -x509 -newkey rsa:2048 -sha256 -nodes -days 2 \
  -subj "$SAME_DN" \
  -addext 'basicConstraints=critical,CA:TRUE' \
  -addext 'keyUsage=critical,keyCertSign,cRLSign' \
  -keyout "$TMP_ROOT/same-dn-root-key.pem" -out "$TMP_ROOT/same-dn-root-cert.pem" >/dev/null 2>&1
openssl req -new -newkey rsa:2048 -sha256 -nodes \
  -subj "$SAME_DN" \
  -keyout "$TMP_ROOT/same-dn-leaf-key.pem" -out "$TMP_ROOT/same-dn-leaf.csr" >/dev/null 2>&1
cat > "$TMP_ROOT/same-dn-ext.cnf" <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=codeSigning
EOF
openssl x509 -req -in "$TMP_ROOT/same-dn-leaf.csr" \
  -CA "$TMP_ROOT/same-dn-root-cert.pem" -CAkey "$TMP_ROOT/same-dn-root-key.pem" -CAcreateserial \
  -days 1 -sha256 -extfile "$TMP_ROOT/same-dn-ext.cnf" -out "$TMP_ROOT/same-dn-leaf-cert.pem" >/dev/null 2>&1
check_policy reject "$TMP_ROOT/same-dn-leaf-cert.pem"

echo "Authenticode certificate-policy regression: PASS"
