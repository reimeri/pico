# Multi-workspace main-agent refactor plan

Status: Phase 7 implemented
Audience: implementation team and reviewers  
Scope owner: the developer integrating each phase

This plan is normative. Implement the names, ownership rules, ordering, and behavior described here. If current code conflicts with this document, change the code; do not add compatibility wrappers for the old architecture. If an unforeseen requirement makes a step impossible, stop and amend this plan before choosing a different design.

## 1. Goal

Allow one Pico process and one window to run multiple user-facing main agents concurrently. Different main agents may belong to different workspaces. Main agents in the same canonical directory share one workspace runtime. Existing subagent concurrency remains supported.

The refactor is complete when two workspace runtimes can execute overlapping agent turns with isolated settings, instructions, extensions, sessions, asks, mailboxes, reload, cancellation, and closure.

## 2. Non-goals

Do not add these during this refactor:

- A workspace/sidebar/tab UX. The existing UI may continue to show one selected agent.
- Moving a live agent or session from one workspace to another.
- Cross-workspace subagent delegation.
- A child process or IPC backend per workspace.
- Persisting the list of open workspaces or agents across process restart.
- Filesystem sandboxing, extension trust prompts, or a permission system.
- Backward compatibility for `PicoApp`, extension ABI 12, or old extension callback signatures.
- More than one live workspace runtime for the same canonical directory.

## 3. Fixed terminology and type names

Use these names consistently in code and documentation:

- **Host** / `PicoHost`: one process/window owner. Owns process services, UI state, IDs, module code, and all workspaces.
- **Workspace** / `PicoWorkspace`: one canonical directory and its complete execution environment. It replaces the ownership role currently split between `PicoApp` and `PicoAgentManager`.
- **Main agent**: a user-owned root agent. Rename `PICO_AGENT_NORMAL` to `PICO_AGENT_MAIN`.
- **Subagent**: a delegated agent with a parent in the same workspace.
- **Module generation**: one loaded version of extension code from one source file.
- **Extension instance**: mutable state created from a module generation for either the host or one workspace.
- **Registration generation**: one immutable set of workspace callback registrations. A turn retains the generation it started with.

Public opaque declarations:

```c
typedef struct PicoHost PicoHost;
typedef struct PicoWorkspace PicoWorkspace;
typedef uint64_t PicoWorkspaceId;
typedef uint64_t PicoAgentId;
```

Zero is invalid for every ID. IDs are monotonically allocated by `PicoHost` on the main thread and are never reused during the process lifetime.

## 4. Required ownership model

### 4.1 `PicoHost` owns

- The array of live `PicoWorkspace *` values.
- The selected `PicoAgentId` used only by UI adapters.
- Composer, transcript selection, scrolling, modal presentation, clipboard, fonts, theme, and other window state.
- Host preferences: font scale, chat width, and other window-only settings.
- The auth store and provider login/refresh coordination.
- One process-wide curl initialization and cleanup.
- Agent, workspace, ask, and runtime-generation ID allocation.
- Agent lookup across all workspaces.
- Extension source discovery, compilation, and loaded module generations.
- Host-scoped extension instances and registrations.
- Fair pumping of all workspaces.
- Process shutdown and the final retained-shutdown decision.

### 4.2 `PicoWorkspace` owns

- A back-pointer to its host.
- Its immutable `PicoWorkspaceId`.
- Its immutable canonical absolute path.
- Its lifecycle state.
- All main agents, subagents, retired runtimes, delegation jobs, and session reservations in that workspace.
- Resolved execution settings and model catalog.
- Workspace instructions, subagent profiles, and extension-disable settings.
- Workspace extension instances and the active registration generation.
- Workspace plugin reload/poll state.
- Workspace file index, git diff model, TODO state, mailbox storage, and other workspace caches.
- Workspace warnings and errors that are not process-global.

`PicoWorkspace` absorbs `PicoAgentManager`; do not retain a second owner object with a duplicate agent list or lifecycle. Rename and migrate `agent_manager.c/.h` to `workspace.c` and `workspace_internal.h` when Phase 3 reaches its rename step.

### 4.3 `PicoAgent` owns

- An immutable pointer to its `PicoWorkspace`.
- Its immutable globally unique `PicoAgentId`.
- Parent identity, depth, profile, and purpose.
- Transcript, runtime, session, usage, model/effort selection, compaction state, and tool policy.

Remove `PicoAgent.manager`. Do not store `PicoHost *` separately on an agent; reach it through `agent->workspace->host` on the main thread.

### 4.4 Ownership table for current mixed state

| Current state | Final owner |
|---|---|
| `PicoApp.agents` | Folded into each `PicoWorkspace` |
| `PicoApp.workspace`, `pending_workspace` | Immutable `PicoWorkspace.path`; no pending path mutation |
| `PicoApp.settings` | Split into `PicoHostPreferences` and `PicoWorkspaceSettings` |
| Models/catalog | Resolved and owned by each workspace |
| Composer and chat selection | Host UI state |
| Tools/providers/execution hooks | Workspace registration generation |
| Views/commands/completers | Host or workspace instance according to registration scope |
| Auth registrations/store | Host |
| `g_plugins` and poll timer | Host module store plus per-workspace desired/active generations |
| File completion cache | Workspace extension state |
| Diff worker/model | Workspace extension state |
| TODO map | Workspace extension state keyed by agent ID |
| Chat/composer/footer/prompt globals | Host extension state |
| Ask records | Workspace-owned; IDs host-allocated |
| UI mailboxes | Workspace-owned and keyed by agent/generation/name |
| curl lifecycle | Host |

## 5. Hard invariants

Every phase must preserve these invariants once the relevant type exists:

1. One canonical path maps to at most one non-closed workspace in a host.
2. A workspace path never changes after creation.
3. An agent belongs to one workspace for its full lifetime.
4. A subagent and every descendant belong to the parent's workspace.
5. Backend behavior never depends on the UI-selected agent.
6. Workers do not read or mutate host UI state.
7. Main-thread host/workspace mutation is serialized.
8. Worker callbacks from different agents and workspaces may overlap.
9. A runtime retains the exact immutable registration generation used by its accepted turn until that turn and all of its references quiesce.
10. A module generation remains loaded while any instance, registration, worker callback, or retained runtime can reference it.
11. Reload or close in one workspace does not stop acceptance or pumping in another workspace.
12. Workspace extensions never mutate process-global or file-static state. Mutable state lives in the extension instance.
13. Pico and its extensions never call `chdir()` in the host process. Shell tools may change directory only after `fork()` in the child.
14. Stale workspace, agent, ask, and runtime-generation IDs fail without affecting a different object.
15. Session paths and workspace file paths are derived from the target workspace, never from selection or process CWD.
16. Main-thread extension callbacks must return promptly and must never wait on worker completion. Pico can isolate a stuck worker callback to one workspace, but a stuck main-thread callback blocks the whole host and is an extension contract violation.

