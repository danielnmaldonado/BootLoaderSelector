# Build dependencies

BootLoaderSelector 2.33
Credits: ©DNMTechLabs / dnmtechlabs@gmail.com

This source distribution excludes the executables from the legacy
`toolchain.gcc102.snb` package. The exact origin and corresponding source of
that binary distribution have not been established, so its binaries are not
redistributed in this repository or its source ZIP.

## Linux

The builder uses the host's GCC, G++, gcc-ar, objcopy and NASM. This path neither
executes nor requires the macOS toolchain catalog. The build provenance records
hashes of the tools actually used. See the dependency installation command in
the [project README](../README.md).

## macOS

An existing owner of the original v2.31/Round86 toolchain can keep it outside the
repository and set `BLS_TOOLCHAIN_DIR` to its directory. Use a path without
spaces or symbolic links, and retain the original executable permissions.

```sh
BLS_TOOLCHAIN_DIR=/path/without/spaces/toolchain.gcc102.snb \
  ./Build-BootLoaderSelector-Release.command
```

`gcc102-snb.sha256` catalogs all 951 original files. Before executing any tool
from this dependency, the builder checks the catalog, the exact file set and
every hash. It does not download cross-toolchains automatically. Apple Silicon
also depends on Rosetta compatibility with these Intel binaries; that platform
is not covered by the recorded Intel macOS validation.

The catalog identifies bytes. It does not establish provenance, authorship or
redistribution rights. Use the Linux build path for builds independent of this
legacy dependency. Keep its binaries outside the versioned source tree.

## Signing dependencies

Jsign is separate from the cross-toolchain. If no supported local signer is
available, the signing helper can fetch the official Jsign 7.5 JAR into a cache
outside the project and accept it only after checking its pinned hash.
`BLS_JSIGN_AUTO_DOWNLOAD=0` disables this bootstrap;
`BLS_JSIGN_CACHE_DIR` selects a different cache directory.

The signer also requires Java and OpenSSL. On macOS, expose the installed
OpenSSL `bin` directory on `PATH` rather than using the system LibreSSL. Signing
regressions additionally require `lld-link`; Homebrew provides it in `lld@21`.

Project authorship and upstream attributions are in [CREDITS.md](../CREDITS.md)
and [THIRD-PARTY-NOTICES.txt](../THIRD-PARTY-NOTICES.txt).
