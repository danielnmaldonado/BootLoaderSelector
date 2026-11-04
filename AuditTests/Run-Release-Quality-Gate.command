#!/bin/bash
set -euo pipefail

# Credits: ©DNMTechLabs / dnmtechlabs@gmail.com

cd "$(dirname "$0")/.."
PROJECT_ROOT="$PWD"
SRC="BootLoaderSelectorPkg/BootLoaderSelector"
REPEAT="${BLS_QUALITY_REPEAT:-3}"
BUILD_RELEASE="${BLS_QUALITY_BUILD:-1}"
INCS=(-I MdePkg/Include -I MdePkg/Include/X64 -I "$SRC")
CFLAGS=(-DNO_MSABI_VA_FUNCS -fshort-wchar -std=gnu11 -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wcast-align -Wundef -Wpointer-arith -Wwrite-strings -Wswitch-enum -Wimplicit-fallthrough)
source "$PROJECT_ROOT/AuditTests/Support/Compiler-Capabilities.sh"
QUALITY_WORK="$(mktemp -d "${TMPDIR:-/tmp}/bls-quality-gate.XXXXXX")"
trap 'rm -rf "$QUALITY_WORK"' EXIT
CLANG_COMPILER="$(bls_find_compiler clang || true)"
GNU_COMPILER="$(bls_find_compiler gnu || true)"
if [ -z "$CLANG_COMPILER" ] && [ -z "$GNU_COMPILER" ]; then
  echo "No supported Clang or GNU C compiler found" >&2
  exit 1
fi
echo "Compiler selection: Clang=${CLANG_COMPILER:-unavailable}; GNU GCC=${GNU_COMPILER:-unavailable}"

if ! [[ "$REPEAT" =~ ^[1-9][0-9]*$ ]]; then
  echo "BLS_QUALITY_REPEAT must be a positive integer" >&2
  exit 2
fi

