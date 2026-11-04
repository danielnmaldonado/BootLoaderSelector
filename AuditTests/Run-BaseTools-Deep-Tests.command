#!/bin/bash
# Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd -P)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
SOURCE_C_ROOT="$PROJECT_ROOT/BaseTools/Source/C"
HOST_TEST_DIR="$SCRIPT_DIR/Host"
source "$SCRIPT_DIR/Support/Compiler-Capabilities.sh"
COMPILER="${CC:-$(command -v clang || command -v gcc)}"

if [ -z "$COMPILER" ]; then
  echo "No C compiler found." >&2
  exit 1
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/BootLoaderSelector.BaseToolsDeep.XXXXXX")"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

bls_configure_sanitizers "$COMPILER" undefined "$WORK_DIR/capabilities"
SANITIZERS="$BLS_SANITIZERS"

# BaseTools makefiles emit .o/.d archives and host binaries next to their source.
# Build the deep-test dependency graph in a private copy so running the audit
# can never dirty the immutable project tree or poison a later Release build.
C_ROOT="$WORK_DIR/C"
mkdir -p "$C_ROOT"
cp -R "$SOURCE_C_ROOT/." "$C_ROOT/"

COMMON_FLAGS=(
  -std=gnu11 -fshort-wchar -fno-strict-aliasing
  -Wall -Wextra -Wpedantic -Werror -Wshadow -Wformat=2
  -fsanitize="$SANITIZERS" -fno-omit-frame-pointer
  -I "$C_ROOT" -I "$C_ROOT/Include/Common" -I "$C_ROOT/Include"
  -I "$C_ROOT/Include/IndustryStandard" -I "$C_ROOT/Common" -I "$C_ROOT/Include/X64"
)
LINK_FLAGS=(-fsanitize="$SANITIZERS" -L "$C_ROOT/libs" -lCommon)

# Discard copied build products so every compiler matrix entry rebuilds Common.
make -C "$C_ROOT/Common" BUILD_CC="$COMPILER" clean >/dev/null
make -C "$C_ROOT/Common" BUILD_CC="$COMPILER" >/dev/null

"$COMPILER" "${COMMON_FLAGS[@]}" \
  -c "$C_ROOT/Common/FirmwareVolumeBuffer.c" -o "$WORK_DIR/FirmwareVolumeBuffer.o"

"$COMPILER" "${COMMON_FLAGS[@]}" \
  "$HOST_TEST_DIR/test_base_tools_fv_bounds.c" "$WORK_DIR/FirmwareVolumeBuffer.o" \
  "${LINK_FLAGS[@]}" -o "$WORK_DIR/test_base_tools_fv_bounds"

"$COMPILER" "${COMMON_FLAGS[@]}" -Dmain=VolInfoProgramMain \
  -c "$C_ROOT/VolInfo/VolInfo.c" -o "$WORK_DIR/VolInfo.o"
"$COMPILER" "${COMMON_FLAGS[@]}" \
  "$HOST_TEST_DIR/test_volinfo_section_bounds.c" "$WORK_DIR/VolInfo.o" \
  "$WORK_DIR/FirmwareVolumeBuffer.o" "${LINK_FLAGS[@]}" \
  -o "$WORK_DIR/test_volinfo_section_bounds"

ASAN_OPTIONS="$BLS_ASAN_OPTIONS" UBSAN_OPTIONS=halt_on_error=1 \
  "$WORK_DIR/test_base_tools_fv_bounds"
ASAN_OPTIONS="$BLS_ASAN_OPTIONS" UBSAN_OPTIONS=halt_on_error=1 \
  "$WORK_DIR/test_volinfo_section_bounds"

echo "All BaseTools deep tests passed (2 tests; sanitizers=$SANITIZERS)."
