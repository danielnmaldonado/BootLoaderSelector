# Validation record — version 2.33

Author: MaLd0n / DNMTechLabs
Credits: ©DNMTechLabs / dnmtechlabs@gmail.com

Version **2.32 is the measured baseline** for the counts in this record. Version
2.33 introduces the English project launch, credits, support links and standard
release publication. Its publication requires the same five CI checks. The
authoritative state is available in the [2.33 Actions runs](https://github.com/danielnmaldonado/BootLoaderSelector/actions?query=branch%3Av2.33)
and [2.33 release](https://github.com/danielnmaldonado/BootLoaderSelector/releases/tag/v2.33).
The historical 2.32 results below do not assert a 2.33 CI result.

## Verified 2.32 baseline

The fixes to the received v2.31/Round86 source were checked on 2026-11-03. The
original ZIP was preserved. The corrected source distribution includes license
notices and a hash catalog for an external build dependency, with the legacy
binary toolchain excluded.

All five checks passed for the actual submitted 2.32 source on
[the main run](https://github.com/danielnmaldonado/BootLoaderSelector/actions/runs/41758480113)
and [the v2.32 tag run](https://github.com/danielnmaldonado/BootLoaderSelector/actions/runs/41759794490).
The tag run also completed release publication. These links require access to
the private repository.

| Check | Recorded result |
| --- | --- |
| Linux host quality gate | Passed; 162 runtime test executions |
| macOS host quality gate | Passed; 144 runtime test executions |
| Linux release compilation | Passed |
| macOS artifact verification | Passed |
| OVMF child-chainload smoke test | Passed |
| Tag release job | Passed; 2.32 prerelease published |

## Local 2.32 verification

Host: Intel x86-64 macOS, Apple Clang 21, GNU GCC 15.3, Python 3.14, OpenSSL 4.1
and Jsign 7.5. The EFI build used the original external cross-toolchain after
verification of its exact 951-file catalog and hashes.

| Check | Recorded result |
| --- | --- |
| Quality gate with `BLS_QUALITY_REPEAT=3`, including Release build | Passed |
| Clang ASan/UBSan | 18 tests × 3 runs |
| GNU GCC UBSan | 18 tests × 3 runs |
| Clang ASan/IntegerSan | 18 tests × 1 run |
| Clang ThreadSanitizer | 18 tests × 1 run |
| Total runtime executions | 144 passed |
| BaseTools deep bounds/fuzz | 2 tests per compiler; 4 passed |
| Strict compilation and Clang/GNU static analysis | All 9 project C modules per compiler |
| BaseTools Python tests during build | 285 passed |
| Positive certificate tests through PEM/DER APIs | 2 passed |
| Certificate policy, signer bootstrap and PE/COFF signing | Passed |
| Publication, rollback, locking and cancellation | Passed, including 9 HUP/INT/TERM scenarios |
| Fortified BaseTools with GNU GCC `-O2 -Werror` | Build and 6 valid message cases passed; selected compiler honored |
| GenFv/GenFw stdio-return diagnostics | GNU/Clang builds and valid EFI/HII/FV/capsule conversions passed |
| Split directory, cut-boundary and file-preservation checks | 9 normal/path-error cases passed with GNU/Clang builds |
| GitHub Actions workflow | actionlint syntax/semantic checks passed; official actions pinned by commit |
| Isolated UEFI launch and returning child | Guest log confirmed `call=0x0 child=0x0` |

The DevicePathLib test links the real converter: each execution covers 26 valid
conversions and 76 allocation-failure positions with explicit ownership checks.
It extends the older conversion stub coverage. The independent review left no
actionable findings in the corrected paths.

## Coverage limits and skipped capabilities

Clang LeakSanitizer, GNU GCC ASan and GNU GCC ThreadSanitizer were unavailable on
the local macOS host. Benign capability probes recorded `SKIP`; these are not
counted as passed coverage. Clang ran ASan without leak detection, GNU GCC ran
UBSan, and Clang ThreadSanitizer ran successfully. The Linux CI gate required
ASan with leak detection and did not use the reduced macOS sanitizer matrix.

The local smoke test used a fresh disk image, hash-checked OVMF firmware and the
QEMU supplied with the Android SDK. Its guest log confirmed the chainload. The
Android wrapper terminated by signal during teardown and could close QMP before
the final query. This does not establish graphical interface stability. CI used
native Linux QEMU and Ubuntu 24.04's matching 4M OVMF CODE/VARS pair.

Tests do not install the application on the host EFI partition or change host
NVRAM. The OVMF child is a dedicated test application that returns success; it
is not a real OpenCore or Clover installation. Real hardware boot, integration
with real OpenCore/Clover, Apple Silicon build compatibility and Secure Boot
trust are not certified. Ad-hoc signing does not establish firmware trust.

## Reproducing the checks

From a clean checkout with the dependencies in [README.md](README.md):

```sh
BLS_QUALITY_REPEAT=3 ./AuditTests/Run-Release-Quality-Gate.command
python3 AuditTools/Update-Source-Manifest.py
```

On macOS, set `BLS_TOOLCHAIN_DIR` and put `lld-link` and OpenSSL on `PATH`.
Linux builds do not need the legacy cross-toolchain. Each signing run generates
a new ephemeral key, so signed EFI hashes can differ even for the same payload.
Check the provenance, certificate and verification result for each build.

Version-matching tags require Linux quality, macOS quality, Linux compilation,
macOS artifact verification and OVMF smoke checks before publication. Version
2.33 uses a standard release after those same checks pass. Its version-specific
Actions runs and release are the authoritative records of that result.

See [CREDITS.md](CREDITS.md), [CHANGELOG.txt](CHANGELOG.txt) and
[SECURITY-NOTES.txt](SECURITY-NOTES.txt) for attribution, changes and security
context.
