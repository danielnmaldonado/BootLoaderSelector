#!/bin/bash
set -euo pipefail

# Credits: ©DNMTechLabs / dnmtechlabs@gmail.com

cd "$(dirname "$0")/.."
PROJECT_ROOT="$PWD"
SIGNER="$PROJECT_ROOT/AuditTools/Sign-PeCoff-AdHoc.command"
VERSION="$(sed -n 's/^#define BLS_VERSION_ASCII "\([^"]*\)"/\1/p' BootLoaderSelectorPkg/BootLoaderSelector/Utils.h)"
[ -n "$VERSION" ] || { echo "Unable to resolve release version" >&2; exit 1; }

hash_file() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    echo "Required SHA-256 utility not found" >&2
    return 1
  fi
}

if [ ! -x "$SIGNER" ]; then
  echo "Ad-hoc PE/COFF signing helper is missing or not executable: $SIGNER" >&2
  exit 1
fi
for tool in clang lld-link file openssl; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Required signing regression tool not found: $tool" >&2
    exit 2
  fi
done

PROBE_OUTPUT="$("$SIGNER" probe)"
echo "$PROBE_OUTPUT" | grep -E '^Signer backend: (sbsign|jsign)$' >/dev/null
echo "$PROBE_OUTPUT" | grep -E '^Signer version: .+' >/dev/null
echo "$PROBE_OUTPUT" | grep -Fx 'Signature algorithm: SHA-256 with RSA-3072' >/dev/null

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/BootLoaderSelector.SigningTest.XXXXXX")"
cleanup() {
  rm -rf "$TMP_ROOT"
}
trap cleanup EXIT

cat > "$TMP_ROOT/min.c" <<'C_EOF'
typedef unsigned long long UINTN;
typedef void *EFI_HANDLE;
typedef struct { void *Reserved; } EFI_SYSTEM_TABLE;
__attribute__((ms_abi)) UINTN efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  (void)ImageHandle;
  (void)SystemTable;
  return 0;
}
C_EOF

clang --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector \
  -fshort-wchar -mno-red-zone -c "$TMP_ROOT/min.c" -o "$TMP_ROOT/min.obj"
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64 \
  /out:"$TMP_ROOT/min.efi" "$TMP_ROOT/min.obj"
file "$TMP_ROOT/min.efi" | grep -E 'PE32\+.*EFI.*application.*x86-64' >/dev/null

"$SIGNER" sign \
  "$TMP_ROOT/min.efi" \
  "$TMP_ROOT/min-signed.efi" \
  "$TMP_ROOT/adhoc-cert.pem" \
  "$VERSION" \
  "$TMP_ROOT/signing-metadata.txt"

[ -s "$TMP_ROOT/min-signed.efi" ]
[ -s "$TMP_ROOT/adhoc-cert.pem" ]
[ -s "$TMP_ROOT/signing-metadata.txt" ]
file "$TMP_ROOT/min-signed.efi" | grep -E 'PE32\+.*EFI.*application.*x86-64' >/dev/null
openssl x509 -in "$TMP_ROOT/adhoc-cert.pem" -noout -checkend 31536000 >/dev/null
if grep -Eq 'BEGIN ((RSA|EC) )?PRIVATE KEY' "$TMP_ROOT/adhoc-cert.pem"; then
  echo "Published ad-hoc certificate unexpectedly contains a private key" >&2
  exit 1
fi
grep -Fx 'Signature verification: PASS' "$TMP_ROOT/signing-metadata.txt" >/dev/null
grep -E '^Signer backend: (sbsign|jsign)$' "$TMP_ROOT/signing-metadata.txt" >/dev/null
grep -E '^Signer version: .+' "$TMP_ROOT/signing-metadata.txt" >/dev/null
grep -Fx 'Signature algorithm: SHA-256 with RSA-3072' "$TMP_ROOT/signing-metadata.txt" >/dev/null
grep -E '^Certificate SHA-256: [0-9A-Fa-f]{64}$' "$TMP_ROOT/signing-metadata.txt" >/dev/null
grep -Fx 'Private key published: no' "$TMP_ROOT/signing-metadata.txt" >/dev/null

"$SIGNER" verify "$TMP_ROOT/min-signed.efi" "$TMP_ROOT/adhoc-cert.pem"
cp "$TMP_ROOT/min-signed.efi" "$TMP_ROOT/BOOTX64.efi"
"$SIGNER" verify "$TMP_ROOT/BOOTX64.efi" "$TMP_ROOT/adhoc-cert.pem"
cmp -s "$TMP_ROOT/min-signed.efi" "$TMP_ROOT/BOOTX64.efi"

