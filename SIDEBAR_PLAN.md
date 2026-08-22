# Pico Workspace Sidebar Plan

## Agreed design

- Agents can run concurrently across different workspaces.
- Builtins and user-global extensions, settings, and models are shared.
- Workspace-local `.pico/extensions` and `.pico/settings.json` will no longer load.
- Workspace `.pico/SYSTEM.md` and `AGENTS.md` remain agent-specific.
- The sidebar shows normal sessions only; subagent activity bubbles to the parent session.
- Completed means an unread background result and clears when viewed.
- Existing encoded workspace directory keys remain.

## 1. Workspace registry and metadata

Add `app/workspace.c` and `app/workspace.h`.

Workspace layout:

```text
~/.config/pico/sessions/
├── workspaces.json
├── --home-user-project--/
│   ├── .workspace.json
│   └── *.jsonl
└── ...
```

`workspaces.json` stores:

- workspace key, canonical path, display name, ordering, and collapsed state;
- a rebuildable session cache containing title, model, effort, and mtime.

Each workspace directory gets a small `.workspace.json` manifest containing its key and canonical path. This makes empty workspaces recoverable if the root metadata is corrupted.

Rules:

- Direct workspace directories are authoritative.
- Missing directory or session metadata is pruned.
- Existing directories are inferred from manifests or consistent JSONL `cwd` headers.
- Mixed-workspace directories are marked unavailable rather than guessed.
- Encoding collisions receive a stable hash suffix.
- Metadata is never trusted for session authorization or replay.
- Writes use an advisory lock, no-follow opens, a temporary file, `fsync`, rename, and directory `fsync`.
- Missing project directories remain visible but unavailable until restored.

## 2. Make workspace identity agent-owned

Extend `PicoAgent` with immutable:

- `workspace_key`;
- canonical `workspace_path`;
- `last_selected_seq`;
- per-agent UI state;
- unread completion state.

Public API changes in `app/include/pico/agent.h`:

- `PicoAgentCreateOptions` accepts one optional `workspace_key`; `NULL` means the active workspace.
- `PicoAgentInfo` exposes copied workspace key/path and presentation status.
- Subagents must inherit their parent workspace; workspace overrides are rejected.
- `pico_agent_select()` accepts normal agents only.
- Targeted hook events include copied workspace key/path snapshots so destroy and background hooks never depend on the active workspace.

`app->workspace` becomes a documented alias for the active normal agent's workspace. Background callbacks must use their hook event or `PicoAgentInfo`, never that alias.

## 3. Refactor session handling

Change `app/session.c` APIs to operate on an explicit workspace or target agent rather than `app->workspace`.

Before listing or replaying a session:

- require a regular, non-symlink JSONL file;
- require its canonical parent to be the registered workspace directory;
- validate the schema version, agent kind, exact ID, and filename/header ID agreement;
- require the canonical header `cwd` to equal the workspace path;
- reject duplicate IDs as ambiguous.

Add a per-session advisory lock held for the durable agent's lifetime. This supplements the existing in-process reservation and prevents another Pico process from writing the same JSONL.

Historical session behavior:

- Clicking a live session selects it.
- Clicking an unopened session creates and selects a new agent.
- A live agent without a persisted JSONL yet appears as a temporary `Untitled` row.
- Closing an idle live agent does not delete its JSONL.
- Missing live session files are not silently recreated; persistence enters the failed/error state.

`--session` is preflighted from its header before app initialization and must belong to the registered workspace directory.

## 4. Remove app-global workspace behavior

Replace the current whole-manager workspace transition in `app/app.c`.

New behavior:

- `/cd path`: register the workspace, select its most recently selected live session, or create a new session there.
- `/new`: create another agent in the invoking agent's workspace.
- `/resume`: select an already-open exact session or open it as another agent.
- Existing agents continue running.
- Global reload still waits for all runtimes retaining extension pointers, events, asks, or delegation jobs.

Commands become transactions against the invoking agent:

1. Capture the invoking agent ID.
2. Consume and clear only its command draft.
3. Set `submit_cancel`.
4. Perform creation or selection.

This prevents a command from clearing another session's composer after selection changes.

## 5. Global-only configuration and extensions

Update:

- `app/plugin.c` to stop discovering and polling `.pico/extensions`;
- `app/settings.c` to stop loading `.pico/settings.json`;
- prompt/context loading to accept the target agent workspace explicitly;
- files, footer, completions, prompt overlay, session paths, and shell context to use the target or active agent workspace.

User-global extensions and models remain shared. Reload remains globally coordinated across all agents.

## 6. Per-agent UI state

Add `PicoAgentUiState` for every normal agent:

- composer text, cursor, and selection;
- chat selection;
- chat and composer scroll offsets;
- scrollbar drag state;
- follow-bottom and overflow state;
- unread completion.

Remove the public by-value composer/chat fields from `PicoApp`, add active-agent and explicit-agent composer accessors, and bump `PICO_EXT_ABI`.

Selection behavior:

- save scroll state every frame;
- change active identity and selection epoch;
- close completion/footer menus and clear transient hover geometry;
- restore the destination scroll state after Clay recreates its scroll containers;
- clear unread completion.

Errors and usage are already agent-owned.

### Ask overlays

Keep a core `PicoAskUiStore` keyed by ask ID because asks may belong to hidden subagents. It preserves partial questionnaire answers across session switches.

Only asks belonging to the active normal agent or its descendants are visible and modal. Background asks produce a yellow sidebar status without blocking the active composer.

Ask-store entries record the owner agent ID, runtime generation, and root normal-agent ID. Entries are removed when an ask is answered, cancelled, made stale by runtime replacement, or when its agent is destroyed.

## 7. Presentation statuses

Add a UI-facing status separate from the execution state:

| Condition | Sidebar |
|---|---|
| New, replayed, or acknowledged | Idle, no dot |
| Model, tool, or compaction active | Animated running indicator |
| Root or descendant pending ask | Yellow dot |
| Root or live descendant error | Red dot |
| Successful background root completion | Blue dot |
| Successful visible completion | Idle |
| Cancel or force-cancel | Idle |

Aggregation priority:

```text
error > waiting for user > running > completed > idle
```

Selecting a completed session clears only its unread status, not its error. Child completion never independently marks the parent unread. A child error bubbles only while the child remains live; after the child is destroyed, the parent's state governs the row.

## 8. Built-in sidebar

Add `app/builtins/sidebar.c` and register it in `app/plugin.c`.

Layout:

- approximately 240 pixels wide and vertically scrollable;
- top `Add workspace` button;
- workspace folder icon, name, hover chevron, and `+` button;
- expandable normal-session rows;
- active row using the existing pill/background style;
- theme-consistent status colors and a pulsing running animation;
- hover `×` for idle live agents, without deleting their history.

`Add workspace` opens a built-in modal with:

- folder picker;
- editable path;
- editable name defaulted from the folder basename;
- validation and confirmation.

Adding an existing path expands and focuses its existing workspace instead of creating a duplicate. The workspace `+` button creates and selects a new normal durable agent in that workspace.

Workspace and session behavior:

- Clicking a workspace chevron expands or collapses it and persists the state.
- Clicking a live session selects it and restores its UI state.
- Clicking a historical session resumes it immediately while other agents continue.
- Unavailable workspaces cannot create or resume sessions and show a useful notification.
- Agent-limit, session-lock, persistence, and creation failures are shown through the existing notification/error UI.

## 9. Implementation phases

### Phase 1: Workspace registry

- Implement workspace manifests and `workspaces.json`.
- Add reconciliation, collision handling, corruption recovery, ordering, and collapse persistence.
- Add atomic and concurrent metadata mutation tests.

### Phase 2: Agent workspace identity

- Move workspace key/path into `PicoAgent`.
- Extend copied public snapshots and creation options.
- Make subagents inherit their parent's workspace.
- Centralize active-agent selection and the `app->workspace` alias.
- Reject selection of hidden subagents.

### Phase 3: Session validation and locking

- Make session listing, resolution, creation, and replay workspace-explicit.
- Validate header `cwd`, IDs, canonical parent directories, and file types.
- Add process-wide advisory session locks.
- Add cross-workspace listing/resume and malformed-path tests.

### Phase 4: Global-only runtime configuration

