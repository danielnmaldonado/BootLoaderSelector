#!/bin/bash
set -euo pipefail

# Credits: ©DNMTechLabs / dnmtechlabs@gmail.com

cd "$(dirname "$0")/.."
PROJECT_ROOT="$PWD"
SIGNER="$PROJECT_ROOT/AuditTools/Sign-PeCoff-AdHoc.command"
EXPECTED_URL="https://github.com/ebourg/jsign/releases/download/7.5/jsign-7.5.jar"
EXPECTED_SHA256="602a51c3545a6dc4fb99bd2ea7152b26d1345916d0c93ddfbd5936cb735af91c"

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/BootLoaderSelector.SignerBootstrapTest.XXXXXX")"
cleanup() { rm -rf "$TMP_ROOT"; }
trap cleanup EXIT
mkdir -p "$TMP_ROOT/bin" "$TMP_ROOT/cache"

REAL_SHASUM="$(command -v shasum || true)"
REAL_SHA256SUM="$(command -v sha256sum || true)"

cat > "$TMP_ROOT/bin/curl" <<'SH'
#!/bin/bash
set -euo pipefail
out=""
url=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o|--output)
      out="$2"
      shift 2
      ;;
    http://*|https://*)
      url="$1"
      shift
      ;;
    *) shift ;;
  esac
done
[ -n "$out" ] && [ -n "$url" ] || exit 91
printf '%s\n' "$url" > "$BLS_TEST_CURL_URL_LOG"
printf 'synthetic jsign jar fixture\n' > "$out"
SH
chmod 755 "$TMP_ROOT/bin/curl"

cat > "$TMP_ROOT/bin/java" <<'SH'
#!/bin/bash
set -euo pipefail
if [ "${1:-}" = "-version" ]; then
  echo 'openjdk version "21-test"' >&2
  exit 0
fi
if [ "${1:-}" = "-jar" ] && [ -f "${2:-}" ] && [ "${3:-}" = "--version" ]; then
  echo "Jsign 7.5"
  exit 0
fi
if [ "${1:-}" = "-jar" ] && [ -f "${2:-}" ] && [ "${3:-}" = "verify" ]; then
  echo "jsign: Unknown command 'verify'" >&2
  exit 1
fi
if [ "${1:-}" = "-jar" ] && [ -f "${2:-}" ] && [ "${3:-}" = "extract" ]; then
  last="${@: -1}"
  printf 'synthetic detached Authenticode fixture\n' > "$last.sig"
  printf '%s\n' "$*" > "$BLS_TEST_EXTRACT_LOG"
  exit 0
fi
if [ "${1:-}" = "-cp" ]; then
  printf '%s\n' "$*" > "$BLS_TEST_COMPAT_JAVA_LOG"
  exit 0
fi
exit 92
SH
chmod 755 "$TMP_ROOT/bin/java"

cat > "$TMP_ROOT/bin/javac" <<'SH'
#!/bin/bash
# Regression: macOS may expose javac while the legacy verifier source cannot
# compile against Jsign's minimized/shaded distribution JAR. The signing path
# must not depend on compiling Java source at build time.
exit 99
SH
chmod 755 "$TMP_ROOT/bin/javac"

cat > "$TMP_ROOT/bin/python3" <<'SH'
#!/bin/bash
set -euo pipefail
printf '%s\n' "$*" > "$BLS_TEST_COMPAT_PYTHON_LOG"
exit 0
SH
chmod 755 "$TMP_ROOT/bin/python3"

cat > "$TMP_ROOT/bin/uname" <<'SH'
#!/bin/bash
set -euo pipefail
if [ "${BLS_TEST_UNAME_DARWIN:-0}" = "1" ]; then
  echo "Darwin"
else
  /usr/bin/uname "$@"
fi
SH
chmod 755 "$TMP_ROOT/bin/uname"

cat > "$TMP_ROOT/bin/shasum" <<'SH'
#!/bin/bash
set -euo pipefail
file="${@: -1}"
if [ "${BLS_TEST_FORCE_BAD_HASH:-0}" = "1" ]; then
  printf '%064d  %s\n' 0 "$file"
else
  printf '%s  %s\n' "$BLS_TEST_EXPECTED_SHA256" "$file"
fi
SH
chmod 755 "$TMP_ROOT/bin/shasum"

# Regression origin: v2.10 had no bootstrap; v2.11 added Jsign 7.5 but its CLI has no verify command.
# This first probe preserves the pinned bootstrap invariant.
PROBE_OUT="$(
  PATH="$TMP_ROOT/bin:/usr/bin:/bin" \
  BLS_SIGNER_BACKEND=jsign \
  BLS_JSIGN_AUTO_DOWNLOAD=1 \
  BLS_JSIGN_CACHE_DIR="$TMP_ROOT/cache" \
  BLS_TEST_CURL_URL_LOG="$TMP_ROOT/curl-url.txt" \
  BLS_TEST_EXPECTED_SHA256="$EXPECTED_SHA256" \
  env -u BLS_JSIGN_JAR "$SIGNER" probe
)"
echo "$PROBE_OUT" | grep -Fx 'Signer backend: jsign' >/dev/null
echo "$PROBE_OUT" | grep -Fx 'Signer version: Jsign 7.5' >/dev/null
[ "$(cat "$TMP_ROOT/curl-url.txt")" = "$EXPECTED_URL" ]
[ -f "$TMP_ROOT/cache/jsign-7.5.jar" ]

