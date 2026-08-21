# Working instructions for the repo

## Backwards compatability

Do not add any code for backwards compatability as the project is still in development and backwards compatability is not a consideration.

## Extension docs

Keep `docs/extend/` in sync with the public extension API (`app/include/pico/`, `pico_add_*`, loader behavior in `app/plugin.c`). Update the matching topic page and `contracts.md` when you change registration, lifecycle, threading, ownership, or reload. Add a topic to `docs/extend/README.md` and the `/docs` topic list if you introduce a new surface. Point examples in `examples/` at the same contracts.

## Tests

- Test observable behavior and durable contracts, not the current implementation shape.
- A test must protect a specific failure mode or product rule.
- Prefer one representative assertion per behavior. Do not cover the same transition through several equivalent call patterns unless each pattern can fail independently.
- Choose boundary cases from product semantics (for example, crater center, exact radius, and clearly outside), not from private tolerances or intermediate calculations.
- Avoid exposing production internals solely for tests.
- Tests should change when intended behavior changes, not when code is reorganized or an equivalent representation replaces another.
- Pure-function properties such as deterministic output or non-mutation are worth testing only when callers rely on them as part of the function's contract.