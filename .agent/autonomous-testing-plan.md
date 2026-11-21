# Autonomous test plan for Windows XP

## Goal and agreed constraints

Build a repeatable, low-risk test run for the **latest BiblioLED build** on the local Windows XP machine, which remains the compatibility focus through version 1.0.0.

Confirmed preferences:

- The XP machine is locally accessible and can stay logged in with the desktop unlocked during testing.
- Target display resolution is 800 x 600.
- Run full headless/integration test coverage where practical. GUI automation is not currently selected; UI coverage remains a short manual checklist.
- Python 3.4.4 is the version found on `PATH`. The user identifies an XP-compatible Python 3.10 at `C:\\Python31018\\python.exe`, not on `PATH`, and prefers standard-library-only tooling. If selected, invoke that interpreter explicitly in the runner (or set a process-local `PATH`); avoid changing the machine-wide `PATH`. Smoke-test its version, architecture and standard-library imports before relying on it.
- Use only isolated synthetic databases and disposable folders. Remove temporary data after a fully successful run; retain the run folder, logs and failure artifacts if any test fails.
- Include stress and deliberately induced failures where safe. Do not use the real application installation or data, and do not mistake forced process termination for physical power-loss testing.

## Proposed architecture

### 1. CI-produced XP test bundle

Extend the existing manual XP workflow to produce a separate **test bundle**, distinct from the user-facing 1.0.0 candidate ZIP. Include:

- the exact `BiblioLED.exe` built and inspected by that workflow;
- all nine existing XP-targeted C++ test executables;
- a standard-library-only Python 3 runner; optionally a `.bat` launcher for convenience;
- a manifest/build-info file with source commit, workflow run, target and SHA-256 values;
- concise instructions to run from a fresh disposable directory at 800 x 600.

Do not place a database, settings from the real installation, personal data, or generated test state in the bundle. Keep test output in a unique run-specific directory under `%TEMP%` or a caller-selected scratch directory.

### 2. XP headless test runner

The runner should:

1. Verify required files and record OS version, architecture, display dimensions, current executable hash and start time.
2. Create a unique isolated scratch directory and point test processes at it where their interfaces allow.
3. Execute each existing test binary separately, capture stdout/stderr and exit status, and continue to collect results after an individual test failure.
4. Run repeatable stress loops on selected deterministic tests; cap duration/iterations and report the seed/count.
5. Summarize pass/fail/skip and return non-zero if any test failed.
6. Delete the scratch directory only if every test passed and cleanup is explicitly safe. Preserve logs, scratch data and crash artifacts on failure.

First audit current test binaries' temporary-directory behavior and exit codes. Initial inspection shows the Windows tests create temporary directories internally (mostly through `GetTempPath`) and perform their own cleanup; a few test suites create multiple directories and some use ANSI file APIs. The runner should set a unique `TEMP`/`TMP` before launching a test, but verify XP's `GetTempPath` behavior and cleanup on both pass and failure. Preserve runner logs on failure. If retaining a failing test's generated database is needed, add an explicit opt-in keep-temp switch to that test rather than assuming the runner can preserve files the test deletes itself. Do not assume all tests honor a shared test-data environment variable until verified.

### 3. Test layers

**Existing automated tests (first milestone):** all nine tests already in the XP workflow: application paths, settings store, CSV export, loan rules, statistics, SQLite loan store, compression, retention, and Windows audit log. Re-run the packaged test binaries on the actual XP host, not only on the GitHub Windows runner.

**Additional deterministic fault tests (second milestone):** use synthetic temporary data to test invalid/missing settings, missing/corrupt database handling, SQLite sidecar preservation, write/rename failures, backup corruption, restore rollback, and audit-log recovery. Prefer unit-level fault injection over manipulating the actual filesystem or application data. Each test must prove both the reported failure and the invariant that original synthetic data remains recoverable.

**Stress tests (third milestone):** bounded repeated CRUD/backup/restore cycles, large-but-controlled audit logs/history, interrupted test-process recovery, repeated app start/close, and long-path/Unicode path cases within the XP path limits. Keep stress profiles short enough for Pentium 4 hardware and record elapsed time/peak process memory where a reliable XP-compatible measurement is available.

**GUI smoke (manual):** at 800 x 600, check launch/exit, all main dialogs, Portuguese strings, keyboard shortcuts, sorting/history/statistics, CSV interaction, and backup/restore prompts using synthetic records. Capture the checklist result; avoid brittle coordinate-driven automation for this milestone.

### 4. Safety and artifact handling

- Never point the runner at the existing user data directory. Refuse a run if the chosen scratch path resolves to the installation's `data`, `config`, `exports`, or `logs` directories.
- Create an isolated application copy under a fresh path; use only generated synthetic records.
- Use run identifiers in logs and keep failed-run artifacts for diagnosis. Do not upload private machine details or test data without review.
- Keep the 1.0.0 release-candidate ZIP clean and separate from test executables and logs.
- Treat process-kill tests and physical power-cut testing as separate categories. Do not simulate a power cut on the user's machine.

## Milestones and acceptance

1. **Environment discovery:** record exact XP edition/service pack, CPU/RAM, display mode, available Python version, and whether the machine can run a `.bat` bundle. No system changes yet.
2. **Bundle prototype:** package the current XP test binaries with a batch runner, manifest, hashes and safe scratch handling. Inspect bundle contents before use.
3. **Actual-XP headless baseline:** run all nine tests on the XP host in isolation, retain complete logs, and verify cleanup-on-success / preservation-on-failure.
4. **Coverage gaps:** review each module against recovery/failure scenarios and add only targeted tests for uncovered invariants; verify on CI and actual XP.
5. **Bounded stress and manual GUI checklist:** run repeatable stress profiles and the 800 x 600 GUI checklist on the latest build.
6. **Release gate:** require Core CI, XP CI, actual-XP headless bundle on the latest build, no unexplained failures, and review of the GUI checklist before declaring 1.0.0 ready. Do not publish a GitHub Release without explicit approval.

## Remaining environment check

Use `C:\\Python31018\\python.exe` explicitly as the preferred XP-side interpreter; Python 3.4.4 remains the version on `PATH` and a fallback if the 3.10 smoke check fails. A batch launcher can call the explicit executable without changing the machine-wide `PATH`. Before relying on it, run a tiny smoke check for version, architecture, subprocess execution, filesystem operations, JSON and hashing on XP. AutoIt/AutoHotkey and GUI automation libraries are not selected; GUI smoke remains manual for now.
