# Working instructions for the repo

## Backwards compatability

Do not add any code for backwards compatability as the project is still in development and backwards compatability is not a consideration.

## Extension docs

Keep `docs/extend/` in sync with the public extension API (`app/include/pico/`, `pico_add_*`, loader behavior in `app/plugin.c`). Update the matching topic page and `contracts.md` when you change registration, lifecycle, threading, ownership, or reload. Add a topic to `docs/extend/README.md` and the `/docs` topic list if you introduce a new surface. Point examples in `examples/` at the same contracts.