## 6. Public host/workspace API contract

Create `app/include/pico/host.h` and `app/include/pico/workspace.h`. Move shared message/tool/provider declarations that are not host-specific into `app/include/pico/app.h`; `app.h` must no longer define `PicoApp`.

The final public lifecycle and routing surface must provide these operations. Exact parameter structs may be expanded only when a phase explicitly requires another field.

```c
typedef enum PicoWorkspaceState {
    PICO_WORKSPACE_OPEN = 0,
    PICO_WORKSPACE_RELOADING,
    PICO_WORKSPACE_CLOSING,
    PICO_WORKSPACE_CLOSED,
} PicoWorkspaceState;

typedef enum PicoResult {
    PICO_OK = 0,
    PICO_INVALID,
    PICO_NOT_FOUND,
    PICO_BUSY,
    PICO_LIMIT,
    PICO_ALREADY_OPEN,
    PICO_SESSION_IN_USE,
    PICO_SESSION_INVALID,
    PICO_NO_MEMORY,
} PicoResult;

typedef struct PicoWorkspaceInfo {
    PicoWorkspaceId id;
    PicoWorkspaceState state;
    char path[4096];
    int main_agent_count;
    int total_agent_count;
} PicoWorkspaceInfo;

typedef enum PicoHostShutdownResult {
    PICO_HOST_SHUTDOWN_CLEAN = 0,
    PICO_HOST_SHUTDOWN_RETAINED,
} PicoHostShutdownResult;

PicoResult pico_host_init(PicoHost **out, Font *fonts, bool safe_mode);
PicoHostShutdownResult pico_host_free(PicoHost *host);
void pico_host_pump(PicoHost *host);

int pico_workspace_count(const PicoHost *host);
bool pico_workspace_info(const PicoHost *host, int index, PicoWorkspaceInfo *out);
PicoResult pico_workspace_open(PicoHost *host, const char *path, PicoWorkspaceId *out);
PicoResult pico_workspace_request_reload(PicoHost *host, PicoWorkspaceId id);
PicoResult pico_workspace_request_close(PicoHost *host, PicoWorkspaceId id);

PicoResult pico_main_agent_create(PicoHost *host, PicoWorkspaceId workspace_id,
                                  const PicoAgentCreateOptions *options,
                                  PicoAgentId *out);
PicoResult pico_agent_submit(PicoHost *host, PicoAgentId id,
                             const char *text, const char *parts_json);
PicoResult pico_agent_cancel(PicoHost *host, PicoAgentId id);
PicoResult pico_agent_force_cancel(PicoHost *host, PicoAgentId id);
PicoResult pico_agent_close(PicoHost *host, PicoAgentId id);
```

Rules:

- `pico_workspace_open` trims the input, resolves it with `realpath`, verifies it is a directory, and stores the canonical absolute path.
- Opening an already-open canonical path returns `PICO_ALREADY_OPEN` and returns the existing ID in `out`.
- Opening a workspace does not create a main agent.
- Closing the last main agent does not close the workspace.
- `pico_workspace_request_close` changes `OPEN` or `RELOADING` to `CLOSING`, rejects new turns and delegation, requests cancellation for every agent, and continues pumping until all callbacks and retired runtimes finish. It then destroys workspace instances and changes to `CLOSED` before removal from the host array.
- A worker callback that never returns leaves the workspace in `CLOSING`; other workspaces continue. Main-thread callbacks cannot be isolated: they must return promptly and a violation blocks the host pump.
- Only `pico_host_free` applies the process-wide bounded shutdown deadline and retained-shutdown behavior.
- Main-agent creation accepts only `PICO_AGENT_MAIN`; delegated creation remains private to workspace delegation code.
- Agent-targeted calls locate the workspace by scanning the host's bounded workspace/agent arrays through one helper, `PicoHost_FindAgent`. Do not add a second mutable lookup map in this refactor.
- Keep `PICO_MAX_AGENTS` as a per-workspace limit of 16. Add `PICO_MAX_WORKSPACES` = 8 and `PICO_MAX_TOTAL_AGENTS` = 32. Main-agent or subagent creation fails with `PICO_LIMIT` if either agent limit is reached.

The CLI startup path is a convenience sequence, not a special lifecycle:

1. `pico_host_init`
2. `pico_workspace_open(initial_path)`
3. `pico_main_agent_create`
4. Optional session resume on that explicit agent
5. Set the UI-selected agent ID

## 7. Selection and UI adapters

The host may keep one `selected_agent_id`, but only UI code may read it. Core agent, session, settings, extension, and workspace code must receive an explicit ID or pointer.

Replace current active-agent helpers as follows:

- Delete `PicoApp_ActiveAgent` and `PicoAgentManager_Active` from backend code.
- Add an internal `PicoHost_SelectedAgent` helper in the UI module only.
- `PicoHost_SelectedAgent` calls `PicoHost_FindAgent`; it does not own an independent pointer.
- Selection resets only host transcript selection/scroll state.
- Composer submission snapshots the selected ID once, then calls `pico_agent_submit` with that ID. A selection change during hooks cannot retarget the submission.
- Cancel/Esc snapshots and targets one selected agent ID.
- UI commands that change model, effort, session, or compaction explicitly target that snapshot.

Do not implement the future workspace-management UX in this refactor.

## 8. Settings contract

Split the current `PicoSettings`:

```c
typedef struct PicoHostPreferences {
    double font_scale;
    int chat_width;
    char disabled_host_extensions[PICO_MAX_DISABLED_EXTENSIONS][PICO_DISABLED_EXT_NAME];
    int disabled_host_extension_count;
} PicoHostPreferences;

typedef struct PicoWorkspaceSettings {
    char default_model[128];
    int context_limit_fallback;
    double compact_ratio;
    bool compact_enabled;
    bool resume_last;
    char disabled_extensions[PICO_MAX_DISABLED_EXTENSIONS][PICO_DISABLED_EXT_NAME];
    int disabled_extension_count;
} PicoWorkspaceSettings;
```

Rules:

- Host preferences come only from user-global settings/environment.
- `disabled_host_extensions` controls only host instances and is stored in user-global settings.
- `PicoWorkspaceSettings.disabled_extensions` controls only workspace instances in that workspace. It never disables the module's host instance or its instances in another workspace.
- A dual-scope module can therefore have its host instance enabled while one or more workspace instances are disabled, or the reverse.
- Each workspace loads a resolved model catalog and execution defaults from user-global configuration plus `<workspace>/.pico/settings.json`.
- Workspace settings are defaults for newly created main agents.
- Each agent owns its selected model, effort, context limit, and compaction selection after creation or replay.
- Changing workspace defaults does not mutate an existing agent's model or effort.
- `AGENTS.md` and `.pico/SYSTEM.md` are read for the target workspace when building each turn's instruction snapshot. A turn keeps the snapshot it started with.
- Settings writes use a unique temporary file, `fsync`, atomic rename, and containing-directory `fsync`. Protect user-global read/modify/write with one host mutex. Workspace setting writes use one mutex per workspace.
- Auth secrets remain host-global. Replace fixed `<auth>.tmp` with a unique temporary file before atomic rename.

