# Audit coverage

Prepared on 2026-11-04 for the v2.33 documentation update. The completed execution results below describe the corrected **v2.32** baseline at commit `ddc9a38ced0a8e6001316a612115cfadb053fe05`. This document update does not constitute a new test run.

Credits: ©DNMTechLabs / dnmtechlabs@gmail.com

## Evidence terms

| Term | Meaning |
| --- | --- |
| Integrity checked | File bytes match a recorded SHA-256. This does not establish semantic correctness or independent provenance. |
| Directly reviewed | Source behavior was inspected within the stated scope. This does not imply every dependency was reviewed function by function. |
| Executed | A named check ran successfully in the recorded environment. Its result applies to the exercised paths. |
| Skipped | A capability probe found that the host could not run the requested sanitizer mode. No passing application coverage is credited for that mode. |
| Unvalidated | No evidence establishes the stated property. Hardware compatibility and platform Secure Boot trust remain in this category. |

No statement in this record represents measured 100% line, branch, path or firmware coverage. Historical source inventories and function counts are retained with their original version in Git; they are not current coverage percentages.

## Completed execution matrix

| Check | Recorded result | Boundary |
| --- | --- | --- |
| Full local release quality gate | Passed with `BLS_QUALITY_REPEAT=3`, including signed release build | Intel x86-64 macOS and a separately supplied, catalog-checked cross-toolchain |
| Local Clang ASan/UBSan | 18 host programs × 3 runs = 54 executions | Leak detection unavailable locally |
| Local GNU GCC UBSan | 18 host programs × 3 runs = 54 executions | GNU ASan unavailable locally |
| Local Clang ASan/IntegerSan | 18 host programs × 1 run = 18 executions | Exercised integer and address checks; not firmware execution |
| Local Clang ThreadSanitizer | 18 host programs × 1 run = 18 executions | Host regression behavior; not a firmware concurrency proof |
| Local unavailable sanitizer modes | Clang LeakSanitizer, GNU ASan and GNU ThreadSanitizer explicitly skipped | Zero passing executions credited for these unavailable modes |
| Remote Linux host matrix | 162 successful host-program executions | Includes 18 GNU ThreadSanitizer executions |
| Remote macOS host matrix | 144 successful host-program executions | GNU ThreadSanitizer explicitly skipped |
| Remote leak checks | Clang and GNU ASan/UBSan with leak detection passed on Linux and macOS | Does not erase local capability skips or prove every allocation path was exercised |
| BaseTools deep/bounds/fuzz | Two programs per compiler; four local executions | Firmware-volume and section-boundary host harnesses |
| Strict compilation and static analysis | Nine application C modules with Clang and GNU GCC | Compiler/analyzer findings within their supported models |
| BaseTools build/Python suite | 285 tests passed during the local build | Upstream host-tool and Python checks |
| Positive certificate interfaces | Two tests passed through PEM and DER APIs | Known valid self-signed leaf-certificate cases |
| Certificate policy, bootstrap and PE/COFF signing | Passed | Cryptographic and publication policy, not firmware trust enrollment |
| Publish/rollback/lock/cancellation | Passed, including nine HUP/INT/TERM cases | Tested release and signer transaction scenarios |
| Native Linux release | Two completed builds, including stale-output regression | Generated PE/COFF artifacts; signatures checked |
| Independent artifact verification | Linux-built artifacts verified on macOS | Published certificate and Authenticode verification |
| OVMF integration | Dedicated signed SmokeChild chainload and return passed | `call=0x0 child=0x0`; isolated virtual firmware |
| Real hardware, real OpenCore/Clover, Secure Boot trust | Unvalidated | Requires target-specific validation |

The local total is **144 successful executions of 18 host programs**, plus four BaseTools deep-test executions. Repetition and sanitizer variants do not increase the number of distinct application test programs. The 285 BaseTools Python tests and the two positive-certificate tests are separate checks.

Each remote host ran 54 Clang ASan/UBSan, 54 GNU ASan/UBSan, 18 Clang integer and 18 Clang ThreadSanitizer executions. Linux also ran 18 GNU ThreadSanitizer executions. Exact compiler output and capability decisions remain in the Actions logs.

