# Hooks

Four families:

- **Notifications** — `pico_host_add_hook` (host scope) and `pico_workspace_add_hook` (workspace scope). Main thread, with a `PicoHookEvent` target.
- **Tool interceptors** — `pico_add_tool_before_hook` and `pico_add_tool_after_hook` (workspace scope).
- **LLM/request context** — `pico_add_llm_hook` and `pico_add_context_hook` (workspace scope), with a target agent ID.
- **Tool-row clicks** — `pico_add_tool_row_hook` (workspace scope). Main thread, when a chat tool row is activated.

```c
pico_host_add_hook(host, PICO_HOOK_AFTER_LAYOUT, MyAfterLayout);
pico_workspace_add_hook(workspace, PICO_HOOK_BEFORE_SUBMIT, MyBeforeSubmit);
pico_add_tool_before_hook(workspace, MyBeforeTool);
pico_add_tool_after_hook(workspace, MyAfterTool);
pico_add_llm_hook(workspace, MyBeforeLlm);
```

Callbacks run in registration order. Each family has its corresponding `PICO_MAX_*_HOOKS` limit.

## Notifications

```c
static void MyHostHook(PicoHost *host, const PicoHookEvent *event, void *state)
{
    (void)host;
    (void)event;
    (void)state;
}

static void MyWorkspaceHook(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    PicoAgentId target = event->agent_id;
    (void)workspace;
    (void)state;
}
```

All notifications run on the main thread.

### Host Notification Hooks (`pico_host_add_hook`)
Registered during `host_init`. Only valid for host-global UI hooks:
- `PICO_HOOK_AFTER_LAYOUT` — after Clay layout, before render. Host-global UI work; `event->agent_id` is the UI-selected agent, or zero.
- `PICO_HOOK_AFTER_RENDER` — after `Clay_Raylib_Render`. Host-global UI work; `event->agent_id` is the UI-selected agent, or zero.

### Workspace Notification Hooks (`pico_workspace_add_hook`)
Registered during `workspace_init`. Valid for agent lifecycle hooks:
- `PICO_HOOK_BEFORE_SUBMIT` — intercept/rewrite the snapshotted submit for that agent. A later selection change cannot retarget it.
- `PICO_HOOK_ON_SUBMIT` — the target user message was logged and its turn started.
- `PICO_HOOK_ON_MESSAGE` — a message was added to the target transcript.
- `PICO_HOOK_ON_COMPACT` — target compaction is starting.
- `PICO_HOOK_AFTER_COMPACT` — target history was replaced with a briefing.
- `PICO_HOOK_ON_TURN_END` — target became idle after a completed turn, not cancel/error.
- `PICO_HOOK_ON_CANCEL` — target turn was cancelled.
- `PICO_HOOK_ON_ERROR` — target entered `PICO_AGENT_ERROR`.
- `PICO_HOOK_ON_SESSION_RESET` — target starts a new/resumed/ephemeral session, or a reload re-announces a live session; clear only that ID's session state. After reload, structured tool details are replayed to rebuild it.
- `PICO_HOOK_ON_AGENT_DESTROY` — target is about to become invalid; remove its ID-keyed extension state.

## BEFORE_SUBMIT

Composer send snapshots the selected agent ID, then `PicoHost_Submit` clears cancel, agent input, and agent parts and runs the hook for that ID. Call `pico_host_request_submit_cancel(host)` to swallow the send. Call `pico_host_set_agent_input(host, text)` with a malloc'd replacement sent to the model; the composer text remains the display text and Pico takes ownership. Whitespace-only composer text is skipped unless pasted image attachments are present.

For structured input, call `pico_host_set_agent_parts(host, parts_json)` with a malloc'd JSON array of canonical parts in model-facing order. Supported user parts are `text` (`text`), `image`, and `audio` (`path`, optional `mime` / `url`). Keep bytes and base64 out of this JSON; provider converters read local `path` values when sending the request. When parts are set, include the complete text part as well as attachments because it replaces the normal one-text-part user item. After the hook, Pico validates those parts, then appends any pasted composer images (or builds `[text, image…]` when hooks left parts unset). Invalid hook parts reject the submission without discarding the composer draft. Pasted images also block submission, preserving the draft, when the active model does not support vision. Chat display may include markdown images for those files; agent input and the text part stay as typed/hook text. Pico frees the replacement input and parts after submit or cancellation. Each setter frees any previous value.

