#!/bin/bash
set -u

if ! cd "$(dirname "$0")"; then
  echo "Unable to enter the project directory" >&2
  exit 1
fi
PROJECT_ROOT="$PWD"
# Serialize all writes to the shared EDK2 build tree and canonical log.
BUILD_LOCK="$PROJECT_ROOT/.BlsBuild.lock"
if ! mkdir "$BUILD_LOCK" 2>/dev/null; then
  echo "Build already running or stale lock requires inspection: $BUILD_LOCK" >&2
  exit 1
fi
trap 'rmdir "$BUILD_LOCK" 2>/dev/null || true' EXIT
LOG_DIR="$PROJECT_ROOT/BuildLogs"
if [ -L "$LOG_DIR" ] || { [ -e "$LOG_DIR" ] && [ ! -d "$LOG_DIR" ]; }; then
  echo "Refusing unsafe build-log directory: $LOG_DIR" >&2
  exit 1
fi
if ! mkdir -p "$LOG_DIR"; then
  echo "Unable to create project build-log directory" >&2
  exit 1
fi
LOG="$LOG_DIR/BootLoaderSelector-Build.log"
if [ -L "$LOG" ] || { [ -e "$LOG" ] && [ ! -f "$LOG" ]; }; then
  echo "Refusing unsafe build log: $LOG" >&2
  exit 1
fi
if [ -e "$LOG" ] && [ -n "$(find "$LOG" -type f -links +1 -print -quit 2>/dev/null)" ]; then
  echo "Refusing multiply-linked build log: $LOG" >&2
  exit 1
fi
if ! : > "$LOG"; then
  echo "Unable to initialize project build log: $LOG" >&2
  exit 1
fi

# Credits: ©DNMTechLabs / dnmtechlabs@gmail.com

echo "BootLoaderSelector Release build" | tee "$LOG"
echo "Project: $PROJECT_ROOT" | tee -a "$LOG"
echo "Log: $LOG" | tee -a "$LOG"


BUILD_ROOT="$PROJECT_ROOT"
TMP_LINK=""
TMP_DIR=""
NORMALIZE_DIR=""
NATIVE_TOOL_DIR=""
TOOLCHAIN_VERIFY_MANIFEST=""
PROJECT_FILE_LIST=""
MANIFEST_FILE_LIST=""
MANIFEST_COVERAGE_DIFF=""
RELEASE_VERSION=""
SIGNING_HELPER="$PROJECT_ROOT/AuditTools/Sign-PeCoff-AdHoc.command"
SIGNER_BACKEND=""
SIGNER_VERSION=""
SIGNATURE_ALGORITHM=""
ADHOC_CERT_SHA256=""
PROVENANCE_TMP=""
RELEASE_COMPLETE=0
RELEASE_STAGE=""
RELEASE_BACKUP=""
RELEASE_PUBLISHED=0

# BaseTools invokes Python during host-tool builds and may leave __pycache__/pyc
# files inside the source tree. Those files are generated state, not release
# inputs: purge them both before manifest coverage and on exit so one build can
# never poison the next build or the FULL-SOURCE package.
clean_basetools_python_cache() {
  if [ ! -d "$PROJECT_ROOT/BaseTools" ]; then
    return 0
  fi
  find "$PROJECT_ROOT/BaseTools" -type d -name '__pycache__' -prune \
    -exec rm -rf {} + 2>/dev/null &&
  find "$PROJECT_ROOT/BaseTools" -type f \
    \( -name '*.pyc' -o -name '*.pyo' \) -exec rm -f {} + 2>/dev/null
}

log_optional_command() {
  local command_name
  command_name="$1"
  shift
  if command -v "$command_name" >/dev/null 2>&1; then
    "$command_name" "$@" 2>&1
  else
    echo "$command_name: not available on this host"
  fi
}

append_debug_diagnostics() {
  local exit_status error_summary
  exit_status="$1"
  {
    echo "=== DEBUG DIAGNOSTICS ==="
    echo "Project: $PROJECT_ROOT"
    echo "Log: $LOG"
    echo "=== SYSTEM ==="
    log_optional_command uname -a
    log_optional_command sw_vers
    log_optional_command xcodebuild -version
    log_optional_command clang --version
    log_optional_command python3 --version
    log_optional_command make --version | head -n 1
    echo "=== TOOLCHAIN CHECK ==="
    echo "External toolchain: ${BLS_TOOLCHAIN_DIR:-not configured; Linux uses native tools}"
    if [ -n "${BLS_TOOLCHAIN_DIR:-}" ] && [ -d "$BLS_TOOLCHAIN_DIR/cross/bin" ]; then
      file "$BLS_TOOLCHAIN_DIR/cross/bin/x86_64-clover-linux-gnu-gcc" 2>&1
      file "$BLS_TOOLCHAIN_DIR/cross/bin/x86_64-clover-linux-gnu-g++" 2>&1
    fi
  } | tee -a "$LOG"

  echo "=== ERROR SUMMARY ===" | tee -a "$LOG"
  error_summary="$(grep -nEi "error:|fatal error|undefined reference|(^|[[:space:]])ld:|No rule to make|cannot|failed|required native build tool not found|warning:.*error|build.py.*error" "$LOG" | tail -n 150 || true)"
  if [ -n "$error_summary" ]; then
    printf '%s\n' "$error_summary" | tee -a "$LOG"
  else
    echo "No matching errors found" | tee -a "$LOG"
  fi
  echo "Exit status: $exit_status" | tee -a "$LOG"
}

