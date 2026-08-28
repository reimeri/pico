# Completers

Composer completions fire when the cursor is in a triggered token.

```c
static int HashQuery(PicoHost *host, const char *prefix, PicoCompleteItem *out, int max, void *state)
{
    (void)host;
    (void)prefix;
    (void)state;
    if (max < 1)
    {
        return 0;
    }
    snprintf(out[0].label, sizeof(out[0].label), "todo");
    snprintf(out[0].detail, sizeof(out[0].detail), "example");
    snprintf(out[0].insert, sizeof(out[0].insert), "#todo");
    return 1;
}

static int HashInit(PicoHost *host, void **state_out)
{
    (void)state_out;
    pico_host_add_completer(host, '#', false, HashQuery, NULL);
    return 0;
}
```

Workspace-scoped completers (e.g. `@` files) register via `pico_workspace_add_completer(workspace, '@', false, WorkspaceQuery, NULL)` in `workspace_init`.

## Fields

- `trigger` — character that starts the token (`/` and `@` are taken by builtins).
- `bol_only` — if true, only when the trigger is at the start of the composer (commands). If false, the trigger must be at a token start (preceded by start-of-text or whitespace).
- `query(host/workspace, prefix, out, max, state)` — fill up to `max` items (`PICO_MAX_COMPLETE_ITEMS` is 24). `prefix` is the text after the trigger. Return the count.
- `accept` — optional. Return true if you handled insertion yourself; otherwise Pico replaces the token with `item->insert` (or `label` if `insert` is empty).

Each item:

- `label` — list row, up to `PICO_COMPLETE_LABEL_MAX` (288) UTF-8 bytes
- `detail` — muted secondary text, right-aligned in the row
- `insert` — text that replaces the whole token, **including the trigger** (e.g. `/model gpt-5.6-sol`, `@src/foo.c`)

## Contract

- Query/accept run on the **main thread** while typing.
- Esc (or a click outside the popup) dismisses completions. Query stays skipped until the composer text changes or the cursor moves to a different token.
- Max 16 completers (`PICO_MAX_COMPLETERS`). First match for a trigger wins (`bol_only` preferred when the cursor is at bol).
- Builtins: `/` commands, `@` workspace files. `@` rescans the workspace when a mention token starts and keeps that snapshot while the cursor stays in the token.
