# Contracts

Read this before writing an extension. Getting these wrong crashes Pico or silently no-ops. Host and workspace lifecycle details are in [host](host.md) and [workspace](workspace.md).

## Composer session references

`/`, `@`, and `#` are builtin completion triggers; custom completers can use
other registered characters. Query and accept remain main-thread callbacks.
Builtin `#` completion defaults to the selected conversation's logical project
including worktrees; an explicitly selected project qualifier retains canonical
identity even when display names collide. Only durable main conversations are
offered, excluding the current conversation. Completion reads a complete scoped indexed-metadata snapshot
on a worker, never a session transcript. A ready snapshot lasts for its token and
scope; query edits and backspacing only filter it locally. The display result cap
does not limit searchable metadata, and prefix edits during the initial load do
not invalidate that same-scope load. New catalog/title changes become visible in
a new token or scope snapshot. Snapshots are not reused across tokens,
conversations, or project qualifiers.

The composer backing text contains portable session-reference links while the UI
shows atomic title chips. `PicoComposer_SetText` can restore portable copied text;
`PicoComposer_ReplaceRange` expands a partial reference selection to its full span.
Keep using these mutation helpers rather than editing bytes inside reference
payloads. Byte offsets refer to backing text, not the title's visible length.
References remain display atoms even inside Markdown code or next to image syntax;
Markdown rendering must not expose their hidden payload or turn them into images.
Copy/paste retains title, session ID, checkout identity and the absolute JSONL path;
copying into other applications discloses those fields.

Composer preprocessing replaces references with quoted titles/absolute JSONL
paths for normal sends and steering; it preserves the portable visible message.
It neither validates file existence nor reads/attaches prior conversation content.
The path is live, not a snapshot. Metadata and generated replacements are not
rescanned as file mentions. Accepted steering freezes the projected text, not the
referenced file's contents. Replay restores visible and model forms as persisted,
without re-resolving names. Explicit `pico_agent_submit`/`pico_agent_steer` APIs do
not run composer preprocessing. Details: [completers](completers.md).

## Composer file mentions