# Output/certificate/metadata targets must never alias the immutable input path.
# Without an explicit target-identity check, publishing the generated certificate
# over INPUT_EFI succeeds and silently destroys the caller's source image.
cp "$TMP_ROOT/min.efi" "$TMP_ROOT/alias-input.efi"
ALIAS_BEFORE="$(hash_file "$TMP_ROOT/alias-input.efi")"
set +e
"$SIGNER" sign \
  "$TMP_ROOT/alias-input.efi" \
  "$TMP_ROOT/alias-signed.efi" \
  "$TMP_ROOT/alias-input.efi" \
  "$VERSION" \
  "$TMP_ROOT/alias-metadata.txt" \
  >"$TMP_ROOT/alias.log" 2>&1
ALIAS_RC=$?
set -e
ALIAS_AFTER="$(hash_file "$TMP_ROOT/alias-input.efi")"
if [ "$ALIAS_RC" -eq 0 ]; then
  echo "Signing accepted PUBLIC_CERT_PEM aliasing INPUT_EFI" >&2
  cat "$TMP_ROOT/alias.log" >&2
  exit 1
fi
if [ "$ALIAS_BEFORE" != "$ALIAS_AFTER" ]; then
  echo "Signing alias rejection modified INPUT_EFI" >&2
  exit 1
fi
[ ! -e "$TMP_ROOT/alias-signed.efi" ]
[ ! -e "$TMP_ROOT/alias-metadata.txt" ]

# Publication is a three-file transaction: a late failure must not leave a new
# signed EFI paired with an old certificate/metadata (or destroy pre-existing
# destinations). Inject a one-shot failure on the certificate's final rename.
REAL_MV="$(command -v mv)"
mkdir -p "$TMP_ROOT/fake-mv-bin"
cat > "$TMP_ROOT/fake-mv-bin/mv" <<'SH_EOF'
#!/bin/bash
set -euo pipefail
last=""
for arg in "$@"; do last="$arg"; done
if [ -n "${BLS_FAIL_PUBLISH_DEST:-}" ] && [ "$last" = "$BLS_FAIL_PUBLISH_DEST" ] &&
   [ ! -e "$BLS_FAIL_ONCE_MARKER" ]; then
  : > "$BLS_FAIL_ONCE_MARKER"
  exit 97
fi
exec "$BLS_REAL_MV" "$@"
SH_EOF
chmod 755 "$TMP_ROOT/fake-mv-bin/mv"
printf 'preexisting signed output\n' > "$TMP_ROOT/rollback-output.efi"
printf 'preexisting certificate\n' > "$TMP_ROOT/rollback-cert.pem"
printf 'preexisting metadata\n' > "$TMP_ROOT/rollback-metadata.txt"
ROLLBACK_OUTPUT_BEFORE="$(hash_file "$TMP_ROOT/rollback-output.efi")"
ROLLBACK_CERT_BEFORE="$(hash_file "$TMP_ROOT/rollback-cert.pem")"
ROLLBACK_METADATA_BEFORE="$(hash_file "$TMP_ROOT/rollback-metadata.txt")"
set +e
PATH="$TMP_ROOT/fake-mv-bin:$PATH" \
BLS_REAL_MV="$REAL_MV" \
BLS_FAIL_PUBLISH_DEST="$TMP_ROOT/rollback-cert.pem" \
BLS_FAIL_ONCE_MARKER="$TMP_ROOT/mv-failed-once" \
  "$SIGNER" sign \
    "$TMP_ROOT/min.efi" \
    "$TMP_ROOT/rollback-output.efi" \
    "$TMP_ROOT/rollback-cert.pem" \
    "$VERSION" \
    "$TMP_ROOT/rollback-metadata.txt" \
    >"$TMP_ROOT/rollback.log" 2>&1
ROLLBACK_RC=$?
set -e
if [ "$ROLLBACK_RC" -eq 0 ]; then
  echo "Injected late publication failure unexpectedly succeeded" >&2
  cat "$TMP_ROOT/rollback.log" >&2
  exit 1
fi
[ -e "$TMP_ROOT/mv-failed-once" ] || { echo "Late publication failure was not injected" >&2; exit 1; }
[ "$ROLLBACK_OUTPUT_BEFORE" = "$(hash_file "$TMP_ROOT/rollback-output.efi")" ] || {
  echo "Failed signing transaction did not restore pre-existing EFI output" >&2
  exit 1
}
[ "$ROLLBACK_CERT_BEFORE" = "$(hash_file "$TMP_ROOT/rollback-cert.pem")" ] || {
  echo "Failed signing transaction modified pre-existing certificate" >&2
  exit 1
}
[ "$ROLLBACK_METADATA_BEFORE" = "$(hash_file "$TMP_ROOT/rollback-metadata.txt")" ] || {
  echo "Failed signing transaction modified pre-existing metadata" >&2
  exit 1
}

