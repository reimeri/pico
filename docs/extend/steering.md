# Steering

Steering is durable user input queued for an active user-main agent. It does not
cancel work or start a second turn. Composer Enter sends a normal turn while
idle and queues steering while busy; Shift+Enter remains newline. Pending input
appears in a separate section inside chat. Delivery creates ordinary user
bubbles; stopped input remains visible as **Steering not delivered** warnings.
There are no queue edit/remove or automatic retry controls.

## Explicit target API

```c
PicoResult result = pico_agent_steer(host, agent_id, "Prefer the smaller change", NULL);
```

Call on the main thread. The target is explicit and never follows UI selection.
Pico copies text and canonical `parts_json`; callers retain their buffers.
Parts replace the one-text-part model item, so include the complete model-facing
text in the parts array. Supported parts and local-file ownership follow
[hooks](hooks.md#before_submit). Parts-only input derives a display from its text
and media paths. Composer steering also supports existing pasted attachments
and `@file` preprocessing. The explicit API does not run composer hooks or read
composer state, matching `pico_agent_submit`.

Results:

- `PICO_OK`: accepted into the pending FIFO, not delivered or disk-confirmed.
- `PICO_NOT_FOUND`: stale target ID.
- `PICO_INVALID`: missing host, non-user-main target, empty/malformed input, or
  media unsupported by the running model.
- `PICO_BUSY`: idle/error/cancelling agent, pending session replacement or replay,
  or workspace that is reloading/closing/not accepting external work.
- `PICO_LIMIT`: pending message/byte budget exhausted or message IDs exhausted.
- `PICO_NO_MEMORY`: input could not be prepared or copied.
- `PICO_PERSISTENCE_FAILED`: acceptance could not enter the persistence path.

Main agents performing manual or automatic compaction are eligible. Delegated
children and internal clarification helpers are not. Parent steering never
propagates to children. A tool waiting on a questionnaire is still part of the
batch: steering can queue, but it does not answer or bypass the ask. Clarification
composer input retains its existing routing.

## Delivery and lifecycle

Finish the in-flight provider response and its **entire tool batch** before
appending all pending messages separately, in FIFO order, to canonical model
history. Tool calls and their results remain contiguous with respect to new user
input. A final answer with pending steering continues the same turn with another
request. Compaction applies its summary first, then appends pending steering
verbatim. All continued requests use the turn's pinned model, effort, Fast mode,
and registration snapshot; next-turn setting changes do not affect steering or
attachment capability checks.

**Delivered** means appended to canonical model history. It does not promise
provider receipt, a successful request, or execution of the instructions. A
provider-start failure after delivery leaves the user message delivered.
`ON_TURN_END` fires only when the extended turn actually finishes.

Cancel/force-cancel, error, reload request, or workspace close immediately stop
pending delivery. Successful session replacement/agent close cannot transfer the
queue into another session. Already-delivered input is preserved. Pending input
becomes a transcript-only warning containing the original display content;
neither that warning nor pending records enter provider history or compaction.
Session restore never reconstructs a live FIFO or automatically runs input. An
acceptance without a delivered/stopped event restores as not delivered.

## Hooks and inspection

Composer preprocessing runs `BEFORE_SUBMIT` once on acceptance with
`event->submit_kind == PICO_SUBMIT_STEERING`; idle turn submission uses
`PICO_SUBMIT_TURN`. Both use the existing cancel/input/parts setters. Registered
slash commands retain their explicit busy opt-in restrictions. Unknown
slash-prefixed text remains ordinary input. `ON_SUBMIT` remains turn-start-only.
`ON_STEER` fires once for accepted input from either the composer or explicit API,
with the steering kind. It is not emitted by replay and is not a delivery signal.

At delivery, history and the durable user event are committed before
`ON_MESSAGE`. `PicoMessage.steering_id` identifies delivered user input and stopped
warning notices (zero on ordinary messages). A callback cancelling the turn
preserves the delivered message and stops the remainder. Steering accepted by a
delivery callback waits for the **next** boundary; it cannot recursively extend
the current drain. New turns and close return busy while error teardown is
emitting its notifications; retry after the callback returns.

`PicoAgentInfo.pending_steering` is copied with the agent snapshot. For an extension
view, use `pico_agent_steering_count(host, id)` and
`pico_agent_steering_info(host, id, index, &info)`. `PicoSteeringInfo` is copied,
but its text/display/parts pointers are borrowed, main-thread-only, read-only,
and invalidated by pumping, queue mutation, replacement, or close. IDs are
positive session-local integers, restored so newly accepted IDs are never reused
within that session. No worker-facing steering API is provided.

## Persistence and limits

Acceptance records enter the existing asynchronous session writer immediately.
`OK` means queued for persistence, not fsync complete. Records for a session land
in call order; later disk failures use the normal persistence-failed warning and
resumability rules. Ephemeral sessions keep steering in memory. Synchronous
acceptance failure preserves the composer text and attachments.

Schema 5 stores `steering` acceptance events with complete payload and string
`steering_id`; delivery is a normal user `message` with that ID;
`steering_stopped` records the terminal reason. The accompanying warning `notice`
uses the same ID and may be deferred until a streaming assistant record is saved
so live and restored transcript ordering match. Missing warning records after an
interruption are recovered at EOF. Replay reconciles across the whole session,
including acceptance before and delivery after compaction. Read-only transcript
snapshots follow the same rules; extension-reload tool-detail restoration neither
requeues messages nor repeats acceptance notifications.

Pending count and copied payload bytes are bounded by
`PICO_MAX_STEERING_MESSAGES` and `PICO_MAX_STEERING_BYTES`. Limits reject the new
message without replacing or discarding accepted input.

See [contracts](contracts.md) and [the example](../../examples/steering.c).
