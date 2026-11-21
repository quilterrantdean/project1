# Building BiblioLED

## Project status and scope

BiblioLED is being rewritten as a native C++11 / Win32 / SQLite application. The intended Windows target is **32-bit Windows XP SP3**, including Pentium 4/SSE2 hardware.

The current application is a proof of concept, not a supported release. It already implements loan CRUD, history/statistics, CSV export, settings, compressed backup/restore, retention, SQLite schema migration, guided recovery, and internal/external audit logging. The safety and CI work is substantially more complete than the GUI's target-machine validation.

**Do not use the current proof of concept with the real BiblioLED database.** Automated tests create isolated synthetic databases. The downloadable XP artefact contains no database.

The user confirmed on 2026-11-16 that the executable runs well on Windows XP. Secondary-window Portuguese text encoding was confirmed corrected on 2026-11-15. The exact executable hash, XP build and exercised scenarios have not been recorded. The recovery wizard, backup/restore failure paths and a reproducible full target-system test record remain outstanding.

## Toolchains and dependencies

### XP-targeted Windows build

The pinned baseline is the **x86 edition of w64devkit 2.10.0**. The expected compiler target is `i686-w64-mingw32`; the x86 package requires SSE2 and is intended to support Windows XP. This matches the Pentium 4 target.

The CI workflow downloads the upstream release archive and verifies SHA-256 before extraction:

- w64devkit x86 2.10.0: `513ff7fc571cf6764a5aedbca92f43c513f8d01085c65df5133a82db13a7e63d`

The XP CMake preset uses CMake 3.28 or later (preset schema version 6). The w64devkit package used by CI contains the CMake executable and build tools used by that workflow.

### Windows dependencies

At first Windows configure, CMake downloads and verifies these source archives:

- SQLite amalgamation 3.53.4, statically linked. SHA3-256: `628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e`.
- zlib 1.4, statically linked. The source is fetched from the upstream `madler/zlib` v1.4 GitHub Release asset, not the zlib.net fossil endpoint. SHA-256: `02dd159783677a749c04adbdd02b55b8a2bcc7e054f51b76b549199a916ad584`.

SQLite 3.53.4 is pinned because the Windows implementation in SQLite 3.54.0 introduced a dependency on Slim Reader/Writer Locks, which Windows XP does not provide. Do not update SQLite without a deliberate XP compatibility review.

The zlib URL was moved to the official upstream GitHub Release asset after the zlib.net fossil endpoint returned inconsistent contents and repeated SHA-256 mismatches in CI. The published upstream digest is retained; the hash check has not been weakened.

### Linux development/test host

On Debian/Ubuntu, install the system development packages:

```sh
sudo apt-get update
sudo apt-get install -y cmake g++ make libsqlite3-dev zlib1g-dev
```

The portable Linux build uses the host's SQLite and zlib development packages. The Windows build uses the pinned SQLite amalgamation and pinned static zlib source.

## Build and test

### Linux core tests

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The Linux suite covers the portable core, SQLite store, backup compression, and retention. It does not build the Win32 UI or the Windows-only external audit-log test.

### Win32 build with Visual Studio

Use a 32-bit Visual Studio generator:

```powershell
cmake -S . -B build -A Win32 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

`-A Win32` is a Visual Studio generator option. It is not used with MinGW generators. The modern-Windows CI job compiles the Win32 application and runs the Windows tests, but it is not an XP runtime test.

### Windows XP target with w64devkit x86

Open the x86 w64devkit shell and change to the repository root. Check that the compiler is the expected 32-bit target:

```sh
gcc -dumpmachine
```

Expected output:

```text
i686-w64-mingw32
```

Then configure, build, and run the tests:

```sh
cmake --preset xp-x86-w64devkit
cmake --build --preset xp-x86-w64devkit --parallel
ctest --preset xp-x86-w64devkit
```

The executable is written to:

```text
build/xp-x86-w64devkit/BiblioLED.exe
```

The XP workflow additionally checks that the executable is PE32/i386, uses Windows GUI subsystem 5.01, and does not import a selected list of post-XP APIs or API-set DLLs. It packages a disposable test ZIP containing the executable, example settings, PE inspection report, and test instructions.

### CI test matrix

The `Core tests` workflow has three jobs:

- Ubuntu: portable core/store/backup tests.
- Modern Windows: Win32 compilation plus SQLite-store and Windows audit-log tests.
- Cppcheck: version-gated Cppcheck 2.19.0 analysis of selected portable C++11 sources and tests.

The separate `XP-targeted Win32 build` workflow uses pinned w64devkit x86 2.10.0, builds the application and tests, runs all nine XP-targeted tests, inspects the PE executable/imports, and uploads the disposable package.

The Windows/XP test set comprises:

1. `application_paths_tests`
2. `settings_store_tests`
3. `loan_csv_export_tests`
4. `loan_rules_tests`
5. `loan_statistics_tests`
6. `loan_store_tests`
7. `backup_compression_tests`
8. `backup_retention_tests`
9. `audit_log_tests` (Windows only)

### Latest validated build

Current application-source head: `e469c816f10451188ff4b3363d18c3ff5bf675f1`.

- [Core tests run 44536364468](https://github.com/quilterrantdean/BiblioLED/actions/runs/44536364468) passed on this source head: Ubuntu core tests (8/8), modern Win32 application/store build and tests (9/9), and Cppcheck 2.19.0. This includes the canonical date parser, local-calendar day-count/DST tests, and the 800×600 main-window adjustment.
- The latest successful [XP-targeted build run 43903363630](https://github.com/quilterrantdean/BiblioLED/actions/runs/43903363630) is from source revision `749de809ed1c291139fca701056ea1fac9169336`. Its pinned w64devkit x86 2.10.0 build, all nine XP tests, PE32/i386, GUI subsystem 5.01, and post-XP import checks passed. The current application source has not yet been rebuilt with the XP toolchain.

The latest successful disposable artefact is [14243881773](https://github.com/quilterrantdean/BiblioLED/actions/runs/43903363630/artifacts/14243881773), `BiblioLED-XP-PoC`:

- ZIP size: 1,568,595 bytes

The archive contains exactly four files: `BiblioLED.exe`, `config/settings.ini`, `pe-inspection.txt`, and `LEIA-ME-TESTE.txt`. It does not contain a database or a separate icon file; the application icon is embedded in the executable. The Portuguese test guide is UTF-8 with BOM for Windows XP Notepad.

This ZIP is a disposable CI test artefact, not a normal portable distribution or release. The user confirmed that the executable runs well on Windows XP; the exact manually tested executable hash and scenario list remain unrecorded.

## Portable directory layout and settings

The application stores its data beside the executable. The expected structure is:

```text
BiblioLED/
  BiblioLED.exe
  config/
    settings.ini
    database.initialized
  data/
    biblioteca.sqlite
    backups/
  exports/
  logs/
    audit.log
    audit_*.log
