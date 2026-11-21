# Architecture

This document describes the target architecture and the C++/Win32/SQLite implementation now being built on development. The application remains a proof of concept until the remaining safety work is complete and the user's manual target-system test is documented sufficiently to reproduce its conditions. The user reports having tested it; exact build and scenarios remain unrecorded.

## Responsibilities and dependency direction

### 1. User interface

Responsibilities:
- Native Win32 windows, menus, dialogs, tables, keyboard shortcuts, and status information.
- Main operational view of active loans and availability.
- Separate full-history window, filters, and export commands.
- Settings, statistics, About, backup, and restore UI.
- Present progress and errors without implementing business rules.

The UI calls application services/use cases. It does not issue SQL directly.

### 2. Business rules

Responsibilities:
- Daily and rolling-window loan limits.
- Active-loan and expiry calculations.
- Next-availability calculations.
- Statistics and date/time rules.
- Validation of loan operations and settings.

This component must run and be tested without creating a GUI.

### 3. Persistence

Responsibilities:
- SQLite connections and transactions.
- Repositories/queries for loans and audit records.
- Schema versioning and migrations.
- Backup creation, integrity validation, and restore support.

The current database schema is the compatibility baseline: `loans(id, title, loan_date, type)`. Existing version-0 databases must retain the original declaration contract: `id INTEGER PRIMARY KEY AUTOINCREMENT`, followed by `title`, `loan_date`, and `type` as `TEXT NOT NULL`. The store rejects look-alike tables with an incompatible ID type, missing primary key, or extra unsupported columns rather than treating names alone as proof of compatibility.

### 4. Infrastructure

Responsibilities:
- Portable path resolution based on the executable location, not the process working directory. `BuildApplicationPaths` centralises the layout in a pure C++11 model; startup rejects a failed/truncated executable path and never substitutes `.` or the current working directory.
- Settings file loading, validation, and saving.
- Diagnostic log writing and archival.
- Single-instance coordination uses a named Win32 mutex derived from the executable directory. If Windows cannot create the mutex, startup fails closed rather than opening the database without the single-instance guarantee.
- File-system operations and application lifecycle support.

The application should not depend on AppData, registry installation, Windows services, or administrator privileges.

## Suggested dependency direction

- UI -> application services/use cases.
- Application services -> business rules and persistence interfaces.
- SQLite persistence implements the persistence interfaces.
- Infrastructure provides file, settings, logging, and lifecycle services.
- Business rules should not depend on Win32, SQLite implementation details, or GUI types.

Keep the design proportionate to a small desktop utility. Avoid unnecessary frameworks, elaborate plugin systems, and excessive abstraction.

## Portable folder layout

Suggested shape (final names may change with the chosen implementation):

```text
BiblioLED/
  BiblioLED.exe  (application icon embedded as a Win32 resource)
  config/
    settings.ini  (application settings and persistent table layout)
  data/
    biblioteca.sqlite
    backups/
  exports/
  logs/
```

All paths are resolved from the executable directory. The existing application icon is compiled into BiblioLED.exe as a Win32 resource; the portable distribution does not need a separate icon file. Keep port.ico in the source repository as the resource input. The loan table supports header drag-and-drop; its display order and per-column widths are persisted in config/settings.ini. Program updates must not overwrite user data, settings, backups, exports, or logs.

## Application path model

`include/biblioled/application_paths.h` and `src/core/application_paths.cpp` define a filesystem-independent C++11 path model. It receives the absolute executable filename and produces the application root, config/data/backup/export/log directories, settings file, database file, and initialisation marker in one value object. Drive-root and UNC paths are supported; forward slashes are normalised to Win32 backslashes, and Unicode is retained in wide strings.

The Win32 entry point obtains the executable filename with `GetModuleFileNameW`. An API failure, a truncated result at the Windows `MAX_PATH` boundary, or a non-absolute path stops startup before the mutex, settings, or database are opened. There is deliberately no fallback to `.` or the process working directory, which could otherwise cause a second database to be created or opened when the application is launched from a shortcut or another shell directory.

