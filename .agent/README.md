# Agent workspace

This directory contains project context intended to help coding agents and future contributors continue work without relying on the original conversation.

The repository-root `AGENTS.md` is the agent instruction entry point. It directs agents to these notes.

## Files

- `plan.md` — project phases, current status, and next work.
- `decisions.md` — user-approved product, technical, and workflow decisions.
- `architecture.md` — proposed component boundaries and portable directory layout.
- `codebase-review.md` — factual review of the existing implementation and known issues.
- `handoff.md` — current-session state, verified CI status, and next steps for a new session.

## Current handoff

Before resuming work in a new session, read `.agent/handoff.md` after the project plan and decisions. It is a dated snapshot; verify the live branch head and workflow runs before making changes.

## Working rules

- Treat user-approved decisions as requirements unless the user changes them.
- Distinguish confirmed decisions from provisional recommendations and unresolved questions.
- Update these notes when implementation changes the architecture or resolves an open question.
- Keep notes concise, factual, and useful to another agent.
- Do not store credentials, tokens, private user data, database contents, or other secrets here.
- All development changes belong on the `development` branch. Never commit directly to `main` or `master`.

These files are project documentation, not application runtime data.


## Local command environment and GitHub authentication

The user operates Git for Windows through the Mintty terminal using the `MINGW32` Git Bash environment. The observed Git version is `2.10.0.windows.1`.

The user's GitHub credentials are already configured at account level for Git operations. **Do not routinely ask the user to create, paste, or export a new personal access token or `GH_TOKEN`.** For local scripts that need GitHub HTTPS authentication, first reuse the configured Git credential helper (the successful workflow-history cleanup used the helper route through `git credential fill`). Read credentials only in process memory; never print them, commit them, or write them to project documentation or script files. Ask for a new token only if the configured helper cannot provide a usable credential or lacks the required permission.

The user's installed Python 3.10.18 interpreter is `C:\\Python31018\\python.exe`. In the observed Git Bash environment, the unqualified `python` command resolves to a different installation under `C:\\Program Files\\Python\\python`; use the explicit interpreter path for scripts that require the known Python 3.10.18 environment.

The workflow-history cleanup was run locally from Mintty/Git Bash, not from PowerShell. It reduced the Actions history from 225 runs to the two required successful validation runs recorded in `plan.md`. The cleanup was verified afterwards through the GitHub Actions API. Future cleanup scripts must preview their targets, preserve the latest successful Core tests and XP-targeted build, skip in-progress runs, and require an explicit confirmation before deletion.
