# Session handoff — 2026-11-16

## Repository and branch

- Repository: `quilterrantdean/BiblioLED`
- Development branch: `development`
- Repository default branch: `development` (the user changed this in GitHub Settings on 2026-11-16; verified through repository metadata).
- Current branch head at handoff creation: `95fb0be479a06f455e019065cb1295af3ce9a691` — `ci: filter automatic tests and make XP build manual`.
- `main` remains the unchanged Python reference implementation. Do not commit to, force-update, or otherwise modify `main` or `master`.
- Before every write, verify that the target is `development`; use the current head as an expected SHA/lease when updating the branch.

## CI configuration and verified status

### Core tests

`.github/workflows/core-tests.yml`:
- Automatic on `push` and `pull_request` targeting `development`, but only when build manifests, configuration, headers, source, tests or workflow definitions change.
- Documentation-only changes in `docs/**`, `.agent/**`, `README.md` and `AGENTS.md` do not start this workflow.
- Also supports optional `workflow_dispatch`.

Current-head run: [43921021534](https://github.com/quilterrantdean/BiblioLED/actions/runs/43921021534) — **success** on `95fb0be`:
- Ubuntu core tests: 8/8.
- Modern Windows Win32 compile/store tests: 9/9.
- Cppcheck 2.19.0: success.

### XP-targeted build

`.github/workflows/xp-toolchain.yml`:
- Workflow name: `XP-targeted Win32 build (manual)`.
- Trigger: `workflow_dispatch` only. Do not restore automatic `push` or `pull_request` triggers.
- Uses pinned w64devkit x86 2.10.0 (archive SHA-256: `513ff7fc571cf6764a5aedbca92f43c513f8d01085c65df5133a82db13a7e63d`).
- Builds/tests with CMake preset `xp-x86-w64devkit`, checks PE32/i386, Windows GUI subsystem 5.01 and selected post-XP imports, then publishes a disposable test ZIP.

**Important repository change:** because the user set `development` as the default branch and the workflow file is present there, the GitHub website can now expose **Actions → XP-targeted Win32 build (manual) → Run workflow**. Choose `development` in the branch selector. This resolves the previous default-branch obstacle; no workflow file needed to be copied to `main`.

The last successful XP run is [43902567911](https://github.com/quilterrantdean/BiblioLED/actions/runs/43902567911), but it built older source head `c4a749d715d0cd39ecfe6dad4afb5aa0e48fb0c5`. All 9 XP tests, PE checks and post-XP import checks passed there. Its artefact [14241453581](https://github.com/quilterrantdean/BiblioLED/actions/runs/43902567911/artifacts/14241453581) is 1,568,598 bytes with ZIP SHA-256 `94932edf2c39095ba2ddb5c1b88cc01df224c69be3726c018fec31c76b7b7e20`.

**Immediate validation still required:** run the manual XP workflow against the current `development` head. It has not yet been validated by the XP toolchain after the recent invalid-`settings.ini` recovery implementation and the Win32 settings-reader correction. The modern Core run passing is not a substitute for this XP build.

In the GitHub connector available during this handoff, actions exist to inspect runs/jobs/logs and rerun failed jobs, but no action exists to create a new initial `workflow_dispatch` run. The user must start this first XP run in the website; a subsequent session can inspect its result. Re-check available tools in the new session rather than assuming this limitation is permanent.

## Current implementation state

The project is a C++11 / native Win32 / SQLite proof of concept targeting Windows XP SP3 32-bit. The Python implementation on `main` remains the functional reference and has not been replaced or modified.

Recent work includes:
- Extracted portable INI persistence into `settings_store.*`.
- Validation of known settings, duplicate keys, oversized input and malformed INI files.
- Atomic settings writes through a unique temporary file and flushed replacement.
- Guided recovery for an invalid settings file: preserve the invalid original under an `.invalid` name and recreate defaults only after explicit user consent. Existing `.invalid` collisions are not overwritten; failures preserve the original.
- Corrected the Win32 `ReadSettingsBytes` path by initialising `bool ok = true;` in commit `95fb0be`.
- Updated CI trigger policy and the documentation describing it.

The user confirmed that the application runs well on Windows XP and that the secondary-window Portuguese encoding issue is fixed. The exact executable hash, XP build and manual test scenarios have not been captured. Treat this as a user-confirmed smoke check, not a reproducible full validation record.

No real user database, settings file or personal records have been opened, copied, migrated or modified. Continue to use isolated temporary databases and disposable application folders only.

## Next steps

1. **Run the current XP workflow.** The user starts it at [Actions / XP-targeted Win32 build](https://github.com/quilterrantdean/BiblioLED/actions/workflows/xp-toolchain.yml), choosing branch `development`. Then inspect every job and test result, PE inspection and uploaded artefact.
2. If it fails, diagnose the actual failing step/log and fix the issue on `development`; do not infer that a modern Win32 pass guarantees XP compatibility.
3. Once the current XP artefact passes, test it on the Windows XP SP3 machine in a new disposable folder. Exercise the invalid-settings recovery path and the important backup/restore and UI flows without using the real application data folder. Record the artefact/hash and exact scenarios.
4. Update `.agent/plan.md`, `.agent/codebase-review.md` and `docs/building.md` with the resulting XP run and target-machine record.
5. Resume the remaining plan work: complete the XP validation matrix; measure startup time, executable size, memory use and responsiveness; decide whether the w64devkit baseline is final; review remaining UI/audit workflows; and defer later SQLite schema migrations until requirements justify them. Physical power-loss testing remains distinct from the existing abrupt-process-termination and rollback-failure tests.
6. Do not prepare a normal portable release until the remaining safety and target-system validation work is complete.

## Key project rules

- Read `AGENTS.md`, then `.agent/README.md`, `.agent/plan.md`, `.agent/decisions.md`, `.agent/architecture.md` and `.agent/codebase-review.md` before substantive changes.
- Work only on `development`; never write directly to `main` or `master`.
- Keep repository text/commit messages in English; the application UI remains European Portuguese with pre-1990 spelling.
- No real user data in tests. Do not add automatic CI triggers for documentation-only commits or automatic XP builds.