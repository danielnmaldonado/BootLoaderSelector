#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd -P)"

# Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
# Creates or verifies a cryptographic ad-hoc Authenticode signature for UEFI
# PE/COFF images. Private key material is ephemeral and is never published.

usage() {
  cat >&2 <<'USAGE'
Usage:
  Sign-PeCoff-AdHoc.command probe
  Sign-PeCoff-AdHoc.command sign INPUT_EFI OUTPUT_EFI PUBLIC_CERT_PEM VERSION METADATA_TXT
  Sign-PeCoff-AdHoc.command verify EFI_FILE PUBLIC_CERT_PEM

Signer selection:
  1. sbsign + sbverify, when both are available
  2. jsign executable
  3. java -jar "$BLS_JSIGN_JAR", when BLS_JSIGN_JAR is set
  4. pinned Jsign 7.5 bootstrap into a user cache (java + curl/wget required)

Set BLS_SIGNER_BACKEND=auto|sbsign|jsign to constrain backend selection.
Set BLS_JSIGN_AUTO_DOWNLOAD=0 to disable the pinned Jsign bootstrap.
Set BLS_JSIGN_CACHE_DIR to override the external cache directory.
USAGE
}

fail() {
  echo "Ad-hoc PE/COFF signing error: $*" >&2
  exit 1
}

hash_file() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    fail "SHA-256 utility not found (shasum or sha256sum required)"
  fi
}

require_regular_input() {
  local path="$1"
  [ -f "$path" ] || fail "input is not a regular file: $path"
  [ ! -L "$path" ] || fail "symbolic-link input is not accepted: $path"
  [ -s "$path" ] || fail "input is empty: $path"
}

validate_publish_target() {
  local path="$1"
  local parent
  parent="$(dirname "$path")"
  [ -d "$parent" ] || fail "output directory does not exist: $parent"
  [ ! -L "$parent" ] || fail "symbolic-link output directory is not accepted: $parent"
  if [ -e "$path" ] || [ -L "$path" ]; then
    [ -f "$path" ] && [ ! -L "$path" ] || fail "unsafe output target: $path"
  fi
}

canonical_target_path() {
  local path="$1"
  local parent base resolved_parent
  parent="$(dirname "$path")"
  base="$(basename "$path")"
  [ -d "$parent" ] || fail "path parent does not exist: $parent"
  resolved_parent="$(cd "$parent" 2>/dev/null && pwd -P)" || fail "unable to resolve path parent: $parent"
  printf '%s/%s\n' "$resolved_parent" "$base"
}

validate_distinct_signing_paths() {
  local input="$1" output="$2" public_cert="$3" metadata="$4"
  local input_path output_path cert_path metadata_path

  input_path="$(canonical_target_path "$input")"
  output_path="$(canonical_target_path "$output")"
  cert_path="$(canonical_target_path "$public_cert")"
  metadata_path="$(canonical_target_path "$metadata")"

  if [ "$input_path" = "$output_path" ] || [ "$input_path" = "$cert_path" ] ||
     [ "$input_path" = "$metadata_path" ] || [ "$output_path" = "$cert_path" ] ||
     [ "$output_path" = "$metadata_path" ] || [ "$cert_path" = "$metadata_path" ]; then
    fail "input, signed output, public certificate and metadata paths must be distinct"
  fi
}

PUBLISH_TX_ACTIVE=0
PUBLISH_TX_COUNT=0
PUBLISH_TX_DEST=()
PUBLISH_TX_STAGED=()
PUBLISH_TX_BACKUP=()
PUBLISH_TX_COMMITTED=()

reset_publish_transaction() {
  PUBLISH_TX_ACTIVE=0
  PUBLISH_TX_COUNT=0
  PUBLISH_TX_DEST=()
  PUBLISH_TX_STAGED=()
  PUBLISH_TX_BACKUP=()
  PUBLISH_TX_COMMITTED=()
}

