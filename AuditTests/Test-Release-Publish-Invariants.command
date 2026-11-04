#!/bin/bash
set -euo pipefail
# Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
cd "$(dirname "$0")/.."
BUILDER=Build-BootLoaderSelector-Release.command
DEBUG_WRAPPER=Build-Debug-Full-Log.command

# Debug diagnostics share the canonical build log and must stay inside the
# Release builder's lock ownership window. The wrapper itself must never append.
grep -Fq 'BLS_DEBUG_DIAGNOSTICS=1 bash ./Build-BootLoaderSelector-Release.command' "$DEBUG_WRAPPER"
grep -Fq 'append_debug_diagnostics "$cleanup_status"' "$BUILDER"
! grep -Fq 'tee -a "$LOG"' "$DEBUG_WRAPPER"

# The signed staged release replaces the old one only after provenance is
# published. Previous Round71 accidentally deleted a valid Release even when
# an early metadata check aborted before a new image was compiled.
grep -Fq 'RELEASE_COMPLETE=0' "$BUILDER"
grep -Fq 'RELEASE_STAGE=' "$BUILDER"
grep -Fq 'publish_verified_release' "$BUILDER"
grep -Fq 'if ! publish_verified_release; then' "$BUILDER"
! grep -Fq 'rm -rf Conf Build Release ' "$BUILDER"
grep -Fq 'PROVENANCE_TMP=' "$BUILDER"
grep -Fq 'Unable to write release provenance' "$BUILDER"
grep -Fq 'Unable to publish release provenance' "$BUILDER"

promote_line="$(grep -n -m1 'if ! publish_verified_release; then' "$BUILDER" | cut -d: -f1)"
provenance_line="$(grep -n -m1 'mv -f "$PROVENANCE_TMP" "$RELEASE_STAGE/BUILD-PROVENANCE.txt"' "$BUILDER" | cut -d: -f1)"
[ -n "$promote_line" ] && [ -n "$provenance_line" ] && [ "$promote_line" -gt "$provenance_line" ] || {
  echo 'Release can be published before durable provenance' >&2
  exit 1
}

TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/bls-publish-regression.XXXXXX")"
trap 'rm -rf "$TEST_ROOT"' EXIT

# Bash 3.2 on macOS can lose definitions sourced from process substitution.
# Regular fixture files also let us reject a missing function before testing it.
load_builder_function() {
  local function_name function_file
  function_name="$1"
  function_file="$TEST_ROOT/$function_name.sh"
  sed -n "/^$function_name() {/,/^}/p" "$BUILDER" > "$function_file"
  source "$function_file"
  if ! declare -F "$function_name" >/dev/null; then
    echo "Unable to load builder function: $function_name" >&2
    exit 1
  fi
}

# Executable RED->GREEN regression: a failed preflight must not touch an
# existing fully published release. Use a minimal isolated fixture and force
# the builder to fail on missing version metadata before invoking build tools.
mkdir -p "$TEST_ROOT/preflight/Release"
printf 'known good\n' > "$TEST_ROOT/preflight/Release/BootLoaderSelector.efi"
cp "$BUILDER" "$TEST_ROOT/preflight/$BUILDER"
cp "$DEBUG_WRAPPER" "$TEST_ROOT/preflight/$DEBUG_WRAPPER"
if (cd "$TEST_ROOT/preflight" && bash "$BUILDER" >/dev/null 2>&1); then
  echo 'Incomplete preflight fixture unexpectedly passed' >&2
  exit 1
fi
[ "$(cat "$TEST_ROOT/preflight/Release/BootLoaderSelector.efi")" = 'known good' ] || {
  echo 'Failed build destroyed the previous Release' >&2
  exit 1
}
[ ! -e "$TEST_ROOT/preflight/.BlsBuild.lock" ] || {
  echo 'Build left a stale lock after preflight failure' >&2
  exit 1
}

# Concurrent invocation must reject the existing exclusive lock before
# overwriting its log or deleting build trees.
mkdir "$TEST_ROOT/preflight/.BlsBuild.lock"
printf 'previous log\n' > "$TEST_ROOT/preflight/BuildLogs/BootLoaderSelector-Build.log"
if (cd "$TEST_ROOT/preflight" && bash "$BUILDER" >/dev/null 2>&1); then
  echo 'Concurrent/stale build lock was ignored' >&2
  exit 1
fi
[ "$(cat "$TEST_ROOT/preflight/BuildLogs/BootLoaderSelector-Build.log")" = 'previous log' ]
[ -d "$TEST_ROOT/preflight/.BlsBuild.lock" ]