## 9. Extension ABI 13

Set `PICO_EXT_ABI` to 13 and replace the ABI 12 descriptor. There is no compatibility loader.

```c
typedef int (*PicoHostExtInitFn)(PicoHost *host, void **state_out);
typedef void (*PicoHostExtShutdownFn)(PicoHost *host, void *state);
typedef void (*PicoHostExtFrameFn)(PicoHost *host, void *state, float dt);

typedef int (*PicoWorkspaceExtInitFn)(PicoWorkspace *workspace, void **state_out);
typedef void (*PicoWorkspaceExtShutdownFn)(PicoWorkspace *workspace, void *state);
typedef void (*PicoWorkspaceExtFrameFn)(PicoWorkspace *workspace, void *state, float dt);

typedef struct PicoExt {
    int abi;
    const char *name;
    const char *description;
    PicoHostExtInitFn host_init;
    PicoHostExtShutdownFn host_shutdown;
    PicoHostExtFrameFn host_on_frame;
    PicoWorkspaceExtInitFn workspace_init;
    PicoWorkspaceExtShutdownFn workspace_shutdown;
    PicoWorkspaceExtFrameFn workspace_on_frame;
} PicoExt;
```

Lifecycle rules:

- A builtin or user-global source may provide host callbacks, workspace callbacks, or both.
- A source under `<workspace>/.pico/extensions/` must set every host callback to `NULL`. If it does not, activation fails and the previous active generation remains in use.
- `*_init` returns zero on success and nonzero on failure.
- Before calling init, core sets `*state_out = NULL`.
- Registrations made during init are staged and are not visible until init succeeds.
- If init fails, staged registrations are discarded. If `*state_out` is non-NULL and shutdown exists, core calls shutdown exactly once.
- Shutdown is called exactly once for every successfully initialized instance, in reverse initialization order, after the instance is quiescent.
- `state` may be NULL for a stateless extension.
- Every registered callback receives the owning extension instance's `void *state` as its final parameter. Update every callback typedef and every `pico_add_*` implementation accordingly.
- Registration functions are valid only during the matching init callback. Host init may register only host registrations; workspace init may register only workspace registrations. Invalid-scope registration fails and appends a scoped warning.
- Extension mutable state must not use writable file-static variables. Constants and immutable lookup tables may remain static.

### 9.1 Registration scope matrix

| Registration/API | Host init | Workspace init |
|---|---:|---:|
| Global window views and overlays | yes | no |
| Contextual views shown for selected workspace | no | yes |
| Empty-chat views for selected agent | no | yes |
| Global commands/completers | yes | no |
| Workspace commands/completers | no | yes |
| Auth login/logout registration | yes | no |
| Tools and providers | no | yes |
| Before/after tool hooks | no | yes |
| LLM/context/tool-row hooks | no | yes |
| Agent lifecycle/message/submit hooks | no | yes |
| Host after-layout/after-render hooks | yes | no |
| Workspace `on_frame` | no | descriptor callback |
| Host `on_frame` | descriptor callback | no |

Use separate function names where current `pico_add_*` would be ambiguous, for example `pico_host_add_view` and `pico_workspace_add_view`. The context passed to init determines the registration owner; callers never supply another workspace ID while registering.

Workspace contextual views receive the workspace pointer and explicit selected agent ID. They are called only when the selected agent belongs to that workspace. Workspace `on_frame` runs for every `OPEN` or `RELOADING` workspace, selected or not; it must not draw with Clay or Raylib. Rendering remains a view callback.

### 9.2 Builtin scope split

Migrate builtins to these owners:

- Host instances: chat renderer, composer, footer shell, overlay presentation, extension manager UI, prompt UI, clipboard state, auth UI.
- Workspace instances: TODO state, shell tool, subagent tool, file index/completion, diff worker/model, execution commands, providers, tool ask records.
- Builtins with both responsibilities (`ask_user`, commands, provider/auth modules, diff/footer integration) must use separate host and workspace states connected only through core APIs and IDs. They must not share mutable static data.

## 10. Module and registration generations

Split the current plugin loader responsibilities:

- `app/plugin.c`: source discovery, compile cache, `dlopen`, descriptor validation, module-generation ownership, and source polling. Owned by `PicoHost`.
- `app/host_extensions.c`: host instance staging, activation, frame callbacks, and shutdown.
- `app/workspace_extensions.c`: workspace instance staging, registration-generation creation, per-workspace rollout, replay, and shutdown.

Required structures, with fields expanded as needed:

```c
typedef struct PicoModuleGeneration {
    char source[4096];
    char so_path[4096];
    time_t mtime;
    uint64_t generation;
    void *handle;
    PicoExt descriptor;
    int ref_count;
    bool builtin;
} PicoModuleGeneration;

typedef struct PicoExtensionInstance {
    PicoModuleGeneration *module;
    void *state;
    bool host_scoped;
    bool initialized;
} PicoExtensionInstance;

typedef struct PicoRegistrationGeneration {
    uint64_t id;
    int ref_count;
    /* immutable tools/providers/hooks/commands/views after publication */
} PicoRegistrationGeneration;
```

Rules:

- Never use `RTLD_GLOBAL`; load user modules with `RTLD_NOW | RTLD_LOCAL`.
- Compilation writes to a unique temporary output and atomically renames it to a generation-specific cache path.
- A compile or descriptor-validation failure leaves every workspace on its current working module generation.
- User-global source changes compile once. Each workspace independently stages and publishes the new module generation when that workspace is quiescent.
- Old and new module generations may coexist.
- Workspace-local source changes affect only their owning workspace.
- A workspace rebuild stages a complete registration generation. Publish it atomically only after every required instance initializes successfully.
- On staging failure, shut down staged instances in reverse order and keep the previous complete registration generation active.
- Reload rejects new external turns and delegation only in the affected workspace. Other workspaces continue accepting work.
- An accepted agent submission retains one registration generation for the entire turn. The same generation is used for every LLM request, compaction, tool call, ask, hook, and follow-up until the turn reaches terminal idle, cancel, or error and every event, pending call, ask, delegation reference, and callback from that turn is gone. Force-cancelled retired runtimes retain their turn generation until they actually quiesce. The next accepted turn may retain a newer generation.
- A module generation may be `dlclose`d only when its reference count reaches zero and it is no longer the desired generation for any source.
- Host extension reload occurs between frames. Host extensions cannot register worker callbacks, so only active host callback depth must reach zero before replacement.
- A dual-scope user-global source has independent active-generation slots: one host slot and one slot per workspace. Host activation success/failure does not publish or roll back any workspace slot, and workspace activation success/failure does not publish or roll back the host slot. Each slot keeps its previous generation on failure.
- Extension listing reports one record per active slot. Use `PICO_EXTENSION_HOST` with workspace ID zero for a host slot and `PICO_EXTENSION_WORKSPACE` with an explicit workspace ID for a workspace slot. Each copied record includes source, name, scope, workspace ID, enabled, desired module-generation ID, active module-generation ID, and the latest scoped error. Do not collapse a dual-scope source into one ambiguous enabled/active row.