if [[ "$PROJECT_ROOT" == *" "* ]]; then
  TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/BootLoaderSelector.XXXXXX")" || exit 1
  TMP_LINK="$TMP_DIR/workspace"
  if ! ln -s "$PROJECT_ROOT" "$TMP_LINK"; then
    rm -rf "$TMP_DIR"
    echo "Unable to create path-safe workspace" | tee -a "$LOG"
    exit 1
  fi
  BUILD_ROOT="$TMP_LINK"
  echo "Using path-safe workspace: $BUILD_ROOT" | tee -a "$LOG"
fi

cleanup() {
  local cleanup_status=$?
  clean_basetools_python_cache >/dev/null 2>&1 || true
  if [ -n "$TMP_DIR" ]; then
    rm -rf "$TMP_DIR" 2>/dev/null || true
  fi
  if [ -n "$NORMALIZE_DIR" ]; then
    rm -rf "$NORMALIZE_DIR" 2>/dev/null || true
  fi
  if [ -n "$NATIVE_TOOL_DIR" ]; then
    rm -rf "$NATIVE_TOOL_DIR" 2>/dev/null || true
  fi
  if [ -n "$TOOLCHAIN_VERIFY_MANIFEST" ]; then
    rm -f "$TOOLCHAIN_VERIFY_MANIFEST" 2>/dev/null || true
  fi
  if [ -n "$PROJECT_FILE_LIST" ]; then
    rm -f "$PROJECT_FILE_LIST" 2>/dev/null || true
  fi
  if [ -n "$MANIFEST_FILE_LIST" ]; then
    rm -f "$MANIFEST_FILE_LIST" 2>/dev/null || true
  fi
  if [ -n "$MANIFEST_COVERAGE_DIFF" ]; then
    rm -f "$MANIFEST_COVERAGE_DIFF" 2>/dev/null || true
  fi
  if [ -n "$PROVENANCE_TMP" ]; then
    rm -f "$PROVENANCE_TMP" 2>/dev/null || true
  fi
  # Never delete an already-published Release after a failed preflight, compile
  # or signing operation. Only the new, private staging directory is disposable.
  if [ "$RELEASE_COMPLETE" -ne 1 ]; then
    # Publication intent is set before the atomic stage rename. A signal after
    # that rename must roll back even before the shell executes its next line.
    if [ "$RELEASE_PUBLISHED" -eq 1 ] && [ -d "$PROJECT_ROOT/Release" ]; then
      rm -rf "$PROJECT_ROOT/Release" 2>/dev/null || true
    fi
    if [ -n "$RELEASE_BACKUP" ] && [ -d "$RELEASE_BACKUP" ] &&
       [ ! -e "$PROJECT_ROOT/Release" ] && [ ! -L "$PROJECT_ROOT/Release" ]; then
      mv "$RELEASE_BACKUP" "$PROJECT_ROOT/Release" 2>/dev/null ||
        echo "WARNING: previous release is preserved at $RELEASE_BACKUP" >&2
    fi
  fi
  if [ -n "$RELEASE_STAGE" ] && [ -d "$RELEASE_STAGE" ]; then
    rm -rf "$RELEASE_STAGE" 2>/dev/null || true
  fi
  if [ "$RELEASE_COMPLETE" -eq 1 ] && [ -n "$RELEASE_BACKUP" ]; then
    rm -rf "$RELEASE_BACKUP" 2>/dev/null || true
  fi
  # Build-Debug-Full-Log.command requests diagnostics through this cleanup path
  # so every write to the canonical log remains serialized by .BlsBuild.lock.
  if [ "${BLS_DEBUG_DIAGNOSTICS:-0}" = "1" ]; then
    append_debug_diagnostics "$cleanup_status" || true
  fi
  rmdir "$BUILD_LOCK" 2>/dev/null || true
  return "$cleanup_status"
}
# Final publication is a directory replacement. It is attempted only after
# both signed images, the public certificate and provenance have all passed.
# The old Release is retained for rollback throughout the publication window.
publish_verified_release() {
  if [ -z "$RELEASE_STAGE" ] || [ ! -d "$RELEASE_STAGE" ] ||
     [ -L "$PROJECT_ROOT/Release" ] ||
     { [ -e "$PROJECT_ROOT/Release" ] && [ ! -d "$PROJECT_ROOT/Release" ]; }; then
    echo "Unsafe or missing Release publishing location" | tee -a "$LOG"
    return 1
  fi
  if [ -d "$PROJECT_ROOT/Release" ]; then
    RELEASE_BACKUP="$(mktemp -d "$PROJECT_ROOT/.BlsReleaseBackup.XXXXXX")" || return 1
    rmdir "$RELEASE_BACKUP" || return 1
    if ! mv "$PROJECT_ROOT/Release" "$RELEASE_BACKUP"; then
      echo "Unable to preserve previous Release" | tee -a "$LOG"
      return 1
    fi
  fi
  RELEASE_PUBLISHED=1
  if ! mv "$RELEASE_STAGE" "$PROJECT_ROOT/Release"; then
    echo "Unable to publish staged Release" | tee -a "$LOG"
    return 1
  fi
  RELEASE_STAGE=""
  RELEASE_COMPLETE=1
  return 0
}