# RED on v2.30: when another Release already held the lock, the Release child
# failed immediately but the Debug wrapper still appended diagnostics into that
# other build's log. The wrapper must now leave the active log byte-identical.
if (cd "$TEST_ROOT/preflight" && bash "$DEBUG_WRAPPER" >/dev/null 2>&1); then
  echo 'Debug wrapper unexpectedly succeeded while build lock was held' >&2
  exit 1
fi
[ "$(cat "$TEST_ROOT/preflight/BuildLogs/BootLoaderSelector-Build.log")" = 'previous log' ] || {
  echo 'Debug wrapper modified another build run log after lock rejection' >&2
  exit 1
}
[ -d "$TEST_ROOT/preflight/.BlsBuild.lock" ]
rmdir "$TEST_ROOT/preflight/.BlsBuild.lock"

# Test the real publication function in isolation: on success the staged
# files replace the old files, without mixing paths or publishing stale data.
(
  load_builder_function publish_verified_release
  PROJECT_ROOT="$TEST_ROOT/publish"
  LOG="$TEST_ROOT/publish.log"
  RELEASE_BACKUP=""
  RELEASE_COMPLETE=0
  RELEASE_PUBLISHED=0
  mkdir -p "$PROJECT_ROOT/Release"
  printf 'old\n' > "$PROJECT_ROOT/Release/BootLoaderSelector.efi"
  RELEASE_STAGE="$(mktemp -d "$PROJECT_ROOT/.BlsReleaseStage.XXXXXX")"
  printf 'new\n' > "$RELEASE_STAGE/BootLoaderSelector.efi"
  publish_verified_release
  [ "$RELEASE_COMPLETE" -eq 1 ]
  [ "$RELEASE_PUBLISHED" -eq 1 ]
  [ -z "$RELEASE_STAGE" ]
  [ "$(cat "$PROJECT_ROOT/Release/BootLoaderSelector.efi")" = 'new' ]
  [ "$(cat "$RELEASE_BACKUP/BootLoaderSelector.efi")" = 'old' ]
)

# Inject failure specifically after moving the previous release aside. The
# actual cleanup function must restore that release and discard staging.
(
  load_builder_function publish_verified_release
  load_builder_function cleanup
  clean_basetools_python_cache() { :; }
  PROJECT_ROOT="$TEST_ROOT/rollback"
  LOG="$TEST_ROOT/rollback.log"
  TMP_DIR="" NORMALIZE_DIR="" NATIVE_TOOL_DIR="" TOOLCHAIN_VERIFY_MANIFEST=""
  PROJECT_FILE_LIST="" MANIFEST_FILE_LIST="" MANIFEST_COVERAGE_DIFF=""
  PROVENANCE_TMP="" BUILD_LOCK="$PROJECT_ROOT/.BlsBuild.lock"
  RELEASE_BACKUP="" RELEASE_COMPLETE=0 RELEASE_PUBLISHED=0
  mkdir -p "$PROJECT_ROOT/Release" "$BUILD_LOCK"
  printf 'last good\n' > "$PROJECT_ROOT/Release/BootLoaderSelector.efi"
  RELEASE_STAGE="$(mktemp -d "$PROJECT_ROOT/.BlsReleaseStage.XXXXXX")"
  printf 'unpublished\n' > "$RELEASE_STAGE/BootLoaderSelector.efi"
  mv() {
    if [ "$1" = "$RELEASE_STAGE" ]; then return 1; fi
    command mv "$@"
  }
  if publish_verified_release >/dev/null 2>&1; then
    echo 'Injected publish failure unexpectedly passed' >&2
    exit 1
  fi
  unset -f mv
  cleanup
  [ "$(cat "$PROJECT_ROOT/Release/BootLoaderSelector.efi")" = 'last good' ]
  [ ! -e "$BUILD_LOCK" ]
  [ ! -e "$RELEASE_STAGE" ]
)