## 11. Workspace lifecycle and scheduler

`pico_host_pump` performs one bounded round-robin pass:

1. Pump host posts/process services.
2. Starting after the workspace pumped first on the previous frame, call `PicoWorkspace_Pump` once for each non-closed workspace.
3. Each workspace pump drains at most 256 queued runtime events total in that call. Preserve remaining events for the next frame.
4. Process at most one new delegation request per workspace per pass.
5. Advance workspace reload/close state after its event pass.
6. Run host extension frame callbacks.
7. Render only the selected workspace's contextual views through the normal UI frame.

Do not hold a host or workspace mutex while invoking an extension callback. Worker-to-main queues retain their existing mutex/condition discipline. Main-thread fields do not need additional mutexes.

State transitions:

```text
OPEN --request reload--> RELOADING --publish/failed rollback--> OPEN
OPEN --request close--> CLOSING --quiescent destroy--> CLOSED
RELOADING --request close--> CLOSING
```

`CLOSING` has no transition back to `OPEN`. If close is requested during a staged reload, abort the rollout before entering `CLOSING`: prevent publication, shut down every staged instance in reverse initialization order, discard staged registrations, and release every staged registration/module reference. The previously active generation remains retained until normal close quiescence releases it.

All host-frame, workspace-frame, hook, apply, command, completer, and view callbacks execute on the main thread and must return promptly. They may start asynchronous work but must not join or wait for it. Only non-returning worker callbacks are covered by per-workspace stuck-callback isolation.

Ask handling:

- Ask IDs are host-allocated.
- Each workspace stores its own pending asks.
- `pico_host_pending_ask` returns the oldest live ask across workspaces by host ID.
- Answer routing uses ask ID, then validates workspace, agent ID, and runtime generation.
- Closing a workspace cancels all of its pending asks.

Mailbox handling:

- Storage remains workspace-owned.
- The key is `(agent_id, runtime_generation, name)`, so two agents may use the same name without collision.
- Main-thread lookup requires an explicit agent ID and name.
- Closing, force-cancel, or generation retirement drops matching unpublished and published entries.

## 12. Session and workspace behavior

- Session directory resolution accepts `const PicoWorkspace *`; it never accepts `PicoHost *` or reads UI selection.
- Session reservation remains workspace-local because canonical paths are unique within the host.
- Existing inter-process session locks remain required.
- A session replay must verify that its stored kind is compatible with `PICO_AGENT_MAIN` or `PICO_AGENT_SUBAGENT` and that it is opened in the workspace whose session directory contains it.
- Main agents in one workspace may run concurrently but may not reserve the same session path.
- Workspace close preserves durable session files and releases reservations only after the owning agent/runtime is destroyed.
- Subagent continuation resolves profiles and tools from the parent's workspace's current registration/profile generation.
- File mentions, shell cwd, media persistence, prompt instructions, git diff, and settings paths all receive the target workspace explicitly.

## 13. Implementation phases

Do not enable multiple workspaces early. The executable and full test suite must build and pass at the end of every phase. Commit each phase separately so regressions can be bisected.

### Phase 0 — Record the baseline

Status: complete (2026-08-28)

Owner files: none.

Tasks:

1. Run the existing debug build and full test suite.
2. Save the configure, build, and test commands plus any pre-existing failures in the Phase 0 pull-request description.
3. Inventory current mutable globals and every `app->workspace`/active-agent call site so later phase reviews can confirm removal.

Acceptance:

- No production or test code changes in this phase.
- The existing debug build and test result are recorded before refactoring begins.

#### Phase 0 recorded baseline

This machine has no `cmake` on the default PATH. The flake toolchain was used:

```bash
nix develop -c bash -lc 'cmake -S app --preset debug && cmake --build app/build/debug && ctest --test-dir app/build/debug --output-on-failure'
```

Inside an already-entered `nix develop` shell, the Section 15 commands are:

```bash
cmake -S app --preset debug
cmake --build app/build/debug
ctest --test-dir app/build/debug --output-on-failure
```

Results (2026-08-28, debug preset, `app/build/debug`):

- Configure: success.
- Build: success (`ninja: no work to do`; the existing debug tree was current). Example plugin smoke objects (`*_ext.so`) are `pico` POST_BUILD custom commands and were already present.
- Tests: 24/24 passed, 0 failed, 1.61s real.
- Pre-existing failures: none.

Registered debug tests: `pico_json_tests`, `pico_http_capture_tests`, `pico_http_capture_release_tests`, `pico_text_range_tests`, `pico_markdown_tests`, `pico_diff_lines_tests`, `pico_scrollbar_tests`, `pico_transcript_virtual_tests`, `pico_wrapped_text_tests`, `pico_responses_tests`, `pico_completions_tests`, `pico_xai_auth_tests`, `pico_todo_tests`, `pico_ask_user_tests`, `pico_agent_behavior_tests` (includes `agent_manager_test.c`, `subagent_config_test.c`, `subagent_test.c`), `pico_session_usage_tests`, `pico_files_tests`, `pico_composer_tests`, `pico_settings_agent_tests`, `pico_clay_capacity_tests`, `pico_font_scale_tests`, `pico_chat_width_tests`, `pico_ui_tests`, `pico_docs_path_tests`.

Architecture snapshot before Phase 1:

- Public owner is `PicoApp`. `PICO_EXT_ABI` is 12. Extension callbacks take `PicoApp *` and have no instance `state`.
- `PicoApp_Init` stores one mutable workspace path, creates `PicoAgentManager`, creates one `PICO_AGENT_NORMAL` agent, then loads plugins.
- `curl_global_init` runs in `PicoAgentManager_Create`. Agent IDs come from `static PicoAgentId next_id` in `PicoAgent_Create`.
- Workspace replacement uses `app->pending_workspace` / `app->workspace_change_queued`.

#### Inventory: `app->workspace` call sites

Production reads/writes of `app->workspace` (and the queued-change flags that gate them):

