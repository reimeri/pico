# Transcript notices

Provider failures, command feedback, and status messages are application text,
not assistant-authored output. Emit it with:

```c
PicoHost_AddNotice(host, agent_id, PICO_NOTICE_INFO, "Reloading extensions…");
PicoHost_AddNotice(host, agent_id, PICO_NOTICE_WARNING, "No API key configured.");
PicoHost_AddNotice(host, agent_id, PICO_NOTICE_ERROR, "Request failed.");
```

Call on the main thread with a live agent ID. The host copies the Markdown and
severity before returning. Invalid agent IDs or severities are ignored. Notices
have `PICO_ROLE_NOTICE` and `notice_severity` in transcript snapshots; they render
as labeled blocks (red Error, amber Warning, informational Info). Markdown links,
selection, copying, and chat search remain available. The severity label is UI
chrome, not part of the copied body.

Ordinary notices resolve relative image paths and file links against the agent's
workspace. File-backed builtin `/docs` notices additionally carry
`PicoMessage.markdown_base_dir`: the copied absolute directory of the document
originally loaded. That origin is core-owned metadata, not part of visible or
copied Markdown. It survives transcript copies, session inspection, resume, and
reload. A later Pico installation does not retarget saved notices to newer docs;
if the original files disappear, their links can no longer open. Links to other
documentation pages and examples open externally in the system's default
application, not as another `/docs` message. See
[Markdown paths and links](views.md#markdown-paths-and-links).

Durable agents save notices as dedicated session events and restore their order
and severity, including in session inspection. Ephemeral agents keep them only in
memory. Notices never enter provider conversation history or compaction input.
During a streamed response, a notice appears immediately; its session write waits
until the preceding assistant response is saved or the stream ends. A provider
failure preserves partial output and adds a separate Error notice.

`PicoHost_AddMessage` remains for user/assistant transcript content and does not
persist that content or add provider history by itself. It does not accept the
notice role: use `PicoHost_AddNotice` to supply severity and persistence together.

The opt-in host builtin `cache_miss` demonstrates response-boundary feedback: it
adds an informational notice to the responding main agent's chat for each later
normal response with at least 10,000 uncached input tokens, regardless of cache-hit
percentage. The threshold is fixed, not configurable. It skips the first
successful normal response in each session, compaction, subagents, builtin
clarification helpers, and unknown usage. Enable it in `/extensions` or with F2.
It is off by default; user-global
`enabled_host_extensions` opts it in and `disabled_host_extensions` takes
precedence. See [host preferences](host.md#preferences) and
[LLM response notifications](hooks.md#llm-response-notification).

See [contracts](contracts.md) for thread and ownership rules.