# The same rollback guarantee applies when all three publication targets were
# absent before the operation: a failed transaction must leave none behind.
rm -f "$TMP_ROOT/mv-failed-once"
set +e
PATH="$TMP_ROOT/fake-mv-bin:$PATH" \
BLS_REAL_MV="$REAL_MV" \
BLS_FAIL_PUBLISH_DEST="$TMP_ROOT/rollback-new-cert.pem" \
BLS_FAIL_ONCE_MARKER="$TMP_ROOT/mv-failed-once" \
  "$SIGNER" sign \
    "$TMP_ROOT/min.efi" \
    "$TMP_ROOT/rollback-new-output.efi" \
    "$TMP_ROOT/rollback-new-cert.pem" \
    "$VERSION" \
    "$TMP_ROOT/rollback-new-metadata.txt" \
    >"$TMP_ROOT/rollback-new.log" 2>&1
ROLLBACK_NEW_RC=$?
set -e
if [ "$ROLLBACK_NEW_RC" -eq 0 ]; then
  echo "Injected late publication failure for new targets unexpectedly succeeded" >&2
  cat "$TMP_ROOT/rollback-new.log" >&2
  exit 1
fi
[ ! -e "$TMP_ROOT/rollback-new-output.efi" ] || { echo "Failed transaction left new EFI output" >&2; exit 1; }
[ ! -e "$TMP_ROOT/rollback-new-cert.pem" ] || { echo "Failed transaction left new certificate" >&2; exit 1; }
[ ! -e "$TMP_ROOT/rollback-new-metadata.txt" ] || { echo "Failed transaction left new metadata" >&2; exit 1; }

# Cancellation must be terminal, preserve the conventional signal status and leave
# neither private-key workspace nor partially published outputs behind. Inject a
# SIGTERM from an OpenSSL wrapper exactly after the private workspace exists.
REAL_OPENSSL="$(command -v openssl)"
mkdir -p "$TMP_ROOT/fake-bin"
cat > "$TMP_ROOT/fake-bin/openssl" <<'SH_EOF'
#!/bin/bash
set -euo pipefail
if [ "${1:-}" = "req" ]; then
  keyout=""
  while [ "$#" -gt 0 ]; do
    if [ "$1" = "-keyout" ] && [ "$#" -ge 2 ]; then
      keyout="$2"
      break
    fi
    shift
  done
  [ -n "$keyout" ] || exit 98
  dirname "$keyout" > "$BLS_SIGNAL_MARKER"
  kill -TERM "$PPID"
  exit 99
fi
exec "$BLS_REAL_OPENSSL" "$@"
SH_EOF
chmod 755 "$TMP_ROOT/fake-bin/openssl"
set +e
PATH="$TMP_ROOT/fake-bin:$PATH" \
BLS_REAL_OPENSSL="$REAL_OPENSSL" \
BLS_SIGNAL_MARKER="$TMP_ROOT/signal-workspace.txt" \
  "$SIGNER" sign \
    "$TMP_ROOT/min.efi" \
    "$TMP_ROOT/cancelled.efi" \
    "$TMP_ROOT/cancelled-cert.pem" \
    "$VERSION" \
    "$TMP_ROOT/cancelled-metadata.txt" \
    >"$TMP_ROOT/cancel.log" 2>&1
CANCEL_RC=$?
set -e
if [ "$CANCEL_RC" -ne 143 ]; then
  echo "SIGTERM signing cancellation returned $CANCEL_RC instead of 143" >&2
  cat "$TMP_ROOT/cancel.log" >&2
  exit 1
fi
[ -s "$TMP_ROOT/signal-workspace.txt" ] || { echo "Cancellation workspace marker missing" >&2; exit 1; }
CANCEL_WORKSPACE="$(cat "$TMP_ROOT/signal-workspace.txt")"
[ ! -e "$CANCEL_WORKSPACE" ] || { echo "Signing workspace survived cancellation: $CANCEL_WORKSPACE" >&2; exit 1; }
[ ! -e "$TMP_ROOT/cancelled.efi" ]
[ ! -e "$TMP_ROOT/cancelled-cert.pem" ]
[ ! -e "$TMP_ROOT/cancelled-metadata.txt" ]

echo "Ad-hoc PE/COFF signing regression: PASS"