| File | Lines | Role |
|---|---|---|
| `app/app.c` | 873, 877, 928, 1046, 1057, 1069, 1090, 1101, 1139, 1141, 1183 | Init path, `/cd` resolve/queue/apply, reload barrier |
| `app/plugin.c` | 174, 569, 622 | Workspace extension dir; reload vs workspace-change barrier |
| `app/agent.c` | 2164, 2721 | Session path under workspace; copy into worker context |
| `app/session.c` | 60, 611 | Session lock encoding; persist cwd |
| `app/settings.c` | 486, 488, 1437, 1439, 1443, 1470, 1471, 1484 | `.pico/settings.json`, `.pico/SYSTEM.md`, `AGENTS.md` |
| `app/builtins/files.c` | 128, 130, 396 | File index walk; mention expansion |
| `app/builtins/diff.c` | 590, 637, 639, 645 | Diff worker workspace capture/compare |
| `app/builtins/footer.c` | 601, 786 | CWD chip; folder picker start |
| `app/builtins/commands.c` | 542, 553 | `/cd` listing relative to current workspace |
| `app/builtins/composer.c` | 754 | Paste-temp directory |

Tests: `app/tests/files_test.c:138` writes `app->workspace`.

Related mutable fields on `PicoApp`: `workspace[4096]`, `pending_workspace[4096]`, `workspace_change_queued`.

#### Inventory: active-agent call sites

Helpers: `PicoApp_ActiveAgent` / `PicoApp_ActiveAgentConst` in `app/agent_internal.h` forward to `PicoAgentManager_Active` / `PicoAgentManager_ActiveConst` (`app/agent_manager.c:79,84`). No other production callers of `PicoAgentManager_Active*` besides those inlines.

Production `PicoApp_ActiveAgent*` uses (backend and UI mixed):

| File | Approx. call sites | Typical use |
|---|---|---|
| `app/app.c` | 11 | Submit/cancel/modal, init session, debug transcript APIs, frame pump |
| `app/main.c` | 1 | Window title |
| `app/agent_manager.c` | 1 | Selection handoff when creating/selecting an agent |
| `app/builtins/footer.c` | 22 | Model/effort menus, busy/error/usage chips, Esc/cancel |
| `app/builtins/chat.c` | 14 | Transcript, tool-row, inspect, empty state |
| `app/builtins/commands.c` | 8 | `/model`, `/effort`, `/compact`, `/new`, `/resume` |
| `app/builtins/overlay.c` | 8 | Error overlay, dismiss agent error |
| `app/builtins/prompt.c` | 1 | Instruction preview |
| `app/builtins/composer.c` | 1 | Vision/model for attachments |
| `app/builtins/files.c` | 1 | Vision/model for mention expansion |

Tests: heavy use in `app/tests/agent_behavior_test.c`; also `subagent_test.c`, `subagent_config_test.c`. These tests treat the manager's selected agent as the subject.

#### Inventory: mutable process/file-static state

Core / loader (must leave process-global ID/plugin ownership):

- `app/agent.c`: `g_ask_id_mu`, `g_ask_next_id`; `static PicoAgentId next_id` in `PicoAgent_Create`
- `app/plugin.c`: `g_plugins[]`, `g_plugin_count`, `g_last_poll`
- `app/app.c`: `g_pico_process_retired`
- `app/docs_path.c`: `g_app_dir`
- `app/http_capture.c`: `g_capture_mu`, `g_capture_sequence`
- `app/theme.c`: font tables/scale, Clay reinit/scroll/capacity flags
- `app/chat_sel.c`: `s_msgs`, `s_hits`, selection cursors
- `app/scrollbar.c`: `s_dragging`
- `app/richtext.c`: measure callback and `s_link_serial`

Builtin mutable globals (Phase 4/5 extract into host or workspace instance state):

- `chat.c`: `g_app`, inspect stack, transcript virtual caches, think-label arena
- `composer.c`: `g_app`, attachments, preview texture, clipboard child process
- `complete.c`: `g_complete`
- `footer.c`: `g_app`, menus, CWD/status chip buffers
- `overlay.c`: notify toast, ask UI
- `prompt.c`: `g_app`, prompt text/spans
- `extensions.c`: `g_app`, manager-UI open state
- `files.c`: `g_files`, `g_file_count`, `g_scanned`, `g_token_id`
- `diff.c`: worker thread/lock/cond, `g_pending`/`g_model`, `g_workspace`, `g_app`
- `todo.c`: `g_states[PICO_MAX_AGENTS]`, layout cache
- `ask_user.c`: `g_ui`
- `openai.c` / `hyper.c` / `xai.c`: refresh mutex/cond, in-flight refresh owner, `g_login`

`shell.c` and `subagent.c` have no writable file-static state. `auth.c` / `settings.c` keep mutable data on `PicoApp` / `PicoAuthStore`, not file-static tables.

`rg "app->workspace|PicoApp_ActiveAgent|PicoAgentManager_Active|g_plugins|static PicoAgentId next_id" app --glob '*.[ch]'` currently matches all of the production sites above. That command must have no production matches after Phase 6.

### Phase 1 — Atomic host/workspace and ABI 13 foundation with one workspace

Status: complete (2026-08-28)

This phase is intentionally one atomic integration phase. `PicoApp` cannot be removed while ABI 12 callbacks still require `PicoApp *`, and this project does not permit a compatibility facade. Complete the ordered steps below on one integration branch and merge only after the whole phase builds and passes.

Owner files:

- New `app/include/pico/host.h`, `app/include/pico/workspace.h`
- New `app/host_internal.h`, `app/workspace_internal.h`
- `app/include/pico/app.h`, `app/include/pico/plugin.h`, `app/main.c`, `app/app.c`, `app/plugin.c`
- Every builtin, every file in `examples/`, extension compile-smoke tests, new `app/tests/host_workspace_test.c`, and `app/CMakeLists.txt`

Tasks, in required order:

1. Add opaque `PicoHost` and `PicoWorkspace` declarations plus the ABI 13 descriptor and callback typedefs from Section 9. Do not remove ABI 12 call sites yet in this unmerged branch step.
2. Add host/workspace extension-instance records, callback `state` plumbing, and staged registration activation. The active registration arrays may still be mutable after publication in this phase; immutable retained registration generations arrive in Phase 6.
3. Convert all builtin and example descriptors/callback signatures to ABI 13. Host and workspace init functions must register through their matching context. State may temporarily be NULL for builtins whose mutable globals are extracted in Phase 4.
4. Convert every remaining ABI 12 invocation and registration call site. Confirm no code constructs or accepts the ABI 12 descriptor.
5. Move the current public `PicoApp` fields into internal host UI state and workspace execution state according to the ownership table, then remove `PicoApp` entirely. Do not create a `PicoApp` typedef, facade, alias, or adapter.
6. Initialize exactly one workspace through `pico_workspace_open` during startup.
7. Allocate workspace and agent IDs from host counters.
8. Move curl initialization from manager creation to host initialization.
9. Keep behavior single-workspace; do not add a second workspace yet.
10. Add canonical open/duplicate tests, including a symlink alias, plus compile-time assertions for zero-invalid IDs and the limits.
11. Compile every example against ABI 13 and update the extension smoke tests.