rollback_publish_transaction() {
  local rc=0
  local i destination staged backup committed

  [ "${PUBLISH_TX_ACTIVE:-0}" -eq 1 ] || return 0

  for ((i=PUBLISH_TX_COUNT-1; i>=0; i--)); do
    destination="${PUBLISH_TX_DEST[$i]:-}"
    staged="${PUBLISH_TX_STAGED[$i]:-}"
    backup="${PUBLISH_TX_BACKUP[$i]:-}"
    committed="${PUBLISH_TX_COMMITTED[$i]:-0}"

    if [ "$committed" -eq 1 ] && [ -n "$destination" ] &&
       { [ -e "$destination" ] || [ -L "$destination" ]; }; then
      if ! rm -f "$destination"; then
        echo "Ad-hoc PE/COFF signing rollback error: unable to remove replacement: $destination" >&2
        rc=1
      fi
    fi

    if [ -n "$backup" ] && { [ -e "$backup" ] || [ -L "$backup" ]; }; then
      if ! mv -f "$backup" "$destination"; then
        echo "Ad-hoc PE/COFF signing rollback error: unable to restore destination: $destination" >&2
        rc=1
      fi
    fi

    if [ -n "$staged" ] && { [ -e "$staged" ] || [ -L "$staged" ]; }; then
      if ! rm -f "$staged"; then
        echo "Ad-hoc PE/COFF signing rollback error: unable to remove staged file: $staged" >&2
        rc=1
      fi
    fi
  done

  reset_publish_transaction
  return "$rc"
}