# Cancel real Bash subprocesses after either directory rename, including a
# first publication with no previous Release. All fixtures use temporary files
# and the builder's actual publication, cleanup and signal handlers.
load_builder_function publish_verified_release
load_builder_function cleanup
load_builder_function handle_signal
grep -Fq "trap 'handle_signal 129' HUP" "$BUILDER"
grep -Fq "trap 'handle_signal 130' INT" "$BUILDER"
grep -Fq "trap 'handle_signal 143' TERM" "$BUILDER"
cat > "$TEST_ROOT/cancel.sh" <<'CANCEL_FIXTURE'
#!/bin/bash
set -euo pipefail
fixture_root="$1"
PROJECT_ROOT="$2"
cancel_signal="$3"
cancel_point="$4"
prior_release="$5"
source "$fixture_root/publish_verified_release.sh"
source "$fixture_root/cleanup.sh"
source "$fixture_root/handle_signal.sh"
declare -F publish_verified_release >/dev/null
declare -F cleanup >/dev/null
declare -F handle_signal >/dev/null
clean_basetools_python_cache() { printf 'cleanup\n' >> "$PROJECT_ROOT/cleanup-count"; }
LOG="$PROJECT_ROOT/publish.log"
TMP_DIR='' NORMALIZE_DIR='' NATIVE_TOOL_DIR='' TOOLCHAIN_VERIFY_MANIFEST=''
PROJECT_FILE_LIST='' MANIFEST_FILE_LIST='' MANIFEST_COVERAGE_DIFF=''
PROVENANCE_TMP='' BUILD_LOCK="$PROJECT_ROOT/.BlsBuild.lock"
RELEASE_BACKUP='' RELEASE_COMPLETE=0 RELEASE_PUBLISHED=0
mkdir -p "$BUILD_LOCK"
if [ "$prior_release" -eq 1 ]; then
  mkdir "$PROJECT_ROOT/Release"
  printf 'last good\n' > "$PROJECT_ROOT/Release/BootLoaderSelector.efi"
fi
RELEASE_STAGE="$(mktemp -d "$PROJECT_ROOT/.BlsReleaseStage.XXXXXX")"
STAGED_SOURCE="$RELEASE_STAGE"
printf 'unpublished\n' > "$RELEASE_STAGE/BootLoaderSelector.efi"
printf '%s\n' "$RELEASE_STAGE" > "$PROJECT_ROOT/stage-path"
trap cleanup EXIT
trap 'handle_signal 129' HUP
trap 'handle_signal 130' INT
trap 'handle_signal 143' TERM
mv() {
  local cancel_here=0
  command mv "$@" || return $?
  if [ "$cancel_point" = old ] && [ "$1" = "$PROJECT_ROOT/Release" ]; then
    cancel_here=1
  elif [ "$cancel_point" = stage ] && [ "$1" = "$STAGED_SOURCE" ]; then
    cancel_here=1
  fi
  if [ "$cancel_here" -eq 1 ]; then
    printf '%s\n' "$cancel_point" > "$PROJECT_ROOT/signal-point"
    kill -s "$cancel_signal" "$$"
    # A missing signal handler must not finish publication successfully.
    echo 'Cancellation did not stop publication' >&2
    return 1
  fi
}
publish_verified_release
CANCEL_FIXTURE

for publication_case in old:1 stage:1 stage:0; do
  cancel_point="${publication_case%:*}"
  prior_release="${publication_case#*:}"
  for signal_case in HUP:129 INT:130 TERM:143; do
    cancel_signal="${signal_case%:*}"
    expected_status="${signal_case#*:}"
    case_name="$cancel_point-prior$prior_release-$cancel_signal"
    cancel_root="$TEST_ROOT/cancel-$case_name"
    if /bin/bash "$TEST_ROOT/cancel.sh" "$TEST_ROOT" "$cancel_root" \
        "$cancel_signal" "$cancel_point" "$prior_release" \
        > "$TEST_ROOT/cancel-$case_name.log" 2>&1; then
      cancel_status=0
    else
      cancel_status=$?
    fi
    [ "$cancel_status" -eq "$expected_status" ] || {
      cat "$TEST_ROOT/cancel-$case_name.log" >&2
      echo "Cancellation $case_name returned $cancel_status; expected $expected_status" >&2
      exit 1
    }
    [ "$(cat "$cancel_root/signal-point")" = "$cancel_point" ]
    if [ "$prior_release" -eq 1 ]; then
      [ "$(cat "$cancel_root/Release/BootLoaderSelector.efi")" = 'last good' ]
    else
      [ ! -e "$cancel_root/Release" ] && [ ! -L "$cancel_root/Release" ]
    fi
    [ ! -e "$cancel_root/.BlsBuild.lock" ]
    [ ! -e "$(cat "$cancel_root/stage-path")" ]
    [ -z "$(find "$cancel_root" \( -name '.BlsReleaseBackup.*' -o -name '.BlsReleaseStage.*' \) -print -quit)" ]
    [ "$(wc -l < "$cancel_root/cleanup-count")" -eq 1 ]
  done
done

echo 'Release publish/rollback/lock/cancellation regressions: PASS (9 cancellation cases: HUP/INT/TERM after both renames, with/without prior Release)'