Completed remote evidence: [main run 41758480113](https://github.com/danielnmaldonado/BootLoaderSelector/actions/runs/41758480113) and [v2.32 tag run 41759794490](https://github.com/danielnmaldonado/BootLoaderSelector/actions/runs/41759794490). All five prerequisite gates and the tag release completed successfully. Results for later commits must be read from their own Actions runs.

## Application host programs

The suite in [`AuditTests/Run-Audit-Tests.command`](../AuditTests/Run-Audit-Tests.command) compiles these 18 programs with the selected compiler and sanitizer mode. Each harness covers specific behavior with host mocks; a passing harness is not complete coverage of its associated module.

| Program in `AuditTests/Host/` | Principal scope |
| --- | --- |
| `test_ui_mode_bound.c` | Bounded console-mode selection |
| `test_mp_topology_bound.c` | Bounded multiprocessor topology handling |
| `test_fs_handle_bound.c` | Bounded filesystem-handle enumeration |
| `test_bootscan_policy.c` | Bootloader-discovery and selection policy |
| `test_remembered_autoboot_integrity.c` | Remembered-entry and direct-autoboot integrity |
| `test_boot_image_lifecycle.c` | Loaded child-image lifecycle and cleanup |
| `test_watchdog_lifecycle.c` | Child-boot watchdog lifecycle |
| `test_event_lifecycle.c` | Event ownership and cleanup |
| `test_file_protocol_contracts.c` | File-protocol operation and cleanup contracts |
| `test_protocol_access_contracts.c` | Firmware-protocol access contracts |
| `test_hotkey_timer_priority.c` | Hotkey and timer event priority |
| `test_menu_countdown_starvation.c` | Menu countdown progress under event activity |
| `test_firmware_table_bounds.c` | Firmware-table and mapped-memory bounds |
| `test_config_parser.c` | Configuration-parser input handling |
| `test_config_lifecycle.c` | Configuration persistence, recovery and cleanup |
| `test_screenshot_bounds.c` | Screenshot buffer and size bounds |
| `test_logger_lifecycle.c` | Logger resource lifecycle and persistence behavior |
| `test_device_path_conversion.c` | Real DevicePathLib conversion and allocation ownership |

The DevicePath program executes **26 valid conversions and 76 allocator-failure positions per run** against real DevicePathLib code. The older conversion stub remains insufficient evidence for those actual library paths. The new harness explicitly checks ownership of allocations.

## Source and tool scope

| Area | Completed evidence | Limit |
| --- | --- | --- |
| Application runtime | Strict compilation and analysis of `Main.c`, `BootScan.c`, `Config.c`, `HardwareInfo.c`, `Logger.c`, `Menu.c`, `Screenshot.c`, `Ui.c` and `Utils.c`; targeted host programs above | No complete firmware state-space or branch-coverage claim |
| DevicePathLib | Direct correction/review and real conversion/failure-path regression | Does not establish a complete semantic audit of MdePkg |
| BaseTools firmware-volume/section handling | `test_base_tools_fv_bounds.c` and `test_volinfo_section_bounds.c`, run with both compilers | Host harnesses exercise selected bounds and malformed-input behavior |
| BaseTools diagnostic output | GNU GCC fortification with `-O2 -Werror`; six valid bounded-message cases | Covers the corrected message paths and selected compiler behavior |
| GenFv/GenFw I/O | Seven GenFv and six GenFw read results checked; GNU/Clang builds and valid EFI/HII/FV/capsule conversions | Does not cover every format or filesystem failure |
| Split | Nine normal/path-error cases, directory creation, cut boundaries, file preservation and handled current-working-directory failure | Selected input/path cases |
| Release transactions | Actual signing, certificate policy, bootstrap, rollback, locking and nine cancellation cases | Tested host process/filesystem behavior |
| CI | actionlint checks, commit-pinned official Actions and completed Linux/macOS/build/verification/OVMF gates | Each subsequent revision requires its own successful run |
| Distribution integrity | Source manifest plus 951-file external toolchain catalog | Hashes identify bytes; they do not establish redistribution rights or an external trust anchor |

BaseTools deep tests build private copies so audit execution does not leave generated objects in the immutable source tree. The legacy binary toolchain is excluded from this source distribution. Original upstream notices remain in their source files and in [`THIRD-PARTY-NOTICES.txt`](../THIRD-PARTY-NOTICES.txt).

## Integration limits

The dedicated OVMF smoke child establishes that the signed selector can discover, load, call and receive successful return from that child in the tested virtual environment. It does not validate real OpenCore/Clover releases or every firmware implementation.

The separate local Android-SDK QEMU run recorded guest chainload success, then its wrapper ended with signal `-9` during teardown and could close QMP before the last query. This is guest-log evidence, not graphical-interface stability certification. The remote pipeline uses native Linux QEMU with the Ubuntu OVMF CODE/VARS 4M pair.

Real-hardware boot, display behavior across target machines, platform Secure Boot trust and complete manual review of all vendored dependencies remain unvalidated. Self-signed ad-hoc Authenticode signatures establish integrity relative to the published certificate; platform trust must be established separately.

## Record continuity

The current English records replace redundant historical snapshots. Unmodified originals remain available at Git tag `v2.32`, including the previous function inventory and Round67–85 patches. An additional byte-identical external backup was created before replacement. See [`Historical/README.md`](Historical/README.md). Historical evidence has not been translated into new patches or counted as new execution.

For dependencies and reproduction, see [`README.md`](../README.md), [`VALIDATION.md`](../VALIDATION.md) and [`AUDIT-REPORT.txt`](AUDIT-REPORT.txt). Root and upstream licenses remain in force; no coverage record changes them.
