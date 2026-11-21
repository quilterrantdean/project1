# Agent instructions

This repository contains BiblioLED, a small personal application for recording and managing the user's own BiblioLED loans. It is not library-management software for a public or institutional library.

## Read before working

Read these project notes before making changes:

1. `.agent/README.md`
2. `.agent/plan.md`
3. `.agent/decisions.md`
4. `.agent/architecture.md`
5. `.agent/codebase-review.md`

Update the relevant notes when a material decision, implementation detail, or project status changes.

## Branch and repository safety

- The only branch for assistant development work is `development`.
- Never commit directly to `main` or `master`.
- Never move, force-update, or otherwise modify the `main` or `master` refs.
- Do not manually merge a pull request into `main` or `master` unless the user explicitly requests it in that specific task.
- Pull Requests and GitHub Auto-merge are permitted after the configured checks pass. Do not bypass required checks.
- Before every write operation, confirm that the target branch is `development`.
- Do not modify application code, user data, configuration, or database files unless the user has requested that work.

## Data safety

- Never use the user's real database as a test database.
- Tests must use isolated temporary databases, settings, and synthetic records.
- Database migrations, restore operations, and destructive changes must preserve recoverability and follow the decisions in `.agent/decisions.md`.
- Never silently discard, rewrite, or repair user data.

## Product constraints

- Primary target: Windows XP SP3, 32-bit (Win32).
- Performance and resource efficiency on older Windows hardware are primary design constraints.
- SQLite is a required data store and must be considered when evaluating languages and toolchains.
- A native Win32 interface is preferred.
- The application must be portable, run without administrator privileges, and avoid services, registry installation, and mandatory background components.
- Keep dependencies to a minimum and justify each non-trivial dependency.
- User-facing language is European Portuguese, using pre-1990 spelling conventions.
- The interface should remain a compact, clear Windows utility. Avoid decorative dashboards, oversized cards, animation, and unnecessary visual effects.

## Engineering approach

- Treat the current Python implementation as a functional reference, not as a mandatory implementation language.
- The current preferred direction is C++ / Win32 / SQLite, but it is provisional until a proof of concept has been built and tested on Windows XP.
- Keep the interface, business rules, persistence, and infrastructure concerns separated.
- The business rules must be testable without starting the graphical interface.
- Prefer small, reviewable, documented changes over broad rewrites.
- Do not assume an unresolved behaviour. Record it as an open question or make a conservative implementation choice and document it.
- Keep build tools and dependencies updateable. Record the versions used for each build so older builds can be reproduced.

## Validation

- Add and run relevant automated tests for changes.
- Run static analysis and dependency checks where the chosen toolchain supports them.
- Preserve the ability to run appropriate tests on Windows XP as well as on newer development systems.
- Report tests that could not be run; never imply that an unexecuted test passed.