- Remove workspace extension discovery and workspace settings/model overrides.
- Make prompt/context generation workspace-explicit per agent.
- Audit tools, files, completions, footer, prompt overlay, hooks, and reload behavior.

### Phase 5: Commands and lifecycle

- Replace whole-manager workspace transitions.
- Implement transactional `/cd`, `/new`, and `/resume`.
- Update startup, `--resume`, `--session`, active fallback after close, and shutdown behavior.
- Verify concurrent agents continue while another workspace is opened.

### Phase 6: Per-agent UI state

- Move composer, chat selection, scrolling, and follow-bottom state into normal agents.
- Add composer accessors and bump the extension ABI.
- Restore state on selection without leaking fixed Clay scroll-container state.
- Close or reset transient completion/footer/hover state on selection changes.

### Phase 7: Asks and statuses

- Add target/root-tree ask enumeration and routing.
- Preserve builtin ask drafts by ask ID.
- Implement unread completion and descendant status aggregation.
- Add transition tests for success, error, waiting, answer, cancel, force-cancel, selection, and close.

### Phase 8: Sidebar and modal

- Implement the built-in sidebar view and pointer handling.
- Implement workspace collapse, session selection/resume, new session, and idle close.
- Implement the add-workspace modal and folder picker.
- Match existing theme, spacing, hover, and active-row styling.

### Phase 9: Documentation and final verification

- Update user and extension contracts.
- Build with warnings as errors.
- Run all tests and semantic diagnostics.
- Manually verify concurrent sessions, background status updates, session switching, asks, and persistence.

## 10. Verification matrix

Add focused tests for:

- metadata recovery, unsupported versions, corruption, collisions, and concurrent mutation;
- empty-workspace recovery from `.workspace.json`;
- rejection of mixed-`cwd` workspace directories;
- cross-workspace concurrent providers and correct tool working directories;
- workspace-specific `.pico/SYSTEM.md` and `AGENTS.md` context;
- user-global-only extensions, settings, and models;
- session traversal, symlink, duplicate-ID, filename/header mismatch, `cwd` mismatch, and lock rejection;
- command-driven selection preserving source and destination drafts;
- independent composer, selection, scroll, and follow-bottom state;
- background completion acknowledgement;
- simultaneous asks across normal agents and hidden descendants;
- descendant status aggregation;
- normal-only selection and sidebar rows;
- global agent cap, idle close, active fallback, reload, and shutdown;
- startup through cwd, `--resume`, and `--session`.

Final verification commands should include:

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

Run LSP diagnostics on all changed C headers and source files as well.

## 11. Documentation updates

Update:

- `README.md`;
- CLI help in `app/main.c`;
- `/docs` topic list in `app/builtins/commands.c`;
- `docs/subagents.md`;
- `docs/extend/README.md`;
- `docs/extend/agents.md`;
- `docs/extend/anatomy.md`;
- `docs/extend/contracts.md`;
- `docs/extend/context.md`;
- `docs/extend/views.md`;
- `docs/extend/hooks.md`;
- `docs/extend/tools.md`;
- `docs/extend/commands.md`;
- relevant examples that use the composer, hooks, or agent lifecycle API.

Document explicitly:

- removal of workspace-local extensions and `.pico/settings.json`;
- global reload behavior across concurrent agents;
- active-only meaning of `app->workspace`;
- explicit target requirements for background hooks;
- per-agent composer and viewport ownership;
- workspace/session metadata authority;
- ask routing and unread completion semantics;
- session-lock and stale-ID behavior.

## Definition of done

The feature is complete when:

- sessions from multiple workspaces can run concurrently;
- switching sessions restores their transcript, composer, selection, scroll, follow-bottom, usage, error, model, effort, and visible ask state;
- background running, waiting, error, and unread completion states update in the sidebar;
- historical sessions open without stopping other agents;
- workspace/session metadata is recoverable and cannot redirect session replay;
- session files cannot have multiple live writers across Pico processes;
- workspace-specific prompt context is correct while runtime extensions/settings remain user-global;
- reload, cancellation, force-cancel, close, and shutdown remain safe with all agents;
- the sidebar and modal match Pico's existing visual style;
- tests, build, diagnostics, examples, and public documentation all pass and agree with the implementation.