## ON_COMPACT

```c
static void OnCompact(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    char *briefing = /* malloc */;
    (void)state;
    pico_agent_set_compact_summary(pico_workspace_host(workspace), event->agent_id, briefing);
}
```

Call only during `PICO_HOOK_ON_COMPACT`. Pico takes ownership. A stale/mismatched ID is rejected and the string is freed.

## Before-tool interceptor

```c
static void Before(PicoAgentContext *ctx, PicoToolEvent *event, void *state)
{
    (void)state;
    /* worker thread; callback-scoped ctx */
}

pico_add_tool_before_hook(workspace, Before);
```

Before hooks run on the worker immediately before the offered tool. They may rewrite `args_json_out`, set `deny`, set a malloc'd denial `result`, or call `pico_tool_ask(ctx, ...)`. First deny stops later before hooks. Cancellation wins over denial.

The callback may overlap worker callbacks for other agents. Do not use Clay/UI or mutate main-thread extension state.

## After-tool interceptor

```c
static void After(PicoWorkspace *workspace, PicoAgentId agent_id, PicoToolEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    /* serialized main thread */
}

pico_add_tool_after_hook(workspace, After);
```

After hooks run on the main thread after structured details have been applied. A malloc'd `event->result` rewrites model-visible output; later hooks see the rewrite. They are skipped on turn cancellation.

`PicoToolEvent` fields:

- `name`, `call_id`, `args_json` — current call, core-owned.
- `args_json_out` — before only; malloc'd replacement.
- `deny` / `result` — before denial.
- `output`, `details_json`, `executed`, `is_error` — after outcome.
- `result` — after only; malloc'd output replacement.

Hidden or unoffered calls invoke neither before nor after hooks.

## LLM interceptor

```c
static void Llm(PicoWorkspace *workspace, PicoAgentId agent_id, PicoLlmEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    event->extra_instructions = JsonDup("Prefer short answers.");
}
```

Full file: [`../../examples/extra_instructions.c`](../../examples/extra_instructions.c).

LLM hooks run on the serialized main thread for every request, including compaction. They see only tools permitted by the agent policy. Hooks run twice per request in registration order: first a filtering pass where `exclude[i] = true` hides a tool from this request (any `extra_instructions` set during this pass is discarded), then an instructions pass where every hook sees the final exclusion set and malloc'd `extra_instructions` is appended under a shared `## Additional instructions` section for later hooks. The heading is omitted when no hook contributes a non-empty extra.

The provider receives a retained copy of the final catalog. `/show-prompt` runs the same hooks with the selected agent's workspace. Reload of that workspace cannot unload hooks while a runtime retains a callback/catalog snapshot; it waits for that workspace's quiescence and releases idle snapshots first. Other workspaces are not blocked.

## Tool-row click

```c
static void OnRow(PicoWorkspace *workspace, PicoToolRowEvent *ev, void *state)
{
    (void)state;
    if (!ev->name || strcmp(ev->name, "web_search") != 0)
    {
        return;
    }
    (void)pico_ui_modal_push(pico_workspace_host(workspace), "web_search");
    ev->handled = true;
}

pico_add_tool_row_hook(workspace, OnRow);
```

Chat calls `pico_tool_row_activate` when the user activates a tool row (main transcript or nested inspect). Hooks run in registration order on the main thread. The first hook that sets `handled` skips later hooks and the default expand/collapse. Builtin `subagent` inspect is one of these hooks.

`PicoToolRowEvent` fields are borrowed from the trace line and valid only during the callback:

- `agent_id`, `name`, `call_id`
- `args_json` — the original provider arguments, not the formatted transcript label
- `output` — NULL while the call is still running
- `child_id`, `child_session_id` — linked subagent identity when available
- `is_error`
- `handled`

Do not retain the pointers. Maximum 64 hooks (`PICO_MAX_TOOL_ROW_HOOKS`).