CODE_VERSION="$(sed -n 's/^#define BLS_VERSION_ASCII "\([^"]*\)"/\1/p' "$SRC/Utils.h")"
TEXT_VERSION="$(sed -n 's/^#define BLS_VERSION_TEXT[[:space:]]*L"BootLoaderSelector v\([^"]*\)"/\1/p' "$SRC/Utils.h")"
INF_VERSION="$(sed -n 's/^[[:space:]]*VERSION_STRING[[:space:]]*=[[:space:]]*//p' "$SRC/BootLoaderSelector.inf")"
DSC_VERSION="$(sed -n 's/^[[:space:]]*PLATFORM_VERSION[[:space:]]*=[[:space:]]*//p' BootLoaderSelector.dsc)"
INFO_VERSION="$(sed -n 's/^Versão atual:[[:space:]]*//p' INFO.txt)"
CHANGELOG_VERSION="$(sed -n 's/^Versão atual:[[:space:]]*//p' CHANGELOG.txt)"
if [ -z "$CODE_VERSION" ] || [ -z "$TEXT_VERSION" ] ||
   [ "$CODE_VERSION" != "$TEXT_VERSION" ] || [ "$CODE_VERSION" != "$INF_VERSION" ] ||
   [ "$CODE_VERSION" != "$DSC_VERSION" ] || [ "$CODE_VERSION" != "$INFO_VERSION" ] ||
   [ "$CODE_VERSION" != "$CHANGELOG_VERSION" ]; then
  echo "Release version metadata mismatch: code=$CODE_VERSION text=$TEXT_VERSION inf=$INF_VERSION dsc=$DSC_VERSION info=$INFO_VERSION changelog=$CHANGELOG_VERSION" >&2
  exit 1
fi
echo "== Release metadata version: $CODE_VERSION =="

REQUIRED_CREDIT='©DNMTechLabs / dnmtechlabs@gmail.com'
CREDIT_FILES=(
  "$SRC"/*.c
  "$SRC"/*.h
  "$SRC/BootLoaderSelector.inf"
  BootLoaderSelector.dsc
  Build-BootLoaderSelector-Release.command
  Build-Debug-Full-Log.command
  AuditTests/Run-Audit-Tests.command
  AuditTests/Run-BaseTools-Deep-Tests.command
  AuditTests/Run-Release-Quality-Gate.command
  AuditTests/Test-AdHoc-Signing.command
  AuditTests/Test-Signer-Bootstrap.command
  AuditTests/Test-Authenticode-Verifier-Policy.command
  AuditTests/Test-Release-Publish-Invariants.command
  AuditTests/Support/*.sh
  AuditTools/Sign-PeCoff-AdHoc.command
  AuditTools/Verify-PeCoff-Authenticode.py
  AuditTests/Host/*.c
  AuditTests/UefiSmokeChild/*.c
  AuditTests/UefiSmokeChild/*.inf
  AuditReports/AUDIT-REPORT.txt
  SECURITY-NOTES.txt
  THIRD-PARTY-NOTICES.txt
  INFO.txt
  CHANGELOG.txt
)
for CREDIT_FILE in "${CREDIT_FILES[@]}"; do
  if ! grep -Fq "$REQUIRED_CREDIT" "$CREDIT_FILE"; then
    echo "Required project credit is missing from: $CREDIT_FILE" >&2
    exit 1
  fi
done
echo "== Project credits: PASS =="

# BaseLib already scopes -fno-tree-dse to the component that needs the GCC 10.2
# workaround. Disabling DSE in the platform DSC would silently pessimize every
# RELEASE translation unit, including BootLoaderSelector itself.
if grep -Fq -- '-fno-tree-dse' BootLoaderSelector.dsc; then
  echo "Release platform globally disables dead-store elimination" >&2
  exit 1
fi
if ! grep -Fq -- '-fno-tree-dse' MdePkg/Library/BaseLib/BaseLib.inf; then
  echo "Expected component-scoped BaseLib GCC DSE workaround is missing" >&2
  exit 1
fi
echo "== Release DSE scope: PASS =="

# Remembered autoboot must not probe/switch/clear the console before the direct
# chainload path decides to show selector UI. This prevents startup mode scans
# and blank intermediate frames from creeping back into the no-menu fast path.
MAIN_SOURCE="$SRC/Main.c"
CAPTURE_LINE="$(grep -n -m1 'BlsUiCaptureOriginalState();' "$MAIN_SOURCE" | cut -d: -f1 || true)"
HOTKEY_LINE="$(grep -n -m1 'if (!BlsWaitForSelectorHotkey())' "$MAIN_SOURCE" | cut -d: -f1 || true)"
FIRST_INIT_LINE="$(grep -n -m1 'BlsUiInitReadableMode();' "$MAIN_SOURCE" | cut -d: -f1 || true)"
if [ -z "$CAPTURE_LINE" ] || [ -z "$HOTKEY_LINE" ] || [ -z "$FIRST_INIT_LINE" ] ||
   [ "$CAPTURE_LINE" -ge "$HOTKEY_LINE" ] || [ "$FIRST_INIT_LINE" -le "$HOTKEY_LINE" ] ||
   ! grep -Fq 'if (UiModeInitialized) BlsUiClear();' "$MAIN_SOURCE"; then
  echo "Remembered-autoboot UI deferral invariant is missing" >&2
  exit 1
fi
echo "== Remembered-autoboot UI deferral: PASS =="

# Hardware fields originate in fixed BLS_MAX_TEXT buffers. Rendering must never
# infer a larger readable source merely because the firmware console is wider.
if ! grep -Fq 'BlsStrLen16Bounded(Value, BLS_MAX_TEXT)' "$SRC/Menu.c" ||
   grep -Fq "Value[I] != L'\\0' && MaxValue" "$SRC/Menu.c"; then
  echo "Hardware-card source bound invariant is missing" >&2
  exit 1
fi
echo "== Hardware-card source bound: PASS =="

# Provenance must be derived from the same validated release metadata. A stale
# numeric literal here previously allowed a successful build to claim an old
# version even while runtime/INF/DSC/INFO/CHANGELOG were synchronized.
if grep -Eq 'BootLoaderSelector v[0-9]+[.][0-9]+ build provenance' Build-BootLoaderSelector-Release.command ||
   ! grep -Fq 'BootLoaderSelector v${RELEASE_VERSION} build provenance' Build-BootLoaderSelector-Release.command; then
  echo "Release provenance header is not bound to validated RELEASE_VERSION" >&2
  exit 1
fi

# A canonical Release is not complete until the normalized PE/COFF payload is
# signed with a fresh ad-hoc certificate and both published EFI paths are
# verified against that certificate. Keep this as a source-level gate so a
# future refactor cannot silently return to publishing unsigned images.
SIGNING_HELPER="AuditTools/Sign-PeCoff-AdHoc.command"
if [ ! -x "$SIGNING_HELPER" ] ||
   ! grep -Fq 'Sign-PeCoff-AdHoc.command' Build-BootLoaderSelector-Release.command ||
   ! grep -Fq 'BootLoaderSelector-AdHoc.pem' Build-BootLoaderSelector-Release.command ||
   ! grep -Fq 'Signature verification: PASS' Build-BootLoaderSelector-Release.command; then
  echo "Canonical Release ad-hoc signing/verification invariant is missing" >&2
  exit 1
fi
if grep -Fq 'TestSigningPrivateKey.pem' "$SIGNING_HELPER"; then
  echo "Release signer references an EDK2 test private key" >&2
  exit 1
fi
echo "== Release PE/COFF signing invariant: PASS =="

# macOS CI must remain compatible with Jsign 7.x, where native `jsign verify`
# is unavailable. Use the project helper, which capability-probes
# native verification and falls back to certificate extraction + independent
# Authenticode verification when necessary.
CI_WORKFLOW=".github/workflows/uefi-ci.yml"
if grep -Eq '^[[:space:]]*jsign[[:space:]]+verify([[:space:]]|$)' "$CI_WORKFLOW"; then
  echo "macOS CI bypasses the Jsign 7.x-compatible verification helper" >&2
  exit 1
fi
CI_HELPER_VERIFY_COUNT="$(grep -Ec 'AuditTools/Sign-PeCoff-AdHoc[.]command[[:space:]]+verify([[:space:]]|$)' "$CI_WORKFLOW" || true)"
if [ "$CI_HELPER_VERIFY_COUNT" -lt 2 ]; then
  echo "macOS CI does not verify both published selector EFI paths through the compatibility helper" >&2
  exit 1
fi
echo "== macOS Jsign-compatible verification route: PASS =="

echo "== Authenticode certificate-policy regression =="
"$PROJECT_ROOT/AuditTests/Test-Authenticode-Verifier-Policy.command"
echo "== Authenticode positive self-signed leaf certificate regression =="
PYTHONDONTWRITEBYTECODE=1 python3 "$PROJECT_ROOT/AuditTests/Test-Authenticode-Certificate-Positive.py"

echo "== Ad-hoc signer bootstrap regression =="
"$PROJECT_ROOT/AuditTests/Test-Signer-Bootstrap.command"

echo "== Ad-hoc PE/COFF signing regression =="
"$PROJECT_ROOT/AuditTests/Test-AdHoc-Signing.command"

echo "== Release publish/failure regression =="
"$PROJECT_ROOT/AuditTests/Test-Release-Publish-Invariants.command"

run_runtime_tests() {
  local cc="$1"
  echo "== Runtime sanitizer capability + host tests: $cc x $REPEAT =="
  BLS_AUDIT_REPEAT="$REPEAT" CC="$cc" AuditTests/Run-Audit-Tests.command
  echo "== BaseTools deep bounds/fuzz: $cc =="
  CC="$cc" AuditTests/Run-BaseTools-Deep-Tests.command
}

if [ -n "$CLANG_COMPILER" ]; then
  run_runtime_tests "$CLANG_COMPILER"
else
  echo "SKIP Clang runtime/static-analysis matrix: compiler unavailable"
fi
if [ -n "$GNU_COMPILER" ]; then
  run_runtime_tests "$GNU_COMPILER"
else
  echo "SKIP GNU GCC runtime/static-analysis matrix: genuine GNU GCC unavailable"
fi

if [ -n "$CLANG_COMPILER" ]; then
  echo "== Runtime ASan/IntegerSan capability: $CLANG_COMPILER x 1 =="
  BLS_AUDIT_SANITIZER_MODE=integer BLS_AUDIT_REPEAT=1 CC="$CLANG_COMPILER" AuditTests/Run-Audit-Tests.command
else
  echo "SKIP IntegerSan matrix: Clang unavailable"
fi

for THREAD_COMPILER in "$CLANG_COMPILER" "$GNU_COMPILER"; do
  if [ -n "$THREAD_COMPILER" ]; then
    echo "== Runtime ThreadSanitizer capability: $THREAD_COMPILER x 1 =="
    BLS_AUDIT_SANITIZER_MODE=thread BLS_AUDIT_REPEAT=1 CC="$THREAD_COMPILER" AuditTests/Run-Audit-Tests.command
  fi
done

echo "== GCC optimized maybe-uninitialized regression =="
if [ -n "$GNU_COMPILER" ] &&
    bls_probe_compile "$GNU_COMPILER" "$QUALITY_WORK/maybe-uninitialized" -O2 -Wmaybe-uninitialized; then
  "$GNU_COMPILER" "${CFLAGS[@]}" "${INCS[@]}" -O2 -Wmaybe-uninitialized -c \
    "$SRC/HardwareInfo.c" -o "$QUALITY_WORK/HardwareInfo.o"
else
  echo "SKIP GNU optimized maybe-uninitialized regression: compiler/flag unavailable"
fi

echo "== Production strict compilation/static analysis =="
GNU_ANALYZER=0
CLANG_ANALYZER=0
if [ -n "$GNU_COMPILER" ]; then
  if bls_probe_compile "$GNU_COMPILER" "$QUALITY_WORK/gnu-analyzer" -fanalyzer; then
    GNU_ANALYZER=1
  else
    echo "SKIP GNU -fanalyzer: compilation probe rejected it"
    cat "$QUALITY_WORK/gnu-analyzer/compile.log"
  fi
fi
if [ -n "$CLANG_COMPILER" ]; then
  if bls_probe_clang_analyzer "$CLANG_COMPILER" "$QUALITY_WORK/clang-analyzer"; then
    CLANG_ANALYZER=1
  else
    echo "SKIP Clang analyzer: compilation probe rejected it"
    cat "$QUALITY_WORK/clang-analyzer/compile.log"
  fi
fi
for f in "$SRC"/*.c; do
  if [ -n "$CLANG_COMPILER" ]; then
    "$CLANG_COMPILER" "${CFLAGS[@]}" "${INCS[@]}" -fsyntax-only "$f"
    if [ "$CLANG_ANALYZER" -eq 1 ]; then
      "$CLANG_COMPILER" --analyze -Xanalyzer -analyzer-output=text \
        "${CFLAGS[@]}" "${INCS[@]}" "$f" -o "$QUALITY_WORK/clang-$(basename "$f").plist"
    fi
  fi
  if [ -n "$GNU_COMPILER" ]; then
    "$GNU_COMPILER" "${CFLAGS[@]}" "${INCS[@]}" -fsyntax-only "$f"
    if [ "$GNU_ANALYZER" -eq 1 ]; then
      "$GNU_COMPILER" "${CFLAGS[@]}" "${INCS[@]}" -fanalyzer -c "$f" \
        -o "$QUALITY_WORK/gnu-$(basename "$f").o"
    fi
  fi
done

echo "== Generated-residue check =="
test -z "$(find BaseTools/Source/C -type f \( -name '*.o' -o -name '*.d' -o -name '*.a' \) -print -quit)"
test ! -e BaseTools/Source/C/bin
test ! -e BaseTools/Source/C/libs
# Python bytecode/cache is generated state regardless of where a helper lives.
# Reject it project-wide so a post-audit py_compile/import cannot silently make
# the FULL-SOURCE tree fail the canonical manifest-coverage preflight.
test -z "$(find . -type d -name '__pycache__' -print -quit)"
test -z "$(find . -type f \( -name '*.pyc' -o -name '*.pyo' \) -print -quit)"
if ! grep -Fq 'clean_basetools_python_cache' Build-BootLoaderSelector-Release.command; then
  echo "Release builder does not purge generated BaseTools Python caches" >&2
  exit 1
fi

if [ "$BUILD_RELEASE" != "0" ]; then
  echo "== Canonical Release build =="
  "$PROJECT_ROOT/Build-BootLoaderSelector-Release.command"
else
  echo "Canonical Release build skipped by BLS_QUALITY_BUILD=0"
fi

echo "BootLoaderSelector release quality gate: PASS"