begin_publish_transaction() {
  local source destination mode parent base staged backup
  local i

  [ "$#" -gt 0 ] && [ $(( $# % 3 )) -eq 0 ] || fail "internal publication transaction argument error"
  [ "${PUBLISH_TX_ACTIVE:-0}" -eq 0 ] || fail "internal publication transaction already active"
  reset_publish_transaction
  PUBLISH_TX_ACTIVE=1

  while [ "$#" -gt 0 ]; do
    source="$1"
    destination="$2"
    mode="$3"
    shift 3

    validate_publish_target "$destination"
    parent="$(dirname "$destination")"
    base="$(basename "$destination")"
    staged="$(mktemp "$parent/.${base}.publish.XXXXXX")" || fail "unable to stage output: $destination"
    PUBLISH_TX_DEST[$PUBLISH_TX_COUNT]="$destination"
    PUBLISH_TX_STAGED[$PUBLISH_TX_COUNT]="$staged"
    PUBLISH_TX_BACKUP[$PUBLISH_TX_COUNT]=""
    PUBLISH_TX_COMMITTED[$PUBLISH_TX_COUNT]=0
    PUBLISH_TX_COUNT=$((PUBLISH_TX_COUNT + 1))

    cp "$source" "$staged" || fail "unable to stage output: $destination"
    chmod "$mode" "$staged" || fail "unable to set output mode: $destination"
  done

  for ((i=0; i<PUBLISH_TX_COUNT; i++)); do
    destination="${PUBLISH_TX_DEST[$i]}"
    staged="${PUBLISH_TX_STAGED[$i]}"
    parent="$(dirname "$destination")"
    base="$(basename "$destination")"
    backup=""

    if [ -e "$destination" ] || [ -L "$destination" ]; then
      backup="$(mktemp "$parent/.${base}.backup.XXXXXX")" || fail "unable to reserve publication backup: $destination"
      rm -f "$backup" || fail "unable to prepare publication backup: $destination"
      PUBLISH_TX_BACKUP[$i]="$backup"
      mv -f "$destination" "$backup" || fail "unable to preserve existing output: $destination"
    fi

    mv -f "$staged" "$destination" || fail "unable to publish output: $destination"
    PUBLISH_TX_STAGED[$i]=""
    PUBLISH_TX_COMMITTED[$i]=1
  done
}

commit_publish_transaction() {
  local i backup staged

  [ "${PUBLISH_TX_ACTIVE:-0}" -eq 1 ] || fail "internal publication transaction is not active"

  # All destinations are now present and have already passed the caller's final
  # validation. From this point the transaction is committed; hidden backups are
  # cleanup-only and never part of the published release surface.
  PUBLISH_TX_ACTIVE=0
  for ((i=0; i<PUBLISH_TX_COUNT; i++)); do
    backup="${PUBLISH_TX_BACKUP[$i]:-}"
    staged="${PUBLISH_TX_STAGED[$i]:-}"
    [ -z "$backup" ] || rm -f "$backup" 2>/dev/null || true
    [ -z "$staged" ] || rm -f "$staged" 2>/dev/null || true
  done
  reset_publish_transaction
}

BACKEND=""
JSIGN_CMD=()
JSIGN_JAR_PATH=""
JSIGN_NATIVE_VERIFY_CAPABILITY=""
SIGNING_TEMP_ROOT=""
JSIGN_VERIFY_TEMP_ROOT=""
JSIGN_BOOTSTRAP_TEMP=""
JSIGN_BOOTSTRAP_VERSION="7.5"
JSIGN_BOOTSTRAP_URL="https://github.com/ebourg/jsign/releases/download/7.5/jsign-7.5.jar"
JSIGN_BOOTSTRAP_SHA256="602a51c3545a6dc4fb99bd2ea7152b26d1345916d0c93ddfbd5936cb735af91c"
cleanup_signing_temp() {
  if [ -n "${SIGNING_TEMP_ROOT:-}" ]; then
    rm -rf -- "$SIGNING_TEMP_ROOT" 2>/dev/null || true
    SIGNING_TEMP_ROOT=""
  fi
  if [ -n "${JSIGN_VERIFY_TEMP_ROOT:-}" ]; then
    rm -rf -- "$JSIGN_VERIFY_TEMP_ROOT" 2>/dev/null || true
    JSIGN_VERIFY_TEMP_ROOT=""
  fi
  if [ -n "${JSIGN_BOOTSTRAP_TEMP:-}" ]; then
    rm -f "$JSIGN_BOOTSTRAP_TEMP" 2>/dev/null || true
    JSIGN_BOOTSTRAP_TEMP=""
  fi
}
cleanup_all() {
  rollback_publish_transaction || true
  cleanup_signing_temp
}
handle_signal() {
  local exit_code="$1"
  # A cancellation signal is terminal. Roll back any partially published release
  # tuple, remove private material exactly once and preserve 128+signal status.
  trap - EXIT HUP INT TERM
  rollback_publish_transaction || true
  cleanup_signing_temp
  exit "$exit_code"
}
trap cleanup_all EXIT
trap 'handle_signal 129' HUP
trap 'handle_signal 130' INT
trap 'handle_signal 143' TERM

default_jsign_cache_dir() {
  if [ -n "${BLS_JSIGN_CACHE_DIR:-}" ]; then
    printf '%s\n' "$BLS_JSIGN_CACHE_DIR"
    return 0
  fi

  case "$(uname -s 2>/dev/null || true)" in
    Darwin)
      [ -n "${HOME:-}" ] || return 1
      printf '%s\n' "$HOME/Library/Caches/DNMTechLabs/BootLoaderSelector"
      ;;
    *)
      if [ -n "${XDG_CACHE_HOME:-}" ]; then
        printf '%s\n' "$XDG_CACHE_HOME/dnmtechlabs/BootLoaderSelector"
      elif [ -n "${HOME:-}" ]; then
        printf '%s\n' "$HOME/.cache/dnmtechlabs/BootLoaderSelector"
      else
        printf '%s\n' "${TMPDIR:-/tmp}/dnmtechlabs-BootLoaderSelector-cache"
      fi
      ;;
  esac
}

bootstrap_jsign() {
  local enabled="${BLS_JSIGN_AUTO_DOWNLOAD:-1}"
  local cache_dir jar_path actual_hash version_output downloader

  case "$enabled" in
    0|1) ;;
    *) fail "BLS_JSIGN_AUTO_DOWNLOAD must be 0 or 1" ;;
  esac
  [ "$enabled" = "1" ] || return 1

  if ! command -v java >/dev/null 2>&1 || ! java -version >/dev/null 2>&1; then
    echo "Jsign bootstrap unavailable: a working Java runtime is required" >&2
    return 1
  fi

  if command -v curl >/dev/null 2>&1; then
    downloader="curl"
  elif command -v wget >/dev/null 2>&1; then
    downloader="wget"
  else
    echo "Jsign bootstrap unavailable: curl or wget is required" >&2
    return 1
  fi

  cache_dir="$(default_jsign_cache_dir)" || {
    echo "Jsign bootstrap unavailable: unable to resolve cache directory" >&2
    return 1
  }
  [ -n "$cache_dir" ] || fail "empty Jsign cache directory"
  if [ -L "$cache_dir" ] || { [ -e "$cache_dir" ] && [ ! -d "$cache_dir" ]; }; then
    fail "unsafe Jsign cache directory: $cache_dir"
  fi
  umask 077
  mkdir -p "$cache_dir" || fail "unable to create Jsign cache directory: $cache_dir"
  [ -d "$cache_dir" ] && [ ! -L "$cache_dir" ] || fail "unsafe Jsign cache directory: $cache_dir"
  chmod 700 "$cache_dir" 2>/dev/null || true

  jar_path="$cache_dir/jsign-${JSIGN_BOOTSTRAP_VERSION}.jar"
  if [ -e "$jar_path" ] || [ -L "$jar_path" ]; then
    if [ ! -f "$jar_path" ] || [ -L "$jar_path" ]; then
      fail "unsafe cached Jsign artifact: $jar_path"
    fi
    actual_hash="$(hash_file "$jar_path")"
    if [ "$actual_hash" != "$JSIGN_BOOTSTRAP_SHA256" ]; then
      rm -f "$jar_path" || fail "unable to remove invalid cached Jsign artifact: $jar_path"
    fi
  fi

  if [ ! -f "$jar_path" ]; then
    JSIGN_BOOTSTRAP_TEMP="$(mktemp "$cache_dir/.jsign-${JSIGN_BOOTSTRAP_VERSION}.download.XXXXXX")" || \
      fail "unable to create Jsign download staging file"
    chmod 600 "$JSIGN_BOOTSTRAP_TEMP"
    if [ "$downloader" = "curl" ]; then
      if ! curl --fail --location --silent --show-error --retry 1 \
        --connect-timeout 15 --max-time 180 --proto '=https' --tlsv1.2 \
        -o "$JSIGN_BOOTSTRAP_TEMP" "$JSIGN_BOOTSTRAP_URL"; then
        fail "unable to download pinned Jsign ${JSIGN_BOOTSTRAP_VERSION} from the official release"
      fi
    else
      if ! wget -q --https-only --timeout=180 -O "$JSIGN_BOOTSTRAP_TEMP" "$JSIGN_BOOTSTRAP_URL"; then
        fail "unable to download pinned Jsign ${JSIGN_BOOTSTRAP_VERSION} from the official release"
      fi
    fi

    actual_hash="$(hash_file "$JSIGN_BOOTSTRAP_TEMP")"
    if [ "$actual_hash" != "$JSIGN_BOOTSTRAP_SHA256" ]; then
      rm -f "$JSIGN_BOOTSTRAP_TEMP" 2>/dev/null || true
      JSIGN_BOOTSTRAP_TEMP=""
      fail "downloaded Jsign SHA-256 mismatch; refusing untrusted signer"
    fi
    mv -f "$JSIGN_BOOTSTRAP_TEMP" "$jar_path" || fail "unable to publish verified Jsign cache artifact"
    JSIGN_BOOTSTRAP_TEMP=""
    chmod 644 "$jar_path" || fail "unable to set cached Jsign permissions"
  fi

  actual_hash="$(hash_file "$jar_path")"
  [ "$actual_hash" = "$JSIGN_BOOTSTRAP_SHA256" ] || fail "cached Jsign SHA-256 mismatch after publication"
  version_output="$(java -jar "$jar_path" --version 2>&1 | head -1 || true)"
  [ "$version_output" = "Jsign ${JSIGN_BOOTSTRAP_VERSION}" ] || \
    fail "pinned Jsign version verification failed: ${version_output:-no version output}"

  BACKEND="jsign"
  JSIGN_JAR_PATH="$jar_path"
  JSIGN_CMD=(java -jar "$jar_path")
  JSIGN_NATIVE_VERIFY_CAPABILITY=""
  return 0
}

