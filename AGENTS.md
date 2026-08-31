# Working instructions for the repo

Use `nix develop` environment for development tooling.

## Backwards compatability

Do not add any code for backwards compatability as the project is still in development and backwards compatability is not a consideration.

## Extension docs

Keep `docs/extend/` in sync with the public extension API (`app/include/pico/`, `pico_add_*`, loader behavior in `app/plugin.c`). Update the matching topic page and `contracts.md` when you change registration, lifecycle, threading, ownership, or reload. Add a topic to `docs/extend/README.md` and the `/docs` topic list if you introduce a new surface. Point examples in `examples/` at the same contracts.

## Clay viewport layout

The shell has a durable geometry contract: while chat follows the bottom, repeated Clay layout passes must not change the bounds of `Root`, `Body`, `Sidebar`, `RightColumn`, `MainColumn`, chat, composer, or footer unless content or the viewport actually changes.

- Full-height structural panes must take an exact bounded height from their parent (`FIXED` for the window root or `PERCENT(1)` for a parent's inner height). Do not use vertical `GROW` or `FIT` for a wrapper that represents the viewport or a full-height pane.
- `GROW` remains appropriate for content regions such as `MainColumn` and `ChatRow` once an ancestor establishes an exact viewport height.
- A wrapper added only for alignment or width constraints must use the child's computed fixed height when that height is already known. A vertical `FIT` wrapper around fixed-height content can participate in Clay compression and accumulate sub-pixel residue.
- Treat shell, sidebar, chat, composer, and footer hierarchy changes as scroll-layout changes. Clay stops grow/compression distribution within an epsilon, and bottom pinning, transitions, or same-frame relayout can feed that remainder into the next pass.
- Extend `TestBottomFollowShellGeometryStable` when adding a new viewport pane or changing shell nesting. It must exercise repeated bottom-follow layouts with and without the sidebar and assert stable observable bounds, not source macros.
- Run `cmake --build app/build/debug --target pico_host_workspace_tests && app/build/debug/pico_host_workspace_tests` after these changes.

## Tests

- Test observable behavior and durable contracts, not the current implementation shape.
- A test must protect a specific failure mode or product rule.
- Prefer one representative assertion per behavior. Do not cover the same transition through several equivalent call patterns unless each pattern can fail independently.
- Choose boundary cases from product semantics (for example, crater center, exact radius, and clearly outside), not from private tolerances or intermediate calculations.
- Do not assert against a value of a constant or variable that is meant to be configurable. Assert the behavior or relationship it produces so changing the value does not break the test.
- Avoid exposing production internals solely for tests.
- Tests should change when intended behavior changes, not when code is reorganized or an equivalent representation replaces another.
- Pure-function properties such as deterministic output or non-mutation are worth testing only when callers rely on them as part of the function's contract.