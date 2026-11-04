# Historical audit records

Credits: ©DNMTechLabs / dnmtechlabs@gmail.com

The current audit and coverage records are [`../AUDIT-REPORT.txt`](../AUDIT-REPORT.txt) and [`../COVERAGE.md`](../COVERAGE.md). They describe completed verification of the corrected v2.32 baseline and identify the limits of that evidence.

The previous mixed-language reports, Round67–85 diffs and consolidation patch are preserved unchanged in Git history at [tag v2.32](https://github.com/danielnmaldonado/BootLoaderSelector/tree/v2.32/AuditReports). Their exact source commit is `ddc9a38ced0a8e6001316a612115cfadb053fe05`.

Before the English documentation update, all 24 files in the original `AuditReports` tree were also copied byte for byte to the external workspace archive `work/english-original-audit-records/AuditReports`. Its adjacent `BACKUP-MANIFEST.json` records each original path, byte size and SHA-256. The local archive is outside the repository and is not required to obtain the originals from the tag.

The current tree removes these redundant historical snapshots:

- `AuditReports/CHANGES.patch`, the previous report-consolidation patch.
- `AuditReports/Diffs/ROUND67.diff` through `ROUND85.diff`, 19 historical diffs.
- The historical material embedded in the former `AUDIT-REPORT.txt` and `COVERAGE.md`, now replaced by current English records.

The patches were archived rather than translated: changing their bytes would make them different historical evidence. Original attribution, content and checksums remain available in the archive and tag. The root license, upstream file headers, third-party notices and license copies remain authoritative.

[`SECURITY-NOTES-v2.31.txt`](SECURITY-NOTES-v2.31.txt) remains here as an English historical record. Its descriptions of the then-bundled toolchain and source layout apply to v2.31. Consult [`../../SECURITY-NOTES.txt`](../../SECURITY-NOTES.txt), [`../../Toolchains/README.md`](../../Toolchains/README.md) and [`../../THIRD-PARTY-NOTICES.txt`](../../THIRD-PARTY-NOTICES.txt) for the current distribution and trust boundaries.
