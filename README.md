# BootLoaderSelector

An x64 UEFI application for finding and launching OpenCore and Clover, with a
remembered selection, plist settings, hardware information, debug logging and
screenshots.

**Version 2.33** · Author: **MaLd0n / DNMTechLabs**
Credits: ©DNMTechLabs / dnmtechlabs@gmail.com

## Using the selector

Launch `BootLoaderSelector.efi` from a UEFI shell or your firmware's boot menu on
a test EFI volume. `BOOTX64.efi` contains the same bytes and provides the standard
x64 removable-media filename. The build script creates files in `Release/`; it
does not install them on the host EFI partition or change firmware settings.

The selector checks these paths on EFI filesystem volumes:

```text
\EFI\OC\OpenCore.efi
\EFI\BOOT\OpenCore.efi
\EFI\CLOVER\CLOVERX64.efi
\EFI\BOOT\CLOVERX64.efi
```

| Key | Action |
| --- | --- |
| Up / Down | Move the selection |
| Enter / Right | Boot the selected entry or open Options |
| Esc / Left | Exit or go back |
| F1 | Show help |
| Space | Show details for a boot entry |
| F5 | Rescan EFI filesystem volumes |
| F10 | Save a screenshot when GOP is available |

Options controls the timeout, remembered selection, hardware card and debug log.
Settings are stored in `\EFI\BOOT\BLS.plist`. A remembered entry is launched
after a two-second arrow-key window only when a complete scan identifies it
uniquely. Press an arrow key during that window to open the selector. Missing or
ambiguous entries keep the menu available for manual selection.

For settings, defaults, file locations and runtime contracts, see
[INFO.txt](INFO.txt). For changes, see [CHANGELOG.txt](CHANGELOG.txt).

## Building from source

On Linux x86-64, install GCC/G++, binutils, make, NASM, Python 3.10+, Clang/LLD,
OpenSSL, uuid-dev and sbsigntools. On Ubuntu:

```sh
sudo apt-get install gcc g++ binutils make nasm python3 clang lld openssl uuid-dev sbsigntool
./Build-BootLoaderSelector-Release.command
```

On macOS, install Xcode Command Line Tools, Python 3.10+, Java, Jsign and OpenSSL.
The system LibreSSL is not a substitute for OpenSSL. For example, install
`jsign` and `openssl@3` with Homebrew, then put OpenSSL's `bin` directory on
`PATH`. The UEFI build also requires the original external cross-toolchain,
supplied separately by an existing owner. It is not included in this repository.
See [Toolchains/README.md](Toolchains/README.md).

```sh
BLS_TOOLCHAIN_DIR=/path/without/spaces/toolchain.gcc102.snb \
  ./Build-BootLoaderSelector-Release.command
```

The builder checks that external toolchain against its exact file and hash
catalog before execution. Matching filenames alone are insufficient. Linux
builds use native tools and do not require this legacy macOS dependency.

The two output files, `BootLoaderSelector.efi` and `BOOTX64.efi`, are
byte-identical. Each build uses a new ephemeral ad-hoc signing key. The signature
can be checked against the generated certificate; it does not automatically make
the executable trusted by Secure Boot firmware.

Use `./Build-Debug-Full-Log.command` for additional build diagnostics. Both build
entry points use `BuildLogs/BootLoaderSelector-Build.log`. On macOS,
`BLS_REVEAL_LOG=1` opts into revealing that log in Finder.

## Verification

Run the source and host quality gate without building the final EFI:

```sh
BLS_QUALITY_BUILD=0 ./AuditTests/Run-Release-Quality-Gate.command
```

The gate runs runtime and BaseTools tests, signing and publication regressions,
strict compilation and static analysis. Signing tests require `lld-link` on
`PATH`. With Homebrew, install `lld@21` separately and expose
`$(brew --prefix lld@21)/bin`; `llvm@21` alone does not provide that command.
Compilers are identified by implementation. Unsupported sanitizer capabilities
are reported as `SKIP` and are not counted as passing coverage.

The source manifest covers source files, documentation and CI configuration.
After reviewing an intentional change, update it with:

```sh
python3 AuditTools/Update-Source-Manifest.py --write
```

Without `--write`, the command verifies the manifest. The builder rejects
mismatches and unexpected immutable files. Generated build outputs, Git metadata
and `.DS_Store` are documented exclusions.

See [VALIDATION.md](VALIDATION.md) for recorded results, CI evidence and testing
limits, including the distinction between the verified 2.32 baseline and 2.33.

## CI and releases

Pushes to `main`, pull requests and `v*` tags run GitHub Actions. Release
publication requires all five checks: Linux quality, macOS quality, Linux release
build, macOS artifact verification and the OVMF chainload smoke test. The tag must
match the source version. The release includes source and EFI ZIPs, SHA-256
checksums and license notices. Release and artifact visibility follow this
repository's private visibility.

## Credits and licenses

BootLoaderSelector is by **MaLd0n / DNMTechLabs**. The project's own code uses
[BSD-3-Clause](LICENSE). Third-party components retain their own licenses and
attributions. See [CREDITS.md](CREDITS.md),
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt) and [LICENSES/](LICENSES/).

Public upstream test keys in BaseTools are fixtures, identified in
[SECURITY-NOTES.txt](SECURITY-NOTES.txt). They are not production credentials.

## Support the project

- [Ko-fi](https://ko-fi.com/olarila)
- [PayPal](https://www.paypal.com/donate/?hosted_button_id=AA4QBFJQYW2N2)
- [Revolut](https://revolut.me/olarila)
- [Remitly](https://remit.ly/pay/danielm2937)
- [Mercado Pago](https://mpago.la/2wFPzHP)
