# Pico Concurrent Agents and Subagents Plan

## Scope

Implement the infrastructure for:

1. Extracting a first-class `PicoAgent` from `PicoApp`.
2. Making the public extension APIs agent-aware.
3. Adding a `PicoAgentManager` capable of running multiple full agents concurrently.
4. Discovering named subagent profiles from user configuration.
5. Adding synchronous subagent delegation with optional continuation of a previous child session.
6. Hardening cancellation, reload, persistence, shutdown, and extension contracts.

A sidebar is explicitly out of scope. This work will expose enumeration, selection, and lifecycle APIs so a sidebar can be implemented independently afterward.

## Minimal design principles

- Keep one `PicoApp` per process and host multiple agents inside it.
- Use one worker thread per live agent; do not add a thread pool.
- Keep each individual agent sequential: model, tools, then model again.
- Do not parallelize tool calls within one agent.
- Use one JSONL file per logical session and never interleave agents in a session file.
- Share host services such as plugins, providers, model catalog, and authentication rather than cloning them.
- Use fixed limits and controlled errors instead of a general scheduler.
- Use a full reload quiescence barrier rather than plugin generations.
- Do not add a generic extension-state framework in this work.

---

# 1. Core ownership model

```text
PicoApp
├── workspace and global UI state
├── plugin/tool/provider registrations
├── authentication store
├── immutable model catalog and defaults
└── PicoAgentManager *
    ├── PicoAgent *
    │   ├── transcript and provider history
    │   ├── runtime and worker thread
    │   ├── session persistence
    │   ├── selected model, effort, and compaction config
    │   ├── usage, error, and activity state
    │   ├── kind, purpose, profile, parent, and depth
    │   └── effective tool policy
    ├── session writer reservations
    ├── pending asks and delegation jobs
    └── retired runtimes
```

## Identity

Use separate in-memory and durable identities:

- `PicoAgentId`: process-lifetime ID for one in-memory `PicoAgent` instance.
- Session ID: durable ID for one JSONL conversation.
- Runtime generation: identifies one worker generation within an agent.

Continuing a previous subagent session creates a new `PicoAgentId` but keeps the session ID. Force-cancelling an agent replaces its runtime and increments its runtime generation.

Worker contexts bind to an agent ID and runtime generation. An abandoned worker must never be able to ask, cancel, delegate, log, or bind a child process against the replacement generation.

## Initial limits

Use fixed limits consistent with Pico's existing design:

- `PICO_MAX_AGENTS`: 16 live normal agents and subagents combined.
- `PICO_MAX_SUBAGENT_PROFILES`: 32 discovered profiles.
- `PICO_MAX_DELEGATION_DEPTH`: 4, with a normal root agent at depth 0.
- `PICO_MAX_RETIRED_RUNTIMES`: 16.
- Tool names per restricted profile: `PICO_MAX_TOOLS`.
- Profile name: 64 bytes using `[A-Za-z0-9][A-Za-z0-9._-]*`.
- Profile description: 256 UTF-8 bytes.
- Purpose: 1024 UTF-8 bytes.
- Delegated task: 64 KiB.

Cap exhaustion is a normal manager or `subagent` tool error. If the retired-runtime cap prevents worker replacement, force cancellation degrades to cooperative cancellation instead of creating unbounded zombies.

---

# 2. Subagent profile configuration

## Discovery location

Pico creates the user-global directory if it does not exist:

```text
$XDG_CONFIG_HOME/pico/subagents/
```

or, when `XDG_CONFIG_HOME` is unset:

```text
~/.config/pico/subagents/
```

Pico does not create default profile files. Users add their own definitions, either manually or by asking an agent to create them.

Only direct, regular, non-hidden `*.json` files are loaded. Files are parsed as JSONC using the same comment-tolerant conventions as `settings.json`. The filename stem is the profile name, for example:

```text
~/.config/pico/subagents/exploration.json
```

becomes profile `exploration`.

Profiles are loaded at startup and on F5 or `/reload`. Profile reload uses the same main-thread quiescence path as extension reload. Running agents keep copied profile values and are not changed underneath an invocation.

There are no workspace-local profiles in this scope.

## Profile schema