Builtin file completion/submission share `@bare/path` and `@"escaped path"` representations. Simple bare paths contain ASCII letters/digits and `_`, `-`, `.`, `/`; names with spaces, other punctuation, or Unicode are quoted with JSON-style string escapes. Tokens start at whitespace/start-of-text and end at whitespace/end-of-text; quoted completion queries can span spaces, and accepting from inside an existing quote replaces its whole span. Invalid/incomplete/NUL-containing/oversized tokens never resolve a shortened prefix or rescan their interiors as attachments. Unrepresentable completion labels/insertions are omitted rather than truncated. Paths resolve against the explicit target workspace, not process CWD or later UI selection; existing bounded text/media attachment rules still apply. These are ordinary editable text mentions, not session-reference chips. See [completers](completers.md#builtin-file-mentions).

## Markdown image resources

Relative image paths and file-link targets in chat resolve against the displayed transcript's workspace, including live and saved subagent transcripts, never process CWD. Absolute paths remain unchanged and remote image URLs are not fetched. `MdView_SetBaseDir()` copies a main/render-thread setting used by subsequent document renders; custom Markdown views must set their own base when needed (`NULL`/empty resets to process CWD). `MdView_HoveredLink()` returns a current-layout borrow with relative file targets resolved using the hovered document's base; later base changes must not retarget it. URI schemes and fragment-only link targets remain unchanged. Clicks use the system's default handler, not an embedded preview or anchor navigator. See [Markdown paths and links](views.md#markdown-paths-and-links) and [images](images.md#images-in-chat-markdown).

`MdView_BeginFrame()` invalidates the prior layout's image borrows, just as a new Clay layout invalidates its command storage. Image nodes/textures remain stable through the current layout's render consumption; keep neither pointer beyond that lifetime. Unused retained cache entries are evictable, but the current/previous working sets may exceed the idle retention budget. File changes trigger refresh/redraw; failed refresh keeps a usable old texture and is retryable. `MdView_ClearImages()` is main/render-thread-only with a live graphics context and no pending command references. The graphics owner clears before context teardown; extensions must not restart or clear the shared image lifetime from view callbacks. See [views](views.md#markdown-image-lifetime).

## Fast-mode provider contract

`PicoModel.supports_fast` is explicit catalog capability, not a default-on setting. The optional `PicoProvider.supports_fast(host, model, state)` callback runs on the main thread with borrowed arguments, must be prompt/read-only/nonblocking, and must not do network I/O or credential refresh. NULL is unsupported. Its workspace-instance state may concurrently be used by worker `stream` calls; apply the same synchronization and registration-generation lifetime rules as other provider callbacks. The worker independently validates its actual authentication route.

`PicoLlmTurn.fast` is snapshotted once per agent turn, including tool follow-ups and automatic compaction; UI changes affect the next turn. Manual compaction takes a new snapshot. `PicoLlmResult.service_tier` is a fixed-size, NUL-terminated copied array containing the actual reported tier, or empty for unknown; it is never inferred from the request and requires no free. Unsupported priority requests fail rather than silently retrying at standard tier. The builtin xAI provider permits Fast with API-key or OAuth credentials; OAuth priority is experimental and backend acceptance remains unverified. Its normal worker-side refresh and authentication checks still apply. Usage records distinguish selected/requested Fast from served tier. `PicoAgentInfo.fast` is the next-turn selection; `last_service_tier` describes the last successful result. Subagents opt in with profile `fast: true`, never inherit the parent's Fast selection, and reapply the current profile on continuation. Details: [providers](providers.md#fast-mode).

The builtin OpenAI provider sends an advisory `x-codex-routing-hint` only on ChatGPT/Codex OAuth requests. It uses the turn's pinned model and requested tier (`model=<model>;tier=priority` with Fast on, `model=<model>` with Fast off), consistently across follow-ups, compaction, and retries. Invalid HTTP-header bytes omit the hint. It neither replaces the body tier nor reports the served tier, and is separate from the server-issued turn-local routing token. API-key/custom-endpoint requests do not send the hint.

## Reload and workspace quiescence

Builtin software recommendations are base system-prompt text, not extension registrations. Executable availability is cached per workspace from Pico's inherited `PATH` on open and refreshed after successful quiescent reload; relative/empty entries resolve from the workspace directory. Model requests reuse that cache. See [workspace](workspace.md).

Workspace reload queues while that workspace has a live or retired runtime with provider/tool work, pending calls, an offered-tool snapshot, a pending ask, undrained events, retained extension callbacks, or a delegation wait/job. A `.c` or subagent-profile file written in this turn does not load in that workspace until that work finishes. Automatic detection scans sources and recorded include dependencies on a core worker, normally every ~0.5s; the main thread discards scans with outdated workspace/root identity and alone decides reload and activates registrations. Explicit reload, change-triggered validation, and in-flight compile freshness checks remain synchronous. Compilation is asynchronous, has a 30-second deadline, and leaves the current generation active until main-thread validation and activation succeed. Obsolete builds are discarded. An unchanged source that failed to compile is not retried until the source changes or the user reloads. F5 and `/reload` request host-extension replacement (user-global / config sources only), compiling asynchronously when needed and request reload of the selected agent's workspace only; other workspaces keep accepting work. A compile or validation failure in one workspace's local extensions does not block host-extension replacement. As soon as a workspace reload is requested, that workspace refuses new external turns and delegations while continuing to pump existing follow-ups, cancellation, and asks. Host-extension replacement does not wait for workspace workers. Once a workspace is quiescent, Pico compiles and initializes a complete candidate module/registration generation before replacing the active one. Failed compilation, loading, validation, or initialization leaves the previous generation and instance state active. Retained runtimes release their exact immutable registration generation only after their callbacks and queued events finish. Restricted live agents are revalidated, each live session is announced, and structured tool details are replayed.

`/cd` canonicalizes and opens the target workspace (or reuses an already-open canonical path) and selects a main agent through `pico_agent_select`. Relative paths resolve against the command agent's workspace, not UI selection. Ordinary directories reuse an existing main agent or create one when none exists. Opening an exact linked-worktree root starts a fresh main session there; selecting a durable sidebar row resumes that exact session instead. A newly opened workspace is closed if creating its first main agent fails. `/cd` does not select into a closing workspace. The previous workspace stays open and keeps pumping.

Capacity is self-evicting before it fails: `pico_workspace_open` at the 8-live-workspace cap and user-facing agent creation/session load at the 16/32 agent caps first close the least-recently-active idle workspace or main agent inline (bounded drain; `PICO_LIMIT` only when every candidate is protected or busy, or the victim's close cannot quiesce within the inline drain budget). Eviction never cancels work — busy agents, pending asks, in-flight session loads, running background jobs, delegation jobs, and the selected workspace/agent are exempt — and the workspace or agent whose hook, `on_frame`, completer, or `workspace_init` callback is dispatching (and the target of a create/load in progress) is held, so a callback that opens a workspace or creates an agent at a cap gets `PICO_LIMIT` or evicts another idle object instead of destroying something it will return into. Beyond those holds, eviction is invisible to extensions: they see only the normal `PICO_HOOK_ON_AGENT_DESTROY` and workspace-close sequences. The existing agent being replaced by an asynchronous load remains protected between replay batches, and a new load's target is held through cancellation of the previous candidate. Invalid creation options and invalid, missing, or already-open synchronous resumes fail before eviction and preserve existing agents/selection; allocation/runtime failures may still occur after eviction. Synchronous resume validates/reserves the file before eviction and applies that same prepared data only once capacity is available. If replay callbacks consume the slot and the private candidate cannot be published, it receives an agent-destroy hook before being discarded. Details: [workspace](workspace.md), [agents](agents.md#agent-capacity-eviction).

A second Esc force-cancels a stuck turn: the UI goes idle and a new worker starts so the user can keep chatting, but reload still waits for every abandoned provider/tool worker. That worker may outlive the turn; do not touch Clay, Raylib, or chat/composer state from it.

On process exit, every agent, delegation job, retired runtime, core-tracked OpenAI login attempt, and extension source scanner shares one absolute deadline of about one second. `pico_host_free` returns `PICO_HOST_SHUTDOWN_CLEAN` when all workers join. If any worker (including a source scan stalled on filesystem I/O) or callback remains blocked, it returns `PICO_HOST_SHUTDOWN_RETAINED`: Pico detaches it and retains the heap execution host plus every registration, auth store, builtin state, and user-extension `.so` handle it can reach. No extension `shutdown`, `dlclose`, retained auth destruction, or curl cleanup occurs. Pico is then permanently retired in that process; later `pico_host_init` and plugin lifecycle attempts are rejected, and the caller must proceed to process exit. Host UI state is never worker-owned. An extension-owned HTTP transfer still active at normal shutdown likewise retains curl global state and retires Pico; new HTTP borrows are rejected. Only `pico_host_free` applies this process deadline; workspace close does not.

`--safe` skips user extensions. `--no-workspace` initializes host extensions and the window with zero live workspaces; explicit session startup options retain their normal cwd workspace behavior. Compile errors and failed `pico_add_tool` registrations set the host overlay warning. `pico_status_warn` appends a line to that overlay.

Pico owns window destruction; extensions must not call Raylib `CloseWindow()`. On clean CLI shutdown, extension `shutdown` callbacks run while the graphics context is still available. See [host shutdown](host.md#cli-shutdown) for the exit and cleanup sequence.

## Redraw and frame callbacks

Pico still pumps host and workspace `on_frame` callbacks when the window is
unchanged; a pump is not necessarily a presentation. An unchanged pump also skips
Clay layout, so view render callbacks and `PICO_HOOK_AFTER_LAYOUT` hooks run only on
laid-out pumps. Clay scroll state from the last layout survives skipped-layout pumps,
so scrolling an idle chat does not lose its offset or virtualized transcript viewport.
The idle pump cadence is not display-paced: the loop blocks until an
input event, a worker notification, a scheduled redraw deadline, or an idle cap of
about 100 ms, so `on_frame` observes roughly 10 Hz while idle and `dt` can be large
after a sleep. Pico queues notify the loop on worker handoff, but a wake may arrive
before a task becomes joinable; adoption can then take another idle tick. Work
polled without a notification (including child-process completion and extension
state read only from `on_frame`) can also take up to the idle cap to be observed.
Linux composer paste discovers clipboard formats and retrieves the selected
image or text on a core-tracked worker, using only the active display backend.
Helper completion wakes the host; main-thread adoption inserts text or attaches
the image and requests redraw. Cancellation or composer replacement invalidates
the result without waiting on the UI thread. The worker owns helper processes
and buffers and participates in the shared process-shutdown deadline. This is
compiled-in work, not a public extension worker API, and does not affect copying
text to the clipboard.
Visible extension-owned state does **not** invalidate the window automatically:
call `pico_host_request_redraw(host)` on the main thread when changing a Clay
view or a direct after-render drawing, or
`pico_host_request_redraw_after(host, seconds)` for the next timed visual
change. A request coalesces with other requests and causes a full redraw at or
after that frame/deadline. A request issued during `AFTER_RENDER` schedules a
future frame, not recursion. Registration/reload invalidates the shell, but a
running extension must explicitly invalidate its own subsequent UI changes.
Do not call these functions on workers or request a redraw on every idle pump:
workers use `pico_ui_post` or other core queues and the main-thread adoption
requests a redraw; extension-specific main-thread state changes need a request
from the owning callback. No Clay-owned pointer survives a frame or same-frame
reflow even when no image was presented.

## Commands during streaming

While an agent is streaming, ordinary submits and slash commands are blocked before submit hooks; only commands explicitly opted in during registration with `pico_host_command_allow_while_busy` or `pico_workspace_command_allow_while_busy` run. The builtin `/background`, `/help`, `/docs`, `/settings`, `/extensions`, and `/show-prompt` are opted in. A busy-allowed command must clear and cancel its submission and must not interrupt the active turn. See [commands](commands.md).

## Threads

Main thread: `init`, `shutdown`, `on_frame`, view render, notification hooks (including `PICO_HOOK_ON_LLM_RESPONSE`), after-tool hooks, tool-row hooks, tool apply callbacks, LLM/context hooks, command `run`, completer query/accept, auth login/logout, `pico_ui_modal_push` / `pop`, `pico_agent_ui_latest` / `pico_agent_ui_clear`. Host `init`, `on_frame`, sidebar/main/overlay views, and UI notification hooks may run with zero workspaces and active agent ID zero; host extensions must treat that as a normal state. Composer/footer slots and workspace contextual/empty-state views do not render without a selected agent. Agent-scoped callbacks receive a `PicoAgentId`; keep mutable agent/session extension state in an ID-keyed map. Main-thread callbacks are serialized and must return promptly. They must not join or wait for asynchronous work. A user `.so` must not leave an extension-owned background thread running across frames: the public lifecycle has no pre-shutdown quiescence callback, `shutdown` cannot wait, and `dlclose` follows it. A stuck main-thread callback blocks the whole host and is an extension contract violation. Sidebar session selection, `/resume`, and startup validate JSONL and prepare a bounded number of large-message Markdown documents on a core worker, but replay hooks and tool `apply` callbacks still run in original record order on the main thread in bounded batches. Prepared Markdown is used only if it matches the actual message after earlier replay hooks. Saved-subagent inspection reads and prepares its transcript on a core worker before atomically publishing its snapshot; the inspect overlay may show Loading in the meantime. The builtin `/resume` search and `#` token/scope snapshot loading run on core workers and may initially show no results; `/resume` may use a briefly stale cached list. `#` filters its complete ready metadata snapshot locally without further query I/O, and in-flight snapshots can be adopted after prefix edits within the same token/scope but never for another token, selected conversation, project scope, or builtin instance. Extension completer query/accept callbacks remain synchronous. During these callbacks a private candidate can be looked up by its ID (for example with `pico_agent_message`), but it is not counted, selected, or otherwise published between callbacks and may be discarded if loading is cancelled or fails. On cancellation after callbacks have run, Pico emits `PICO_HOOK_ON_AGENT_DESTROY` for the private candidate so extensions can discard agent-ID-keyed state; arbitrary external callback side effects are not rolled back. Replayable tool apply must be idempotent. Public `pico_main_agent_create` with `PICO_SESSION_RESUME`, named child continuation, and extension reload tool-detail restoration remain synchronous and can block the UI if invoked from main-thread work. A large message beyond the bounded preparation set, or another exceptionally large record, can still hitch the UI during unsliced core parsing/mutation or an extension callback; the per-pump budget cannot interrupt a callback. View render callbacks are declarative and may run more than once per displayed frame during same-frame reflow; keep durable state changes, I/O, and input consumption in `on_frame` or hooks.

Builtin OpenAI browser/device login is core-tracked compiled-in work, not a user-extension thread. Each worker owns its attempt data and callback sockets, never host/UI state; main-thread application validates the still-current attempt before displaying its authorization link, opening a browser, or committing initial credentials. Cancel, replacement login, key selection, logout, and host-extension replacement invalidate queued events without waiting for network work. Retiring attempts are reaped by the host; process shutdown cancels and quiesces them within its shared deadline before curl/auth cleanup, retaining the host if work is still blocked. Browser launch and explicit Markdown link clicks use argv-based process spawning without a shell or UI-thread wait; targets remain literal, including apostrophes and shell metacharacters. Explicit link clicks report both spawn failure and later opener failure (nonzero exit or signal) as a visible toast while the host is alive. Successful spawning is not proof of display. Launchers are reaped independently and may outlive a host without retaining it; detached reaping after teardown cannot notify the former UI or terminate the external application. No public worker or lifecycle API is added. See [OpenAI login](auth.md#builtin-openai-login).

Worker thread: `PicoToolFn`, `PicoToolBeforeFn`, `PicoProviderStreamFn`. They receive a callback-scoped opaque `PicoAgentContext *`, never `PicoHost *`. Do not retain it. Parallel-eligible tool callbacks and before-tool hooks may overlap within the same agent; callbacks from different agents and workspaces also overlap. They must be reentrant. Main-thread apply and after-tool callbacks are serialized with each other, not with sibling workers. Use context accessors, `pico_tool_ask`, `pico_ui_post`, `pico_tool_set_child`, and `pico_auth_copy_ctx`; do not touch UI, transcript, session, settings, model catalog, or unsynchronized agent-scoped extension state. `pico_agent_context_workspace` / `pico_agent_context_workspace_id` are the immutable path and ID copied when the turn was accepted, not the UI-selected workspace. `pico_agent_context_registration_generation` is the workspace registration generation copied for that same accepted turn. `pico_tool_ask` and `pico_ui_post` may be called only from a tool or before-tool callback. Builtin `run_background` does not call `pico_tool_set_child`: jobs belong to the workspace table, survive extension reload, and are killed on session reset, agent destroy, workspace free, or host shutdown. On Linux, each background job's private supervisor keeps it running until the launching command and all descendants in its original process group finish, reaps orphaned group descendants, and preserves the command's final exit code or signal. Group ownership survives launcher exit and closed output pipes; explicit kill and lifecycle cleanup signal that whole group. Descendants that deliberately leave it are outside this boundary. The supervisor's parent-death handler kills the group if the host disappears, but not merely when its spawning worker thread exits. The host itself is not made a subreaper. See [background job lifetime](tools.md#background-job-lifetime). Do not block on your own condition variable — Esc, force-cancel, reload, and shutdown cannot wake it.

Do not use Clay, Raylib drawing, or composer/chat mutation from the worker. Tools return a `PicoToolResult` with malloc'd fields; providers use `on_delta` / `PicoLlmResult`. UI code answers a pending ask from the main thread with `pico_tool_answer`. `PICO_LLM_DELTA_THINKING` appends; `PICO_LLM_DELTA_THINKING_SUMMARY` replaces the current summary (zero-length starts the next step). Pico coalesces consecutive summaries until a tool call. The widget title is the latest step; expanding shows every step.

Reload of a workspace is deferred while any of its live workers is busy, and while any force-cancelled worker in that workspace is still in a tool or provider call, so those pointers stay valid until the call returns. That includes a worker blocked in `pico_tool_ask` or the builtin `subagent` tool. Other workspaces are not blocked. A worker callback that never returns leaves only its workspace in `CLOSING`.

Named delegation is a worker/main-thread handshake (several sibling calls may wait independently): the parent tool waits on a core-owned job without holding workspace locks; the main thread creates and pumps the child. Worker callbacks may therefore run for a child while its parent worker is blocked. Child asks are still published through the normal global ask routing. The builtin chat inspects a child from its parent `subagent` tool row without selecting that child.

Pico and its extensions never call `chdir()` in the host process. Shell tools may change directory only after `fork()` in the child.

## Tool batch ordering and lifetime

See [tools](tools.md#execution-policy-and-parallel-calls) for registration and scheduling. Native multi-call responses form a bounded batch: parallel-eligible calls overlap, sequential tools/profiles are per-agent barriers, and no model follow-up starts until the batch settles. Failures do not cancel siblings. Apply/after callbacks and persistence run in completion order; transcript rows retain response order. Use call IDs, not completion position, to associate results. Execution policy and callbacks resolve from the retained offered snapshot. Builtin subagent calls also require their profile's explicit `parallel_safe: true`; false/omission, unparseable profile arguments, and unknown profiles are parent-batch barriers. This flag is a strict boolean and never grants workspace-wide exclusivity or read-only enforcement. Eligibility is snapshotted before any batch callbacks run. Each before-tool rewrite must preserve `profile` and `session_id` identity/presence; changing either returns an error without delegation. Task edits and equivalent JSON string encodings remain allowed.

Each invocation has a separate callback context, ask state, and tracked child process. Cancellation wakes all asks and delegations and stops queued dispatch; force-cancel kills all tracked groups and retires all remaining callbacks. A process registered after cancellation raced with spawn is killed immediately. Late completion cannot update the replacement generation; already-completed results survive cancellation. Reload and shutdown must account for every worker and its retained registration/state.

Mailbox identity does not gain a call ID: same-agent/name posts still share a stream (text append order, latest status wins). Ask IDs remain globally unique and answer only their owning invocation. Ask hooks notify per ask, so one `ON_ASK_END` does not imply the agent has no other pending ask.

## Ownership

- Pico owns Clay context/arena lifecycle. Extensions must not initialize or replace the context or change its capacity limits. Clay-owned pointers are valid only for the current layout; retain IDs or copied values instead. See [Clay layout lifetime](views.md#clay-layout-lifetime).
- `PicoExt.name`, `PicoExt.description`, and `name` / `description` / `help` / `params_json` / provider/auth string fields: must outlive the extension. Use string literals. `PicoExt.description` is optional.
- `PicoToolResult.output` / `details_json`, `images` and every image byte buffer, and optional `media_namespace`: malloc if set; ownership transfers to core on callback return and Pico frees. Zero-initialize the result and set `is_error` for tool-defined failures. Output is text: session/provider JSON serialization preserves valid UTF-8 and replaces each malformed byte with U+FFFD (`�`), as does the SDK's `JsonBuf_String()`. Raw JSON fields such as `details_json` are not repaired. See [text output](tools.md#text-output-and-json-encoding).
- `pico_tool_ask` answer: malloc on `PICO_ASK_OK`; the caller frees it. Always `NULL` on cancel/fail.
- `pico_tool_pending_ask` `request_json`: the oldest live ask owned by the open session (the selected agent or a transitive delegated child); valid until the next pump. Do not retain it across frames.
- `pico_agent_ui_latest` `status` / `text`: borrowed published mailbox storage keyed by `(agent_id, runtime_generation, name)`, invalidated by the next pump, `pico_agent_ui_clear` of that agent and name, force-cancel or generation retirement, or workspace close. Pico copies posted bytes; do not retain worker pointers. `pico_ui_latest` / `pico_ui_clear` are the same lookup for the UI-selected agent.
- `pico_subagent_profile_info`: copies the complete profile snapshot into caller storage. Profile strings and tool names in that copy are caller-owned values, not registry pointers.
- `pico_agent_message`, its optional `markdown_base_dir`, and nested `PicoTraceLine` strings such as `tool_call_id`: borrowed main-thread transcript storage, invalidated by pumping, transcript mutation, agent close, or workspace close. `PicoMessage.trace_group_expanded` and `PicoTraceLine.expanded` are host-owned, ephemeral presentation state; extensions may inspect but must not mutate them, and they are not restored from sessions. `PicoTraceLine.tool_done_t0` is also host-owned ephemeral presentation state for that finished-tool dwell and is not restored from sessions. `PicoTraceLine.tool_streaming` and `tool_stream_bytes` describe a provisional row that the finished result has not yet claimed. Formatted `tool_args` may appear once that call's arguments are complete, before sibling calls finish; they are still provisional and are not session content. These fields are host-owned ephemeral presentation state, never persisted, and cleared when the completed call lands or the turn ends without one.
- `PicoAgentContext *` and all strings returned by its accessors: callback-scoped; never retain them.
- `PicoToolEvent.name` / `call_id` / `args_json` / `output` / `details_json`, `PicoToolRowEvent.name` / `call_id` / `args_json` / `output` / `child_session_id`, `PicoLlmEvent.tools` / `instructions`, and `PicoContextEvent.history_json` / `tools`: core-owned and valid only during the callback.
- `PicoToolEvent.args_json_out` / `result`, `PicoLlmEvent.extra_instructions`, and `PicoContextEvent.extra_context`: malloc if you set them; Pico frees.
- `pico_host_set_agent_input(host, text)`: `text` is malloc'd and ownership transfers to Pico.
- `pico_host_set_agent_parts(host, parts_json)`: optional malloc'd JSON array of canonical user parts; Pico frees.
- `pico_agent_set_compact_summary(host, agent_id, summary)`: `summary` is malloc'd and ownership transfers to Pico.
- `PicoLlmResult` strings/arrays: malloc; Pico calls `pico_llm_result_free`. This includes every nested item/part string (`thinking` / `thinking_signature` on assistant items, `item_id` on tool-call items, `path` / `url` / `mime` on media parts). Providers must project replay-critical state into canonical result items; unknown provider-native output types fail the turn instead of being dropped. Do not store file bytes in history or result JSON.
- `shutdown` must not wait for worker completion. User extensions must have no live extension-owned threads when `shutdown` is invoked; `dlclose` follows immediately. Core-owned worker callbacks are quiesced by Pico before workspace shutdown. Builtins compiled into the host executable may use core-managed detached lifetime, but that pattern is not safe for a user `.so`.

## HTTP helpers

`pico_http_post_sse`, `pico_http_post`, and `pico_http_get` are blocking and may be called concurrently. Pico reuses a small bounded set of idle transport handles across calls and live hosts; per-call options and callback pointers are reset before a handle returns to the pool. Closing one host leaves HTTP usable by other live hosts, and a clean final shutdown permits a later host to start a new pool. Active transfers never share a handle. Calls must finish before clean host shutdown: if an extension-owned call remains outstanding, Pico rejects new HTTP borrows and retains curl global state rather than cleaning it up beneath that call (retained host shutdown). Request URLs, bodies, headers, callbacks, and callback data must remain valid until the call returns. SSE framing is selected by the response `Content-Type: text/event-stream`. If `Content-Type` is absent, a first line beginning with `event:`, `data:`, `id:`, `retry:`, or `:` selects SSE framing. An explicit content type takes precedence; other responses are delivered as a single JSON body. SSE accepts LF, CRLF, and CR line endings, including mixed endings and separators split across writes. SSE requests follow redirects and allow 600 seconds without response-body bytes, including the wait for the first byte. Every nonempty body write, including an SSE keepalive, resets inactivity; response headers alone do not. Each attempt also has a 3600-second total-duration limit and a 600-second connection limit. These deadlines use monotonic time, so slow active streams may run beyond ten minutes, but keepalives cannot keep an attempt alive indefinitely. Deadline expiry returns `PICO_HTTP_FAIL` with an explanatory error, not `PICO_HTTP_CANCEL`. Buffered GET/POST requests follow redirects and retain a 30-second total timeout per attempt. GET ignores `PicoHttpReq.body`.

Builtin OpenAI, Hyper, and xAI adapters also guard decoded generation independently of transport liveness. Tool arguments have a 1 MiB per-call limit, checked on incoming fragments and complete snapshots without double-counting snapshots. Text, reasoning, summaries, and individual argument streams have separate conservative exact-repeat watches: cycles up to 4 KiB, at least 16 KiB repeated, then at least sixty more seconds of uninterrupted repetition. These fixed guards reset per attempt; failure is not cancellation and is not automatically retried, including on an HTTP capacity status. Failed responses never execute tools, and partial text/thinking follow ordinary failed-turn retention. Custom providers do not automatically inherit these builtin parsing guards. See [providers](providers.md#streaming-safeguards).

`PICO_HTTP_OK` means the transport completed, not that the server returned a successful status. Always inspect `out_http`; HTTP 4xx/5xx responses still return `PICO_HTTP_OK`. If set, `out_body` and `out_error` are malloc'd and caller-owned. Cancellation is polled during transfer, returns `PICO_HTTP_CANCEL`, and leaves output strings unset. A callback returning false aborts SSE parsing so the callback's recorded application error can take precedence over a transport error.


Shared HTTP helpers retry transient TLS handshake, host/proxy DNS resolution, and connection failures up to five times after the initial attempt, waiting 1, 2, 4, 8, and 16 seconds. They never retry after receiving any response headers or body, including interim and redirect headers. Certificate verification failures, timeouts, HTTP errors, and interrupted streams are not retried. Deadlines reset for each attempt, not the entire retry sequence: SSE uses 600-second byte inactivity, a 3600-second total limit, and a 600-second connection limit; buffered requests retain a 30-second total limit. Cancellation is checked before attempts and throughout waits (approximately every 100 ms).

Both request structs accept an optional `on_retry(user, retry, max_retries, delay_seconds, error)` callback. It runs synchronously on the calling thread before each wait and again before the corresponding attempt with `delay_seconds == 0`; retry numbers are one-based. `error` is borrowed only for the callback. The callback must return promptly and must not mutate the request or access UI from a worker. All request data remains borrowed across waits until the helper returns. Builtin streaming providers bridge this callback to ephemeral activity; other callers can supply their own observer.

`PICO_LLM_DELTA_STATUS` replaces ephemeral provider activity using `text`/`len` and `call_index = -1`; an empty status restores the normal thinking or compaction label. The host copies the text during `on_delta` and publishes it on the main thread. It is never assistant output, thinking history, or persisted session content. Subsequent output clears the status; completion, failure, cancellation, and the next request do not retain it. The live chat indicator and delegated-agent activity display it. Providers must use deltas, not UI mutation, for worker-side status.

`PICO_LLM_DELTA_RESET` is a worker-side, payload-free request-attempt boundary (`call_index = -1`). Before replaying a response, emit it to discard that attempt's streamed text, thinking/summaries, provisional tool calls, and streaming-rate contribution. Main-thread publication preserves earlier completed rounds and notices, and applies the reset before replacement output even when both arrive between pumps. Reset does not finish the turn, execute calls, persist discarded output, or invoke lifecycle/successful-response hooks. Builtin OpenAI, Hyper, and xAI streams retry HTTP 429/503 and explicit overload/capacity errors up to five times with cancellable 5/10/20/40/60-second waits and live status. Only the final attempt is returned; exhaustion reports the last failure under the ordinary partial-output rules. This provider-layer policy is separate from the shared HTTP helpers and does not replay auth requests. See [providers](providers.md#connection-retries-and-live-status).

`PicoPlugins_Count(host)` / `PicoPlugins_Get(host, index, out)` return diagnostic records per host or workspace slot (builtins and user sources, including failed loads). Pointers in `PicoExtInfo` and indexes are valid until the next registry change/reload. `scope` is `PICO_EXTENSION_HOST` (workspace ID zero) or `PICO_EXTENSION_WORKSPACE` (an explicit workspace ID, or zero for an available module with no open workspace). `loaded` is true when compile/`dlopen` succeeded (builtins always). The enabled preference is **global per module identity**, not per slot: builtin name or full user-source path. All loaded slots of the same module report the same `enabled` value; failed stubs report false. Separate source files with the same declared name remain independent. `PicoPlugins_SetEnabled(host, index, enabled)` changes the indexed module's global preference, including all applicable open and future workspaces; the index does not limit the operation to that slot.

`/extensions` (or F2) groups these diagnostic records into one row/toggle per module identity without host/workspace badges. `PicoHostPreferences` owns the global `disabled_host_extensions` and `enabled_host_extensions` lists: entries are builtin names or full user-source paths, bounded by `PICO_EXTENSION_KEY_MAX` including NUL; overlong keys are rejected rather than truncated. Explicit disable wins over opt-in. `cache_miss` is the current opt-in builtin. These preferences come only from user-global `settings.json`; workspace files and `disabled_extensions` do not control extension activation. `PicoWorkspaceSettings` has no extension preference fields (ABI 29). Disabled instances skip `init` and `on_frame`; `shutdown` runs only for initialized instances. The builtin `extensions` manager and `settings` editor cannot be disabled.

Toggles update the in-memory global preference immediately and enqueue one copied-key persistence job on the host's FIFO worker. A failed write restores the last successfully saved preference unless a newer toggle is pending, warns, and requeues affected reloads. Pending preferences reserve capacity for rollback; a new toggle is refused if that capacity is exhausted. Durable writes preserve unrelated settings and use the same locked atomic replacement as other preferences. Host callbacks reload through the host path; every applicable workspace reloads independently after its existing quiescence barrier. Unrelated workspaces are not paused. A shared preference does not merge instance state, registrations, or active generations.

OAuth refresh is coordinated per provider by `pico_auth_begin_refresh_ctx(ctx, provider, rejected_access_token, cancel, user, &entry)`. A `NULL` rejected token requests proactive refresh. A non-NULL rejected token forces refresh only while it still matches the stored token, regardless of local expiry; a waiter adopts a usable replacement instead of rotating it again. The rejected pointer may alias the incoming owned `entry.access_token`; it is compared before replacing the snapshot. Only `PICO_AUTH_REFRESH_OWNER` may exchange/publish credentials and must call `pico_auth_end_refresh_ctx` on every exit. `ALREADY_VALID` callers use the returned token without exchange/end; `FAILED` callers stop. Builtin OpenAI, Hyper, and xAI retry a rejected OAuth request once. This signature is part of ABI 20; see [auth](auth.md).

## Limits

- 8 live workspaces (`PICO_MAX_WORKSPACES`)
- 16 agents per workspace (`PICO_MAX_AGENTS`), 32 agents host-wide (`PICO_MAX_TOTAL_AGENTS`)
- 16 calls per model response; `max_parallel_tools` is 1–16 per agent, default 4 (`PICO_DEFAULT_PARALLEL_TOOLS`). Workspace settings supply the default and profiles may override it for a child. Limits are snapshotted per turn; excess eligible calls queue.
- 32 user `.c` files
- 16 views per slot
- 16 empty-state views (`pico_workspace_add_empty_view`)
- 64 notification hooks, tool hooks, tool-row hooks, LLM hooks, context hooks, tools, commands
- 16 named modal claims (`PICO_MAX_UI_MODALS`); names shorter than `PICO_UI_MODAL_NAME`
- 16 UI mailbox keys per workspace (`PICO_MAX_UI_POSTS`), keyed by `(agent_id, runtime_generation, name)`; names shorter than `PICO_UI_MODAL_NAME`
- `PICO_UI_POST_TEXT_MAX` (64 KiB) and `PICO_UI_POST_STATUS_MAX` (128 bytes)
- 16 completers, providers, auth registrations
- 24 completion items per query; completion labels are at most `PICO_COMPLETE_LABEL_MAX` (288) UTF-8 bytes
- `PICO_TOOL_DETAILS_MAX`, `PICO_TOOL_ASK_MAX_REQUEST`, and `PICO_TOOL_ASK_MAX_ANSWER` (64 KiB)
- Builtin `ask_user`: 24 questions, 20 options per select question, 16 KiB per free-form answer

Registration APIs (`pico_host_add_*`, `pico_workspace_add_*`, `pico_add_tool`, `pico_set_tool_group_title`, `pico_add_provider`, `pico_add_auth`, etc.) may only be called during their respective `host_init` or `workspace_init` phase; calls outside the active init scope fail and emit warnings. Registrations stage in an isolated staging area and are atomically published to an immutable registration generation only if `init` succeeds (returns 0). A turn retains the generation it accepted, including its tools, providers, hooks, and callback state, until the turn and its queued events are quiescent. If `init` fails or returns non-zero, staged registrations are discarded without modifying the active registry. Registrations are also ignored when a limit is full, arguments are NULL, or the kind/slot is invalid. `pico_add_tool` takes an explicit execution policy (`PICO_TOOL_SEQUENTIAL` or `PICO_TOOL_PARALLEL`) and additionally returns `false` for invalid policies, duplicate names, and malformed/non-object `params_json` schemas, and appends a `status_warn` line naming the tool and the reason. `pico_set_tool_group_title` assigns borrowed singular/plural transcript group labels to a tool already staged in that `workspace_init`; it returns `false` and warns when the call is outside init, the name is unknown, or either label is empty. Group titles are UI metadata, not model-facing catalog fields.

Builtin Responses function tools explicitly use `strict: false`, preserving
registered parameter schemas and their optional-field semantics rather than
implicitly making every field required. Tool callbacks remain responsible for
validating their arguments. See [providers](providers.md#function-tool-schemas).

## Directories

- User extensions: `~/.config/pico/extensions/`
- Workspace extensions: `<workspace>/.pico/extensions/`
- User subagent profiles: `~/.config/pico/subagents/` (or the matching `$XDG_CONFIG_HOME` path)
- User-global settings: `$XDG_CONFIG_HOME/pico/settings.json` or `~/.config/pico/settings.json`. This single file stores host preferences, host-extension disables, models, and workspace defaults. On startup, Pico copies the bundled `examples/settings.json` here only if the path is absent; it leaves any existing file, symlink, or directory untouched. `/settings` (and the sidebar settings button) patches this file for the catalog, default model, compact ratio, resume-last, max parallel tools, font scale, and chat width. Apply validates the complete draft, preserves comments and unrecognized model properties, and publishes all managed fields with one atomic replacement. Disable lists stay in `/extensions`.
- Workspace settings: `<workspace>/.pico/settings.json`. Settings read-modify-write updates preserve the existing file mode and hold an inter-process sidecar lock through atomic replacement.
- Pico-created Git worktrees: `$XDG_DATA_HOME/pico/worktrees/<project-key>/<name>` or `~/.local/share/pico/worktrees/<project-key>/<name>`. Names are a single safe path component and branch name. Creation snapshots the main checkout's committed `HEAD`, does not copy dirty/untracked files, and never removes the worktree automatically.
- Session catalog: `~/.config/pico/sessions/<encoded-key>/` (or `$XDG_CONFIG_HOME/pico/sessions/`). Each directory remains one exact checkout/storage workspace. Parent `*.jsonl` files are sessions; subagent jsonl is omitted from `/resume` and the sidebar. `.workspace.json` stores required `path` (encoded keys are not reversible), display `name`, fallback `order`, `collapsed`, and a listing cache of parent sessions (`id`, `title`, `model`, `effort`, `kind`, the JSONL stat generation, `unseen_complete`, and `inbox_pending`). The canonical sidebar order is the path list in catalog-level `.workspace-order.json`; a drag updates the UI immediately and queues one coalesced atomic replacement on Pico's persistence thread. Successful saves are silent. A failed save preserves the in-memory order and raises a status warning. jsonl is authoritative. Scan stats every jsonl and reuses a cached row when its size, inode, and nanosecond mtime/ctime generation match; a mismatch, a new file, or missing metadata re-reads jsonl (header plus last `model_change`, last `unseen_complete`, and last `inbox_pending`). Overlay-only ids without a jsonl file do not appear. Catalog read-modify-write operations use inter-process locks, and session append/title write-through holds the session lock until the matching catalog generation is committed. Pico rewrites missing metadata from JSONL `cwd` plus its project/worktree fields on scan, and rewrites the listing cache when it changes. The sidebar groups exact checkout-root catalogs by their recorded main checkout while session files and reservations remain checkout-local. A project-level catalog record holds the grouped display label and stash flag; the Stashed sidebar section is collapsed by default, remains browsable/resumable, and opening a stashed workspace does not restore it. The sidebar pen popover renames the group without changing any checkout path, stashes/restores the group, or confirms deletion of all Pico saved history across its checkout catalogs and media. Delete refuses while any group agent is live in the current process; it does not take the inter-process catalog locks, so deletion is not coordinated with other running Pico processes. Media removal covers the composer and listed parent sessions; subagent session media may remain. Delete does not remove project directories or unrelated files. Scan omits a missing ordinary workspace; a recorded linked-worktree catalog remains visible and unavailable when its checkout path disappears, and resume never falls back to the main checkout.
- Extension discovery uses regular `.c` files, skips hidden names, and walks to depth 8.
- Extension compilation runs `${PICO_CC:-cc}` with one packaged SDK include root and the source file's directory. Installed SDK headers live under `share/pico/sdk/include`; development builds mirror that layout beside the executable. Dependency manifests are cache-namespaced by SDK include root, so concurrent installed and development builds do not treat each other's manifests as source changes. Archives and AppImages require a host C99 compiler, while the Nix package supplies GCC. `PICO_DATA_DIR` overrides the discovered `share/pico` data/SDK root for staged-package testing.
- Profile discovery uses only direct, regular, non-hidden `*.json` files and parses them as JSONC.
- `/cd` opens or selects a workspace: the target is canonicalized relative to the command agent's workspace, an already-open path is reused, a main agent is created only if that workspace has none, and selection moves there via `pico_agent_select`. A newly opened workspace is rolled back if that create fails. Closing workspaces are rejected. The previous workspace is not closed or paused. The workspace path is immutable after open.

## `PicoHost` and `PicoWorkspace`

`PicoHost` and `PicoWorkspace` are opaque. Prefer `pico_add_*` / `pico_host_add_*` / `pico_workspace_add_*` and the host setters listed in the topic pages (`pico_host_request_submit_cancel`, `pico_host_set_agent_input`, `pico_host_set_agent_parts`). Conversation/runtime/session/model/usage fields live in opaque workspace-owned `PicoAgent` instances. Use the copied `pico_agent_*` snapshots. See [host](host.md), [workspace](workspace.md), and [agents](agents.md). The private app-level `agent.h`, `agent_internal.h`, `host_internal.h`, `workspace_internal.h`, `session.h`, and `settings.h` are not extension API.

The model catalog is resolved per workspace from user-global configuration plus `<workspace>/.pico/settings.json`. Applying `/settings` reloads that resolved catalog so newly created agents use the new default immediately. The settings editor keeps its draft and shows an animated saving state while a FIFO host-persistence job saves and prepares resolved configuration off-thread; publication and agent reconciliation run on the main thread only after success. Editing/cancellation and host-extension replacement are deferred during that save; a failure leaves the draft/error modal open until retry or explicit dismissal. Extension toggles and project rename/stash are optimistic: they take effect immediately, save durably on the same worker, and restore the last successfully saved state on failure unless newer edits are pending. They report failures without automatic retries. These compiled-in jobs own copied inputs, may outlive an extension generation, resolve live state afresh on completion, and share the existing bounded catalog-persistence shutdown deadline. No extension callback runs on the persistence worker. Every turn pins the selected model's metadata—including provider, base URL, vision, context, and efforts—plus the resolved effort at turn start, and all of the turn's requests (tool follow-ups and compaction included) use that snapshot. Selecting a different model or effort while a turn is busy (footer picker, `/model`, `/effort`) updates and persists the selection immediately but applies only from the next turn. A `/settings` catalog reload likewise leaves a busy turn's snapshot untouched. At that boundary Pico keeps the agent's model ID when it still exists in the reloaded catalog, or switches it to the new default when it was removed. Each agent owns copied effort/context/compaction selection, so changing defaults or replaying a session does not mutate another active turn. `context_limit` comes from the selected catalog model when that model has one; `context_limit_fallback` in workspace settings / `PICO_CONTEXT_LIMIT` is only a fallback.

`pico_session_log_custom(host, agent_id, "myext", "{…json…}")` is called on the main thread and queues a JSONL record for the explicit target; Pico's persist thread performs the actual append. It returns `PICO_SESSION_WRITE_SKIPPED`, `_OK`, or `_FAILED`. `_OK` means the record was accepted for persistence, and queued records for one target land on disk in call order. Skipped means the target is intentionally ephemeral. Stale or already-failed targets fail immediately. An asynchronous open/write/fsync/close failure moves the target to `PICO_SESSION_FAILED`, rolls back a partial final line when the file supports truncation, preserves its ID/path and reservation until reset/close, warns in the overlay, and makes `PicoAgentInfo.resumable` false. Session replay does not dispatch custom records back to extensions. For replayable tool-owned state, return validated structured `details_json` and register a tool apply callback instead. Apply receives the target ID. Details are replayed chronologically on resume and extension reload; use complete snapshots and make replay application idempotent.

Zero is invalid for every ID. Stale workspace, agent, ask, and runtime-generation IDs fail without affecting a different object.

## Turn-local transport state and prompt snapshots

ABI 27 adds borrowed `PicoLlmTurn.turn_state` and malloc-owned
`PicoLlmResult.turn_state`. A validated successful result replaces the runtime's
transport state (NULL clears it); all other returned state is freed with the result.
Only the provider worker accesses retained transport state. Core forwards it to
subsequent requests of that same agent turn, including tools, steering, and automatic
compaction, but never across a new user turn, standalone compaction, or runtime
replacement. It is neither model-visible canonical history nor session metadata.
The provider must bind it to the actual backend/credential owner and keep retries
local to its callback; shared provider state is not a per-agent routing store.

The session cache key survives compaction and resume; new/reset sessions rotate it.
A main-thread prompt snapshot retains final normal instructions and a filtered
catalog for compaction. Its tool entries borrow the runtime's retained registration;
core drops them before releasing that generation. Compatible compaction does not
invoke LLM hooks again. Fallback builds use normal hook semantics (`compact == false`),
including after resume. Model/provider/URL/capability, effort/Fast, base-instruction,
tool-eligibility, registration, and history-reset changes invalidate reuse. Request
context is excluded from snapshots. See [providers](providers.md#compaction-prompt-snapshots).

`PicoHttpPost.on_headers` runs synchronously on the calling worker, once per complete
origin response header block, before its body. Status and raw non-NUL-terminated bytes
are borrowed for that callback only. Informational and redirect blocks are included;
proxy CONNECT headers and trailers are not delivered as response blocks. Providers
must check status, copy only required values, validate values before sending them as
headers, and never persist secrets/routing tokens. See [http](http.md).

## Compaction checkpoints and provider state

ABI 26 separates `PicoLlmTurn.compaction_history_json` / `compaction_history_count`
from outgoing `input_json`. Native providers borrow the original canonical source
only for user retention, normal media budgeting, and checkpoint construction;
they must send the filtered `input_json` instead. Non-native requests supply
NULL/0. Both arrays and all strings are immutable and live only for the provider
callback. Retained image references survive missing-file recovery and remain
restorable; ordinary compaction can still drop messages outside its budget.

ABI 25 adds `PicoProvider.native_compaction`, `PicoLlmTurn.native_compact`,
`context_limit`, and `compact_seed`, and the malloc-owned, mutually exclusive
`PicoLlmResult.compaction_json` result variant. The route query has the same
main-thread, nonblocking, borrowed-argument and generation lifetime contract as
`supports_fast`. Provider execution remains worker-scoped and may overlap across
agents. A provider opting in must validate every `provider_state` against the
actual backend/credential authority before sending, including after refresh.
No provider may silently discard opaque state or route it to another authority.
See [providers](providers.md#native-compaction) for schema and wire behavior.

Native checkpoints carry a complete ordered history replacement plus scoped
opaque state, not assistant output or a portable text summary. The session record
has `type: "compaction"`, `kind: "native"`, `version: 1`, `history`, and
`tokens_before`. Text checkpoints retain their summary representation because
text compaction is still supported. Replay validates a complete native checkpoint
before selecting it as the continuation boundary; malformed/unknown checkpoints
fail rather than producing empty context. Failure to allocate required native
history also aborts replay instead of publishing a context-less candidate. Visible
transcript history is not erased by either checkpoint kind.

Replacement is staged without modifying active input. For durable sessions a
checkpoint is a FIFO persistence barrier with a refcounted completion acknowledgment;
queue acceptance is not completion. Only a successful record write/fsync installs
the prepared history, preserves session cache affinity, and fires `AFTER_COMPACT`. Ephemeral
sessions commit in memory. Failed preparation, remote compaction, or persistence
leaves old history intact. After checkpoint submission cancellation stops further
work but settles against the write result: a written checkpoint is installed before
cancellation finishes. Force-cancel does not retire that pending commit; reset/close
invalidates its continuation without letting a late acknowledgment change a new
session. No extension callback runs on the writer. Pending checkpoint bytes share
the existing bounded per-session/host queue budgets and shutdown deadline.

Official OpenAI routes use native compaction by default; custom gateways retain
text compaction. There is no automatic text fallback or parallel portable summary.
State is scoped to provider/backend and OAuth account or effective-key fingerprint,
not to model/effort/Fast. OAuth token rotation within the same account is allowed.
Backend/model rejection surfaces explicitly without stripping state. API key
changes require restoring the old key or starting a new conversation. Credentials
are never checkpoint payload metadata. Transcript inspection does not require
compatible credentials.

`ON_COMPACT` may still supply an explicit text replacement; after successful commit
it removes opaque state and its restriction. The extension owns that summary's
adequacy. Synthetic briefings are marked `synthetic: true` in canonical user-shaped
history, so native retention cannot mistake them for genuine user input. Native
compaction excludes request-context hooks and reuses a compatible normal prompt
snapshot (or builds a normal prefix on fallback). Those instructions can influence
the artifact. Successful provider usage is counted once
regardless of a subsequent local write failure; replacement-window usage is unknown
until a later successful request measures it. See [hooks](hooks.md#on_compact) and
[context](context.md).

## Sessions and delegation

Session JSONL schema version 5 requires a `kind` in every header. Main and subagent headers also record the actual `cwd`, logical `project_path`, and whether that checkout is a linked worktree so catalog recovery preserves exact routing and project grouping. A subagent header also requires durable `profile` and `initial_purpose` metadata and may include `parent_session_id`. The header may include `title`, the `/resume` label; when it is absent Pico derives that label from the first user message. Saved sidebar rows use the same catalog title and activity-ordered pagination whether or not their agents are loaded; being selected does not exempt a saved session from pagination. Only unsaved drafts are prepended, newest first. Active and undismissed sessions remain independently available in Inbox mode with saved titles read on the catalog worker, even outside the project page or when the session is closed. Projects mode shows only the project tree, not an embedded Inbox. The clickable mode title preserves selection; its user setting survives restart. Inbox's top `+` clears UI selection and a pending UI session load, not live agents, drafts, or running work. Its no-selection chat picker lists non-stashed projects and reuses an empty idle draft or creates a main session. Each changed title appends a `title` event and rewrites the header `title` in one persistence operation; setting the existing title is a no-op. The builtin TODO state reconciles its task title after durable replay, so a process exit between the tool-result write and the next frame cannot leave the label stale. A background turn that finishes while the session is not selected appends `unseen_complete` with `complete` true; opening the session appends `complete` false. Listing reads the last such event (or the listing cache when the JSONL stat generation matches). Replay restores `unseen_complete` onto the live agent. A main session that finishes or stops a turn, or enters an error, appends `inbox_pending` with `pending` true. Cancellation marks it when the turn settles, not while tools are still draining. This flag retains idle sessions for review; it is not a run state. Inbox groups open questions/approvals and errors under Needs input, running provider/tool/compaction work under Working, and undismissed idle turns under Completed. Opening a session clears its unread marker but does not dismiss its Completed row. The Completed checkmark clears both `inbox_pending` and `unseen_complete`, including unread completions, but never acknowledges working, waiting, or error sessions and never changes run state. Closed-session dismissal appends both existing event types under one writer lock. Closed-session acknowledgements hide rows optimistically and queue durable writes on the same host persistence worker as live session events; later writes remain ordered after them. Pending state survives host-extension reload and is reconciled when a session is reopened. Failed rows return with a warning, without automatic retry, while successful rows in the batch stay cleared. Deletion refuses sessions with queued or in-flight acknowledgements, and shutdown shares the existing catalog-persistence deadline. A session with no `inbox_pending` event is not pending. Listing and replay use the last `inbox_pending` event the same way as `unseen_complete`. Session appends, title rewrites, and resume repair use the same per-session lock, excluding both other processes and other threads in this host. A rewrite uses a unique temporary file, syncs its contents, atomically replaces the session, and syncs the containing directory before reporting success. Replay does not dispatch title events. Every assistant-message and tool-call event carries a non-negative `message_group`; consecutive events with the same group restore into the same assistant bubble, while a changed group starts a new bubble. This preserves the live transcript independently of provider item boundaries. Assistant events may include `thinking_parts`, an ordered array of reasoning-summary steps whose final entry is the widget title. Events with raw `thinking` or structured `thinking_parts` may also include `thinking_ms` (elapsed time of that reasoning burst). Resume restores the steps and duration onto the think line so expanded summaries and duration labels survive. Pico does not replay older schemas. One workspace reservation owns each open session path, and failed replay releases the reservation without publishing an agent. UI session loads reserve after worker validation, replay into an unpublished candidate, and release that reservation on cancellation or failure; only a completed candidate replaces the old chat. Cancelling after callbacks have run does not roll back extension-owned state. Session directory resolution accepts the target workspace; it never reads UI selection or process CWD. A `PICO_SESSION_NEW` main agent has no file until the first JSONL write. Selecting another agent, or creating a selected main agent, closes that unused draft; assigning a session identity or writing the JSONL keeps the agent. Footer and `/model` `/effort` assign that identity on the settings call and enqueue session-directory creation plus the header/`model_change`/catalog write on a host persist thread so the UI can update immediately. Close and reset wait for queued work with a bounded timeout; session appends and title rewrites never wait — they queue behind earlier records for that session and land in call order. Host pump never waits; it observes checkpoint acknowledgments and applies completed failures, which move the affected agent to `PICO_SESSION_FAILED`. `pico_session_log_custom`, other session appends, and title rewrites are queued on the persist thread rather than written synchronously by the calling thread.

Session recency changes with conversation activity, not bookkeeping. Pico preserves the JSONL modification timestamp (including subsecond precision) for inbox/completion flags, read markers, title/model edits, token accounting, and extension custom-state writes. These updates remain durable and refresh catalog metadata without changing sidebar or `/resume` ordering or the displayed activity age. A queued batch containing conversation records still advances recency even when bookkeeping records surround them. Timestamp preservation happens under the session writer lock before file sync and catalog publication; title rewrites preserve it on the replacement file. Successful append rollback and resume-tail repair also preserve the preceding recency. File size, inode, and change time still invalidate stale catalog rows.

Latest-session resume orders candidates by modification time (including subsecond precision) and descending filename for exact ties, excludes subagents, and reuses the normal locked replay validator. It selects the newest recoverable main and warns on successful fallback past newer corrupt candidates. Explicit ID/path loads never fall back. A missing or child-only directory is empty main history; all-corrupt main history is an error. I/O/locking/allocation failures and failed required repairs stop selection rather than silently choosing older state. See [agents](agents.md).

Resume validates all complete JSONL records before recovery. Only a malformed final unterminated record is discarded, under the session writer lock, with truncation and file sync before continuation. A valid unterminated final record is retained and receives its missing delimiter. Invalid complete records, headers, and semantic record contracts remain errors. Catalog and transcript inspection are read-only. Async load lock waiting is cancellable; an already-performed repair remains on disk and its warning is delivered on the main thread even if that load was superseded. A failed sync after truncation reports the partial recovery and does not publish the candidate. Recovery does not invoke extension callbacks on a worker. See [agents](agents.md).

Partial streamed assistant text, raw thinking, and reasoning-summary steps are saved when a non-compaction provider request fails or is cancelled, except output explicitly discarded by a provider `RESET` before retry. Text and raw thinking are also retained in continuation history; failed results do not execute tool calls or contribute usage. Failed compaction output is not saved as an assistant message. Builtin Chat Completions token-limit (`length`) and content-filter termination fail the entire response; no calls from it execute, even if some argument fragments were already complete. Tool-call projection validates the entire argument batch as complete JSON objects before publishing any call, and absent arguments are not defaulted to `{}`. Partial streamed text follows the same failed-turn persistence contract, without automatic continuation. See [providers](providers.md).

Fresh named delegation never uses ambient `resume_last`. Continuation resolves an exact session ID, requires the stored and requested profile names to match, and reapplies current profile purpose/model/effort/tools after replay. Only a session whose header and delegated user event were durably written is returned as `resumable`; write failure preserves diagnostic identity but does not return a reusable ID in the delegation result.

Delegation jobs and child/session ownership survive force cancellation until every worker, workspace, and child reference releases them. Parent cancellation wakes the waiting generation immediately and requests child cancellation. A late child terminal event cannot publish to a replacement parent generation. A subagent and every descendant belong to the parent's workspace.

## Tool authorization

Ordinary agent requests start from registered tools, apply the agent policy, then LLM-hook exclusions. The builtin clarification helper instead uses its dedicated instructions and `sh` allowlist without ordinary LLM/context hooks (see Questionnaire UI ownership). Each LLM hook runs once per request on the main thread in registration order: it may exclude eligible tools and append instructions in the same invocation. Later hooks see preceding exclusions and previously appended instructions; only the final filtered catalog reaches context hooks and the provider. For a named child, the policy is the current profile's exact allowlist snapshot; omission allows all tools and an empty array allows none. Continued sessions refresh this policy instead of restoring an old catalog. Context hooks inspect the final effective catalog. The provider receives a retained snapshot, and tool execution/apply resolve only from that snapshot—not from the live registry. A hidden or unoffered call becomes a logged tool error and bypasses before hooks, tool code, apply, and after hooks. Empty/duplicate call IDs, malformed call arrays, and calls beyond the pending-call limit fail the provider round explicitly. Pending calls follow their retained execution policies: eligible calls overlap up to the turn's limit, sequential calls act as ordering barriers, and excess calls remain queued.

## Questionnaire UI ownership

The builtin custom questionnaire (`type: "questionnaire", ui: "custom"`) is a
non-modal composer replacement. `PicoUi_ModalOpen` excludes this surfaced ask,
but still includes named modal claims, confirmation asks, and other custom asks.
Ask IDs and answer ownership are unchanged. A pending confirmation from the
visible clarification helper takes UI priority, without resolving the original
questionnaire. Returning to answers restores normal selected-session ask ordering.
The ordinary composer yields rendering and input in questionnaire-answer mode;
in clarification mode it routes text to the isolated helper, not the selected agent;
extension views still stack in their registered slots.

Questionnaire drafts are host-owned copies keyed by ask ID, retained across
session/workspace selection, and discarded after the ask ends, its owner closes,
or host extensions are replaced. Borrowed `request_json` is never retained across
pumps. Collapsing does not answer or cancel; only the focused expanded panel
consumes question keyboard input, and named modals/chat search take precedence.
Questionnaire submission creates no extra transcript message beyond the tool result.

Clarification runtime ownership is core/workspace-owned, keyed by the ask owner,
runtime generation, and ask ID; UI mode and drafts never retain borrowed request
or transcript pointers. One ephemeral helper is retained per questionnaire, counts
against agent caps, and never publishes a result or transcript into its owner.
Back preserves drafts and does not cancel work. Escape in the clarification view
cancels only the helper.
Ask end/owner cancellation/workspace close cancels and drains the helper; stale
completion cannot reopen an ended conversation. Reload gates new helper turns
while continuing existing work, answer routing, and cancellation.

Clarification requests deliberately bypass ordinary submit, LLM, request-context,
and `ON_COMPACT` hooks to preserve their clean instruction/context contract.
Tool allowlisting and retained registration snapshots still govern execution;
tool before/after hooks, apply, and lifecycle notifications remain active.
Compaction restores the immutable questionnaire/task seed in the helper's history.
The `sh` inspection-only policy is prompt guidance, **not filesystem or process
isolation**. Helpers use distinct ephemeral session/cache identities and are not
saved, resumable, or selected user sessions.

## Successful LLM response notifications

`PICO_HOOK_ON_LLM_RESPONSE` is a main-thread notification registered through
`pico_host_add_hook` or `pico_workspace_add_hook` in the matching init callback.
It fires once for each accepted successful provider response after result
ingestion and usage recording, before tool execution or turn completion. Tool
follow-ups and compaction each produce their own notification;
failures and cancelled requests do not. Host callbacks run before the responding
agent's workspace callbacks, in registration order within each scope. Dispatch
uses the actual responding agent ID, not current UI selection.

`PicoHookEvent.llm_response` is borrowed for the callback and is set only for this
hook. Its `PicoLlmResponse` has `input_tokens` (zero if usage is unknown),
`cached_tokens` normalized to `[0, input_tokens]`, `compact`, and `first_response`.
The latter is true on the first accepted successful normal (non-compaction)
response in a session, even when usage is unavailable. This state is persisted in
session usage records and restored during replay; replay and extension reload do
not dispatch response hooks. Compaction does not consume the flag. Session reset
clears it; model changes do not. `PicoHost_AddNotice` is safe from this callback:
it runs on the main thread and notices are excluded from future provider context.

## Transcript notices

- `PicoHost_AddNotice` is main-thread-only and copies the Markdown and severity;
  the caller retains ownership of its input. Invalid IDs/severities are ignored.
- `PICO_ROLE_NOTICE` messages carry `notice_severity`. They are application
  feedback, never assistant/model content. `PicoHost_AddMessage` accepts only
  user and assistant roles; emit notices using the dedicated API.
- Notices fire `PICO_HOOK_ON_MESSAGE` after their role and severity are set.
  They persist for durable agents, restore in transcript order with severity,
  and remain excluded from provider history and compaction. Ephemeral agents
  do not write session records. A notice emitted during streaming is displayed
  immediately but written after the preceding assistant record is finalized.
- `PicoMessage.markdown_base_dir` is an optional core-owned absolute document
  directory. NULL uses the workspace; file-backed `/docs` notices copy and
  persist their original directory as notice metadata. Source text is unchanged.
  That origin survives streaming-deferred writes, snapshot copies, inspection,
  resume, and extension reload; later installations do not retarget saved text.
  Following messages revert to their own workspace default. Linked documents
  and examples open externally, not as another in-chat docs command.
- Snapshot notice text and metadata have the same borrowed lifetime as other
  transcript messages. No extension pointer or state is retained; reload does
  not remove notices already emitted. See [notices](notices.md).

## Steering

`pico_agent_steer` is main-thread-only and copies text/normalized parts for an
explicit active user-main target. `PICO_OK` means acceptance, not delivery or
confirmed persistence. Pending input is separate from transcript/provider
history; all queued messages deliver FIFO at a settled model/tool-batch boundary,
after compaction when applicable, without repinning turn settings or ending the
turn early. Child/clarification steering, idle starts, and worker calls are not
supported. Cancel/error/reload/close stop undelivered input immediately; session
replacement never transfers it. Restore and reload never resume the FIFO.

Composer `BEFORE_SUBMIT` includes `PicoHookEvent.submit_kind`; `ON_SUBMIT` remains
new-turn-only and `ON_STEER` is acceptance-only. Delivered history is committed
before message notifications, whose `PicoMessage.steering_id` identifies input.
Hook-generated arrivals wait for the next boundary. Error teardown refuses new
turns/close until its callbacks return. Queue inspection returns a copied
`PicoSteeringInfo` with borrowed read-only strings, invalidated by pumping,
mutation, replacement, or close. Pending count/bytes are bounded; rejection does
not discard accepted input. Session-local IDs are recovered on replay.

Acceptance uses ordered async persistence (ephemeral sessions skip disk);
`PICO_PERSISTENCE_FAILED` reports immediate write-path rejection. Later failures
use normal session-failure rules. Schema 5 distinguishes acceptance, canonical
delivered user records, and stopped records plus transcript-only notices. An
acceptance-only crash restores as not delivered, never model input. See
[steering](steering.md) for full eligibility, lifecycle, and ownership contracts.

## Image-bearing tool results

ABI 24 accepts original encoded PNG/JPEG/WebP tool results through
`PicoToolImage.data`, with `PicoProvider.tool_images` opt-in.
`pico_agent_context_accepts_tool_images` reports the immutable turn snapshot:
vision model AND capable retained provider. Unsupported results fail explicitly.
Core detects format from content, validates/decodes images, and persists them on
the worker; up to four images, 64 MiB encoded total, and 4096 × 4096 per image. Core owns all transferred
allocations and cleans unadopted/cancelled-generation assets. Accepted files live
at `<workspace>/.pico/media/{tools|browser}/<session-id>/`; only those two namespace
values are accepted, default `tools`. Parent-directory symlinks are refused.
Stored MIME/extensions match the detected content; original encoded bytes are
preserved without resizing, conversion, or metadata stripping. Animated WebP
validation decodes all frames, but persistence and provider projection preserve
the entire encoded animation; backend acceptance/frame handling is provider-
dependent. Transcript previews show the first frame. Canonical MIME must match
the actual file content when preparing a request.

The parallel-eligible builtin `view_image` resolves relative paths against the
calling agent's workspace and accepts absolute paths outside it, including
symlink targets. It reads regular local files only and uses the same image
limits/capability gate. Its per-request LLM hook excludes it unless the calling
agent's active model and retained provider both accept tool images; agent/profile
restrictions still apply, and the execution-time guard remains. In-flight turns
keep their pinned model and retained provider across follow-ups. A model change
between turns updates the catalog without re-registering the builtin.
Source files remain caller-owned; accepted tool images
are independent session-owned copies. See [images](images.md).

Canonical/session `tool_result.parts` contains image paths/MIME, never bytes.
Interrupted replay repairs every unanswered tool-call ID independently, preserving
completed image results even when siblings finished in reverse order.
Replay does not capture or act. Compaction drops summarized images from active
context but retains transcript/assets; saved-history deletion removes owned
files. Agents must never move, rename, delete, or modify Pico-owned `.pico/media/`
assets or their parent directories during cleanup. Core omits missing local
image parts (`ENOENT`/`ENOTDIR`) from request copies, across user/assistant/tool
history and normal/text/native-compaction requests. No omission warning is added
to model instructions or context. A user-only transcript warning is emitted once
per path per saved session; notice records
carry `missing_image_path` metadata so deduplication survives resume, independently
of editable notice wording. Original history/assets are untouched by recovery;
restoring a file includes it again (subject to ordinary compaction replacement).
Unreadable/corrupt images, malformed media, audio, and remote URLs retain their
existing failure/validation behavior. Providers still reject invalid media that
reaches them, including files removed after core's request-copy check. Expanded tool
rows and copied transcripts retain image references; `tool_parts_json` is
borrowed core storage with ordinary trace invalidation rules. After-tool text
replacement does not modify image ownership. Builtin adapters project real
images as labeled untrusted observation messages after sibling tool replies,
without adding canonical human input. See [tools](tools.md#image-bearing-results)
and [providers](providers.md#image-bearing-tool-results).

## Image generation

The OpenAI workspace builtin registers `generate_image`; its per-request auth
filter and execution-time guard require selected OpenAI OAuth credentials.
Agent/profile tool restrictions remain authoritative. It is a sequential,
callback-local tool worker, not a background task. Different agents may overlap;
it shares the provider's single-flight OAuth refresh coordinator and retries a
definite 401 once. Other image outcomes are not automatically replayed, and image
POSTs do not redirect credentials. No public registration/auth/HTTP ABI changes
are introduced.

An explicit new `.png` output path is required; relative paths use the calling
workspace. Parents must exist, owned `.pico/media` destinations are rejected,
and atomic publication never replaces an existing destination, including one
created while the request runs. Outputs are caller-owned. Optional local
references are uploaded explicitly, not taken implicitly from history. The
result contains original validated image bytes only when the turn accepts tool
images; otherwise it explicitly returns file metadata without a visual preview.
Core's accepted preview copies retain ordinary image-result ownership/lifetime.
Cancellation cannot refund remote usage or roll back an already published
caller-owned output file. See [image generation](image-generation.md) for limits,
request models, backend availability, and detailed cancellation behavior.

## Embedded browser ownership

The dock's web icon expands/collapses presentation without closing its page;
it is muted while collapsed and bright while expanded. The icon-only control
slider places Agent (robot) left and User right. Clicking a side or dragging
between sides selects that mode immediately, never toggles an already-selected
side, and retains the existing takeover/input-release behavior. Manual open
starts in User mode; agent open starts in Agent mode. Selector animation is
presentation-only and must not resize the toolbar or bottom-follow shell.

The expanded dock's left edge is a core-owned resize handle with a horizontal
resize cursor. A proximity spotlight is clipped to that border and remains lit
while resizing; it is presentation-only and does not enlarge the input target or
change shell geometry. The dock width is main-session-local presentation state,
retained through collapse/reopen and selection changes but never persisted. Expanded
width is bounded to 300 logical pixels minimum and half the right column maximum;
the maximum wins when space is narrower. Passive window/sidebar changes clamp
presentation without overwriting the preferred width. Toolbar controls wrap into
fixed-height rows when needed. A resize gesture owns pointer input through
physical release; Escape, focus loss, minimization, a modal, selection change, or
page/session close stops resizing without handing the held gesture to chat or
Clay drag-scrolling. Resizing the dock never starts Chromium.

The core browser service is lazy and per-main-session; subagents and clarification
helpers cannot use any browser tool, including diagnostics, inspection,
evaluation, or viewport control. Tools
validate calling identity/runtime generation, never target UI selection, and
serialize per session independently. Open returns at queued navigation;
observe requires an open browser, waits for load plus composited capture, and
works hidden or under User control. Act requires Agent control and a matching
browser/navigation/viewport identity; older observations within that identity
remain usable. Ordinary DOM/input changes are not detected as staleness.

Batches are validated before dispatch and use acknowledged CEF-native DevTools
input to the same embedded page, never OS input. Takeover/cancellation stops the
remaining events, reconciles an outstanding acknowledgment, and releases held
input; partial/uncertain delivery is reported without rollback or retry. Runtime
unload/close invalidates work and can report uncertainty for already dispatched
input. An unacknowledged input/release failure can close the page. Collapse and
selection alone do not cancel; actual resize/navigation invalidates coordinates.
Compositor synchronization precedes fresh OSR view/popup capture, not a sleep or
network-idle heuristic. Application readiness still requires visual verification.
An active observe/act request reissues missing paint demands once per second:
the initial current-size view before compositor synchronization, then only
still-missing view/shown-popup paints afterward, until completion or its original
deadline. Recovery never repeats input or the compositor
snapshot, reloads a page, changes frame rate, resets freshness or uses an old
image. CEF-UI-thread delayed tasks validate request, paint-wait generation,
navigation and viewport; cancellation, restart/loading/identity changes, finish
and retirement invalidate obsolete work. No retries run while idle, loading,
executing input or awaiting the compositor. This is paint recovery, not an
application-readiness wait or an automatic retry of tool side effects.

`browser_diagnostics` and `browser_inspect` (accessibility or selector-scoped DOM)
require an existing page, work in both control modes, and do not require vision.
`browser_evaluate` and `browser_viewport` require Agent control but not vision.
None of these tools creates a browser, selects a session, or expands its dock.
Evaluation can mutate page state and needs no per-call confirmation. Outstanding
evaluation is interrupted on cancellation/takeover by closing the page; effects
are not rolled back and must not be blindly retried. Read-only inspection uses
DevTools DOM/Accessibility methods rather than executing page JavaScript.

Optional browser-tool fields treat `null` as omission; required fields, including
action-specific `browser_act` fields, remain required and non-null. Prefer
preset-only or complete dimensions-only `browser_viewport` calls. A complete
valid dimension pair overrides any preset; incomplete dimensions fall back to a
valid preset's entire size. Invalid non-null dimensions never fall back. Argument
validation errors leave the viewport unchanged and explain the invalid fields.
See [browser](browser.md#viewport-control).

Diagnostics start before the initial requested navigation and append complete
collected records to private per-browser `/tmp` journals across navigation.
Reads use browser-scoped opaque cursors without consuming data. There is no ring
buffer/size eviction; failures explicitly report incomplete collection. Snapshot
copying uses a CEF file worker with a fixed journal end offset, not UI-thread bulk
copying. Resource callbacks only share the locked journal and atomic navigation
identity. Accepted observe/act requests always append capture-phase records to
that same journal, with request-scoped correlation, monotonic elapsed timings,
loading/visibility/viewport state, paint/compositor counters and input progress.
The request telemetry is CEF-UI-thread-owned, uses the existing locked journal,
and logs transitions/retry rounds rather than every frame/input event.
`paint_retry` records and the request-local `paint_retries` counter expose active
paint recovery; timeout summaries include that count. Telemetry adds no idle
periodic wakeups, URLs or page content, does not alter deadline/control semantics,
and retains no in-memory event history. Helper timeout errors also include a
compact current-state summary; an unresponsive helper/host watchdog can leave
only the last phase record, without a helper summary or finish. These timings
start at helper request acceptance, not host queueing or runtime startup. See
[browser capture diagnostics](browser.md#diagnosing-capture-timeouts).
Console remote-object descriptions are not deep object snapshots.
No request/response bodies, headers or cookies are collected. All page-derived
content and its files are untrusted, not instructions.

Large text results return complete `0600` temporary files with path and byte size,
not truncated previews. Returned files and journals survive browser close/process
exit until external temporary-file cleanup. They are not durable image assets;
replay never regenerates them. Host workers adopt/read files without holding the
browser service mutex; the receiver handles only bounded result metadata. Small
and cancelled/unadopted snapshots are cleaned up. Browser IPC remains private;
no public extension ABI or generic file-result API is added by these tools.

Viewport settings and remembered custom dimensions are live-browser-local,
survive navigation/collapse/extension reload, and reset on page retirement. The
human dropdown applies presets immediately; Custom opens a floating Width/Height
popover and applies valid dimensions via Enter or Apply. Invalid values leave the
current viewport unchanged. Dropdown/popover input stays browser-owned for the
entire input pass, including after Enter or Escape closes it and through same-frame
relayouts, so the same key cannot submit the composer or cancel an agent turn.
The popovers do not resize the toolbar, browser surface, or bottom-follow shell.
Presets/custom sizes change dimensions, not device emulation. Fixed pages fit down in the dock with centered letterboxing;
images/agent coordinates retain full logical dimensions and human input maps
through the displayed image, excluding margins. Dock resize alone leaves fixed
page coordinates valid. Only actual logical size changes invalidate identity.
Full-height shell panes remain exactly parent-bounded through repeated layouts.

Helper failure stops all pages owned by that runtime; renderer termination stops
and closes each affected page without stopping independent sessions. The dock
and tools distinguish exit status/signal, transport/protocol failure, queue
exhaustion, and renderer cause where available. Close/reopen is explicit; there
is no automatic recreation, navigation replay, or action/evaluation retry. Pending
mutating effects can be uncertain. Fail-closed queue/transport handling latches
before any subsequent queue flush, and preserves bounded process-group cleanup
even after the helper leader is reaped. Runtime/page initialization have bounded
readiness watchdogs, separate from application readiness. Opt-in private resource
metrics are aggregate host-owned data, not a public extension surface; they do
not change page lifetime or enforce CPU/RAM budgets.

The runtime is not extension-owned and survives extension reload. Live pages end
on explicit close or session-runtime unload; cookie/local-storage profiles
persist by durable session ID. Replay/resume alone never recreates a page.
Hidden pages keep executing at reduced paint cadence and do not retain Pico
display textures. Continuous presentation is GPU-only: callback-borrowed CEF
DMA-BUFs are copied and completed inside the callback, never retained by fd
duplication alone. A bounded helper-owned pool shares descriptors and acquire
fences; Pico flushes its batched sampling and returns a release fence plus the
matching slot/serial acknowledgment before reuse. Resize cannot overwrite leased
storage. Dropped frames return their untouched acquire fence. Native Ozone and
EGL/DMA-BUF/native-fence driver support are required on Wayland and X11, with
explicit failures rather than a CPU fallback. Only requested observations read
back fresh GPU view/popup composition and encode PNGs. CEF/tool
workers never touch Clay or Raylib; the graphics thread owns display textures
and retains retired render data through the current layout lifetime. The IPC and
DevTools channel are private, with no remote listener. Sandbox protection stays
enabled. See [browser](browser.md) for schemas, readiness, limits, singleton-root
ownership, storage, and tests.