select_backend() {
  local requested="${BLS_SIGNER_BACKEND:-auto}"
  case "$requested" in
    auto|sbsign|jsign) ;;
    *) fail "BLS_SIGNER_BACKEND must be auto, sbsign, or jsign" ;;
  esac

  if [ "$requested" = "auto" ] || [ "$requested" = "sbsign" ]; then
    if command -v sbsign >/dev/null 2>&1 && command -v sbverify >/dev/null 2>&1; then
      BACKEND="sbsign"
      return 0
    fi
    [ "$requested" != "sbsign" ] || fail "requested sbsign backend requires both sbsign and sbverify"
  fi

  if [ "$requested" = "auto" ] || [ "$requested" = "jsign" ]; then
    if command -v jsign >/dev/null 2>&1; then
      BACKEND="jsign"
      JSIGN_JAR_PATH=""
      JSIGN_CMD=(jsign)
      JSIGN_NATIVE_VERIFY_CAPABILITY=""
      return 0
    fi
    if [ -n "${BLS_JSIGN_JAR:-}" ]; then
      [ -f "$BLS_JSIGN_JAR" ] && [ ! -L "$BLS_JSIGN_JAR" ] && [ -r "$BLS_JSIGN_JAR" ] || \
        fail "BLS_JSIGN_JAR is not a readable regular non-symlink file: $BLS_JSIGN_JAR"
      command -v java >/dev/null 2>&1 || fail "java is required for BLS_JSIGN_JAR"
      BACKEND="jsign"
      JSIGN_JAR_PATH="$BLS_JSIGN_JAR"
      JSIGN_CMD=(java -jar "$BLS_JSIGN_JAR")
      JSIGN_NATIVE_VERIFY_CAPABILITY=""
      return 0
    fi
    if bootstrap_jsign; then
      return 0
    fi
    [ "$requested" != "jsign" ] || fail "requested jsign backend was not found and pinned bootstrap is unavailable"
  fi

  fail "no supported PE/COFF signer found; install sbsigntools/jsign, set BLS_JSIGN_JAR, or provide Java plus curl/wget for the pinned Jsign bootstrap"
}