```jsonc
{
  // Optional short text shown in tool descriptions and future UI.
  "description": "Fast repository exploration",

  // Required system-level role for this subagent.
  "purpose": "Explore the repository and return concise, evidence-based findings. Do not modify files.",

  // Optional. Omission inherits the parent agent's current model.
  "model": "gpt-5.6-sol",

  // Optional. See effort resolution below.
  "effort": "low",

  // Optional exact-name allowlist. Omission allows every registered tool.
  // An empty array gives the subagent no tools.
  "tools": ["sh"]
}
```

Required field:

- `purpose`: non-empty UTF-8 string.

Optional fields:

- `description`: short human-readable summary.
- `model`: exact model catalog ID.
- `effort`: effort supported by the resolved model.
- `tools`: exact-name tool allowlist.

Unknown object keys produce a warning but do not prevent loading, allowing future schema additions. Wrong field types, invalid names, duplicate tools, oversized values, unknown models, unsupported effort values, or unknown tool names make only that profile unavailable. Other valid profiles continue loading. Pico reports invalid files through the existing warning/overlay mechanism with the path and reason.

Tool names are validated after plugins have loaded. A profile whose tool disappears on a later reload becomes unavailable until corrected or the tool returns.

## Model and effort resolution

A named profile may override the parent model and reasoning effort.

Resolve a fresh child as follows:

1. Model:
   - Use `profile.model` when present.
   - Otherwise inherit the parent agent's current model.
2. Effort:
   - Use `profile.effort` when present.
   - Otherwise, if the child uses the same model as the parent, inherit the parent's current effort.
   - Otherwise use the resolved model's configured default effort from `settings.json`, falling back to its first supported effort or `none`.
3. Validate the final effort against the resolved model before creating the child.

The model catalog becomes immutable at runtime. Replace mutable `PicoModel.selected_effort` state with:

- immutable model capabilities and configured default effort in the app catalog;
- selected model and effort in each `PicoAgent`.

A profile's model and effort apply only to the child. They do not modify the parent or workspace defaults.

## Named profiles only

The `subagent` tool accepts a discovered profile name. It does not accept ad-hoc purpose, model, effort, or tool overrides.

This keeps behavior inspectable and makes user configuration the single source of subagent roles and permissions.

## Example profiles

Add documented examples under:

```text
examples/subagents/exploration.json
examples/subagents/review.json
```

`exploration.json`:

```jsonc
{
  "description": "Fast repository exploration",
  "purpose": "Explore the repository for the delegated question. Return concise findings with exact paths and symbols. Do not modify files.",
  "model": "gpt-5.6-sol",
  "effort": "low",
  "tools": ["sh"]
}
```

`review.json`:

```jsonc
{
  "description": "Thorough implementation and contract review",
  "purpose": "Review the delegated changes or plan for correctness, regressions, security, lifecycle, and contract issues. Report prioritized findings with exact paths. Do not modify files.",
  "model": "gpt-5.6-sol",
  "effort": "high",
  "tools": ["sh"]
}
```

These are examples only. Pico does not install or copy them into user configuration automatically.

The `sh` allowlist is not a filesystem sandbox. The purpose instructs these profiles not to edit, but Pico does not attempt to classify or restrict shell commands.

---

# 3. Step 1 — Extract `PicoAgent`

## Goal

Move all conversation- and runtime-specific state out of `PicoApp` while preserving current single-agent behavior. Concurrency remains disabled during this step.

## New files and types

Add:

- `app/include/pico/agent.h`: public opaque handles, IDs, information snapshots, and lifecycle declarations.
- `app/agent_internal.h`: private `PicoAgent` and runtime ownership.

Initial public types:

```c
typedef uint64_t PicoAgentId;

typedef struct PicoAgent PicoAgent;
typedef struct PicoAgentContext PicoAgentContext;

typedef enum PicoAgentKind {
    PICO_AGENT_NORMAL = 0,
    PICO_AGENT_SUBAGENT,
} PicoAgentKind;

typedef struct PicoAgentInfo {
    PicoAgentId id;
    PicoAgentId parent_id;
    PicoAgentKind kind;
    PicoAgentState state;
    int depth;

    char session_id[40];
    char profile[65];
    char purpose[1025];
    char model[128];
    char effort[PICO_EFFORT_LEN];
    char activity[256];

    bool busy;
    bool cancelling;
    bool resumable;
} PicoAgentInfo;
```

