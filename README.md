# BiblioLED

Personal application for recording and managing the user's own BiblioLED loans. This is not public-library management software.

## Project status

This repository contains the C++ / Win32 / SQLite 1.0.0 release candidate. The legacy Python/Tkinter implementation is retained as a functional reference.

The C++ application is being prepared as a 1.0.0 release candidate; it is not yet an official supported release. The user reports that target-machine testing on Windows XP SP3 was successful; the exact tested package/hash and scenario-by-scenario record are not yet attached to this repository. Date validation, calendar-day expiry, operational visibility and quota calculations are separated into portable core code with independent tests. It includes an About dialog, a general settings dialog (changes apply on next launch), immediate backup-compression settings, and a searchable history window for expired and future-dated loans. The read-only Statistics window reports type totals, annual evolution and year comparisons, selected-year daily/weekday/hour patterns, and full-history mean/median intervals between active dates. It also creates at most one automatic backup before the first loan-data change per local day, retaining 7 daily, 4 weekly and 12 monthly automatic snapshots. Manual backups and restore/migration safety copies are separate and are not pruned automatically. Older generic `.sqlite` backups are also preserved because their original category cannot be determined safely. New backups use statically linked zlib 1.4 with configurable gzip/zlib format and compression level (0–9, default 6); changes apply immediately without restarting, compressed round-trip integrity is checked, and older uncompressed `.sqlite` backups remain restorable. Backup paths on Windows use native backslashes. The backup/compression implementation has passed Linux, modern Win32 and XP-targeted w64devkit x86 CI on earlier source revisions. The XP workflow is manual-only; the current Arena branch has a passing XP-targeted run recorded below. The user reported on 2026-11-21 that XP testing was successful; the exact artifact/hash and scenario-by-scenario record are still needed for release traceability. The application icon is embedded in the executable; main-table columns are draggable and their order and widths persist in `config/settings.ini`. Guided recovery for missing/corrupt databases, the general settings UI, schema version 0-to-1 migration, and the internal/external audit trail are implemented and covered by isolated tests. Later schema migrations remain deferred until required, and performance measurements on the target hardware are outstanding. The user reports that XP testing passed; the exact tested package/hash and scenario matrix still need to be recorded for release traceability. The current branch creates a 1.0.0 candidate ZIP; it is not yet an official supported release. The persistence layer independently validates loan dates before inserts/updates; these checks and the SQLite connection-close fallback passed Core CI run [44938469974](https://github.com/quilterrantdean/project1/actions/runs/44938469974). The current Arena branch passed the XP-targeted w64devkit build, all 9 tests (including year-2099 date conversion), PE32/i386, subsystem 5.01, and import checks in [run 44939158463](https://github.com/quilterrantdean/project1/actions/runs/44939158463). The workflow also creates a candidate ZIP with a manifest check and SHA-256 file; the current ZIP hash is `5c4ae7225d520b8262d8e74f9b6252827ae502a5fa8d0e14b1920697b149ba8e`. The user reports successful XP testing; the exact artifact/hash and scenario record are still pending. Do not treat this candidate as an official release; maintain independent backups and do not migrate the real database until a supported release is approved. The complete loan history can also be exported as semicolon-delimited UTF-8 CSV; exports are audited and are not database backups.

## Current direction

- Windows XP SP3, 32-bit (Win32) is the primary target.
- Native Win32 controls and a compact, utility-style interface.
- SQLite is the required database engine.
- C++11 and CMake are used for the initial proof of concept.
- The initial XP-targeted compiler baseline is the x86 edition of w64devkit; the dedicated workflow pins a release and checks the resulting PE target.
- SQLite 3.53.4 is pinned for the XP prototype because SQLite 3.54.0 introduced a Windows SRW-lock dependency unavailable on XP.
- SQLite user_version=1 identifies the current schema. Legacy version 0 databases require explicit, backed-up migration before writes.
- The application icon is compiled into `BiblioLED.exe`; no separate `.ico` is required in the runtime folder or ZIP.
- Main-table column order and widths are saved to `config/settings.ini`.
- User data, settings, backups, exports, and logs remain in the portable application folder.

## Build and tests

See [docs/building.md](docs/building.md).

GitHub Actions builds and tests the SQLite store on Linux, checks a modern Win32 build, and builds with the XP-targeted w64devkit x86 toolchain. The XP build passes the loan-store and audit-log tests and PE32/i386/subsystem checks, then publishes a disposable test artefact. No workflow running on a modern Windows host can replace the final execution test on Windows XP SP3.

## Development notes

- `AGENTS.md` contains instructions for coding agents.
- `.agent/plan.md` tracks the project phases.
- `.agent/decisions.md` records user-approved requirements.
- `.agent/architecture.md` describes the target component boundaries.
- `.agent/codebase-review.md` records the legacy implementation review and prototype status.

This repository contains source imported from the upstream BiblioLED `development` branch. Active Arena development follows the branch named by the current session; never commit directly to `main` or `master`.