The path builder is independent of Win32 and is covered by isolated tests for ordinary paths, drive roots, UNC shares, Unicode names, slash normalisation, and invalid/relative inputs. Win32 filesystem calls continue to use wide paths; the SQLite store boundary receives UTF-8 paths as before.

## Database safety model

- SQLite is the authoritative store.
- Database schema version is stored in SQLite PRAGMA user_version.
- Version 0 is the existing loans-only schema; it remains readable but read-only until the user confirms migration.
- Version 1 adds audit_events and its indexes. New databases are created directly at this version. Its `audit_events` table is validated against the full 11-column contract (declared type, required/optional fields and INTEGER primary key), not just column names. The required fields are `occurred_at`, `action`, and `details` (`TEXT NOT NULL`); `id` is an INTEGER primary key; `loan_id` is nullable INTEGER; the before/after title/date/type fields are nullable TEXT. The exact 11-column count is enforced; a wrong action type, nullable details, or an extra column is rejected.
- The version 0 to 1 migration creates and validates a full backup first, then adds the audit table and indexes transactionally. It does not rewrite or discard loan records. The legacy `loans` table is accepted only when it has exactly four supported columns: `id INTEGER PRIMARY KEY`, `title TEXT NOT NULL`, `loan_date TEXT NOT NULL`, and `type TEXT NOT NULL`. Same-name look-alike schemas with a text ID, no integer primary key, or nullable required fields and additional unsupported columns are rejected without being modified.
- Do not silently create a fresh database when an expected database cannot be opened.
- A small initialisation marker in `config/database.initialized` distinguishes first launch from a database that has gone missing. If the marker exists but the database does not, never create an empty database; offer the guided recovery flow instead.
- If the marker exists but the database is missing, or if an existing database cannot be opened, startup offers explicit recovery from a user-selected backup. The candidate is decompressed and checked for full SQLite integrity and a supported BiblioLED schema, copied to a staging database and validated again before installation. Existing database files and any `-journal`, `-wal`, or `-shm` sidecars are renamed together and retained; the chosen backup is never consumed. An invalid candidate does not alter the destination. All SQLite-backed temporary basenames (snapshot, verification, decompression and recovery staging) are skipped if the main file or any `-journal`, `-wal`, or `-shm` sidecar already exists; cleanup removes the temporary file group created by the operation. A corrupt automatic backup and any sidecars are moved out of the automatic-retention namespace as one group; if a partial move fails, already moved files are rolled back. Recovery is never automatic.
- Test databases are always isolated from the user's real database.
- Use SQLite's Online Backup API to create a consistent snapshot in a temporary file.
- Create at most one automatic snapshot per local calendar day, immediately before the first actual loan-data mutation that day. Reuse a valid same-day snapshot. Audit-only events (including startup/shutdown), settings changes, and read-only launches do not trigger automatic snapshots.
- Automatic, manual, and safety backups have separate filename namespaces: `BiblioLED_auto_*`, `BiblioLED_manual_*`, and `BiblioLED_safety_*`.
- Retain automatic backups using the approved rolling schedule: the latest 7 daily snapshots, one per week for the following 4 weeks, then one per month for the following 12 months. Prune only recognised automatic backup files, and only after a new automatic snapshot has been created and validated. Unknown or malformed names are preserved.
- Build filesystem paths with the platform-native separator (`\\` on Windows, `/` on POSIX) even though Win32 file APIs accept both slash styles; do not emit mixed separators in user-visible backup paths.
- The store's Windows path inputs are UTF-8. Existence checks convert to UTF-16 and use `GetFileAttributesW`; the settings INI is read/written through wide-character Win32 file APIs so non-ASCII executable-directory paths work with the portable layout. At startup, each required portable directory is checked as an actual directory (not merely an existing path); a file/directory collision or creation failure stops startup before the database is opened.
- Manual backups remain until the user deletes them. Safety backups made before restore or migration are also preserved and are never removed by automatic retention. Historical backups created by older versions with generic `BiblioLED_YYYYMMDD_HHMMSS.sqlite` names are also left untouched: their original purpose cannot be reliably distinguished, and they remain restorable.
- Flush completed compressed and decompressed output with `fflush` and then the native OS durability primitive (`FlushFileBuffers` on Windows, `fsync` on POSIX) before validating or installing the final backup. A failed flush aborts the operation and removes the temporary output. The Windows final rename uses `MOVEFILE_WRITE_THROUGH`; on POSIX the containing directory is not separately `fsync`'d after `rename`, so the file data is flushed but survival of the final directory entry across sudden power loss is not guaranteed by this step.
- Compress new backups using statically linked zlib 1.4. The supported standard wrappers are gzip (`.sqlite.gz`) and zlib (`.sqlite.zlib`); the format and level (0–9, default 6) are selected in the backup settings and stored in `config/settings.ini`. Do not use raw DEFLATE. Changes made in the backup-settings dialog are applied to the open `LoanStore` immediately; they affect the next backup created without restarting. A valid automatic snapshot already created for the current day remains reused.
- Stream compression/decompression with a fixed-size buffer; never load the complete database into memory.
- Backup creation sequence: SQLite snapshot -> SQLite integrity check -> compression -> decompression round-trip -> second SQLite integrity check -> move to final filename. A failed step must not publish a final backup or allow the pending loan mutation.
- Restore accepts the new compressed formats and legacy uncompressed `.sqlite` files. Decompress to a temporary file when necessary, validate SQLite integrity and schema compatibility, create and validate a safety backup of the current database, and only then perform the restore. Verify the restored database and attempt rollback from the safety backup on failure.
- A backup is not considered valid merely because a file exists; validate the complete stream and SQLite integrity/schema.