`PicoAgent` remains opaque to extensions.

## State moved from `PicoApp` to `PicoAgent`

- transcript messages, count, and capacity;
- `PicoAgentState`;
- `PicoAgentRt`;
- error and activity text;
- compaction summary and state;
- session ID, path, persistence status, and cumulative usage;
- current token usage;
- selected model, effort, context limit, and compaction settings;
- provider history and prompt cache key;
- kind, profile name, purpose, parent ID, and depth;
- resolved tool policy.

Keep app-owned:

- composer and transient submit rewrite/cancel state;
- Clay, selection, scroll, and hover state;
- workspace;
- fonts and rendering registrations;
- plugin/tool/provider/auth registrations;
- auth store;
- model catalog and defaults;
- reload state.

The composer remains one global draft for v1.

## Internal API conversion

Convert agent, transcript, session, usage, settings, and compaction functions to take an explicit target:

```c
PicoAgent_StartTurn(app, agent, text);
PicoAgent_Pump(app, agent);
PicoAgent_Cancel(agent);
PicoAgent_Compact(app, agent);

PicoSession_LogUser(app, agent, content, display);
PicoUsage_Apply(agent, input_tokens, cached_tokens, out_cached);
```

No core worker or session path may recover a target through `app->agent` or a mutable global active-agent pointer.

## Heap ownership prerequisite

Before this step is complete:

- `PicoAgentRt` no longer retains `PicoApp *`.
- Runtime, callback context, job, and other worker-reachable state are heap-owned.
- Worker-facing values such as workspace, model, effort, instructions, tools, and session identity are copied or referenced through a retained heap host service.
- A detached worker cannot reference stack-owned app/UI state.

## Model/settings cleanup

Refactor `app/settings.c` so:

- the app owns immutable model entries and defaults for new agents;
- each agent owns its current model and effort;
- `/model` and `/effort` target the active agent and log to its session;
- those commands may update workspace defaults for future agents, but never other live agents;
- session replay changes only the target agent.

## Primary files

- `app/include/pico/app.h`
- `app/include/pico/agent.h`
- `app/agent_internal.h`
- `app/agent.c`
- `app/agent.h`
- `app/app.c`
- `app/session.c`
- `app/session.h`
- `app/settings.c`
- `app/settings.h`
- `app/usage.c`
- `app/usage.h`
- `app/builtins/chat.c`
- `app/builtins/footer.c`
- `app/builtins/overlay.c`
- `app/builtins/commands.c`
- `app/builtins/prompt.c`

## Acceptance criteria

- Pico still starts and behaves as a single-agent app.
- Existing chat, sessions, compaction, usage, normal cancellation, force cancellation, and shutdown tests pass.
- Agent-owned fields are no longer addressed implicitly through `PicoApp`.
- Worker code has no pointer to stack-owned app or UI state.
- Model and effort are isolated to the extracted agent.

---

# 4. Step 2 — Make APIs agent-aware

## Goal

Remove the implicit current-agent assumption from extension callbacks before enabling concurrent workers. Bump `PICO_EXT_ABI`; no compatibility layer is required.

## Worker callback context

Worker callbacks receive an opaque, callback-scoped `PicoAgentContext *`:

```c
typedef void (*PicoToolFn)(PicoAgentContext *ctx,
                           const char *args_json,
                           PicoToolResult *out);

typedef int (*PicoProviderStreamFn)(PicoAgentContext *ctx,
                                    const PicoLlmTurn *turn,
                                    PicoLlmCancelFn cancel,
                                    PicoLlmDeltaFn on_delta,
                                    void *user,
                                    PicoLlmResult *out);
```

The context:

- cannot be retained after the callback;
- never exposes `PicoApp *`;
- binds to one agent ID and runtime generation;
- provides copied/read-only workspace, purpose, profile, agent ID, and session ID;
- provides synchronized auth access;
- provides cancellation, ask, child PID, and internal delegation helpers;
- cannot mutate UI or write directly to a session.