signer_version() {
  local version
  case "$BACKEND" in
    sbsign)
      version="$(sbsign --version 2>&1 | head -1 || true)"
      ;;
    jsign)
      version="$("${JSIGN_CMD[@]}" --version 2>&1 | head -1 || true)"
      ;;
    *)
      version=""
      ;;
  esac
  if [ -z "$version" ]; then
    version="unknown"
  fi
  printf '%s\n' "$version"
}

jsign_has_native_verify() {
  local output rc version major

  if [ "$JSIGN_NATIVE_VERIFY_CAPABILITY" = "1" ]; then
    return 0
  elif [ "$JSIGN_NATIVE_VERIFY_CAPABILITY" = "0" ]; then
    return 1
  fi

  set +e
  output="$("${JSIGN_CMD[@]}" verify --help 2>&1)"
  rc=$?
  set -e
  if [ "$rc" -eq 0 ]; then
    JSIGN_NATIVE_VERIFY_CAPABILITY="1"
    return 0
  fi
  if printf '%s\n' "$output" | grep -F "Unknown command 'verify'" >/dev/null 2>&1; then
    JSIGN_NATIVE_VERIFY_CAPABILITY="0"
    return 1
  fi

  version="$(signer_version)"
  major="$(printf '%s\n' "$version" | sed -n 's/^Jsign \([0-9][0-9]*\)\..*/\1/p')"
  if [ -n "$major" ] && [ "$major" -ge 8 ]; then
    JSIGN_NATIVE_VERIFY_CAPABILITY="1"
    return 0
  fi

  fail "unable to determine whether the selected Jsign supports native verification: ${output:-no diagnostic}"
}