Acceptance:

- `PicoApp` and ABI 12 are absent from public headers and production call sites.
- Host/workspace extension callbacks receive the correct opaque owner and state argument.
- Startup, submit, cancel, session resume, and shutdown behavior remain observable-equivalent.
- No process-static `next_id` remains in `agent.c`.
- The executable, all examples, and the full tests build together; no intermediate compatibility layer is merged.

#### Phase 1 recorded result

```bash
nix develop -c bash -lc 'cmake -S app --preset debug && cmake --build app/build/debug && ctest --test-dir app/build/debug --output-on-failure'
```

25/25 tests passed. Examples compile as ABI 13 smoke tests. `PicoApp` and ABI 12 are gone from public headers and production sources. One live workspace; a second distinct open returns `PICO_LIMIT`.

### Phase 2 — Remove ambient active/workspace backend access

Status: complete (2026-08-28)

Owner files:

- `app/app.c`, `app/agent.c`, `app/session.c`, `app/settings.c`
- `app/builtins/commands.c`, `composer.c`, `files.c`, `footer.c`, `shell.c`

Tasks:

1. Implement `PicoHost_FindWorkspace`, `PicoHost_FindAgent`, and UI-only `PicoHost_SelectedAgent`.
2. Convert submit/cancel/model/effort/compact/session APIs to explicit agent IDs.
3. Snapshot selection once at each UI action boundary.
4. Replace every `app->workspace` read with an explicit `PicoWorkspace *` or immutable path from an agent context.
5. Replace every backend `PicoApp_ActiveAgent` use.
6. Make worker contexts hold workspace ID/path and the registration generation used by the turn.
7. Add a CI check command documented in this file: `rg "app->workspace|PicoApp_ActiveAgent|PicoAgentManager_Active" app --glob '*.[ch]'` must return no backend matches. UI-only selected helpers must use their new names.

Acceptance:

- Backend tests can submit to two main agents in the single workspace without changing UI selection.
- Selection changes cannot retarget an in-progress submit hook or cancel action.
- The forbidden-pattern search has no backend matches.

#### Phase 2 recorded result

```bash
nix develop -c bash -lc 'cmake -S app --preset debug && cmake --build app/build/debug && ctest --test-dir app/build/debug --output-on-failure'
```

25/25 tests passed. `PicoHost_SelectedAgent` is UI-only. `pico_agent_submit` to a non-selected main agent leaves selection unchanged. A `BEFORE_SUBMIT` hook that selects another agent cannot retarget the snapshotted submit.

```bash
rg "app->workspace|PicoApp_ActiveAgent|PicoAgentManager_Active" app --glob '*.[ch]'
```

No `PicoApp_ActiveAgent` or `PicoAgentManager_Active` matches. Remaining `app->workspace*` hits are the host `workspaces` array and `workspace_change_queued` (Phase 8), not the old ambient workspace path field.

### Phase 3 — Fold `PicoAgentManager` into `PicoWorkspace`

Status: complete (2026-08-28)

Owner files:

- Rename `app/agent_manager.c` to `app/workspace.c`
- Rename `app/agent_manager.h` to `app/workspace_internal.h` and merge with the Phase 1 file
- `app/agent.c`, `app/agent_internal.h`, `app/CMakeLists.txt`

Tasks:

1. Move manager arrays, delegation queues, retired runtimes, reservations, profiles, mailboxes, and lifecycle locks directly onto `PicoWorkspace`.
2. Rename manager functions to `PicoWorkspace_*`.
3. Replace `agent->manager` with `agent->workspace`.
4. Remove manager creation/destruction as a separate allocation.
5. Remove `active_id` from workspace state.
6. Keep main/subagent creation, delegation, cancellation, and session behavior unchanged.

Acceptance:

- No `PicoAgentManager` type or `agent_manager.*` file remains.
- Existing concurrency and subagent tests pass.
- A workspace may contain multiple main agents and their independent child trees.

#### Phase 3 recorded result

```bash
nix develop -c bash -lc 'cmake -S app --preset debug && cmake --build app/build/debug && ctest --test-dir app/build/debug --output-on-failure'
```

25/25 tests passed. `PicoAgentManager` and `agent_manager.*` are gone. `PicoWorkspace` directly owns agent arrays, child trees, delegation queues, session reservations, profiles, and lifecycle locks.

### Phase 4 — Split settings and workspace-owned caches

Status: complete (2026-08-28)

Owner files:

- `app/settings.c/.h`, `app/auth.c/.h`
- `app/builtins/files.c`, `diff.c`, `todo.c`, `chat.c`, `composer.c`, `footer.c`, `prompt.c`, `ask_user.c`

Tasks:

1. Implement `PicoHostPreferences` and `PicoWorkspaceSettings` exactly as specified.
2. Give each workspace its own resolved model catalog and instructions/profile snapshots.
3. Move mutable builtin globals into explicit host/workspace state structs and attach them to the ABI 13 instances introduced in Phase 1. Phase 5 completes mixed-builtin splitting and strict scope enforcement.
4. Make settings and auth writes atomic and locked as specified.
5. Ensure file completion and diff workers use immutable workspace paths and have independent stop/join lifecycles.
6. Leave only immutable constants, process library locks, and rendering resources as file-static state.

Acceptance:

- Two state structs instantiated in tests do not share file lists, diff results, TODOs, prompt buffers, asks, or composer attachments.
- Model changes on one agent do not mutate another agent or workspace defaults.
- `rg '^static .*g_|^static .*s_' app/builtins --glob '*.[ch]'` is manually reviewed; every writable result is documented as immutable/process service or removed.

#### Phase 4 recorded result

```bash
nix develop -c bash -lc 'cmake -S app --preset debug && cmake --build app/build/debug && ctest --test-dir app/build/debug --output-on-failure'
nix develop -c bash -lc 'cmake -S app --preset release && cmake --build app/build/release && ctest --test-dir app/build/release --output-on-failure'
```

25/25 tests passed in Debug and Release presets. `PicoHostPreferences` and `PicoWorkspaceSettings` are independent and written atomically with parent directory sync. Model catalogs and selections are per-workspace. Background diff worker is refcounted with generation cancellation (`DiffWorkerCtx`). `diff`, `files`, and `todos` are workspace-scoped extensions. Host and workspace plugin slot tables provide instance-specific state lookup. Isolation tests in `app/tests/host_workspace_test.c` verify per-workspace model isolation, instance plugin isolation, and preferences persistence.

### Phase 5 — Enforce scoped registrations and complete instance isolation

Status: complete (2026-08-28)

Owner files:

- Public callback headers and registration implementations
- `app/app.c`, `app/plugin.c`
- Every builtin and every file in `examples/`
- Extension tests and compile-smoke targets

Tasks:

1. Replace every temporary NULL/static builtin state from Phase 1 with the host/workspace state structs created in Phase 4.
2. Split and enforce registration storage/functions according to the Section 9.1 scope matrix.
3. Split mixed-responsibility builtins into separate host and workspace instances connected only through core IDs/APIs.
4. Stage registrations during init; publish on success and discard on failure according to Section 9.
5. Reject workspace-local host callbacks and invalid-scope registrations with clear warnings.
6. Add at least one host UI example and one stateful workspace tool example.
7. Test distinct instance state, independent host/workspace disable settings, init rollback, and reverse-order shutdown.

Acceptance:

- No callback depends on mutable extension file-static state.
- Two workspace instances of the same module receive distinct state pointers.
- Host registrations cannot add tools/providers; workspace registrations cannot add global UI/auth.
- Disabling one scope/instance does not disable another scope/instance.
- All examples compile with ABI 13.

#### Phase 5 recorded result

```bash
nix develop -c bash -lc 'cmake -S app --preset debug && cmake --build app/build/debug && ctest --test-dir app/build/debug --output-on-failure'
nix develop -c bash -lc 'cmake -S app --preset release && cmake --build app/build/release && ctest --test-dir app/build/release --output-on-failure'
```

25/25 tests passed in Debug and Release presets. Scoped registrations are strictly enforced with isolated staging (`PicoHostStaging`) and rollback on failed init. Workspace-scoped registries (`views`, `empty_views`, `hooks`, `tool_before_hooks`, `tool_after_hooks`, `llm_hooks`, `context_hooks`, `tool_row_hooks`, `tools`, `commands`, `completers`, `providers`, `workspace_plugins`) live directly on `PicoWorkspace`. Dual-scope builtins (`ask_user`, `commands`, `diff`, `todo`, `openai`, `hyper`, `xai`) are cleanly split into isolated host and workspace instances with no mutable file-static state. Provider auth token refresh uses `pico_auth_begin_refresh_ctx` / `pico_auth_end_refresh_ctx` with condvar coordination. Workspace-local extensions declaring host callbacks are rejected. Decoupled host and workspace disable settings. All 12 example extensions compile as ABI 13 smoke tests including the new thread-safe stateful `counter_tool.c`. `docs/extend/` synchronized with public API and contracts.

### Phase 6 — Implement module and registration generations

Status: complete.

Owner files:

- `app/plugin.c`
- `app/host_extensions.c`, `app/workspace_extensions.c`
- `app/agent.c`, `app/workspace.c`

Tasks:

1. Move `g_plugins`, plugin count, and poll time into host-owned module storage.
2. Implement generation-specific compilation and `RTLD_LOCAL` loading.
3. Implement staged host/workspace instance activation.
4. Implement immutable registration generation retain/release.
5. Make runtime, pending tool calls, asks, events, and retired runtimes retain their generation.
6. Implement per-workspace rollout for user-global and workspace-local changes.
7. Preserve the old active generation after compile, load, validation, or init failure.
8. Add reference-count assertions in debug builds.

Verification:

```bash
nix develop -c bash -lc 'cmake -S app --preset debug && cmake --build app/build/debug && ctest --test-dir app/build/debug --output-on-failure'
nix develop -c bash -lc 'cmake -S app --preset release && cmake --build app/build/release && ctest --test-dir app/build/release --output-on-failure'
```

25/25 tests passed in Debug and Release presets. Module and registration generations are fully implemented: host-owned module storage (`PicoModuleGeneration`), generation-specific compile with atomic cache rename and `RTLD_LOCAL`, isolated host/workspace staging and rollback (`PicoHostExtensions_Activate`, `PicoWorkspaceExtensions_Activate`), reference-counted immutable registration generations (`PicoRegistrationGeneration`), turn generation retention through worker runtimes and retired runtimes until joined/freed, per-workspace rollout and workspace-local source discovery, independent publication and rollback for dual-scope modules, `dlclose` on last generation reference release, and scoped extension listing (`PicoPlugins_Count`, `PicoPlugins_Get`, `PicoPlugins_SetEnabled`) returning separate `PICO_EXTENSION_HOST` and `PICO_EXTENSION_WORKSPACE` records with desired/active generations, workspace IDs, and scoped error tracking. Verified zero forbidden legacy global references.

### Phase 7 — Enable multiple workspace lifecycles and fair pumping

Status: complete (2026-08-29).

Owner files:

- `app/host.c`, `app/workspace.c`, `app/main.c`
- Ask/mailbox routing modules
- `app/tests/host_workspace_test.c`

Tasks:

1. Remove the temporary one-workspace restriction.
2. Enforce canonical uniqueness and host/workspace/agent limits.
3. Implement the lifecycle states and transitions in Section 11.
4. Implement bounded round-robin pumping.
5. Implement global oldest-ask selection and explicit answer routing.
6. Change mailbox keys and lookup APIs as specified.
7. Keep only one selected agent in the existing UI.
8. Ensure inactive workspaces continue execution and workspace `on_frame` without rendering.

Acceptance:

- The multi-workspace test matrix in Section 14 passes.
- Reloading or closing one workspace does not block turns in another.
- A stuck callback leaves only its workspace in `CLOSING`.
- The host remains responsive under event load from two workspaces.

Verification:

```bash
nix develop -c bash -lc 'cmake -S app --preset debug && cmake --build app/build/debug && ctest --test-dir app/build/debug --output-on-failure'
nix develop -c bash -lc 'cmake -S app --preset release && cmake --build app/build/release && ctest --test-dir app/build/release --output-on-failure'
```

25/25 tests passed in Debug and Release presets. Multiple workspace lifecycles are fully enabled: `PICO_MAX_WORKSPACES` (8), `PICO_MAX_AGENTS` (16 per workspace), and `PICO_MAX_TOTAL_AGENTS` (32 host-wide) limits enforced; canonical path uniqueness verified via `pico_workspace_open`; workspace extension/profile activation on open wired via `PicoPlugins_InitWorkspace`; safe workspace close quiescence without use-after-free; reload staging abort on close request; isolated workspace-specific agent-destroy hooks (`PicoWorkspace_RunHooks`); closing main agents drains only their descendant subagent trees; workspace lifecycle states (`OPEN`, `RELOADING`, `CLOSING`, `CLOSED`) and non-blocking background transitions implemented; fair bounded round-robin pumping with `pump_rr_index` and 256-event draining budget per agent pass implemented (`PicoAgent_PumpBounded`); frame callbacks (`PicoWorkspaceExtensions_OnFrame`, `PicoHostExtensions_OnFrame`) executed across all open workspaces and host in `pico_host_pump`; global monotonic ask ID allocation and oldest-ask routing (`pico_tool_pending_ask`, `pico_tool_answer`) implemented; strict UI mailbox keying by `(agent_id, runtime_generation, name)` and explicit lookup (`pico_agent_ui_latest`, `pico_agent_ui_clear`) implemented without selection fallback; closing last main agent leaves workspace open with 0 agents; multi-workspace test matrix validated across all 22 observable behavior rules without synthetic test hooks.

