#!/bin/bash
# Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd -P)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
HOST_TEST_DIR="$SCRIPT_DIR/Host"
source "$SCRIPT_DIR/Support/Compiler-Capabilities.sh"

if [ -n "${CC:-}" ]; then
  COMPILER="$CC"
elif command -v clang >/dev/null 2>&1; then
  COMPILER="$(command -v clang)"
elif command -v gcc >/dev/null 2>&1; then
  COMPILER="$(command -v gcc)"
else
  echo "No C compiler found (set CC or install clang/gcc)." >&2
  exit 1
fi

TMP_BASE="${TMPDIR:-/tmp}"
if [ ! -d "$TMP_BASE" ] || [ ! -w "$TMP_BASE" ]; then
  echo "TMPDIR is not a writable directory: $TMP_BASE" >&2
  exit 1
fi

AUDIT_REPEAT="${BLS_AUDIT_REPEAT:-1}"
SANITIZER_MODE="${BLS_AUDIT_SANITIZER_MODE:-undefined}"
case "$SANITIZER_MODE" in
  undefined)
    ;;
  integer)
    ;;
  thread)
    ;;
  *)
    echo "BLS_AUDIT_SANITIZER_MODE must be 'undefined', 'integer' or 'thread'." >&2
    exit 1
    ;;
esac
case "$AUDIT_REPEAT" in
  ''|*[!0-9]*)
    echo "BLS_AUDIT_REPEAT must be an integer from 1 to 1000." >&2
    exit 1
    ;;
esac
if [ "$AUDIT_REPEAT" -lt 1 ] || [ "$AUDIT_REPEAT" -gt 1000 ]; then
  echo "BLS_AUDIT_REPEAT must be in the range 1..1000." >&2
  exit 1
fi

WORK_DIR="$(mktemp -d "$TMP_BASE/BootLoaderSelector.AuditTests.XXXXXX")"
cleanup() {
  rm -rf "$WORK_DIR"
}
trap cleanup EXIT

if bls_configure_sanitizers "$COMPILER" "$SANITIZER_MODE" "$WORK_DIR/capabilities"; then
  SANITIZERS="$BLS_SANITIZERS"
else
  CAPABILITY_STATUS=$?
  if [ "$CAPABILITY_STATUS" -eq 2 ]; then
    echo "Audit matrix entry skipped; no application tests ran (mode=$SANITIZER_MODE)."
    exit 0
  fi
  exit "$CAPABILITY_STATUS"
fi

COMMON_FLAGS=(
  -DNO_MSABI_VA_FUNCS
  -fshort-wchar
  -std=gnu11
  -Wall
  -Wextra
  -Werror
  -Wshadow
  -Wconversion
  -Wsign-conversion
  -Wformat=2
  -Wstrict-prototypes
  -Wmissing-prototypes
  -ffunction-sections
  -fdata-sections
  -fsanitize="$SANITIZERS"
  -fno-omit-frame-pointer
  -I "$PROJECT_ROOT/MdePkg/Include"
  -I "$PROJECT_ROOT/MdePkg/Include/X64"
  -I "$PROJECT_ROOT/BootLoaderSelectorPkg/BootLoaderSelector"
)

if [ "$(uname -s)" = "Darwin" ]; then
  LINK_FLAGS=(-Wl,-dead_strip -fsanitize="$SANITIZERS")
else
  LINK_FLAGS=(-Wl,--gc-sections -fsanitize="$SANITIZERS")
fi

TESTS=(
  test_ui_mode_bound
  test_mp_topology_bound
  test_fs_handle_bound
  test_bootscan_policy
  test_remembered_autoboot_integrity
  test_boot_image_lifecycle
  test_watchdog_lifecycle
  test_event_lifecycle
  test_file_protocol_contracts
  test_protocol_access_contracts
  test_hotkey_timer_priority
  test_menu_countdown_starvation
  test_firmware_table_bounds
  test_config_parser
  test_config_lifecycle
  test_screenshot_bounds
  test_logger_lifecycle
  test_device_path_conversion
)

for TEST in "${TESTS[@]}"; do
  "$COMPILER" "${COMMON_FLAGS[@]}" \
    "$HOST_TEST_DIR/$TEST.c" \
    "${LINK_FLAGS[@]}" \
    -o "$WORK_DIR/$TEST"
  for ((RUN = 1; RUN <= AUDIT_REPEAT; RUN++)); do
    ASAN_OPTIONS="$BLS_ASAN_OPTIONS" \
    UBSAN_OPTIONS=halt_on_error=1 \
    TSAN_OPTIONS=halt_on_error=1 \
      "$WORK_DIR/$TEST"
  done
done

echo "All BootLoaderSelector audit tests passed (${#TESTS[@]} tests; $AUDIT_REPEAT run(s) per test; sanitizers=$SANITIZERS)."