The general settings dialog exposes the daily and monthly limits, rolling-window length, loan duration, diagnostic logging toggle, and external-log rotation threshold. These values are saved to `config/settings.ini` and intentionally remain pending until the next application launch (D-016). The dedicated backup dialog remains separate because compression changes apply immediately (D-057). The `AppSettings` model and INI parsing/persistence are implemented in the portable `settings_store` service rather than the Win32 entry point.

## Audit logging model

Internal audit events are now stored in audit_events within the same SQLite transaction as the associated loan create/edit/delete. Events retain the action, timestamp, loan ID, before/after values, and details. If the event insert fails, the loan operation rolls back as well. The schema migration itself records a schema_migration event. A read API is available to retrieve these events.

The external append-only UTF-8 text log is implemented at `logs/audit.log`, with timestamped `audit_*.log` archives. The scanner reads each file with a fixed 64 KiB buffer and bounded per-line parser state rather than copying complete archives into memory. Its fingerprint de-duplication `std::set` does retain one key per distinct historical event, so total synchronisation memory still grows with the number of retained event fingerprints; the improvement bounds file buffering, not total memory independently of history length. Each line includes an event fingerprint derived from its durable content and identity. Synchronisation scans the active log and archives to avoid duplicates, repairs an incomplete final line in the active log after an interrupted write, and appends any missing SQLite events. The active file is rotated before an append would exceed the configured maximum (default 5 MB); archives are never deleted automatically. Internal events currently cover loan create/edit/delete, schema migration, application startup/shutdown, successful manual backup/restore operations, and general/backup settings changes. A generic `RecordAuditEvent` store operation writes operational events transactionally without invoking the loan-data backup gate. Consequently startup/shutdown, settings-change, and other audit-only events do not create automatic database snapshots. They continue to obey the external-log synchronisation rules.

When file logging is deliberately disabled, internal SQLite audit events continue to be recorded and external synchronisation pauses; the backlog is replayed when logging is re-enabled. When logging is enabled but synchronisation fails, the application remains readable but blocks further database mutations until the log is healthy again. The Win32 UI exposes **Ficheiro > Tentar sincronizar registo de auditoria...** as the explicit recovery command. F5 is no longer used for main-window refresh or audit retry.

CSV export is generated by portable `csv_export.*` from all records returned by `LoanStore::GetAll`, using the legacy `Title;LoanDate;Type` columns, UTF-8 BOM, semicolon delimiter and correct quoting. The Win32 save dialog writes a temporary file in the destination folder and atomically replaces the destination only after a complete flush. Successful export is audited as `records_exported`; it is not a database backup.

## Business rules

