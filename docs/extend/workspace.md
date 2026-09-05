# Workspace

`PicoWorkspace` is one canonical directory and its complete execution environment: agents, settings, instructions, extension instances, registration generation, sessions, asks, mailboxes, and workspace caches. It is opaque; include `pico/workspace.h` (or `pico/plugin.h`).

A host may run several workspaces at once. Main agents in the same canonical directory share one workspace runtime. Different canonical directories are isolated.

## Open

```c
PicoWorkspaceId id = 0;
PicoResult rc = pico_workspace_open(host, path, &id);
```

`pico_workspace_open` trims the input, resolves it with `realpath`, verifies it is a directory, and stores that canonical absolute path. The path never changes after creation.

- Opening an already-open canonical path, including a symlink alias, returns `PICO_ALREADY_OPEN` and writes the existing ID to `out`.
- Opening a workspace does not create a main agent.
- At most one non-closed workspace exists for a canonical path.
- At most `PICO_MAX_WORKSPACES` (8) live workspaces. A ninth distinct path returns `PICO_LIMIT`.

`pico_workspace_count` / `pico_workspace_info` copy live entries. `PicoWorkspaceInfo` includes `id`, `state`, `path`, `main_agent_count`, and `total_agent_count`.

`pico_workspace_host(workspace)` returns the owning host. Use it from workspace callbacks that need host APIs (`pico_ui_modal_push`, `pico_agent_set_compact_summary`, …).

## Lifecycle

```text
OPEN --request reload--> RELOADING --publish or failed rollback--> OPEN
OPEN --request close--> CLOSING --quiescent destroy--> CLOSED
RELOADING --request close--> CLOSING
```

`CLOSING` has no transition back to `OPEN`. Closing the last main agent does **not** close the workspace; it stays `OPEN` with zero agents until `pico_workspace_request_close`.

`pico_workspace_request_close` changes `OPEN` or `RELOADING` to `CLOSING`, rejects new turns and delegation, requests cancellation for every agent, and keeps pumping until callbacks and retired runtimes finish. It then destroys workspace instances and becomes `CLOSED` before removal from the host array. Durable session files are preserved; reservations release after the owning agent/runtime is destroyed. Pending asks in that workspace cancel.

If close is requested during a staged reload, Pico aborts the rollout: it does not publish, shuts down every staged instance in reverse initialization order, discards staged registrations, and releases staged references. The previously active generation stays retained until close quiescence releases it.

A worker callback that never returns leaves **that** workspace in `CLOSING`. Other workspaces continue. Main-thread callbacks cannot be isolated: they must return promptly, and a violation blocks the host pump.

Closing a workspace is backend-only (`pico_workspace_request_close`). The builtin sidebar is the workspace-management UX: it lists the **disk catalog** under `~/.config/pico/sessions/` (each encoded-key folder, with `.workspace.json` for `path` / display name / order / collapsed and a stat-generation-keyed session listing cache, while parent `*.jsonl` files remain authoritative sessions). Catalog mutations are serialized per workspace, and scans re-parse only new or changed JSONL files. Scan omits a catalog workspace whose `path` is not an existing directory. A catalog entry is not a live `PicoWorkspace` until the user opens it. Normal CLI startup opens cwd, creates one main agent, and ensures cwd has a catalog folder. The installed desktop launcher starts with zero live workspaces; opening a catalog workspace/session or using Add workspace creates the live workspace and its first main agent. Catalog size is not capped at 8; opening a ninth live workspace still returns `PICO_LIMIT` (“Too many workspaces are open.”). This pass has no close/remove control in the sidebar. `/cd` never closes the previous workspace.

## `/cd`

`/cd` canonicalizes and opens the target (or reuses an already-open canonical path), creates a main agent there if that workspace has none, and selects a main agent with `pico_agent_select`. Relative paths resolve against the command agent's workspace, never UI selection. A newly opened workspace is closed if creating its first main agent fails. `/cd` does not select into a closing workspace. The previous workspace stays open and keeps pumping.

The folder picker (footer cwd and sidebar Add workspace) uses the same open-or-select path. Sidebar Add workspace also creates the catalog folder and `.workspace.json` before opening.

## Agents

```c
PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN};
PicoAgentId id = 0;
pico_main_agent_create(host, workspace_id, &opt, &id);
pico_agent_submit(host, id, "hello", NULL);
pico_agent_cancel(host, id);
pico_agent_close(host, id);
```

`pico_main_agent_create` accepts only `PICO_AGENT_MAIN`. Delegated subagent creation is private to workspace delegation. An agent belongs to one workspace for its lifetime. A subagent and every descendant belong to the parent's workspace; a cross-workspace parent ID is rejected.

