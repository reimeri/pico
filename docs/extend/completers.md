# Completers

Composer completions fire when the cursor is in a triggered token.

Host-scoped completers register in `host_init`; workspace-scoped completers (for example `@` files) register in `workspace_init`. Query/accept run on the main thread while typing.

```c
static int DollarQuery(PicoHost *host, const char *prefix, PicoCompleteItem *out, int max, void *state)
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
    snprintf(out[0].insert, sizeof(out[0].insert), "$todo");
    return 1;
}

static int DollarInit(PicoHost *host, void **state_out)
{
    (void)state_out;
    pico_host_add_completer(host, '$', false, DollarQuery, NULL);
    return 0;
}
```

Workspace-scoped completers register via `pico_workspace_add_completer(workspace, '@', false, WorkspaceQuery, NULL)` in `workspace_init`. The `@` builtin rescans the target workspace, not UI selection or process CWD.

## Fields

- `trigger` — character that starts the token (`/`, `@`, and `#` are taken by builtins).
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
- Builtins: `/` commands, `@` workspace files, `#` session references. Builtin `/resume` searches and initial `#` metadata snapshots run on core workers and may initially be empty; custom completer query/accept remain synchronous. `@` and `#` retain their discovered snapshot for an active token and filter it synchronously while typing; `/resume` may use a briefly stale cached search list.
- Non-command triggers are recognized at token boundaries; normal tokens end at whitespace. A selected builtin project qualifier can contain spaces in its name, but its query still ends at whitespace. Existing `/` command completion includes arguments on the first composer line.

## Builtin session references

Type `#` to browse main conversations in the current project, including its
recorded worktrees. Search matches session titles and IDs. Both open and
saved/closed sessions are eligible; the current conversation, unsaved empty
drafts, and subagent conversations are omitted.

Matching sidebar project names appear alongside current-project sessions.
Accepting a project inserts a qualifier such as `#clay-app:`; type more to search
that project's sessions across its worktrees. A uniquely named project can also
be qualified directly with `#clay-app:query`. Choose a project result when names
contain spaces/colons or are duplicated: selection retains its canonical project path,
not just the display name. Project results show paths, and session results show
checkout/worktree details and IDs. Search uses the durable catalog, whose refresh
is independent of completion typing; it does not enumerate or read transcripts
on the UI thread. Starting a token or changing its project scope loads a complete
snapshot of the eligible indexed metadata on a worker. The popup limit applies
only to displayed suggestions, not to the snapshot's searchable sessions or
project names. Once loaded, query edits and backspacing filter this snapshot
immediately without launching searches or replacing it with narrower results.
Prefix edits made while the initial snapshot is loading do not discard it; it is
filtered against the current query when adopted. Catalog/title changes appear in
a fresh token or scope snapshot, not in the middle of the active token. Changing
the token, conversation, or project scope clears the previous snapshot rather
than offering references from the previous scope.

Accepting a session creates an inline `#Title` chip. Spaces in titles need no
quoting or brackets. Cursor movement, selection, Backspace, and Delete treat the
reference as one item; long labels are visually abbreviated without changing the
stored title or identity. Copy/cut produces portable Markdown-link text containing
the title, durable session ID, canonical checkout, and absolute session-file path.
Pasting that text into Pico restores the chip, including across instances; other
applications see the explicit reference. Copying a referenced user message from
chat preserves the same portable identity. Clipboard references expose local paths
and should be shared only intentionally.

The model receives the title and absolute native JSONL session path **in place of
the tag**, for both a normal composer send and queued steering. Pico does not load,
attach, summarize, snapshot, resume, or switch to the referenced conversation. The
agent may read the path using its existing tools. It is a live file: later source
messages may be visible, and deleted/unreadable paths are ordinary tool read
failures rather than submission errors. Session titles/paths are not re-scanned
as `@file` mentions. Ordinary unaccepted hashtags remain literal user text.

The visible/persisted user message retains its portable reference while the
model history retains the projected title/path text. Replay preserves these two
forms without re-resolving names. The transport and session catalog are builtin
implementation details, not a public extension session-reading API.