### Phase 8 — Remove obsolete workspace-switch behavior

Owner files:

- `app/app.c`, `app/builtins/commands.c`, `app/main.c`
- Related tests

Tasks:

1. Delete mutable `pending_workspace`, queued whole-app replacement, and manager adoption code.
2. Implement `/cd` temporarily as: canonicalize/open target workspace, create a new main agent if none exists there, select a main agent there, and leave the old workspace open.
3. Keep explicit workspace close available only through backend API until UX is designed.
4. Ensure `/reload` targets the selected agent's workspace. Add a separate internal host-reload action for host extensions.
5. Remove all whole-process reload barriers except host-extension replacement and final shutdown.

Acceptance:

- `/cd` never destroys or pauses the old workspace.
- Returning to an already-open canonical path reuses its workspace.
- Existing work continues after selection moves elsewhere.

### Phase 9 — Documentation and cleanup

Owner files:

- `docs/extend/` all affected topics
- `docs/subagents.md`, `README.md`, examples
- Public headers and source comments

Tasks:

1. Rewrite extension anatomy and contracts for host/workspace instances, state, generations, and scope restrictions.
2. Update agents, hooks, tools, views, commands, completers, providers, auth, context, and contracts pages.
3. Document workspace lifecycle, canonical uniqueness, explicit agent targeting, reload rollout, closure, and limits.
4. Add host/workspace topics to `docs/extend/README.md` and `/docs` topic list.
5. Update every example to match the same contracts.
6. Remove dead compatibility names, comments, and tests.

Acceptance:

- Public headers, docs, builtins, and examples describe one consistent ABI.
- No documentation claims `/cd` replaces all agents or reload is globally blocked by workspace work.
- Full debug and release builds pass.

## 14. Required multi-workspace test matrix

Add behavior tests for each numbered rule. Use two temporary canonical workspaces, A and B. Tests must assert observable behavior, not private struct layout.

1. Open A and B; opening a symlink alias of A returns `PICO_ALREADY_OPEN` and A's ID.
2. Create one main agent in each workspace and prove provider callbacks overlap and complete in reverse order.
3. Give A and B different `AGENTS.md` and `.pico/SYSTEM.md`; each provider receives only its workspace instructions.
4. Register the same tool name from different workspace-local extensions; each agent executes its workspace implementation.
5. Give A and B different models/settings; changing A does not affect B or existing agents.
6. File mentions, shell cwd, media paths, diff state, and sessions resolve under the target workspace.
7. Cancel or force-cancel A; B remains busy and completes normally.
8. Delegate a subagent from A; child context/path/session/profile all belong to A. Cross-workspace parent IDs are rejected.
9. Post the same mailbox name from A and B and from two agents in A; explicit lookup returns each isolated value.
10. Queue asks in both workspaces; host returns the lower host ask ID first and answers the correct runtime.
11. Reload A's local extension while B accepts and completes a new turn.
12. Update a user-global extension; let B adopt N+1 while A remains busy on N, then let A adopt N+1 after quiescence.
13. Make N+1 fail compilation and separately fail init; both workspaces retain N.
14. Request close of A; new A work is rejected, A asks cancel, B continues, and A disappears only after quiescence.
15. Block an A worker-side provider or tool callback with a test barrier; A remains `CLOSING`, B continues, then release A and verify closure. Do not block a main-thread hook/view/frame callback in this test.
16. Close a main agent with children; only its delegation tree is cancelled/drained. Another main agent in A survives.
17. Close the last main agent; A remains open with zero agents until explicitly closed.
18. Delete A's directory after opening; existing identity remains, filesystem operations fail against A, and Pico does not retarget to another path.
19. Exhaust per-workspace and host total-agent limits; creation returns `PICO_LIMIT` without partial publication.
20. Host shutdown drains all workspaces and returns retained only when the process deadline expires.
21. Stale agent/workspace/ask IDs after closure return not-found and cannot target newly created objects.
22. Pump heavy event streams in A and a short completion in B; B completes without waiting for A's entire queue to drain.

## 15. Verification commands

Run these from the repository root at the end of every phase:

```bash
cmake -S app --preset debug
cmake --build app/build/debug
ctest --test-dir app/build/debug --output-on-failure
```

Run these before merging Phases 5–9:

```bash
cmake -S app --preset release
cmake --build app/build/release
ctest --test-dir app/build/release --output-on-failure
```

Also run and inspect:

```bash
rg "app->workspace|PicoApp_ActiveAgent|PicoAgentManager_Active|g_plugins|static PicoAgentId next_id" app --glob '*.[ch]'
rg '^static .*g_|^static .*s_' app/builtins --glob '*.[ch]'
```

The first command must have no production matches after Phase 6. The second is a review list, not an automatic failure: every writable static must be removed or explicitly justified as immutable/process-wide in a code comment.

Use `lsp_diagnostics` on all changed C/header files when available. Warnings are errors in CMake targets.

## 16. Team execution rules

- Assign one integration owner for `host`, `workspace`, and public-header changes. These files are not split across developers in the same phase.
- Do phases in order. Phase 4 builtin state extraction may be divided by builtin file after the state interfaces are merged. Phase 5 builtin ABI conversion may use the same file ownership.
- A developer changing a public callback or lifecycle contract must update the matching `docs/extend/` page and example in the same pull request.
- Do not merge a phase with skipped tests, temporary compatibility aliases, duplicate state owners, or TODO behavior branches.
- Tests must protect the observable rules in Section 14; do not expose private fields solely for tests.
- Review every lifecycle change specifically for callback lifetime, cancellation, retained runtime, shutdown order, and `dlclose` safety.
- Keep commits small within a phase: types/ownership, call-site migration, tests, then docs. Do not combine unrelated UI work.

## 17. Final definition of done

The refactor is done only when all statements below are true:

- One host runs at least two workspaces and two main agents concurrently.
- Same-workspace main agents share one workspace runtime but retain independent agent/session state.
- Different workspaces have isolated settings, instructions, registrations, extension state, caches, sessions, asks, mailboxes, reload, and closure.
- No backend operation uses UI selection or ambient workspace state.
- No workspace-dependent mutable process/file-static state remains.
- Extension ABI 13 supports explicit host and workspace instances with callback userdata.
- Per-workspace module-generation rollout is safe while old callbacks remain active.
- A stuck worker callback keeps only its workspace closing and cannot stop unrelated agents; main-thread callbacks are documented and tested as prompt-return contract code.
- `/cd` selects/opens rather than replaces the process execution host.
- Every required test and verification command passes in debug and release.
- `docs/extend/`, public headers, builtins, and examples describe the implemented behavior exactly.