handle_signal() {
  local signal_status="$1"
  # Exit via the common cleanup, retaining the cancellation status.
  trap '' HUP INT TERM
  exit "$signal_status"
}
trap cleanup EXIT
trap 'handle_signal 129' HUP
trap 'handle_signal 130' INT
trap 'handle_signal 143' TERM

cd "$BUILD_ROOT" || exit 1

export WORKSPACE="$BUILD_ROOT"
export EDK_TOOLS_PATH="$WORKSPACE/BaseTools"
export CONF_PATH="$WORKSPACE/Conf"
# Toolchain executables are external dependencies, not source distribution assets.
TOOLCHAIN_DIR="${BLS_TOOLCHAIN_DIR:-}"
export TOOLCHAIN_DIR
GCC53_BIN=""
NASM_PREFIX=""
TOOLCHAIN_MODE="native"
export PYTHON_COMMAND=python3
export PATH="$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH"

HOST_OS="$(uname -s 2>/dev/null || echo unknown)"
HOST_ARCH="$(uname -m 2>/dev/null || echo unknown)"
SOURCE_MANIFEST="AuditReports/MANIFEST.sha256"
MUTABLE_BUILD_LOG_MANIFEST_PATH="./BuildLogs/BootLoaderSelector-Build.log"
# Source and the independently pinned toolchain catalog are verified before use.
TOOLCHAIN_CATALOG="Toolchains/gcc102-snb.sha256"
EXPECTED_TOOLCHAIN_CATALOG_SHA256="53d8010aecd6ba67fa5f736f4c6f81006267f6d1ce15fbe419ad8aaa563647ff"
EXPECTED_GCC_SHA256="3a5bf61928867c6a728ce31e14ef29e7549454add9639066bb9bafadfa027f0d"
EXPECTED_GXX_SHA256="4b3cca2a933ceb7abccfe125fbdd96b32615f8260aa4f410d8b528ae1e20c949"
EXPECTED_NASM_SHA256="8811104c7520c0a9d60aaaba6ff577386c525fa7ffb237a613f91fbd6d1c1310"

hash_file() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    echo unavailable
  fi
}

run_signing_helper_logged() {
  local output status
  if output="$(bash "$SIGNING_HELPER" "$@" 2>&1)"; then
    status=0
  else
    status=$?
  fi
  if [ -n "$output" ]; then
    printf '%s\n' "$output" | tee -a "$LOG"
  fi
  return "$status"
}

verify_sha256_manifest() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 -c "$1" >/dev/null
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum -c "$1" >/dev/null
  else
    return 1
  fi
}

