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

Durable agents save notices as dedicated session events and restore their order
and severity, including in session inspection. Ephemeral agents keep them only in
memory. Notices never enter provider conversation history or compaction input.
During a streamed response, a notice appears immediately; its session write waits
until the preceding assistant response is saved or the stream ends. A provider
failure preserves partial output and adds a separate Error notice.

`PicoHost_AddMessage` remains for user/assistant transcript content and does not
persist that content or add provider history by itself. It does not accept the
notice role: use `PicoHost_AddNotice` to supply severity and persistence together.
See [contracts](contracts.md) for thread and ownership rules.
