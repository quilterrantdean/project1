# BiblioLED

Personal application for recording and managing the user's own BiblioLED loans. This is not public-library management software.

## Project status

This repository contains an early C++ / Win32 / SQLite proof of concept. The legacy Python/Tkinter implementation is retained as a functional reference while the replacement is developed.

The C++ application is being prepared as a 1.0.0 release candidate; it is not yet an official supported release. The user has confirmed that it starts and runs on Windows XP SP3; this is a manual smoke test, not a completed validation matrix. Date validation, calendar-day expiry, operational visibility and quota calculations are separated into portable core code with independent tests. It includes an About dialog, a general settings dialog (changes apply on next launch), immediate backup-compression settings, and a searchable history window for expired and future-dated loans. The read-only Statistics window reports type totals, annual evolution and year comparisons, selected-year daily/weekday/hour patterns, and full-history mean/median intervals between active dates. It also creates at most one automatic backup before the first loan-data change per local day, retaining 7 daily, 4 weekly and 12 monthly automatic snapshots. Manual backups and restore/migration safety copies are separate and are not pruned automatically. Older generic `.sqlite` backups are also preserved because their original category cannot be determined safely. New backups use statically linked zlib 1.4 with configurable gzip/zlib format and compression level (0–9, default 6); changes apply immediately without restarting, compressed round-trip integrity is checked, and older uncompressed `.sqlite` backups remain restorable. Backup paths on Windows use native backslashes. The backup/compression implementation has passed Linux, modern Win32 and XP-targeted w64devkit x86 CI on earlier source revisions. The XP workflow is manual-only; the current Arena branch has a passing XP-targeted run recorded below. The user's XP smoke test does not replace full recovery, restore and failure-path validation. The application icon is embedded in the executable; main-table columns are draggable and their order and widths persist in `config/settings.ini`. Guided recovery for missing/corrupt databases, the general settings UI, schema version 0-to-1 migration, and the internal/external audit trail are implemented and covered by isolated tests. Later schema migrations (deferred until required), full target-machine recovery and backup/restore scenarios, performance measurements, and the normal portable release process remain outstanding. The current build is a disposable proof of concept, not a supported release. The persistence layer independently validates loan dates before inserts/updates; these checks and the SQLite connection-close fallback passed Core CI run [44934716483](https://github.com/quilterrantdean/project1/actions/runs/44934716483). The current Arena branch passed the XP-targeted w64devkit build, all 9 tests (including year-2099 date conversion), PE32/i386, subsystem 5.01, and import checks in [run 44935522857](https://github.com/quilterrantdean/project1/actions/runs/44935522857). Manual execution and recovery/backup testing on Windows XP SP3 remain outstanding. Do not use this proof of concept with the user's real database. The complete loan history can also be exported as semicolon-delimited UTF-8 CSV; exports are audited and are not database backups.

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

This import was integrated into the active Arena branch `arena/998cf727-project1`. Never commit directly to `main` or `master`.