Date parsing, local calendar-day arithmetic, loan expiry/visibility, quota-window boundaries and remaining-quota calculations live in the portable `biblioled_core` library (`loan_rules.*`). The Win32 interface calls these functions, and `loan_rules_tests` exercises them without starting the GUI or opening SQLite. Future-dated loans remain visible and reserve quota; expired loans stay in history.

Statistics calculations live in `loan_statistics.*`, operating on a supplied record vector without Win32 or database access. Annual evolution, daily calendar activity, weekday/hour distributions and full-history mean/median intervals are covered by `loan_statistics_tests`.

## Main UI and secondary windows

- Main window: active loans and current operational information; do not make the full history the main view.
- History: separate native Win32 dialog; all records including expired and future-dated loans; immediate case-insensitive filtering by title, type, date, and ID, plus a type selector. Search/display strings are cached at load time to avoid repeated conversions and database queries while typing. Edit/delete operations are routed through the existing LoanStore methods and transactional audit path; the history list is refreshed after a successful mutation.
- Statistics: read-only native Win32 dialog with two areas. General/evolution shows all-history totals and type distribution, first/last valid dates, annual totals and year-over-year counts/differences. Usage patterns shows selected-year daily activity and weekday/hour distributions. Mean/median gaps between distinct active dates use the full history (legacy metric), not the selected year. Invalid dates are counted and excluded from date-based metrics; no reading-progress or streak tracking.
- About: native Win32 dialog displaying the embedded application icon, application name, version, short description, compile date/time, and XP SP3 x86 target.
- Appearance: compact native Windows utility, prioritising legibility and functional density over decoration.
- Main table columns are reorderable by dragging headers. Store display order and widths per logical column in the existing settings.ini and restore them at startup. Do not rewrite an invalid settings file merely to save UI preferences.


## Confirmed UI interaction and refresh policy

These UI refinements are implemented:

- General and backup settings are under **Opções**; **Ficheiro** contains file/backup operations and the explicit audit-log synchronisation recovery command.
- Main-list refresh is event-driven: startup and successful create/edit/delete/restore only. There is no periodic polling, manual refresh command/button, or "Actualizado às ..." timestamp.
- History and statistics load a snapshot when opened. Statistics year changes recalculate from cached records; the redundant statistics refresh button and F5 reload path have been removed.
- History is resizable, enforces a minimum track size, supports header drag-and-drop and provides right-click Edit/Delete actions. Double-click-to-edit remains.
- The main and history lists support header-click sorting. Text columns sort alphabetically, IDs/days numerically, and date columns chronologically; repeated clicks toggle ascending/descending.
- Ctrl+N and Ctrl+H use a Win32 accelerator table translated by `TranslateAcceleratorW`. Enter invokes the list-view activation action, and Delete is handled as a list-view key notification.
- The resource script declares code page 65001, and MSVC C++ source compilation explicitly uses UTF-8. Statistics error strings now convert from UTF-8 correctly.
- F5 is not used for main-window refresh or audit retry. Audit-log recovery remains available from the explicit Ficheiro command.
- Clicking a header in the main loans list or history list sorts by that column; a repeated click reverses direction. Titles and types use case-insensitive locale-aware alphabetical comparison, IDs and remaining days use numeric comparison, and loan/expiry dates use chronological comparison.
- The user confirmed on 2026-11-16 that the executable runs well on Windows XP. The 2026-11-15 confirmation also covered the correction of secondary-window Portuguese text encoding. The exact executable hash, XP build and exercised test scenarios remain undocumented; recovery and failure-path validation still need a reproducible record.

## Initial proof-of-concept implementation

The first C++ implementation slice is being introduced under:

```text
CMakeLists.txt
config/settings.ini
include/biblioled/loan_store.h
src/core/loan_store.cpp
src/win32/main.cpp
src/win32/resource.h
src/win32/dialogs.rc
tests/loan_store_tests.cpp
.github/workflows/core-tests.yml
```

The initial UI supports listing, creating, editing, and deleting loan records. The main window shows active/current and future-dated records, while the separate history dialog lists the complete loan table including expired records. The history dialog caches rows for immediate text/type filtering and delegates mutations to the main application window so the existing persistence and audit paths remain authoritative. The application reads the existing `loans(id, title, loan_date, type)` table shape and creates that exact table only for a new database. It uses the portable folder beside the executable.

