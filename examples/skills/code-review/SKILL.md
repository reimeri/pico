---
name: code-review
description: Reviews code changes for correctness, contract violations, and test gaps. Use when the user asks for a code review or before committing changes.
metadata:
  author: pico
  version: "1.0"
---

# Code review

Review the change under discussion, not the whole repository.

1. Read the diff (`git diff` for unstaged work, `git diff --staged` for staged work, or the files the user named).
2. Check each changed behavior against the project's documented contracts (for Pico: `docs/extend/contracts.md`, `AGENTS.md`).
3. Flag only actionable findings: correctness bugs, broken contracts, missing error handling, and behavior changes without matching tests or docs.
4. Report findings as a numbered list ordered by severity, each with a file:line reference and a one-sentence fix suggestion. Close with a one-line verdict.