java_major_version() {
  local version_line major
  version_line="$(java -version 2>&1 | head -1 || true)"
  major="$(printf '%s\n' "$version_line" | sed -n 's/.*version "\([0-9][0-9]*\)\..*/\1/p')"
  if [ "$major" = "1" ]; then
    major="$(printf '%s\n' "$version_line" | sed -n 's/.*version "1\.\([0-9][0-9]*\)\..*/\1/p')"
  fi
  [ -n "$major" ] || return 1
  printf '%s\n' "$major"
}

verify_signature_with_jsign_legacy() {
  local image="$1" cert="$2"
  local verifier="$SCRIPT_DIR/Verify-PeCoff-Authenticode.py"
  local work_image signature_file

  # Jsign 7.x can sign and extract Authenticode data but has no native
  # `verify` command. Verification therefore uses only stable public CLI
  # behavior (`extract`) plus a standalone stdlib/OpenSSL verifier. This
  # deliberately avoids compiling Java source against Jsign's minimized JAR,
  # which is not a supported API surface and fails on some macOS JDK setups.
  command -v python3 >/dev/null 2>&1 || fail "Jsign 7.x verification requires python3"
  command -v openssl >/dev/null 2>&1 || fail "Jsign 7.x verification requires openssl"
  [ -f "$verifier" ] && [ ! -L "$verifier" ] && [ -r "$verifier" ] || \
    fail "Jsign compatibility verifier is missing or unsafe: $verifier"

  JSIGN_VERIFY_TEMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/BootLoaderSelector.JsignVerify.XXXXXX")" || \
    fail "unable to create Jsign compatibility verification workspace"
  chmod 700 "$JSIGN_VERIFY_TEMP_ROOT"
  work_image="$JSIGN_VERIFY_TEMP_ROOT/signed.efi"
  cp -p "$image" "$work_image" || fail "unable to stage signed EFI for Jsign verification"

  local extract_output verify_output
  if ! extract_output="$("${JSIGN_CMD[@]}" extract --format DER "$work_image" 2>&1)"; then
    fail "Jsign 7.x could not extract the Authenticode signature: $image${extract_output:+ — $extract_output}"
  fi
  signature_file="$work_image.sig"
  [ -s "$signature_file" ] && [ ! -L "$signature_file" ] || \
    fail "Jsign 7.x did not produce a usable detached Authenticode signature"

  if ! verify_output="$(python3 "$verifier" "$work_image" "$signature_file" "$cert" 2>&1)"; then
    fail "Jsign compatibility verifier rejected signature: $image${verify_output:+ — $verify_output}"
  fi

  rm -rf -- "$JSIGN_VERIFY_TEMP_ROOT"
  JSIGN_VERIFY_TEMP_ROOT=""
}

verify_signature() {
  local image="$1"
  local cert="$2"
  require_regular_input "$image"
  require_regular_input "$cert"
  command -v openssl >/dev/null 2>&1 || fail "openssl is required"
  openssl x509 -in "$cert" -noout >/dev/null 2>&1 || fail "invalid X.509 certificate: $cert"

  case "$BACKEND" in
    sbsign)
      sbverify --cert "$cert" "$image" >/dev/null 2>&1 || fail "sbverify rejected signature: $image"
      ;;
    jsign)
      if jsign_has_native_verify; then
        "${JSIGN_CMD[@]}" verify --certfile "$cert" "$image" >/dev/null 2>&1 || \
          fail "jsign rejected signature: $image"
      else
        verify_signature_with_jsign_legacy "$image" "$cert"
      fi
      ;;
    *) fail "internal signer backend selection failure" ;;
  esac
}