Refactor helpers accordingly:

```c
pico_tool_ask(ctx, request_json, &answer_json);
pico_tool_set_child(ctx, pid);
pico_auth_copy_ctx(ctx, provider, &entry);
```

## Split worker and main-thread hooks

Replace the current shared tool-hook callback with separate contracts:

```c
typedef void (*PicoToolBeforeFn)(PicoAgentContext *ctx,
                                 PicoToolEvent *event);

typedef void (*PicoToolAfterFn)(PicoApp *app,
                                PicoAgentId agent,
                                PicoToolEvent *event);

typedef bool (*PicoToolApplyFn)(PicoApp *app,
                                PicoAgentId agent,
                                const char *details_json,
                                bool replay);
```

LLM and request-context hooks remain main-thread callbacks and receive the target agent ID:

```c
typedef void (*PicoLlmHookFn)(PicoApp *app,
                              PicoAgentId agent,
                              PicoLlmEvent *event);

typedef void (*PicoContextHookFn)(PicoApp *app,
                                  PicoAgentId agent,
                                  PicoContextEvent *event);
```

Notification hooks receive a `PicoHookEvent` containing the target ID. Document every hook as app-global or agent-scoped. Add an agent-destroy notification so extensions can remove ID-keyed state.

## Extension state contract

Do not add a generic extension-state subsystem.

Instead:

- main-thread extension state is keyed by stable `PicoAgentId`;
- main-thread hooks and apply callbacks are serialized;
- worker callbacks may overlap and must be reentrant;
- worker callbacks must not directly mutate agent/session-scoped extension state;
- agent/session changes are returned through results and applied on the main thread after runtime-generation validation;
- worker-global caches are permitted only when thread-safe and semantically independent of agent/runtime generation.

## Tool policy and authorization

Build each request catalog in this order:

1. Start from registered tools.
2. Apply the agent's profile/tool policy.
3. Run LLM hooks against the permitted catalog.
4. Run request-context hooks with the final effective catalog available for inspection.
5. Copy the final offered catalog into the provider request.
6. Retain that immutable snapshot until all calls from the result complete or are aborted.

`StartNextTool` resolves function pointers only from the retained offered snapshot, never from the current global registry.

A hidden or unoffered tool call:

- is added to history as a controlled tool error;
- is logged to the owning session;
- invokes neither BEFORE hooks, tool code, apply, nor AFTER hooks;
- allows the model to recover in the next round.

Malformed tool call arrays, duplicate/empty call IDs, or calls beyond `PICO_MAX_PENDING_CALLS` fail the provider round explicitly instead of silently dropping calls.

## Builtin migration

### TODO

Convert `app/builtins/todo.c` from process-global session state to a main-thread map keyed by `PicoAgentId`.

- Apply updates the target agent.
- Context reads the target agent.
- Reset/destroy clears only the target.
- Render uses the active agent.
- Reload replays details for every live agent.
- The reminder is omitted when `todo_update` is not in the effective catalog.

### Ask user

Core ask state remains runtime-owned. The custom questionnaire UI may remain one global renderer keyed by globally unique ask ID. Ask instructions are included only when `ask_user` is offered to that agent.

### Audit

Audit every builtin and example for mutable worker globals and single-worker assumptions, especially:

- `app/builtins/openai.c`
- `app/builtins/ask_user.c`
- `app/builtins/todo.c`
- shell and permission tools
- all files under `examples/`

## Acceptance criteria

- Two callback contexts can safely exist simultaneously.
- A retired runtime context cannot affect its replacement.
- Policy-hidden and LLM-hook-hidden tools cannot execute.
- TODO, apply, usage, and session state are isolated by agent.
- Every example compiles against the new ABI.

---

# 5. Step 3 — Add `PicoAgentManager`

## Goal

Support multiple independent full agents concurrently without adding sidebar UI.

## New manager

Add:

- `app/agent_manager.c`
- `app/agent_manager.h`

`PicoApp` owns a heap-allocated `PicoAgentManager *` containing:

- live agent slots;
- active agent ID;
- runtime generations;
- retired runtimes;
- session writer reservations;
- manager-level pending asks;
- delegation request/jobs;
- process-level curl ownership;
- discovered subagent profiles.

## Public manager API

Expose main-thread-only APIs sufficient for future sidebar work:

```c
int pico_agent_count(const PicoApp *app);
bool pico_agent_info(const PicoApp *app, int index, PicoAgentInfo *out);
bool pico_agent_find(const PicoApp *app, PicoAgentId id, PicoAgentInfo *out);

PicoAgentId pico_agent_active(const PicoApp *app);
bool pico_agent_select(PicoApp *app, PicoAgentId id);

PicoAgentResult pico_agent_create(PicoApp *app,
                                  const PicoAgentCreateOptions *options,
                                  PicoAgentId *out);
PicoAgentResult pico_agent_close(PicoApp *app, PicoAgentId id);
PicoAgentResult pico_agent_cancel(PicoApp *app, PicoAgentId id);
PicoAgentResult pico_agent_force_cancel(PicoApp *app, PicoAgentId id);
```

Rules:

- IDs become invalid after close or workspace replacement.
- Close rejects busy agents and agents referenced by jobs or retired runtimes.
- Cancellation and close are separate operations.
- Creation and selection never block.
- Information is returned as copied snapshots.

Builtin chat can use private transcript access. Public message inspection uses bounded main-thread-only borrowed accessors.

## Pumping

Replace the single-agent frame pump with:

```c
PicoAgentManager_Pump(app->agents);
```

The manager pumps every live agent each frame. Transcript and session mutation remains on the main thread. Existing chat/footer/overlay views render only the active agent.

Selecting another agent:

- clears chat selection indices;
- resets follow-bottom and scrollbar state;
- clears stale agent-specific overlay snapshots;
- leaves the global composer draft unchanged.

## Session writer reservation

Each logical session has one JSONL and at most one in-process writer.

Resume uses a two-phase operation:

1. Resolve an exact session ID in the current workspace.
2. Validate schema and canonical path.
3. Reserve the path under the manager.
4. Replay into an unpublished agent without writing.
5. Publish only after complete success.
6. Only then append a synthesized interrupted tool result, if necessary.

Failure releases the reservation and leaves live agents unchanged.

The programmatic manager/subagent APIs accept exact IDs only. `/resume` may continue offering unique-prefix UI completion before resolving to an exact ID.

A session already open by a normal agent, subagent, job, or retained runtime cannot be resumed again.

## Pending asks

The manager exposes the oldest live ask across every agent:

```c
typedef struct PicoToolAsk {
    uint64_t id;
    PicoAgentId agent_id;
    const char *profile;
    const char *purpose;
    const char *request_json;
} PicoToolAsk;
```

- Pending asks are presented FIFO.
- Ask IDs remain globally unique.
- `pico_tool_answer` routes to the owning runtime.
- Cancelled and stale asks disappear.
- A hidden subagent can ask without becoming the active chat agent.
- Only one ask UI is rendered at a time.

## Profile registry API

The manager owns immutable loaded profile snapshots. Add internal/public read APIs for tool descriptions and future UI:

```c
int pico_subagent_profile_count(const PicoApp *app);
bool pico_subagent_profile_info(const PicoApp *app,
                                int index,
                                PicoSubagentProfileInfo *out);
```

The `subagent` tool schema remains a string rather than a generated JSON enum, but its description and system guidance list the currently valid profile names and descriptions.

## Concurrency tests

Add `app/tests/agent_manager_test.c` using condition-variable barriers, not timing assumptions, to prove:

- two provider callbacks overlap;
- reverse completion routes events correctly;
- transcripts, sessions, errors, usage, model, effort, asks, and TODO state remain isolated;
- cancelling or force-cancelling one agent does not affect another;
- one session cannot have two writers;
- switching the active agent cannot leave stale transcript indices.

## Acceptance criteria

- Multiple full agents run concurrently through manager APIs.
- Existing UI renders the selected agent only.
- Session events never cross agent boundaries.
- Hidden asks remain answerable.
- Profile discovery loads valid files and isolates invalid-file failures.

---

# 6. Step 5 — Add named subagent delegation

