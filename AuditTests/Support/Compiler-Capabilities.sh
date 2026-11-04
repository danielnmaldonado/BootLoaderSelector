#!/bin/bash
# Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
# Sourced by the audit runners; keep compatible with macOS Bash 3.2.

bls_compiler_family() {
  local macros
  macros="$("$1" -dM -E -x c /dev/null 2>/dev/null)" || return 1
  if printf '%s\n' "$macros" | grep '^#define __clang__ ' >/dev/null; then
    printf '%s\n' clang
  elif printf '%s\n' "$macros" | grep '^#define __GNUC__ ' >/dev/null; then
    printf '%s\n' gnu
  else
    printf '%s\n' unknown
  fi
}

bls_find_compiler() {
  local family candidate compiler version
  family="$1"
  # A program named gcc may be Apple Clang. Prefer a real versioned GNU GCC.
  if [ "$family" = gnu ]; then
    for version in 20 19 18 17 16 15 14 13 12 11 10 9 8 7 6 5 4; do
      candidate="gcc-$version"
      compiler="$(command -v "$candidate" 2>/dev/null)" || continue
      if [ "$(bls_compiler_family "$compiler")" = gnu ]; then
        printf '%s\n' "$compiler"
        return 0
      fi
    done
    candidate=gcc
  else
    candidate=clang
  fi
  compiler="$(command -v "$candidate" 2>/dev/null)" || return 1
  [ "$(bls_compiler_family "$compiler")" = "$family" ] || return 1
  printf '%s\n' "$compiler"
}

bls_probe_compile() {
  local compiler probe_dir
  compiler="$1" probe_dir="$2"
  shift 2
  mkdir -p "$probe_dir"
  printf 'int main(void) { return 0; }\n' > "$probe_dir/probe.c"
  "$compiler" -std=gnu11 -Werror "$@" -c "$probe_dir/probe.c" \
    -o "$probe_dir/probe.o" > "$probe_dir/compile.log" 2>&1
}

bls_probe_clang_analyzer() {
  local compiler probe_dir
  compiler="$1" probe_dir="$2"
  mkdir -p "$probe_dir"
  printf 'int main(void) { return 0; }\n' > "$probe_dir/probe.c"
  "$compiler" -std=gnu11 -Werror --analyze -Xanalyzer -analyzer-output=text \
    "$probe_dir/probe.c" -o "$probe_dir/probe.plist" > "$probe_dir/compile.log" 2>&1
}

bls_probe_sanitizer() {
  local compiler sanitizers probe_dir leaks probe_status
  compiler="$1" sanitizers="$2" probe_dir="$3" leaks="$4"
  mkdir -p "$probe_dir"
  BLS_PROBE_LOG="$probe_dir/probe.log"
  printf 'int main(void) { return 0; }\n' > "$probe_dir/probe.c"
  if ! "$compiler" -std=gnu11 -Werror -fsanitize="$sanitizers" \
      -fno-omit-frame-pointer "$probe_dir/probe.c" -o "$probe_dir/probe" \
      > "$BLS_PROBE_LOG" 2>&1; then
    BLS_PROBE_REASON='compiler/linker does not support this sanitizer'
    return 1
  fi
  # Use a benign executable, and capture the child shell's signal diagnostic.
  # A compiler accepting a flag does not prove its sanitizer runtime can start.
  if ASAN_OPTIONS="detect_leaks=$leaks:halt_on_error=1" \
      UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 \
      /bin/bash -c '"$1"; result=$?; exit "$result"' _ "$probe_dir/probe" \
      >> "$BLS_PROBE_LOG" 2>&1; then
    return 0
  else
    probe_status=$?
    BLS_PROBE_REASON="benign runtime probe failed (exit=$probe_status)"
    return 1
  fi
}

bls_configure_sanitizers() {
  local compiler mode probe_root family address_reason
  compiler="$1" mode="$2" probe_root="$3"
  family="$(bls_compiler_family "$compiler")" || return 1
  BLS_SANITIZERS=''
  BLS_ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'
  case "$mode" in
    undefined) BLS_SANITIZERS=address,undefined ;;
    integer) BLS_SANITIZERS=address,integer ;;
    thread) BLS_SANITIZERS=thread ;;
    *) echo "Unknown sanitizer mode: $mode" >&2; return 1 ;;
  esac

  if [ "$mode" = thread ]; then
    if bls_probe_sanitizer "$compiler" thread "$probe_root/thread" 0; then
      return 0
    fi
    echo "SKIP ThreadSanitizer ($compiler): $BLS_PROBE_REASON"
    cat "$BLS_PROBE_LOG"
    return 2
  fi
  if [ "$mode" = integer ] &&
      ! bls_probe_sanitizer "$compiler" integer "$probe_root/integer" 0; then
    echo "SKIP IntegerSan ($compiler): $BLS_PROBE_REASON"
    cat "$BLS_PROBE_LOG"
    return 2
  fi

  if bls_probe_sanitizer "$compiler" "$BLS_SANITIZERS" "$probe_root/address-leaks" 1; then
    BLS_ASAN_OPTIONS='detect_leaks=1:halt_on_error=1'
    echo "Sanitizer capability: $BLS_SANITIZERS; LeakSanitizer enabled ($compiler)"
    return 0
  fi
  address_reason="$BLS_PROBE_REASON"
  if [ "$(uname -s)" = Darwin ] &&
      bls_probe_sanitizer "$compiler" "$BLS_SANITIZERS" "$probe_root/address" 0; then
    echo "SKIP LeakSanitizer ($compiler): detect_leaks=1 failed its benign runtime probe"
    echo "Sanitizer capability: $BLS_SANITIZERS; detect_leaks=0 ($compiler)"
    return 0
  fi
  # GNU ASan can link successfully but fail before main on macOS. Retain real
  # GNU UBSan execution while reporting the missing ASan entry explicitly.
  if [ "$family" = gnu ] && [ "$(uname -s)" = Darwin ] && [ "$mode" = undefined ] &&
      bls_probe_sanitizer "$compiler" undefined "$probe_root/undefined" 0; then
    BLS_SANITIZERS=undefined
    echo "SKIP ASan/LeakSanitizer ($compiler): $address_reason"
    cat "$probe_root/address-leaks/probe.log"
    echo "Sanitizer capability: GNU UBSan fallback ($compiler)"
    return 0
  fi
  echo "Sanitizer capability unavailable ($compiler; $BLS_SANITIZERS): $address_reason" >&2
  cat "$probe_root/address-leaks/probe.log" >&2
  # Linux default audits require ASan with leak detection. Never downgrade
  # Linux leak coverage to turn an unavailable runtime into a passing check.
  if [ "$mode" = integer ] && [ "$(uname -s)" = Darwin ]; then
    echo "SKIP ASan/IntegerSan ($compiler): combined runtime unavailable"
    return 2
  fi
  return 1
}