Version 1 audit-schema validation now also requires the full `PRAGMA table_info` scan and statement finalisation to complete successfully; a partial metadata read is rejected instead of being treated as a valid schema. CRUD/audit INSERT/UPDATE/DELETE statements now check every `sqlite3_bind_*`, `sqlite3_step` and successful-path `sqlite3_finalize` result. Loan lookup/list/audit-list/quota-count reads also check parameter binding, consume their expected terminal result and require statement finalisation before returning success. Transaction rollback is centralized: the helper checks `ROLLBACK`, recognises an automatic rollback via autocommit state, and closes the connection if a failed rollback leaves a transaction active. This prevents future operations from reusing an uncertain connection. The same fail-closed rule is now applied to loan-column inspection, `sqlite_master` audit-table discovery, `PRAGMA user_version`, and the complete `PRAGMA quick_check` result set. The external log scanner reads fixed-size chunks, commits event keys only from complete lines, and truncates only an incomplete tail in the active log; archived files remain immutable.

A focused migration failure-injection test also creates a conflicting audit-index name after the audit table has been created; it verifies the DDL transaction rolls back, the legacy record and version 0 remain, and the database reopens as a valid legacy database. Separate tests now force the explicit `ROLLBACK` statement to fail and terminate a child process during an open transaction; physical power-loss and later-schema-migration tests remain outstanding.

The current implementation is deliberately a proof of concept, not the complete application. The store makes and validates one automatic compressed snapshot before the first loan-data mutation per local calendar day, supports separately retained compressed manual and safety backups, and restores compressed or legacy uncompressed backups only after creating a second safety copy of the current database. A marker prevents silent recreation of a missing initialized database; guided startup recovery validates a selected backup and preserves any unavailable/corrupt database and its SQLite sidecars before installing a staged copy. Schema version 1 and an explicit, backed-up version 0 to 1 migration are implemented. Loan changes and internal audit events commit atomically. External audit events are synchronised to a recoverable rotating UTF-8 log with a durable backlog. Internal auditing covers loan create/edit/delete, schema migration, application startup/shutdown, successful manual backup/restore, settings changes, CSV exports, and application warnings/errors/failures. The application icon is embedded in the executable, and main-table order/width preferences persist in settings.ini. Physical power-loss testing, later migrations, remaining UI workflows and other final-application features remain outstanding.

SQLite 3.53.4 is fetched as an amalgamation and statically linked for Windows builds. It is pinned because SQLite 3.54.0 introduced a Windows SRW-lock dependency that is not available on Windows XP. The pin must be revisited only after target-OS testing.


## Latest CI evidence

Current development head `95fb0be479a06f455e019065cb1295af3ce9a691` passed the Core workflow, but the XP-specific workflow has not yet been rerun on this head.

