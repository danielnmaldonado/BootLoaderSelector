# Credits and acknowledgments

BootLoaderSelector is developed, adapted and maintained by **MaLd0n**.
The original project credit is retained: **©DNMTechLabs / dnmtechlabs@gmail.com**.
The copyright holder in the [project license](LICENSE) remains DNMTechLabs.

Thank you to the upstream authors, maintainers and contributors whose work makes
this project possible. Original copyright and license headers remain authoritative.
The root BSD-3-Clause license covers original project work only.

## Included upstream source

### TianoCore / EDK II

[EDK II](https://github.com/tianocore/edk2), maintained by the TianoCore community,
provides `MdePkg`, `BaseTools`, build configuration and `edksetup.sh`.
`MdePkg` supplies UEFI definitions and the libraries selected by
[`BootLoaderSelector.dsc`](BootLoaderSelector.dsc). BaseTools supplies the host
build tools. Other architectures and optional utilities are also retained in
source; their presence does not mean that they are linked into the x64 EFI.

The retained notices credit Intel Corporation; Apple Inc.; ARM Ltd / ARM Limited;
Linaro Ltd / Linaro Limited; Hewlett-Packard Development Company, L.P.; HP
Development Company, L.P.; Hewlett Packard Enterprise Development LP; Red Hat,
Inc.; AMD Incorporated / Advanced Micro Devices; Microsoft Corporation; American
Megatrends, Inc. / American Megatrends International LLC; Dell Technologies;
NVIDIA Corporation; Loongson Technology Corporation Limited; and Paulo Alcantara.
Dave Gilbert is the named author of the optimized ARM `ScanMem` implementation.
Names, dates and legal spellings are preserved in the original notices.

Most files use [BSD-2-Clause-Patent](LICENSES/BSD-2-Clause-Patent.txt). Older files
refer to EDK II's [legacy BSD license](LICENSES/BSD-2-Clause-EDK-II-Legacy.txt).
Some optimized ARM/AArch64 memory routines have their own BSD-3-Clause notices.
Original copyright lines and additional file-specific terms are reproduced in
[EDK II copyright notices](LICENSES/EDK-II-Copyright-Notices.txt) and
[upstream file notices](LICENSES/Upstream-File-Notices.txt).

The supplied archive does not establish a single exact EDK II import revision.
[`AuditReports/MANIFEST.sha256`](AuditReports/MANIFEST.sha256) identifies the
source bytes. Local changes retain the original upstream notices and licenses.

### Brotli

[Brotli](https://github.com/google/brotli), by the Brotli Authors and Google Inc.,
is included in [`BaseTools/Source/C/BrotliCompress`](BaseTools/Source/C/BrotliCompress)
as a host compression tool. Its [original MIT license](BaseTools/Source/C/BrotliCompress/LICENSE)
is preserved, with an identical [distribution copy](LICENSES/MIT-Brotli.txt).
The EDK II integration retains its separate source headers.

### LZMA SDK

[LZMA SDK](https://www.7-zip.org/sdk.html), by **Igor Pavlov**, is included in
[`BaseTools/Source/C/LzmaCompress/Sdk`](BaseTools/Source/C/LzmaCompress/Sdk).
The bundled documentation identifies SDK **18.05**, placed in the public domain.
See the [SDK notice](BaseTools/Source/C/LzmaCompress/Sdk/DOC/lzma-sdk.txt),
[integration provenance note](BaseTools/Source/C/LzmaCompress/LZMA-SDK-README.txt)
and [distribution notice copy](LICENSES/LZMA-SDK-Public-Domain.txt).
The SDK documentation also acknowledges **Dmitry Shkarin** for PPMd and **Wei Dai**
for SHA-256 in the full SDK. Those algorithms are not among this tree's retained
C source files; this acknowledgment preserves the SDK's own wider credits.

### PCCTS / ANTLR / DLG

The Purdue Compiler Construction Tool Set (**PCCTS**), including **ANTLR 1.33**
and **DLG 1.33** (bundled sources identify maintenance release **1.33MR33**), is
included in [`BaseTools/Source/C/VfrCompile/Pccts`](BaseTools/Source/C/VfrCompile/Pccts).
**The upstream BaseTools VFR compiler incorporates PCCTS source and is built
using PCCTS parser-generation tools.** This is the acknowledgment requested by
the PCCTS authors.

Credit to **Terence Parr**, **Russell Quong**, **Will Cohen** and **Hank Dietz**;
**Parr Research Corporation**, **Purdue University Electrical Engineering**, and
**AHPCRC, University of Minnesota**. DLG credits Will Cohen, with modifications by
Terence Parr. Source and maintenance records also credit **Tom Moog**,
**Ger Hobbelt**, **Alexey Demakov**, **Chris Uzdavinis**, **Kenji Tanaka**,
**Volker H. Simonis**, **Jean-Francois Pieronne**, **Jan Langer**,
**Tomasz Babczynski**, **Anton Sergeev**, **Tom Molteno**, **Stanislaw Bochnak**,
and the contributors, porters and problem reporters in the complete upstream
acknowledgment records retained in:

- [`README`](BaseTools/Source/C/VfrCompile/Pccts/README).
- [`CHANGES_FROM_133.txt`](BaseTools/Source/C/VfrCompile/Pccts/CHANGES_FROM_133.txt).
- [`CHANGES_FROM_133_BEFORE_MR13.txt`](BaseTools/Source/C/VfrCompile/Pccts/CHANGES_FROM_133_BEFORE_MR13.txt).
- [`NOTES.msvc`](BaseTools/Source/C/VfrCompile/Pccts/NOTES.msvc) and individual source headers.

PCCTS states that it is in the public domain and asks for credit and intact
headers. Its [original RIGHTS notice](BaseTools/Source/C/VfrCompile/Pccts/RIGHTS)
and [distribution copy](LICENSES/PCCTS-RIGHTS.txt) preserve those terms.
The [complete upstream acknowledgment records](LICENSES/PCCTS-ACKNOWLEDGMENTS.txt)
are also included with distributions so that all names remain available in the
EFI package as well as the source archive.

### FreeBSD ELF definitions

**John D. Polstra** and the FreeBSD project are credited for the ELF definitions
ported into [`GenFw/elf32.h`](BaseTools/Source/C/GenFw/elf32.h),
[`elf64.h`](BaseTools/Source/C/GenFw/elf64.h) and
[`elf_common.h`](BaseTools/Source/C/GenFw/elf_common.h).
These host-tool headers retain Polstra's BSD-2-Clause notice alongside the EDK II
porting notices from Apple and ARM. Full original notices are reproduced in
[`LICENSES/Upstream-File-Notices.txt`](LICENSES/Upstream-File-Notices.txt).

### Python logging fallback

**Vinay Sajip** is credited for the `QueueHandler` fallback in
[`BaseTools/Source/Python/Common/EdkLogger.py`](BaseTools/Source/Python/Common/EdkLogger.py).
Its separate permission notice, restriction on use of the author's name, and
disclaimer are preserved in that file and reproduced in
[`LICENSES/Upstream-File-Notices.txt`](LICENSES/Upstream-File-Notices.txt).
This is host-side Python code.

### Apple cctools / mtoc

**Apple Inc.** is credited for the mtoc sources in
[`BaseTools/Source/C/Mtoc`](BaseTools/Source/C/Mtoc).
[`mtoc-v921_jief.c`](BaseTools/Source/C/Mtoc/mtoc-v921_jief.c) additionally credits
**jief** for retaining `__mod_init_func` to support global C++ variables.
Both it and `mtoc-v921.c.origin` retain Apple's **APSL-2.0** headers.
A full [APSL-2.0 license copy](LICENSES/APSL-2.0.txt) is included from
[Apple's cctools distribution](https://github.com/apple-oss-distributions/cctools/blob/cctools-921/APPLE_LICENSE).
The local filenames identify v921; the complete import and modification history
is not established. These retained host-tool sources are outside the current
BaseTools GNU make target list and are not part of the x64 EFI application.

### Public test fixtures

Public EDK II test keys and certificates are upstream test fixtures. Their paths
and purpose are documented in [`SECURITY-NOTES.txt`](SECURITY-NOTES.txt).
They are not production signing credentials.

## External build, signing and test tools

The following projects and their authors/contributors are acknowledged for tools
used by the documented build, signing, verification and CI workflows. Their
executables are installed separately and are not shipped in the source or EFI
release archives. Their own distributions supply the applicable licenses and
dependency notices.

| Project | Contribution |
| --- | --- |
| [GNU GCC](https://gcc.gnu.org/) and [GNU binutils](https://www.gnu.org/software/binutils/) | C/C++ compilation and object/link tools |
| [NASM](https://www.nasm.us/) | x86 assembly |
| [LLVM / Clang / LLD](https://llvm.org/) | Host tests, analysis, sanitizers and PE/COFF test fixtures |
| [GNU Make](https://www.gnu.org/software/make/) and [GNU Bash](https://www.gnu.org/software/bash/) | Build and test orchestration |
| [Python](https://www.python.org/) and its contributors / Python Software Foundation | EDK II tools, audit, manifest and signature verification scripts |
| [OpenSSL](https://openssl-library.org/) | Certificates, cryptographic operations and signature checks |
| [Java / OpenJDK](https://openjdk.org/) and the installed runtime's supplier | Runtime for Jsign; licensing depends on the installed distribution |
| [Jsign](https://ebourg.github.io/jsign/), Emmanuel Bourg and contributors | Authenticode signing on macOS; the bootstrap obtains the official hash-pinned JAR into an external cache |
| [sbsigntools](https://git.kernel.org/pub/scm/linux/kernel/git/jejb/sbsigntools.git/) | Linux UEFI image signing and verification |
| [QEMU](https://www.qemu.org/) and [TianoCore OVMF](https://github.com/tianocore/edk2/tree/master/OvmfPkg) | Virtual-machine UEFI chainload smoke tests |
| [dosfstools](https://github.com/dosfstools/dosfstools) and [GNU mtools](https://www.gnu.org/software/mtools/) | FAT test-image creation and population |
| [util-linux / libuuid](https://github.com/util-linux/util-linux) | Linux BaseTools build dependency |
| [curl](https://curl.se/), [Git](https://git-scm.com/), [Info-ZIP](https://infozip.sourceforge.net/), and host system utilities | Dependency retrieval, source archives, release packaging and checksums |
| [GitHub Actions checkout](https://github.com/actions/checkout), [upload-artifact](https://github.com/actions/upload-artifact), [download-artifact](https://github.com/actions/download-artifact), and [GitHub CLI](https://cli.github.com/) | Repository checkout, CI artifacts and release publication |
| [Homebrew](https://brew.sh/), Debian/Ubuntu package maintainers, and Apple developer tools | Host dependency installation and tool distributions |

Ancillary upstream BaseTools sources also refer to external
[ANTLR 3](https://www.antlr3.org/) / [ANTLR 4](https://www.antlr.org/) runtimes
(the generated ECC/EOT parsers identify 3.0.1 and 4.7.1),
[wxPython](https://wxpython.org/), [Doxygen](https://www.doxygen.nl/) and Microsoft
HTML Help. These belong to optional upstream analysis/documentation utilities;
they are not bundled runtimes or requirements of the normal selector build.

The legacy `toolchain.gcc102.snb` binary package is supplied separately by users
who already have it. Its matching source and full provenance have not been
established. The project ships a checksum catalog for verification only, as
explained in [`Toolchains/README.md`](Toolchains/README.md). A filename or a build
variable containing a project name is not evidence of that project's authorship.

## Supported bootloaders

**[OpenCore](https://github.com/acidanthera/OpenCorePkg)**, by Acidanthera and its
contributors, and **[Clover](https://github.com/CloverHackyColor/CloverBootloader)**,
by the Clover team and its contributors, are acknowledged as supported chainload
targets. Their EFI binaries are supplied by the user. Detecting and starting
these projects does not establish that their implementation code is included in
BootLoaderSelector, nor does it imply endorsement by either project.

For the component-by-component license index and exact notice locations, see
[`THIRD-PARTY-NOTICES.txt`](THIRD-PARTY-NOTICES.txt). Keep this document, that index,
the root license and `LICENSES/` with distributions, and retain the original
headers when redistributing upstream source.
