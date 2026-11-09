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
- Builtins: `/` commands, `@` workspace files, `#` session references. Builtin `/resume` searches and initial `#` metadata snapshots run on core workers and may initially be empty; custom completer query/accept remain synchronous. `#` filters its ready snapshot synchronously. Builtin `@` discovers and searches paths on core-owned workers and displays a nonselectable loading/status row when pending without previous suggestions. This is not a public asynchronous completer API. `/resume` may use a briefly stale cached search list.
- Non-command triggers are recognized at token boundaries; normal tokens end at whitespace. Quoted `@"..."` file tokens can contain spaces; custom completers still receive their raw prefix after the trigger. A selected builtin project qualifier can contain spaces in its name, but its query still ends at whitespace. Existing `/` command completion includes arguments on the first composer line.

## Builtin file mentions

File discovery uses a reusable, lazy **workspace-owned path index**, shared by
conversations in that workspace. There is no file-count cutoff: the popup limit
bounds displayed results, not which paths can be searched. Discovery, cache I/O,
filesystem watching, directory-scope discovery, sorting, and matching run off the
UI thread. Requests coalesce; superseded results cannot replace the current
query. Ranking is case-insensitive ASCII: exact basename, basename prefix, path
prefix, then path substring, with deterministic basename/path ordering within a tier.

While a new file query is pending within the same token/conversation, the popup
retains its previous suggestion rows, selection, and status instead of collapsing
to a loading row. Those displayed suggestions remain selectable even if they no
longer match the edited prefix; accepting replaces the current mention range.
Completed results replace the rows normally, including closing on no matches.
Moving to another token/workspace/conversation or dismissing the popup clears the
retained rows. Initial loading without prior suggestions still shows Searching.

A token pins the latest ready index generation, including across query edits and
backspacing. A new token or conversation uses the latest ready generation, **not
an obligatory fresh scan**. Initial discovery can show Loading. A persisted cache
can serve a stale generation immediately after loading while reconciliation runs,
without adding an informational cached-list row to the popup. Genuine failures
still produce status feedback. Background changes do not replace an already pinned token. Start a new mention to see a newer index.

Git projects enumerate tracked and non-ignored untracked files using Git; tracked
files remain eligible even if ignored. Non-Git projects use ripgrep with hidden
files enabled and ignore rules respected. Both commands use argv and NUL-delimited
output, not shell interpolation. If neither discovery method succeeds, native
traversal provides results with an explicit warning that ignore rules are not
available; those fallback snapshots are not persisted. `.git` and `.pico` trees
are always omitted. Deleted tracked paths, non-regular files, directory symlinks,
and nested Git projects/submodules are not recursively indexed. Open a submodule
as its own workspace. Long/unrepresentable completion paths are omitted rather
than truncated. Explicit file mentions still resolve independently of the index.

An explicit workspace-relative directory query, such as `@ignored-folder/name`,
can discover an ignored directory on a worker. This scope is pinned while editing
within that directory; changing directories changes the scope. Directory symlinks,
parent traversal, internal trees and nested project boundaries are not traversed.
This does not prevent explicitly submitting an otherwise valid file mention.

Linux uses debounced inotify watches for indexed directories and their ancestors;
watch sets are replaced on refresh. Periodic reconciliation also runs, including
on other POSIX platforms or when watch capacity is exhausted. Changes inside an
existing directory without indexed descendants (for example an empty directory),
worktree Git metadata outside the root, global ignore changes and missed events
may wait for the periodic scan (currently 30 seconds plus discovery time). A token
must still be restarted to adopt that generation.

Versioned, disposable caches live in `$XDG_CACHE_HOME/pico/files/` or
`~/.cache/pico/files/`, never workspace session assets. They contain paths, not
contents, validate the exact root and filesystem identity, and use atomic
replacement. Cache corruption falls back to discovery. Resource/I/O failures
produce a visible status and retain the previous usable index, never silently
publish an arbitrarily truncated list. Construction has a per-snapshot memory
budget; a service may retain both the latest and token-pinned snapshots while
building a replacement or explicit scope. Workspace shutdown/reload cancels the
core service without joining workers on the UI thread. Workers own their remaining
state, reap subprocesses, and release it asynchronously; they invoke no extension
callbacks. The extension registration and synchronous callback ABI are unchanged.

File completion inserts `@src/file.c` for simple ASCII paths and a quoted mention
such as `@"design notes.md"` for spaces, punctuation, or Unicode. Quotes and
backslashes are escaped as `\"` and `\\`; control characters use JSON-style
escapes such as `\n`. File queries continue inside an unfinished quote, including
spaces. Accepting a suggestion while editing inside an existing quoted mention
replaces that whole mention, preserving text before and after it.

Bare mentions accept ASCII letters/digits and `_`, `-`, `.`, `/`; other names
must be quoted. Both forms start at a whitespace/start-of-text boundary and end
at whitespace/end-of-text. Completion and submission share the same encoding;
malformed, incomplete, NUL-containing, or oversized names are not resolved to a
shorter prefix file. A completion that cannot fit the label/insertion limits is
omitted rather than truncated. Quoted interiors and session-reference metadata
are not rescanned for additional mentions.

Submission resolves the exact decoded path against the target workspace and
uses the existing bounded text/media attachment rules. Quoted mentions are
plain editable composer text, not atomic chips. Extensions supplying replacement
composer input through submit hooks must use the same spelling. See
[contracts](contracts.md#composer-file-mentions).

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