- [Core tests run 43921021534](https://github.com/quilterrantdean/BiblioLED/actions/runs/43921021534): Ubuntu core/store tests (8/8), modern Windows SDK Win32 application/store tests (9/9), and Cppcheck 2.19.0 all succeeded. This includes the modern-Windows compile/test check for the recent `settings_store` fix.
- The latest successful [XP-targeted build run 43902567911](https://github.com/quilterrantdean/BiblioLED/actions/runs/43902567911) is at the older source head `c4a749d715d0cd39ecfe6dad4afb5aa0e48fb0c5`: pinned w64devkit x86 2.10.0, all 9 CTest tests, PE32/i386, Windows GUI subsystem 5.01 and post-XP API/import checks passed. It predates the latest invalid-settings recovery changes and must not be treated as validation of the current head.

Latest CI artefact: [14241453581](https://github.com/quilterrantdean/BiblioLED/actions/runs/43902567911/artifacts/14241453581), `BiblioLED-XP-PoC`, 1,568,598 bytes. ZIP SHA-256: `94932edf2c39095ba2ddb5c1b88cc01df224c69be3726c018fec31c76b7b7e20`.

The ZIP contains only the executable, example INI, XP test guide and PE inspection report; it has no SQLite/database file. This remains a disposable validation artefact, not a normal portable release. The user confirmed that the executable runs well on XP; CI compilation and isolated tests do not prove the full manual recovery/failure-path matrix.

## CI action provenance

The core and XP workflows pin JavaScript actions to immutable upstream release commit SHAs rather than mutable major-version tags. `actions/checkout` v7.0.1 is pinned to `3d3c42e5aac5ba805825da76410c181273ba90b1`; `actions/upload-artifact` v7.0.2 is pinned to `cf430e030ddbb5b0abf93d22962f4752f3646cd9`. Both release manifests declare the Node.js 24 runtime. The downloaded SQLite, zlib and w64devkit source/toolchain assets are version-pinned and SHA-256/SHA3-256 checked; runner-managed CMake and OS packages are explicitly image-managed for the Linux/modern-Windows jobs.

## Third-party build dependency provenance

- SQLite amalgamation 3.53.4 is fetched from the official sqlite.org archive and pinned with SHA3-256 `628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e`.
- zlib 1.4 is fetched from the upstream `madler/zlib` v1.4 GitHub Release asset and pinned with SHA-256 `02dd159783677a749c04adbdd02b55b8a2bcc7e054f51b76b549199a916ad584`. The upstream Releases API reports the same asset digest; this replaces the zlib.net fossil endpoint after repeated inconsistent responses from that endpoint in CI.
- The XP workflow downloads the immutable w64devkit x86 2.10.0 release archive and verifies SHA-256 `513ff7fc571cf6764a5aedbca92f43c513f8d01085c65df5133a82db13a7e63d` before extraction. The upstream release identifies the x86 package as Windows XP-compatible and SSE2-required.
- Cppcheck is installed from the Ubuntu runner's package repository, but the workflow requires the reported version to be exactly 2.19.0 so a runner-image/package change cannot silently alter the analysis baseline.
- The portable Linux and modern-Windows CI jobs intentionally use their hosted runner's CMake and OS-provided development packages; the XP-targeted build uses the CMake executable included in the SHA-256-pinned w64devkit package. The CMake preset declares a minimum CMake version of 3.28.0.

These hashes are integrity pins, not a substitute for reviewing release notes and compatibility whenever a dependency is deliberately upgraded.

The modern Windows CI run 43763428212 failed during CMake configuration because six consecutive downloads from `zlib.net/fossils/zlib-1.4.tar.gz` returned six differing SHA-256 values, none matching the official release digest. The build source was switched to the upstream GitHub Release asset while retaining the official expected SHA-256. The failure was external to compilation and did not affect the XP-targeted build, which succeeded.

## Diagnostic audit coverage

The Win32 shell records user-visible validation warnings and operation failures in the internal SQLite audit table. The history and statistics windows also queue read and calculation failures. The store maps the restricted diagnostic severities `warning`, `error`, and `failure` to the audit actions `application_warning`, `application_error`, and `application_failure`, with a context identifier and diagnostic details.

Diagnostic writes are audit-only and do not trigger automatic backups. They may bypass only the external-log health gate, so an event explaining an external logging failure can still be queued in SQLite. This does not modify the external-log health flag, restore file logging, or enable loan-data mutations. Normal audit events and loan mutations remain blocked while enabled external logging is unhealthy. The diagnostic writer does not attempt immediate external synchronisation, avoiding recursive error reporting; queued rows are included by the normal synchronisation lifecycle.

Diagnostics cannot be persisted before a usable versioned database is open, while a legacy database awaits migration, or after a database failure that prevents writes. Startup settings-file warnings are queued only after the database has opened successfully.

## Explicit ROLLBACK failure test

The isolated `loan_store_tests` suite installs a connection-local SQLite authorizer that rejects only the `SQLITE_TRANSACTION` action for `ROLLBACK`. The test opens an isolated database, starts an explicit transaction, inserts an uncommitted loan, and invokes the same private rollback method used by the store's failure handling. It verifies that the rollback failure is reported and the `LoanStore` connection is closed.

The test then reopens the database through a new `LoanStore` instance, confirms that the uncommitted row is absent, and runs `quick_check`. This verifies the close-and-reopen recovery path after an explicit rollback command is refused. It is not a power-loss or forced-process-termination test; that coverage remains outstanding.

## Abrupt process termination recovery test

The isolated `loan_store_tests` suite creates a committed baseline record and starts a separate crash-worker process with an SQLite transaction open. The worker uses rollback-journal mode (`DELETE`), `synchronous=FULL`, a deliberately small page cache and cache spilling, inserts 128 synthetic uncommitted rows, and calls `sqlite3_db_cacheflush` while the transaction remains active.

On Windows the parent starts the same test executable with `CreateProcessW` and the worker terminates itself with `TerminateProcess`, without calling `sqlite3_close` or committing. On POSIX the test forks a child and the child uses `_exit` with the SQLite connection still open. The parent requires the hot `-journal` file to exist, then reopens the database through `LoanStore`. Recovery must preserve the committed baseline record, discard all uncommitted child rows, remove the hot journal, and pass `quick_check`.

This is a deterministic abrupt-process-death test on both platform families. It does not simulate a physical power cut, kernel crash, or loss of volatile write caches in a storage controller; those remain distinct durability scenarios.

## INI settings persistence

The settings model and file handling live in `include/biblioled/settings_store.h` and `src/infrastructure/settings_store.cpp`. The portable `AppSettings` contract retains the existing defaults and fields: loan limits/duration, diagnostics and log size, backup compression, and column order/width. The Win32 entry point passes the executable-relative path as UTF-8; the service converts it to UTF-16 for Windows file operations, including Windows XP.

The reader limits the file to 64 KiB, accepts an optional UTF-8 BOM, validates recognised values and rejects duplicate recognised keys. A missing file is created atomically with defaults. An invalid, oversized, unreadable or non-regular file is not rewritten; defaults are supplied in memory and the error is reported. Unknown keys remain ignored to preserve the existing INI compatibility behaviour.

Before saving, the service validates the in-memory values and rereads an existing target. It refuses to replace a target that is invalid or unreadable, even if a caller bypasses the Win32 UI guard. A missing target is installed with no-replace semantics to avoid overwriting a file created concurrently. Existing valid settings are written to an exclusive, uniquely named temporary file; the full file is written and flushed before atomic installation. Windows uses `FlushFileBuffers` and `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`; POSIX uses `fsync` on the file and `rename`. POSIX parent-directory metadata is not separately fsynced, so the implementation does not claim full filename durability across sudden power loss.

The dedicated `settings_store_tests` executable covers defaults creation, full save/load round-trip, successful replacement of an existing file, invalid-file byte preservation (including refusing a valid update over it), duplicate keys/BOM, invalid in-memory values, stale temporary-name collision, replacement failure, directory-as-file and oversized-file handling. These tests use isolated temporary directories with Unicode names; they never open the user's real settings.

## Future SQLite schema-version rejection

`LoanStore::Open` rejects database versions outside the currently supported range (0–1), after checking the database's SQLite integrity and the supported legacy loan-table contract. On rejection it reports that the database has not been modified and closes the connection; it does not attempt to downgrade or migrate a future schema.

The isolated regression test creates a synthetic schema-version-7 database containing a legacy-shaped `loans` table and one known record. It verifies that `Open` fails and leaves `LoanStore` closed, checks the explanatory error, then opens the file independently in SQLite read-only mode. The test confirms that `PRAGMA user_version` remains 7 and that the original row's title, date and type are unchanged. This is forward-version refusal coverage, not an implementation of a future schema migration; later migrations remain deliberately undefined.

## GitHub Actions execution policy

`Core tests` runs automatically for pushes and pull requests to `development` only when build manifests, configuration, headers, source, tests or workflow definitions change. Its filters exclude documentation-only changes. The separate `XP-targeted Win32 build (manual)` has only the `workflow_dispatch` trigger; its relatively expensive pinned w64devkit build, XP tests, PE/import inspection and artifact packaging run only on explicit manual dispatch.

The repository default branch was changed to `development` by the user on 2026-11-16. Both workflow files are present on that branch, so the manual **Actions → XP-targeted Win32 build (manual) → Run workflow** control is now available. Select `development` when dispatching. Keep `main` unchanged as the Python reference and do not add development workflow files or commits there.