`PICO_MAX_AGENTS` (16) is per workspace. `PICO_MAX_TOTAL_AGENTS` (32) is host-wide. Either cap fails creation with `PICO_LIMIT` and does not publish a partial agent.

Submit, cancel, model, effort, session, and compaction APIs take an explicit agent ID. See [agents](agents.md).

## Settings and files

Each workspace loads a resolved model catalog and execution defaults from user-global `$XDG_CONFIG_HOME/pico/settings.json` (or `~/.config/pico/settings.json`) plus `<workspace>/.pico/settings.json`. On startup, Pico creates the user-global file from its bundled `examples/settings.json` only if the path is absent and leaves any existing file, symlink, or directory untouched. Workspace settings (`PicoWorkspaceSettings`) are defaults for newly created main agents: default model, compact ratio, `resume_last`, `disabled_extensions`, and `context_limit_fallback`. `/settings` edits the user-global file only; a workspace `.pico/settings.json` still replaces the catalog and defaults when present. Apply reloads the resolved catalog for new agents immediately. A running agent keeps its selected model metadata through the active turn; once idle, it keeps the same model ID if that ID still exists or moves to the new default if it was removed. Existing agents keep their copied compaction setting and effort unless reconciliation must choose a valid effort for the reloaded model.

Host-wide `font_scale`, `chat_width`, and `disabled_host_extensions` come only from the user-global `settings.json`; Pico ignores those fields in `<workspace>/.pico/settings.json`. `disabled_extensions` controls only workspace instances in that workspace. It never disables the module's host instance or its instances in another workspace.

`AGENTS.md` and `.pico/SYSTEM.md` are read for the **target** workspace when building each turn's instruction snapshot. A turn keeps the snapshot it started with.

Session directory resolution, file mentions, shell cwd (after `fork` in the child), media persistence, git diff, and settings paths all receive the target workspace explicitly. They never read UI selection or process CWD. Pico and its extensions never call `chdir()` in the host process.

If the workspace directory is deleted after open, the workspace identity remains. Filesystem operations against it fail; Pico does not retarget to another path.

## Reload

`pico_workspace_request_reload` and `/reload` / F5 (for the selected agent's workspace) queue reload until that workspace is quiescent: no live or retired runtime with provider/tool work, pending calls, offered-tool snapshot, pending ask, undrained events, retained callbacks, or delegation wait/job. As soon as reload is requested, **that** workspace refuses new external turns and delegations while continuing to pump follow-ups, cancellation, and asks. Other workspaces keep accepting work.

Once quiescent, Pico compiles asynchronously with a 30-second deadline, then initializes a complete candidate module/registration generation on the main thread and publishes it atomically. Rendering and input remain responsive while compilation is pending. Compile, load, validation, or init failure leaves the previous generation active. Polling does not recompile an unchanged failed workspace-local source; F5 and `/reload` still retry. An accepted turn retains the generation it started with until that turn and every event, pending call, ask, delegation reference, and callback from it is gone.

User-global source changes compile once. Each workspace independently stages and publishes the new module generation when that workspace is quiescent. Workspace-local sources under `<workspace>/.pico/extensions/` affect only their owning workspace. A workspace-local source must set every host callback to `NULL`; otherwise activation fails and the previous generation remains in use.

See [anatomy](anatomy.md) and [contracts](contracts.md).

## Workspace-scoped registrations

Register these only from `workspace_init`:

- Contextual views and empty-chat views (`pico_workspace_add_view`, `pico_workspace_add_empty_view`)
- Workspace commands and completers
- Tools and providers
- Before/after tool hooks, LLM/context/tool-row hooks
- Agent lifecycle / message / submit hooks (`pico_workspace_add_hook`)
- Descriptor `workspace_on_frame`

Workspace contextual views receive the workspace pointer and the explicit selected agent ID. They run only when the selected agent belongs to that workspace. Workspace `on_frame` runs for every `OPEN` or `RELOADING` workspace, selected or not; it must not draw with Clay or Raylib. Rendering remains a view callback.

Workspace builtins include TODO state, shell tool, subagent tool, file index/completion, diff worker/model, execution commands, providers, and tool ask records.

## Mailboxes and asks

Mailbox storage is workspace-owned. The key is `(agent_id, runtime_generation, name)`, so two agents may use the same name without collision. A workspace holds at most `PICO_MAX_UI_POSTS` (16) keys; two agents posting the same name occupy two slots. Closing, force-cancel, or generation retirement drops matching unpublished and published entries.

Ask IDs are host-allocated. Each workspace stores its own pending asks. The host returns the oldest live ask across workspaces. Answer routing uses ask ID, then validates workspace, agent ID, and runtime generation.