## Goal

Implement a builtin synchronous `subagent` tool using discovered named profiles and the same `PicoAgent` runtime as full agents.

Add:

- `app/subagent_config.c`
- `app/subagent_config.h`
- `app/builtins/subagent.c`
- profile registration in `app/include/pico/plugin.h` and `app/plugin.c`
- `app/tests/subagent_config_test.c`
- `app/tests/subagent_test.c`

## Tool schema

```json
{
  "profile": "exploration",
  "task": "Find the session replay and cancellation choke points.",
  "session_id": "optional exact previous child session ID"
}
```

Rules:

- `profile` and `task` are required.
- `profile` must resolve to a valid discovered configuration.
- `session_id` is optional and exact.
- Purpose, model, effort, and allowed tools come only from the named profile and resolution rules.
- Tool output always identifies the profile used.

## Fresh child

Without `session_id`:

- create a new child agent and logical session;
- resolve model and effort from the profile and parent;
- build fresh current workspace instructions;
- append the profile purpose as system-level instructions;
- add only the delegated task as user history;
- apply the profile's effective tool policy;
- never use ambient `resume_last`.

The child shares host registrations and authentication but receives no parent transcript, provider history, compaction briefing, TODO state, or cache key.

## Continued child session

With `session_id`:

1. Resolve and reserve the exact session atomically.
2. Require the stored session profile to match the requested profile.
3. Replay that session's transcript, provider history, compaction boundary, usage, and cache key.
4. Re-resolve model, effort, purpose, and tools from the current profile and current parent environment.
5. Append the new delegated task.

The previous conversation is continued, but execution policy is refreshed from the current profile definition. This allows users to improve profile configuration without discarding reviewer or exploration context.

If the resolved model differs from the model used by the prior child invocation, rotate the prompt cache key before the request. Effort-only changes keep the cache key unless the provider's existing cache contract requires rotation.

A session created under one profile cannot be resumed under another profile. A session already open anywhere in the manager is rejected.

## Purpose assembly

Every child invocation builds instructions in this order:

1. Current user/global `SYSTEM.md`.
2. Current workspace `.pico/SYSTEM.md` and `AGENTS.md`.
3. Existing core instructions.
4. A clearly delimited section:

```text
Subagent profile: exploration
Purpose:
<configured purpose>
```

5. Agent-targeted LLM hook additions.

No previous system prompt or previous profile text is copied into provider history.

## Delegation job state

Use one heap-owned job object with states:

```text
REQUESTED
STARTING
RUNNING
DONE
ERROR
CANCELLED
ABANDONED
```

References may be held by:

- the manager request queue;
- the waiting parent runtime;
- the child terminal callback.

Destinations use agent IDs and runtime generations, not raw agent pointers. Terminal publication occurs exactly once under the job mutex. The final reference frees the result and releases the child and session reservation.

## Worker/main-thread handshake

The `subagent` tool executes on the parent worker:

1. Parse the profile name, task, and optional session ID.
2. Enqueue a manager request.
3. Wait on the job condition variable without holding manager locks.
4. The main thread validates the current profile and session reservation, then creates the child.
5. The manager pumps the child alongside all other agents.
6. Child terminal state publishes a result and wakes the parent.
7. The parent tool returns through the normal tool event path.

This follows the existing `pico_tool_ask` worker/main-thread synchronization pattern without adding a generic asynchronous tool API.

## Completion result

Return model-visible JSON:

```json
{
  "status": "completed",
  "profile": "review",
  "model": "gpt-5.6-sol",
  "effort": "high",
  "session_id": "abc123",
  "resumable": true,
  "final_answer": "Review findings..."
}
```

Terminal statuses:

- `completed`
- `error`
- `cancelled`

`final_answer` is the child's last non-empty assistant message.

For `--no-session` or a persistence failure, do not advertise a reusable ID:

```json
{
  "status": "completed",
  "profile": "exploration",
  "model": "gpt-5.6-sol",
  "effort": "low",
  "resumable": false,
  "final_answer": "..."
}
```

`resumable` becomes true only after the session header and delegated task are durably written.

## Cancellation

Normal parent cancellation:

- immediately wakes the waiting parent worker;
- marks that parent runtime generation abandoned;
- requests child cancellation;
- does not wait for a blocked child provider;
- prevents late child completion from publishing into a replacement parent.

Force cancellation retains the job, child, session reservation, runtime, and plugin references until all owners release them. A child error or cancellation becomes a normal tool error only when the original parent generation remains live.

## Session schema

Move to session schema version 3. Add to the header:

```json
{
  "type": "session",
  "version": 3,
  "id": "...",
  "kind": "subagent",
  "profile": "review",
  "initial_purpose": "...",
  "parent_session_id": "..."
}
```

`parent_session_id` is omitted for an ephemeral parent. The initial profile metadata remains durable, while the current profile file determines execution policy for each resumed invocation.

The parent session needs no separate delegation event: normal persisted tool arguments and results contain the profile, child session ID, resolved model/effort, and outcome.

## Tests

### Profile parsing and discovery

- Missing directory is created and treated as an empty registry.
- Empty directory is valid; invoking an unknown profile returns a controlled error.
- JSONC comments work.
- Filename-to-profile mapping and name validation work.
- One invalid profile does not hide valid profiles.
- Duplicate tools, unknown tools, unknown models, and unsupported efforts invalidate only that profile.
- Reload atomically swaps the registry only after parsing the complete new snapshot.
- Example exploration and review files parse successfully.

### Model and effort

- Omitted profile model inherits the parent model.
- Configured profile model overrides the parent.
- Omitted effort inherits the parent when the model matches.
- Omitted effort uses the target model default when the profile changes model.
- Configured effort overrides both inherited and default effort.
- Invalid combinations fail before publishing a child.
- Child model/effort never mutate the parent or another agent.

### Context and tools

- Fresh child receives no parent history.
- Purpose and profile name appear in instructions.
- Omitted, empty, and restricted tool policies behave correctly.
- Hidden tools cannot execute even if returned by the model.
- Exploration/review examples expose only `sh`.

### Session continuation

- Exact previous session restores only that session's context.
- Continued child appends to the same JSONL.
- Profile mismatch is rejected.
- Current profile policy is reapplied on continuation.
- Model change rotates the cache key.
- Failed resume leaves no published agent or reservation.

### Parent/child lifecycle

- Parent remains in tool wait until child terminal publication.
- Parent resumes exactly once.
- Parent cancellation wakes immediately and cascades.
- Late child events cannot affect a replacement runtime.
- A child ask can be answered while the parent waits when its profile allows `ask_user`.
- Agent, retired-runtime, and depth caps return controlled errors.

---

# 7. Step 6 — Harden lifecycle and contracts

## Reload

Reload uses a full quiescence barrier. It is blocked while any live or retired runtime has:

- queued or executing provider/tool work;
- an offered-tool snapshot or pending calls;
- a pending ask;
- a delegation wait/job;
- undrained events capable of starting follow-up work;
- retained extension-owned pointers.

A queued reload prevents new turns and delegations from starting. The main thread keeps pumping until quiescent, then reloads extensions and subagent profiles atomically.

After reload:

- reinitialize registrations;
- load and validate the profile registry against the new tools/models;
- revalidate idle restricted agents;
- replay structured tool details for every live agent;
- notify extensions of each live agent/session.

No plugin generations are added in v1.

## Workspace changes

Workspace change is deferred, never synchronously waited on:

1. Queue the requested path.
2. Prevent new turns and delegations.
3. Continue pumping and accepting cancellation/ask UI.
4. Wait for the same quiescence barrier.
5. Atomically destroy the old agent set, change workspace, reload settings/plugins, and create one fresh normal agent.

User-global subagent profiles are revalidated against the new workspace's tools and model catalog after the transition. Pico never partially mutates `app->workspace` before the transition can complete.

## Shutdown

Use one absolute shutdown deadline for every live agent, job, and retired runtime:

1. Cancel all live runtimes and delegation jobs.
2. Pump terminal state.
3. Join workers that finish before the shared deadline.
4. Detach workers that remain blocked.
5. Skip extension shutdown, `dlclose`, retained auth destruction, and curl cleanup when detached workers can still reach them.