sign_image() {
  local input="$1"
  local output="$2"
  local public_cert="$3"
  local version="$4"
  local metadata="$5"
  local temp_root key cert signed openssl_config cert_sha metadata_temp

  require_regular_input "$input"
  [[ "$version" =~ ^[0-9]+[.][0-9]+$ ]] || fail "invalid release version: $version"
  validate_distinct_signing_paths "$input" "$output" "$public_cert" "$metadata"
  validate_publish_target "$output"
  validate_publish_target "$public_cert"
  validate_publish_target "$metadata"
  command -v openssl >/dev/null 2>&1 || fail "openssl is required"

  umask 077
  temp_root="$(mktemp -d "${TMPDIR:-/tmp}/BootLoaderSelector.AdHocSign.XXXXXX")" || fail "unable to create signing workspace"
  SIGNING_TEMP_ROOT="$temp_root"
  chmod 700 "$temp_root"
  key="$temp_root/adhoc-private-key.pem"
  cert="$temp_root/adhoc-public-cert.pem"
  signed="$temp_root/signed.efi"
  openssl_config="$temp_root/openssl.cnf"
  metadata_temp="$temp_root/signing-metadata.txt"

  cat > "$openssl_config" <<EOF_CONFIG
[req]
prompt = no
distinguished_name = dn
x509_extensions = ext

[dn]
CN = DNMTechLabs BootLoaderSelector v${version} Ad Hoc
O = DNMTechLabs

[ext]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = codeSigning
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
EOF_CONFIG

  openssl req -new -x509 -newkey rsa:3072 -sha256 -nodes -days 3650 \
    -config "$openssl_config" -keyout "$key" -out "$cert" >/dev/null 2>&1 || \
    fail "unable to generate ephemeral signing certificate"
  chmod 600 "$key"
  chmod 644 "$cert"
  openssl verify -CAfile "$cert" "$cert" >/dev/null 2>&1 || fail "generated ad-hoc certificate failed self-verification"

  case "$BACKEND" in
    sbsign)
      sbsign --key "$key" --cert "$cert" --output "$signed" "$input" >/dev/null 2>&1 || \
        fail "sbsign failed"
      ;;
    jsign)
      cp "$input" "$signed" || fail "unable to stage EFI image for jsign"
      "${JSIGN_CMD[@]}" sign --alg SHA-256 --keyfile "$key" --certfile "$cert" \
        --name "BootLoaderSelector v${version} Ad Hoc" "$signed" >/dev/null 2>&1 || \
        fail "jsign failed"
      ;;
    *) fail "internal signer backend selection failure" ;;
  esac

  verify_signature "$signed" "$cert"
  cert_sha="$(hash_file "$cert")"
  [[ "$cert_sha" =~ ^[0-9a-fA-F]{64}$ ]] || fail "unable to calculate certificate SHA-256"

  {
    echo "Signer backend: $BACKEND"
    echo "Signer version: $(signer_version)"
    echo "Signature algorithm: SHA-256 with RSA-3072"
    echo "Certificate SHA-256: $cert_sha"
    echo "Signature verification: PASS"
    echo "Private key published: no"
  } > "$metadata_temp"

  begin_publish_transaction \
    "$signed" "$output" 644 \
    "$cert" "$public_cert" 644 \
    "$metadata_temp" "$metadata" 644

  # Re-verify the bytes at their final published path while rollback material is
  # still retained. A verification failure therefore restores the complete prior
  # tuple instead of leaving mixed-version release artifacts.
  verify_signature "$output" "$public_cert"
  commit_publish_transaction
  echo "Signer backend: $BACKEND"
  echo "Signer version: $(signer_version)"
  echo "Certificate SHA-256: $cert_sha"
  echo "Signature verification: PASS"
}

[ "$#" -ge 1 ] || { usage; exit 2; }
MODE="$1"
shift
select_backend

case "$MODE" in
  probe)
    [ "$#" -eq 0 ] || { usage; exit 2; }
    command -v openssl >/dev/null 2>&1 || fail "openssl is required"
    echo "Signer backend: $BACKEND"
    echo "Signer version: $(signer_version)"
    echo "Signature algorithm: SHA-256 with RSA-3072"
    ;;
  sign)
    [ "$#" -eq 5 ] || { usage; exit 2; }
    sign_image "$1" "$2" "$3" "$4" "$5"
    ;;
  verify)
    [ "$#" -eq 2 ] || { usage; exit 2; }
    verify_signature "$1" "$2"
    echo "Signature verification: PASS"
    ;;
  *)
    usage
    exit 2
    ;;
esac