# macOS default path must stay outside the source and use the user's Library cache.
mkdir -p "$TMP_ROOT/home"
rm -f "$TMP_ROOT/curl-url.txt"
MAC_PROBE_OUT="$(
  PATH="$TMP_ROOT/bin:/usr/bin:/bin" \
  HOME="$TMP_ROOT/home" \
  BLS_TEST_UNAME_DARWIN=1 \
  BLS_SIGNER_BACKEND=jsign \
  BLS_JSIGN_AUTO_DOWNLOAD=1 \
  BLS_TEST_CURL_URL_LOG="$TMP_ROOT/curl-url.txt" \
  BLS_TEST_EXPECTED_SHA256="$EXPECTED_SHA256" \
  env -u BLS_JSIGN_JAR -u BLS_JSIGN_CACHE_DIR "$SIGNER" probe
)"
echo "$MAC_PROBE_OUT" | grep -Fx 'Signer backend: jsign' >/dev/null
[ -f "$TMP_ROOT/home/Library/Caches/DNMTechLabs/BootLoaderSelector/jsign-7.5.jar" ]
[ "$(cat "$TMP_ROOT/curl-url.txt")" = "$EXPECTED_URL" ]

# A corrupt bootstrap download must be rejected and must never remain cached.
rm -f "$TMP_ROOT/cache/jsign-7.5.jar" "$TMP_ROOT/curl-url.txt"
set +e
PATH="$TMP_ROOT/bin:/usr/bin:/bin" \
BLS_SIGNER_BACKEND=jsign \
BLS_JSIGN_AUTO_DOWNLOAD=1 \
BLS_JSIGN_CACHE_DIR="$TMP_ROOT/cache" \
BLS_TEST_CURL_URL_LOG="$TMP_ROOT/curl-url.txt" \
BLS_TEST_EXPECTED_SHA256="$EXPECTED_SHA256" \
BLS_TEST_FORCE_BAD_HASH=1 \
env -u BLS_JSIGN_JAR "$SIGNER" probe >"$TMP_ROOT/bad.out" 2>&1
BAD_RC=$?
set -e
[ "$BAD_RC" -ne 0 ] || { echo "Corrupt bootstrap signer was accepted" >&2; exit 1; }
[ ! -e "$TMP_ROOT/cache/jsign-7.5.jar" ] || { echo "Corrupt bootstrap signer remained cached" >&2; exit 1; }
grep -E 'SHA-256|hash|digest' "$TMP_ROOT/bad.out" >/dev/null


printf 'synthetic jsign jar fixture\n' > "$TMP_ROOT/cache/jsign-7.5.jar"

# Jsign 7.5 signs successfully but has no `verify` command. The signer helper must
# detect that capability gap and route verification through the pinned Jsign
# library compatibility verifier instead of rejecting a valid signed EFI.
openssl req -new -x509 -newkey rsa:3072 -sha256 -nodes -days 1 \
  -subj '/CN=DNMTechLabs Bootstrap Test/O=DNMTechLabs' \
  -addext 'extendedKeyUsage=codeSigning' \
  -keyout "$TMP_ROOT/test-key.pem" -out "$TMP_ROOT/test-cert.pem" >/dev/null 2>&1
printf 'synthetic PE fixture for routing test\n' > "$TMP_ROOT/test.efi"
: > "$TMP_ROOT/compat-python.log"
: > "$TMP_ROOT/extract.log"
VERIFY_OUT="$(
  PATH="$TMP_ROOT/bin:/usr/bin:/bin" \
  BLS_SIGNER_BACKEND=jsign \
  BLS_JSIGN_JAR="$TMP_ROOT/cache/jsign-7.5.jar" \
  BLS_TEST_COMPAT_PYTHON_LOG="$TMP_ROOT/compat-python.log" \
  BLS_TEST_EXTRACT_LOG="$TMP_ROOT/extract.log" \
    "$SIGNER" verify "$TMP_ROOT/test.efi" "$TMP_ROOT/test-cert.pem"
)"
echo "$VERIFY_OUT" | grep -Fx 'Signature verification: PASS' >/dev/null
grep -F 'Verify-PeCoff-Authenticode.py' "$TMP_ROOT/compat-python.log" >/dev/null
grep -F 'extract --format DER' "$TMP_ROOT/extract.log" >/dev/null

echo "Ad-hoc signer bootstrap regression: PASS"