Change `PicoApp_Free` to report clean versus retained shutdown. If any runtime is detached:

- retain only the heap execution host and required plugin/auth state;
- permanently retire Pico in that process;
- reject later `PicoApp_Init` or plugin lifecycle attempts;
- require the caller to proceed to process exit.

This prevents retained workers from overlapping with reinitialized extension globals.

## Persistence failures

Make JSONL writes return status instead of silently ignoring failures. Track session persistence as:

- ephemeral;
- durable;
- failed.

Surface failures through agent errors or status warnings. A child result reports `resumable: true` only after durable session creation and task persistence.

## Documentation

Add a user guide:

- `docs/subagents.md`

It documents:

- config directory creation;
- JSONC schema;
- named-profile-only invocation;
- model/effort inheritance and overrides;
- tool allowlist semantics;
- session continuation;
- copying the exploration/review examples;
- reload behavior and errors;
- the fact that tool allowlists are not sandboxes.

Update:

- `README.md`
- CLI help in `app/main.c`
- `/docs` topics in `app/builtins/commands.c`
- `docs/extend/README.md`
- `docs/extend/contracts.md`
- `docs/extend/tools.md`
- `docs/extend/hooks.md`
- `docs/extend/context.md`
- `docs/extend/providers.md`
- `docs/extend/auth.md`
- `docs/extend/views.md`
- `docs/extend/anatomy.md`

Add the public extension/manager API topic:

- `docs/extend/agents.md`

Document explicitly:

- callback concurrency and thread rules;
- context lifetime and runtime-generation binding;
- agent/session identity;
- extension state isolation;
- tool authorization;
- manager-level ask routing;
- profile discovery and snapshot lifetime;
- exact session continuation;
- cancellation propagation;
- reload quiescence;
- detached shutdown terminality.

Update all files under `examples/` for the extension ABI change.

---

# 8. Implementation sequence and review gates

Implement as independently verifiable stages:

1. Define opaque public agent IDs/types and private ownership boundaries.
2. Extract all agent state while retaining one agent.
3. Split immutable app model data from per-agent model/effort selection.
4. Convert sessions, usage, transcript, asks, TODO, and settings to explicit targets.
5. Convert worker and main-thread extension callbacks; bump the ABI.
6. Move every worker-reachable object to heap ownership and verify force-cancel safety.
7. Add the manager and multi-agent pumping.
8. Add session reservations and atomic exact resume.
9. Add JSONC profile discovery and validation.
10. Add the named `subagent` tool and delegation job state machine.
11. Add reload/workspace/shutdown barriers.
12. Update examples and all public documentation.

Do not enable concurrent starts before stages 1–6 pass their single-agent and retired-runtime tests.

---

# 9. Final definition of done

The feature is complete when:

- Existing single-agent behavior remains covered.
- Multiple full agents execute concurrently and independently.
- Named subagent profiles are discovered from the user-global config directory.
- Invalid profile files fail independently with useful diagnostics.
- Subagents inherit the parent model and effort by default.
- Profiles can override model and effort without affecting the parent.
- Exploration and review example profiles use `gpt-5.6-sol`, `sh`, and low/high effort respectively.
- Fresh subagents receive no parent conversation context.
- A previous matching-profile session can be explicitly continued.
- Profile tool policies are enforced at both prompt and execution boundaries.
- Parent/child cancellation and force cancellation are race-safe.
- Session files have one writer and atomic resume.
- Multiple pending asks are correctly routed.
- Reload and workspace changes remain responsive while deferred.
- Shutdown uses one global deadline and never exposes freed state.
- All tests pass and extension examples build against the new ABI.
- User and extension documentation match the implemented contracts.

## Deliberately excluded

To preserve Pico's minimal design, this work does not add:

- sidebar UI;
- workspace-local subagent profiles;
- bundled or auto-created profile definitions;
- ad-hoc subagent purposes or per-call model/tool overrides;
- thread pools;
- parallel tools within one agent;
- process isolation;
- filesystem or shell-command sandboxing;
- Git worktree management;
- plugin generations or hot reload during active work;
- a generic asynchronous tool API;
- generic core-owned extension state storage;
- cross-agent transcript merging.