```

`config/settings.ini` is supplied as an example. The icon is embedded in `BiblioLED.exe`; `port.ico` remains in the source repository as the resource input.

The general settings dialog persists loan limits, loan duration, diagnostics, and audit-log size. General settings take effect on the next launch. The separate backup-settings dialog applies compression format and level immediately to the open store.

### INI settings persistence

The portable C++11 `settings_store` service owns the `AppSettings` model, INI parser, value validation, serialization, and file I/O. The existing `config/settings.ini` field names, defaults and CRLF representation are retained. Paths enter the service as UTF-8; Windows XP file operations use wide-character Win32 APIs.

The reader accepts an optional UTF-8 BOM and caps the file at 64 KiB. It rejects malformed values, duplicate recognised keys, embedded NUL bytes, oversized files and read errors. If the file is missing, defaults are created atomically. If it is invalid or unreadable, defaults are used only in memory and the original file is preserved.

Saving validates the new values before writing, then rereads and validates an existing target. The service refuses to overwrite an invalid or unreadable INI even if a future caller bypasses the current UI guard. It uses an exclusive temporary filename containing the process ID and an attempt number, writes the complete contents and flushes them before atomic installation. Windows uses `FlushFileBuffers` plus write-through `MoveFileExW`; POSIX uses `fsync` on the temporary file and `rename`. The POSIX parent directory is not separately fsynced, so this does not claim full directory-entry persistence after physical power loss.

`settings_store_tests` runs in the Linux, modern-Windows and XP-targeted suites. Its cases cover defaults creation, save/load round-trip, replacement of an existing INI, byte-for-byte preservation of invalid files, refusal to overwrite an invalid destination, duplicate keys/BOM, invalid values in memory, temporary-name collision, replacement failure, directory-as-file and oversized files. All paths are created in isolated temporary directories, including Unicode names; no real user settings are opened or modified.

On Windows, the entry point obtains the executable filename with `GetModuleFileNameW` and passes it to the pure C++11 `BuildApplicationPaths` model. The model derives the complete application root, config/data/backup/export/log directories, settings file, database file, and initialisation marker. Drive-root and UNC installations are supported; Unicode is retained in wide strings. The INI file and other Win32 filesystem operations use wide-character APIs, while paths passed to the SQLite store are converted to UTF-8.

The executable path must be absolute and must fit the Windows `MAX_PATH` buffer used by the XP-compatible entry point. Failure or truncation stops startup before the mutex, settings, or database are opened. The application no longer falls back to `.` or the process working directory, preventing the launch location from silently selecting a different database. Required directories are validated on startup; if a required path is a file rather than a directory, startup stops before opening the database.

The path model is covered by isolated tests for normal paths, drive roots, UNC shares, Unicode folder names, slash normalisation, and rejection of relative/incomplete paths.

## SQLite schema, migration, and recovery

The schema version is stored in `PRAGMA user_version`. The legacy `loans` table is accepted only in its exact four-column form: `id INTEGER PRIMARY KEY` with `title`, `loan_date`, and `type` declared `TEXT NOT NULL`. Look-alike tables with a text ID, no integer primary key, or unsupported extra columns are rejected without modification:

- Version 0 is the legacy `loans(id, title, loan_date, type)` table.
- Version 1 is the same loans table plus `audit_events` and its indexes. The audit table has exactly 11 columns: `id INTEGER PRIMARY KEY`, required `occurred_at`, `action`, and `details` as TEXT, nullable INTEGER `loan_id`, and six nullable TEXT before/after fields. Wrong declared types, wrong nullability, a malformed primary key, or extra columns are rejected.

New databases are created directly at version 1. An existing version-0 database can be read, but loan changes are blocked until the user explicitly confirms migration. Migration first creates and validates a complete safety backup, then adds the audit table/indexes and records the migration in one SQLite transaction. The existing loan rows are not rewritten. A failed migration rolls back its partial DDL and leaves the database at version 0.

After first successful initialisation, `config/database.initialized` distinguishes a missing database from first launch. If the marker exists but the database is missing, startup does not create an empty replacement. If the existing database cannot be opened, startup offers guided recovery from a user-selected backup.

The recovery flow is explicit, not automatic. The candidate backup is decompressed and checked for stream integrity, SQLite integrity, and a supported BiblioLED schema. It is copied into a staging database and checked again. The existing database and SQLite `-journal`, `-wal`, and `-shm` sidecars are preserved before the staged database is installed. The selected backup is not consumed. If recovery fails, the application attempts to restore the original files and reports any incomplete rollback. All automated recovery tests use synthetic databases and temporary directories.

## Backups, compression, and retention

Loan-data changes create at most one automatic snapshot per local calendar day, immediately before the first actual loan-data mutation of that day. A valid same-day snapshot is reused. Audit-only events, startup/shutdown events, settings changes, and read-only sessions do not create automatic loan-data snapshots.

Backups use statically linked zlib 1.4 and standard wrappers:

- gzip: `.sqlite.gz`
- zlib: `.sqlite.zlib`

The compression format and level (0–9, default 6) are set in the backup-settings dialog and stored in `config/settings.ini`. Raw DEFLATE is not used. Older uncompressed `.sqlite` backups remain restorable.

The backup pipeline streams through fixed-size buffers. It creates a temporary SQLite Online Backup snapshot, runs `PRAGMA integrity_check`, compresses the snapshot, decompresses it again to a temporary database, and checks integrity/schema again. Only a fully validated result is installed under its final backup name. Completed compressed/decompressed output is flushed through the C runtime and then synchronised to the operating system (`FlushFileBuffers` on Windows, `fsync` on POSIX) before the file is closed and accepted. On Windows, final backup moves use `MoveFileExW` with write-through semantics.

Automatic retention keeps the latest 7 daily snapshots, one per week for the following 4 weeks, then one per month for the following 12 months. Only recognised automatic backups are pruned, and pruning occurs after a new automatic snapshot has been validated. Manual backups and safety backups before restore/migration are separate categories and are never removed by automatic retention. Unknown/malformed filenames are preserved.

Manual backups use the `BiblioLED_manual_*` namespace. Restore/migration safety backups use `BiblioLED_safety_*`. Automatic backups use `BiblioLED_auto_*`.

Restore accepts `.sqlite.gz`, `.sqlite.zlib`, and legacy uncompressed `.sqlite`. It validates the selected source before creating a safety copy of the current database. After replacement, the restored database is checked; if verification fails, the store attempts rollback from the safety copy. A rollback failure is reported, and the database connection is not reused in an uncertain transaction state.

The file-data flush is not a universal promise against every power-loss scenario: POSIX parent-directory metadata is not separately fsynced after rename, and physical power-loss testing remains outstanding. Automated abrupt-process termination and recovery tests exist, but they do not simulate a physical power cut.

## Audit logging

SQLite `audit_events` is the authoritative audit history. Loan create/edit/delete and their audit events are committed in the same transaction. Current internal event coverage includes loan CRUD, schema migration, application startup/shutdown, successful manual backup/restore, general/backup settings changes, CSV export, and application warnings, errors, and failures. Diagnostic events are stored in SQLite; external-log synchronisation remains subject to the documented read-only safety gate.

When external diagnostics are enabled, committed events are synchronised to `logs/audit.log`, with timestamped `audit_*.log` archives. Stable fingerprints prevent duplicate replay. An incomplete final line in the active log can be truncated and replayed from SQLite; an incomplete archived log is rejected unchanged. Archives are not automatically deleted.

The scanner reads files with a fixed 64 KiB buffer and bounded per-line parser state; it does not keep complete archive contents in memory. However, its de-duplication set retains one fingerprint per distinct event found across the active log and archives, so total memory still grows with the number of retained historical events.

If external diagnostics are deliberately disabled, SQLite auditing continues and external synchronisation pauses; the backlog is replayed when diagnostics are re-enabled. If diagnostics are enabled but synchronisation fails, reads remain available but further database mutations are blocked until logging recovers. The recovery command is **Ficheiro > Tentar sincronizar registo de auditoria...**. It is not bound to F5.

## Column sorting

The main loans list and full-history list sort when a column header is clicked. The first click on a column sorts ascending; clicking that same header again sorts descending. Titles and types use case-insensitive alphabetical order, IDs and remaining days use numeric order, and loan/expiry dates use chronological order. Sorting is performed on the displayed in-memory rows and does not modify database records.

## Safe manual XP test

Use a fresh disposable folder, for example `C:\Temp\BiblioLED_ação`. Do not copy the real database or replace the current installation.

The package's `LEIA-ME-TESTE.txt` contains the detailed manual test sequence. It includes:

- first launch and creation of a synthetic database;
- creating, editing, deleting, and viewing fictitious loan records;
- ascending/descending sorting in the main and history lists, including alphabetical text, numeric IDs/days and chronological dates;
- history filters, statistics, CSV export, and settings persistence;
- gzip/zlib format and compression-level changes;
- manual backup and restore from a test backup;
- startup recovery from a deliberately missing/corrupt test database;
- Unicode paths such as `C:\Temp\BiblioLED_ação`;
- required-directory collision behaviour.

Run the tests in a separate disposable directory. Preserve the real installation and data unchanged.

## Compatibility and remaining limitations

The target PE is 32-bit i386 with Windows GUI subsystem 5.01. CI rejects selected post-XP APIs and API-set DLL imports, and the compiler package is the XP-compatible x86 w64devkit build. This is build/import compatibility evidence, not a substitute for running the executable on Windows XP SP3.

Before considering a normal portable distribution, the project still needs:
- a reproducible manual Windows XP SP3 runtime test, including resource glyphs, recovery wizard, and dialogs;
- failure injection for the `ROLLBACK` statement itself and abrupt process/power-loss interruption;
- a fresh XP-targeted build of the latest source;
- later schema migration work as product requirements evolve;
- a final release/build process and portable ZIP review.

Do not use this proof of concept with the user's real application data.

## Diagnostic audit coverage

The application records user-visible validation warnings and operation failures internally as `application_warning`, `application_error`, and `application_failure` audit events. The main window covers loan-dialog validation, general-settings validation, invalid settings files, and operational failures. The history and statistics windows queue their data-read and calculation failures. Entries include a context identifier and diagnostic details; they do not copy full user-entered form contents.

Diagnostic rows can still be queued in SQLite while the external log is unhealthy. This does not mark the external log healthy or permit loan-data changes: the existing read-only safety gate remains active. The queued diagnostic events are included by the normal synchronisation lifecycle when logging becomes available again. Errors before database opening, during legacy migration, or when SQLite cannot accept a write cannot be recorded internally.

## Explicit ROLLBACK failure test

The XP-targeted and modern Win32 test builds execute a deterministic rollback-failure case in `loan_store_tests`. An SQLite authorizer rejects the explicit `ROLLBACK` statement while a transaction contains an uncommitted row. The store must report the failure and close its connection; reopening the same isolated test database must discard the uncommitted row and pass `quick_check`.

This does not simulate sudden power loss or termination of the process. Those tests remain in the development plan.

## Abrupt process termination recovery test

The XP-targeted and modern Win32 test builds run a process-interruption recovery scenario in `loan_store_tests`. A child process inserts 128 synthetic rows inside an uncommitted SQLite DELETE-journal transaction, forces cache spilling and calls `sqlite3_db_cacheflush`, then terminates without closing SQLite. The parent verifies that the hot journal is present, reopens the database, confirms that the committed baseline record is retained and the uncommitted rows are discarded, and checks that the journal is removed and `quick_check` succeeds.

Windows uses `CreateProcessW`/`TerminateProcess`; POSIX uses `fork`/`_exit`. This tests abrupt process termination, not physical power loss or failure of volatile storage-device caches.

## Future SQLite schema versions

The application currently supports schema versions 0 and 1. If `PRAGMA user_version` reports a higher or negative version, `LoanStore::Open` refuses the database, returns an explanatory error and closes the connection. It does not attempt to downgrade a database created by a newer build.

The automated regression test uses an isolated database at schema version 7 with a known loan row. After the application rejects it, the test opens the file read-only and confirms that both the version and the row are unchanged. This covers safe refusal; migrations for future schema versions will require a separately reviewed design and backup strategy.

### Workflow triggers

`Core tests` runs automatically on changes to build manifests, configuration, headers, C++ source, tests and workflow definitions on `development`. Changes confined to documentation or agent notes do not start CI.

`XP-targeted Win32 build (manual)` is configured with `workflow_dispatch` only. It performs the pinned w64devkit x86 build, XP-targeted CTest suite, PE32/XP subsystem/API checks and disposable ZIP packaging; it is intentionally not run for every commit.

The repository default branch is now `development` (changed by the user on 2026-11-16), and the manual XP workflow definition is present on that branch. Start the current XP build from **Actions → XP-targeted Win32 build (manual) → Run workflow**, selecting `development`. The workflow remains manual-only; do not add `push` or `pull_request` triggers. The `main` Python reference remains unchanged.


## Version 1.0 release checklist

Version 1.0 is not ready until every release gate below is verified. Use only disposable synthetic data.


### Build and compatibility
- [ ] Run Core CI on the release candidate: Linux, modern Win32 and Cppcheck all pass.
- [ ] Run the manual XP-targeted workflow on the exact release-candidate commit; confirm all 9 tests, PE32/i386, GUI subsystem 5.01, and import/API checks.
- [ ] Execute that exact artefact on Windows XP SP3/Pentium 4 and record SHA-256, OS details and test results.
- [ ] Verify all windows/dialogs, Portuguese text, icons, keyboard shortcuts, 800×600, sorting, history, statistics and CSV export.
- [ ] Measure cold-start time, peak memory, executable size and responsiveness on the target machine.

### Data safety and recovery
- [ ] In a disposable folder, create, edit and delete synthetic loans; verify audit entries and database integrity.
- [ ] Verify automatic backup before the first loan-data mutation, reuse during the same day and next-day behaviour.
- [ ] Test gzip and zlib manual backups and restore both formats.
- [ ] Test recovery from missing and deliberately corrupt databases; verify preservation of the damaged database and sidecars.
- [ ] Confirm the selected backup remains untouched and restored records match the source.
- [ ] Test settings persistence, invalid-settings recovery and Unicode paths.
- [ ] Verify CSV content/encoding with quotes, semicolons, multiline titles, future and expired records.

### Packaging and release
- [ ] Produce a clean portable ZIP with executable, default settings and required empty directories only; no database, backup, log or test artefacts.
- [ ] Verify launch from a path with spaces/accents and from a different working directory.
- [ ] Add version 1.0 metadata, user instructions, release notes and SHA-256 checksums.
- [ ] Re-extract into a fresh folder and repeat the launch/smoke test.
- [ ] Review branch, commit, release files and documentation; keep `main` unchanged.