verify_release_version_metadata() {
  local code_version text_version inf_version dsc_version info_version changelog_version

  code_version="$(sed -n 's/^#define BLS_VERSION_ASCII "\([^"]*\)"/\1/p' BootLoaderSelectorPkg/BootLoaderSelector/Utils.h)"
  text_version="$(sed -n 's/^#define BLS_VERSION_TEXT[[:space:]]*L"BootLoaderSelector v\([^"]*\)"/\1/p' BootLoaderSelectorPkg/BootLoaderSelector/Utils.h)"
  inf_version="$(sed -n 's/^[[:space:]]*VERSION_STRING[[:space:]]*=[[:space:]]*//p' BootLoaderSelectorPkg/BootLoaderSelector/BootLoaderSelector.inf)"
  dsc_version="$(sed -n 's/^[[:space:]]*PLATFORM_VERSION[[:space:]]*=[[:space:]]*//p' BootLoaderSelector.dsc)"
  info_version="$(sed -n 's/^Versão atual:[[:space:]]*//p' INFO.txt)"
  changelog_version="$(sed -n 's/^Versão atual:[[:space:]]*//p' CHANGELOG.txt)"

  if [ -z "$code_version" ] || [ -z "$text_version" ] || [ -z "$inf_version" ] ||
     [ -z "$dsc_version" ] || [ -z "$info_version" ] || [ -z "$changelog_version" ]; then
    echo "Release version metadata is incomplete." | tee -a "$LOG"
    return 1
  fi
  if [ "$code_version" != "$text_version" ] || [ "$code_version" != "$inf_version" ] ||
     [ "$code_version" != "$dsc_version" ] || [ "$code_version" != "$info_version" ] ||
     [ "$code_version" != "$changelog_version" ]; then
    echo "Release version metadata mismatch: code=$code_version text=$text_version inf=$inf_version dsc=$dsc_version info=$info_version changelog=$changelog_version" | tee -a "$LOG"
    return 1
  fi
  RELEASE_VERSION="$code_version"
  echo "Release version metadata: $RELEASE_VERSION" | tee -a "$LOG"
}

if ! verify_release_version_metadata; then
  exit 1
fi

if ! command -v file >/dev/null 2>&1; then
  echo "Required host utility not found: file" | tee -a "$LOG"
  if [ "$HOST_OS" = "Darwin" ] && [ "${BLS_REVEAL_LOG:-0}" = "1" ]; then open -R "$LOG" 2>/dev/null || true; fi
  exit 1
fi
if [ ! -f "$SOURCE_MANIFEST" ]; then
  echo "Project SHA-256 manifest not found: $SOURCE_MANIFEST" | tee -a "$LOG"
  exit 1
fi
for REQUIRED_UTILITY in awk find sort comm; do
  if ! command -v "$REQUIRED_UTILITY" >/dev/null 2>&1; then
    echo "Required host utility not found: $REQUIRED_UTILITY" | tee -a "$LOG"
    exit 1
  fi
done
if awk -v p="$MUTABLE_BUILD_LOG_MANIFEST_PATH" '
  $2 == p || $2 == substr(p, 3) { found = 1 }
  END { exit(found ? 0 : 1) }
' "$SOURCE_MANIFEST"; then
  echo "Project SHA-256 manifest must exclude mutable build log: BuildLogs/BootLoaderSelector-Build.log" | tee -a "$LOG"
  exit 1
fi
GENERATED_MANIFEST_PATH="$(awk '
  {
    p = $2
    sub(/^[.][\/]/, "", p)
    if (p ~ /^(Build|Conf|Release)\// ||
        p ~ /^BaseTools\/Source\/C\/(bin|libs)\// ||
        p ~ /^BaseTools\/Source\/C\/.*[.](o|d)$/ ||
        p ~ /^BaseTools\/Source\/C\/VfrCompile\/Pccts\/(antlr\/antlr|dlg\/dlg)$/) {
      print $2
      exit
    }
  }
' "$SOURCE_MANIFEST")"
if [ -n "$GENERATED_MANIFEST_PATH" ]; then
  echo "Project SHA-256 manifest must exclude generated build outputs: $GENERATED_MANIFEST_PATH" | tee -a "$LOG"
  exit 1
fi

# Remove every known generated output before manifest coverage and tool preflight.
# This prevents a failed build from leaving stale Release artifacts that look current
# and makes repeated builds independent of host-generated BaseTools state.
if ! clean_basetools_python_cache; then
  echo "Unable to remove stale BaseTools Python caches" | tee -a "$LOG"
  exit 1
fi
if ! rm -rf Conf Build BaseTools/Source/C/bin BaseTools/Source/C/libs; then
  echo "Unable to remove previous generated outputs" | tee -a "$LOG"
  exit 1
fi
if ! find BaseTools/Source/C -type f \( -name "*.o" -o -name "*.d" \) -delete 2>/dev/null; then
  echo "Unable to remove stale BaseTools objects/dependencies" | tee -a "$LOG"
  exit 1
fi
if ! rm -f \
  BaseTools/Source/C/VfrCompile/Pccts/antlr/antlr \
  BaseTools/Source/C/VfrCompile/Pccts/dlg/dlg; then
  echo "Unable to remove stale BaseTools host helpers" | tee -a "$LOG"
  exit 1
fi
PROJECT_FILE_LIST="$(mktemp "${TMPDIR:-/tmp}/BootLoaderSelector.ProjectFiles.XXXXXX")" || exit 1
MANIFEST_FILE_LIST="$(mktemp "${TMPDIR:-/tmp}/BootLoaderSelector.ManifestFiles.XXXXXX")" || exit 1
MANIFEST_COVERAGE_DIFF="$(mktemp "${TMPDIR:-/tmp}/BootLoaderSelector.ManifestCoverage.XXXXXX")" || exit 1
IMMUTABLE_SYMLINK="$(find . \
  ! -path './.git' ! -path './.git/*' \
  ! -path './Build' ! -path './Build/*' \
  ! -path './Conf' ! -path './Conf/*' \
  ! -path './Release' ! -path './Release/*' \
  -type l -print -quit 2>/dev/null || true)"
if [ -n "$IMMUTABLE_SYMLINK" ]; then
  echo "Immutable project tree contains a symbolic link; refusing to build: $IMMUTABLE_SYMLINK" | tee -a "$LOG"
  exit 1
fi
if ! find . -type f \
     ! -path './.git' \
     ! -path './.git/*' \
     ! -path "./$SOURCE_MANIFEST" \
     ! -path "$MUTABLE_BUILD_LOG_MANIFEST_PATH" \
     ! -path "./Build/*" \
     ! -path "./Conf/*" \
     ! -path "./Release/*" \
     ! -name ".DS_Store" \
     -print | LC_ALL=C sort > "$PROJECT_FILE_LIST"; then
  echo "Unable to enumerate immutable project files for SHA-256 coverage verification." | tee -a "$LOG"
  exit 1
fi
if ! awk '
  NF != 2 { malformed = 1; next }
  { p=$2; if (substr(p, 1, 2) != "./") p="./" p; print p }
  END { exit(malformed ? 1 : 0) }
' "$SOURCE_MANIFEST" | LC_ALL=C sort > "$MANIFEST_FILE_LIST"; then
  echo "Project SHA-256 manifest has malformed entries; refusing to build." | tee -a "$LOG"
  exit 1
fi
if ! comm -3 "$MANIFEST_FILE_LIST" "$PROJECT_FILE_LIST" > "$MANIFEST_COVERAGE_DIFF"; then
  echo "Unable to compare project SHA-256 manifest coverage." | tee -a "$LOG"
  exit 1
fi
if [ -s "$MANIFEST_COVERAGE_DIFF" ]; then
  echo "Project SHA-256 manifest coverage mismatch; refusing to build untracked or stale source." | tee -a "$LOG"
  sed -n '1,40p' "$MANIFEST_COVERAGE_DIFF" | tee -a "$LOG"
  exit 1
fi
if ! verify_sha256_manifest "$SOURCE_MANIFEST"; then
  echo "Project source SHA-256 manifest verification failed; refusing to build drifted source." | tee -a "$LOG"
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 -c "$SOURCE_MANIFEST" 2>&1 | grep -v ': OK$' | tee -a "$LOG" || true
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum -c "$SOURCE_MANIFEST" 2>&1 | grep -v ': OK$' | tee -a "$LOG" || true
  fi
  exit 1
fi
if [ ! -f "$SIGNING_HELPER" ] || [ -L "$SIGNING_HELPER" ]; then
  echo "Release signing helper is missing or unsafe: $SIGNING_HELPER" | tee -a "$LOG"
  exit 1
fi
echo "Ad-hoc PE/COFF signer preflight:" | tee -a "$LOG"
if ! run_signing_helper_logged probe; then
  echo "Release requires a working PE/COFF signer/verifier; refusing an unsigned Release." | tee -a "$LOG"
  exit 1
fi
TOOLCHAIN_CATALOG_SHA256="$(hash_file "$TOOLCHAIN_CATALOG")"
if [ "$TOOLCHAIN_CATALOG_SHA256" != "$EXPECTED_TOOLCHAIN_CATALOG_SHA256" ]; then
  echo "Toolchain catalog SHA-256 mismatch; refusing to execute build tools." | tee -a "$LOG"
  exit 1
fi
if [ "$HOST_OS" = "Darwin" ]; then
  if [ -z "$TOOLCHAIN_DIR" ] || [ ! -d "$TOOLCHAIN_DIR" ] || [ -L "$TOOLCHAIN_DIR" ]; then
    echo "macOS requires BLS_TOOLCHAIN_DIR pointing to the original verified cross-toolchain." | tee -a "$LOG"
    echo "See Toolchains/README.md; no unverified toolchain is downloaded or redistributed." | tee -a "$LOG"
    exit 1
  fi
  TOOLCHAIN_DIR="$(cd "$TOOLCHAIN_DIR" && pwd -P)" || exit 1
  if [[ "$TOOLCHAIN_DIR" == *" "* ]]; then
    echo "BLS_TOOLCHAIN_DIR must use a path without spaces for the EDK2 compiler prefix." | tee -a "$LOG"
    exit 1
  fi
  TOOLCHAIN_SYMLINK="$(find "$TOOLCHAIN_DIR" -type l -print -quit)"
  if [ -n "$TOOLCHAIN_SYMLINK" ]; then
    echo "External toolchain must not contain symbolic links." | tee -a "$LOG"
    exit 1
  fi
  if ! (cd "$TOOLCHAIN_DIR" && find . -type f -print | sed 's|^./||' | LC_ALL=C sort) > "$PROJECT_FILE_LIST" ||
     ! awk '{print $2}' "$TOOLCHAIN_CATALOG" | LC_ALL=C sort > "$MANIFEST_FILE_LIST" ||
     ! comm -3 "$MANIFEST_FILE_LIST" "$PROJECT_FILE_LIST" > "$MANIFEST_COVERAGE_DIFF"; then
    echo "Unable to enumerate external toolchain inputs." | tee -a "$LOG"
    exit 1
  fi
  if [ -s "$MANIFEST_COVERAGE_DIFF" ]; then
    echo "External toolchain file set differs from the pinned catalog." | tee -a "$LOG"
    exit 1
  fi
  if ! (cd "$TOOLCHAIN_DIR" && verify_sha256_manifest "$PROJECT_ROOT/$TOOLCHAIN_CATALOG"); then
    echo "External toolchain verification failed; refusing to execute it." | tee -a "$LOG"
    exit 1
  fi
  export GCC53_BIN="$TOOLCHAIN_DIR/cross/bin/x86_64-clover-linux-gnu-"
  export NASM_PREFIX="$TOOLCHAIN_DIR/bin/"
  TOOLCHAIN_MODE="external-verified"
  GCC_SHA256="$(hash_file "${GCC53_BIN}gcc")"
  GXX_SHA256="$(hash_file "${GCC53_BIN}g++")"
  NASM_SHA256="$(hash_file "${NASM_PREFIX}nasm")"
  if [ "$GCC_SHA256" != "$EXPECTED_GCC_SHA256" ] ||
     [ "$GXX_SHA256" != "$EXPECTED_GXX_SHA256" ] ||
     [ "$NASM_SHA256" != "$EXPECTED_NASM_SHA256" ]; then
    echo "Critical external compiler/NASM SHA-256 mismatch." | tee -a "$LOG"
    exit 1
  fi
else
  if [ "$HOST_OS" != "Linux" ]; then
    echo "Supported build hosts are Linux and macOS." | tee -a "$LOG"
    exit 1
  fi
  for NATIVE_TOOL in gcc g++ gcc-ar objcopy nasm; do
    if ! command -v "$NATIVE_TOOL" >/dev/null 2>&1; then
      echo "Required native build tool not found: $NATIVE_TOOL" | tee -a "$LOG"
      exit 1
    fi
  done
  NATIVE_TOOL_DIR="$(mktemp -d "${TMPDIR:-/tmp}/BootLoaderSelector.NativeTools.XXXXXX")" || exit 1
  for NATIVE_TOOL in gcc g++ gcc-ar objcopy; do
    ln -s "$(command -v "$NATIVE_TOOL")" "$NATIVE_TOOL_DIR/x86_64-clover-linux-gnu-$NATIVE_TOOL" || exit 1
  done
  ln -s "$(command -v nasm)" "$NATIVE_TOOL_DIR/nasm" || exit 1
  export GCC53_BIN="$NATIVE_TOOL_DIR/x86_64-clover-linux-gnu-"
  export NASM_PREFIX="$NATIVE_TOOL_DIR/"
  GCC_SHA256="$(hash_file "${GCC53_BIN}gcc")"
  GXX_SHA256="$(hash_file "${GCC53_BIN}g++")"
  NASM_SHA256="$(hash_file "${NASM_PREFIX}nasm")"
fi
TOOLCHAIN_FILE_INFO="$(file "${GCC53_BIN}gcc")"
TOOLCHAIN_CXX_FILE_INFO="$(file "${GCC53_BIN}g++")"
NASM_FILE_INFO="$(file "${NASM_PREFIX}nasm")"
echo "Host OS: $HOST_OS" | tee -a "$LOG"
echo "Host architecture: $HOST_ARCH" | tee -a "$LOG"
echo "Toolchain mode: $TOOLCHAIN_MODE" | tee -a "$LOG"
echo "Toolchain catalog SHA-256: $TOOLCHAIN_CATALOG_SHA256" | tee -a "$LOG"
if ! "${GCC53_BIN}gcc" --version >/dev/null 2>&1 ||
   ! "${GCC53_BIN}g++" --version >/dev/null 2>&1 ||
   ! "${NASM_PREFIX}nasm" -v >/dev/null 2>&1; then
  echo "Compiler/NASM preflight failed; Apple Silicon may require Rosetta 2." | tee -a "$LOG"
  exit 1
fi

if ! mkdir -p Conf; then
  echo "Unable to create build output directories" | tee -a "$LOG"
  exit 1
fi
# Stage every EFI payload, certificate and provenance file privately, on the
# same filesystem as Release, so errors cannot destroy a previous good build.
RELEASE_STAGE="$(mktemp -d "$PROJECT_ROOT/.BlsReleaseStage.XXXXXX")" || exit 1

source ./edksetup.sh > >(tee -a "$LOG") 2>&1
STATUS=$?
if [ "$STATUS" -ne 0 ]; then
  echo "EDK2 environment setup failed" | tee -a "$LOG"
  if [ "$HOST_OS" = "Darwin" ] && [ "${BLS_REVEAL_LOG:-0}" = "1" ]; then open -R "$LOG" 2>/dev/null || true; fi
  exit "$STATUS"
fi
export PATH="$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH"
make -C BaseTools 2>&1 | tee -a "$LOG"
STATUS=${PIPESTATUS[0]}
if [ "$STATUS" -ne 0 ]; then
  echo "BaseTools build failed" | tee -a "$LOG"
  if [ "$HOST_OS" = "Darwin" ] && [ "${BLS_REVEAL_LOG:-0}" = "1" ]; then open -R "$LOG" 2>/dev/null || true; fi
  exit "$STATUS"
fi

MAKE_PATH="$(command -v make 2>/dev/null || true)"
PYTHON_PATH="$(command -v "$PYTHON_COMMAND" 2>/dev/null || true)"
GENFW_PATH="$(command -v GenFw 2>/dev/null || true)"
MAKE_VERSION="$(make --version 2>/dev/null | head -1 || true)"
PYTHON_VERSION="$($PYTHON_COMMAND --version 2>&1 || true)"
GENFW_SHA256="unavailable"
SOURCE_MANIFEST_SHA256="unavailable"
if [ -n "$GENFW_PATH" ] && [ -f "$GENFW_PATH" ]; then
  GENFW_SHA256="$(hash_file "$GENFW_PATH")"
fi
if [ -f AuditReports/MANIFEST.sha256 ]; then
  SOURCE_MANIFEST_SHA256="$(hash_file AuditReports/MANIFEST.sha256)"
fi

build -a X64 -b RELEASE -t GCC53 -p BootLoaderSelector.dsc -n 1 2>&1 | tee -a "$LOG"
STATUS=${PIPESTATUS[0]}

if [ "$STATUS" -eq 0 ]; then
  EFI_PATH=$(find Build/BootLoaderSelector/RELEASE_GCC53/X64 -name 'BootLoaderSelector.efi' -print -quit 2>/dev/null)
  if [ -n "$EFI_PATH" ]; then
    DLL_PATH=$(find Build/BootLoaderSelector/RELEASE_GCC53/X64 \
      -path '*/BootLoaderSelector/DEBUG/BootLoaderSelector.dll' -print -quit 2>/dev/null)
    if [ -n "$DLL_PATH" ]; then
      DLL_PATH="$PWD/$DLL_PATH"
    fi
    NORMALIZE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/BootLoaderSelector.Normalize.XXXXXX")" || STATUS=1
    SIGNING_METADATA="$NORMALIZE_DIR/signing-metadata.txt"
    ADHOC_CERT="$RELEASE_STAGE/BootLoaderSelector-AdHoc.pem"
    if [ "$STATUS" -ne 0 ] || [ ! -s "$DLL_PATH" ]; then
      echo "Unable to locate release linker image for normalization" | tee -a "$LOG"
      STATUS=1
    elif ! ln -s "$DLL_PATH" "$NORMALIZE_DIR/BootLoaderSelector.dll"; then
      echo "Unable to stage release linker image for normalization" | tee -a "$LOG"
      STATUS=1
    elif ! GenFw -e UEFI_APPLICATION \
              -o "$NORMALIZE_DIR/BootLoaderSelector.efi" \
              "$NORMALIZE_DIR/BootLoaderSelector.dll"; then
      echo "Unable to regenerate normalized release image" | tee -a "$LOG"
      STATUS=1
    elif ! GenFw -z -r "$NORMALIZE_DIR/BootLoaderSelector.efi"; then
      echo "Unable to normalize release debug metadata" | tee -a "$LOG"
      STATUS=1
    elif ! run_signing_helper_logged sign \
              "$NORMALIZE_DIR/BootLoaderSelector.efi" \
              "$RELEASE_STAGE/BootLoaderSelector.efi" \
              "$ADHOC_CERT" \
              "$RELEASE_VERSION" \
              "$SIGNING_METADATA"; then
      echo "Release PE/COFF ad-hoc signing failed" | tee -a "$LOG"
      STATUS=1
    elif ! cp -f "$RELEASE_STAGE/BootLoaderSelector.efi" "$RELEASE_STAGE/BOOTX64.efi"; then
      echo "Signed BOOTX64.EFI copy failed" | tee -a "$LOG"
      STATUS=1
    elif ! run_signing_helper_logged verify "$RELEASE_STAGE/BootLoaderSelector.efi" "$ADHOC_CERT" ||
         ! run_signing_helper_logged verify "$RELEASE_STAGE/BOOTX64.efi" "$ADHOC_CERT"; then
      echo "Published Release signature verification failed" | tee -a "$LOG"
      STATUS=1
    elif [ -s "$RELEASE_STAGE/BootLoaderSelector.efi" ] &&
         [ -s "$RELEASE_STAGE/BOOTX64.efi" ] && [ -s "$ADHOC_CERT" ] && [ -s "$SIGNING_METADATA" ]; then
      EFI_FILE_INFO="$(file "$RELEASE_STAGE/BootLoaderSelector.efi" 2>/dev/null || true)"
      SIGNER_BACKEND="$(sed -n 's/^Signer backend: //p' "$SIGNING_METADATA" | head -1)"
      SIGNER_VERSION="$(sed -n 's/^Signer version: //p' "$SIGNING_METADATA" | head -1)"
      SIGNATURE_ALGORITHM="$(sed -n 's/^Signature algorithm: //p' "$SIGNING_METADATA" | head -1)"
      ADHOC_CERT_SHA256="$(hash_file "$ADHOC_CERT")"
      METADATA_CERT_SHA256="$(sed -n 's/^Certificate SHA-256: //p' "$SIGNING_METADATA" | head -1)"
      if [[ "$EFI_FILE_INFO" != *"PE32+"* || "$EFI_FILE_INFO" != *"EFI"* || "$EFI_FILE_INFO" != *"x86-64"* ]]; then
        echo "Release image format validation failed: $EFI_FILE_INFO" | tee -a "$LOG"
        STATUS=1
      elif ! cmp -s "$RELEASE_STAGE/BootLoaderSelector.efi" "$RELEASE_STAGE/BOOTX64.efi"; then
        echo "Release EFI copies are not byte-identical" | tee -a "$LOG"
        STATUS=1
      elif [ -z "$SIGNER_BACKEND" ] || [ -z "$SIGNER_VERSION" ] || [ "$SIGNATURE_ALGORITHM" != "SHA-256 with RSA-3072" ] ||
           [ "$ADHOC_CERT_SHA256" = "unavailable" ] || [ "$ADHOC_CERT_SHA256" != "$METADATA_CERT_SHA256" ] ||
           ! grep -Fxq 'Signature verification: PASS' "$SIGNING_METADATA" ||
           ! grep -Fxq 'Private key published: no' "$SIGNING_METADATA"; then
        echo "Release signing metadata validation failed" | tee -a "$LOG"
        STATUS=1
      elif grep -Eq 'BEGIN ([A-Z0-9]+ )*PRIVATE KEY' "$ADHOC_CERT"; then
        echo "Release public certificate unexpectedly contains private key material" | tee -a "$LOG"
        STATUS=1
      fi
    else
      echo "Signed Release artifacts are incomplete" | tee -a "$LOG"
      STATUS=1
    fi
    if [ "$STATUS" -eq 0 ] && ! cp -R "$PROJECT_ROOT/LICENSE" "$PROJECT_ROOT/LICENSES" \
         "$PROJECT_ROOT/THIRD-PARTY-NOTICES.txt" "$RELEASE_STAGE/"; then
      echo "Unable to stage binary distribution license notices" | tee -a "$LOG"
      STATUS=1
    fi
    if [ "$STATUS" -eq 0 ] && [ -s "$RELEASE_STAGE/BootLoaderSelector.efi" ] && [ -s "$RELEASE_STAGE/BOOTX64.efi" ]; then
      chmod 644 "$RELEASE_STAGE/BootLoaderSelector.efi" "$RELEASE_STAGE/BOOTX64.efi" "$ADHOC_CERT" 2>/dev/null || true
      EFI_SHA256="$(hash_file "$RELEASE_STAGE/BootLoaderSelector.efi")"
      BOOTX64_SHA256="$(hash_file "$RELEASE_STAGE/BOOTX64.efi")"
      PROVENANCE_TMP="$(mktemp "$RELEASE_STAGE/.BUILD-PROVENANCE.XXXXXX")" || STATUS=1
      if [ "$STATUS" -ne 0 ]; then
        echo "Unable to stage release provenance" | tee -a "$LOG"
      elif ! {
        echo "BootLoaderSelector v${RELEASE_VERSION} build provenance"
        echo "Host OS: $HOST_OS"
        echo "Host architecture: $HOST_ARCH"
        echo "Toolchain mode: $TOOLCHAIN_MODE"
        echo "Source manifest SHA-256: $SOURCE_MANIFEST_SHA256"
        echo "Toolchain catalog SHA-256: $TOOLCHAIN_CATALOG_SHA256"
        echo "Compiler: $TOOLCHAIN_FILE_INFO"
        echo "Compiler SHA-256: $GCC_SHA256"
        echo "C++ compiler: $TOOLCHAIN_CXX_FILE_INFO"
        echo "C++ compiler SHA-256: $GXX_SHA256"
        echo "NASM: $NASM_FILE_INFO"
        echo "NASM SHA-256: $NASM_SHA256"
        echo "make: $MAKE_PATH"
        echo "make version: $MAKE_VERSION"
        echo "Python: $PYTHON_PATH"
        echo "Python version: $PYTHON_VERSION"
        echo "GenFw: $GENFW_PATH"
        echo "GenFw SHA-256: $GENFW_SHA256"
        echo "Build command: build -a X64 -b RELEASE -t GCC53 -p BootLoaderSelector.dsc -n 1"
        echo "EFI format: $EFI_FILE_INFO"
        echo "Signer backend: $SIGNER_BACKEND"
        echo "Signer version: $SIGNER_VERSION"
        echo "Signature algorithm: $SIGNATURE_ALGORITHM"
        echo "Ad-hoc certificate: BootLoaderSelector-AdHoc.pem"
        echo "Ad-hoc certificate SHA-256: $ADHOC_CERT_SHA256"
        echo "Signature verification: PASS"
        echo "Private key published: no"
        echo "BootLoaderSelector.efi SHA-256: $EFI_SHA256"
        echo "BOOTX64.efi SHA-256: $BOOTX64_SHA256"
        echo "EFI copies identical: yes"
      } > "$PROVENANCE_TMP"; then
        echo "Unable to write release provenance" | tee -a "$LOG"
        STATUS=1
      elif ! chmod 644 "$PROVENANCE_TMP" ||
           ! mv -f "$PROVENANCE_TMP" "$RELEASE_STAGE/BUILD-PROVENANCE.txt"; then
        echo "Unable to publish release provenance" | tee -a "$LOG"
        STATUS=1
      else
        PROVENANCE_TMP=""
        if ! publish_verified_release; then
          echo "Verified Release staging could not be published" | tee -a "$LOG"
          STATUS=1
        else
          echo "BUILD OK" | tee -a "$LOG"
        fi
        if [ "$STATUS" -eq 0 ]; then
          echo "Output: $PROJECT_ROOT/Release/BootLoaderSelector.efi" | tee -a "$LOG"
          echo "Output: $PROJECT_ROOT/Release/BOOTX64.efi" | tee -a "$LOG"
          echo "Ad-hoc certificate: $PROJECT_ROOT/Release/BootLoaderSelector-AdHoc.pem" | tee -a "$LOG"
          echo "Signature verification: PASS" | tee -a "$LOG"
        fi
      fi
    elif [ "$STATUS" -eq 0 ]; then
      echo "Release copy/signing failed" | tee -a "$LOG"
      STATUS=1
    fi
  else
    echo "Build finished but BootLoaderSelector.efi was not found" | tee -a "$LOG"
    STATUS=1
  fi
else
  echo "BUILD FAILED" | tee -a "$LOG"
fi

if [ "$HOST_OS" = "Darwin" ] && [ "${BLS_REVEAL_LOG:-0}" = "1" ]; then open -R "$LOG" 2>/dev/null || true; fi
exit "$STATUS"
