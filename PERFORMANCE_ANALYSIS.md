# Pico performance analysis

## Scope and evidence

- **Revision reviewed:** `d3c6d83c2cd973ccc194b68a2db859cd0ea0862f` (Pico 0.3.5).
- **Review date:** 2026-09-28, using the workspace clock.
- **Scope:** UI/layout/text, agent scheduling and streaming, providers/HTTP, session storage, filesystem tools, extensions, background work, startup, and the development build.
- **Method:** source and call-path review, independent subsystem reviews, cross-checking existing caches/limits/thread boundaries, and running the existing Release build and local test suite inside `nix develop`.
- **Not performed:** live provider requests, GUI/GPU profiling, cold-start timing, new benchmark implementations, or production-code changes.

**Follow-up:** F1 (synchronous title rewrite on the frame thread) has been implemented after this review. Line numbers in unfixed findings still refer to the revision above.

**Important distinction:** the timings below are measured. The individual findings establish source-level work, blocking, or resource-growth risks; their user-visible cost has **not** been measured in a representative interactive session. Priorities are an engineering triage recommendation, not a profiler ranking. Source links and line numbers refer to the revision above.

## Executive summary

Pico already has substantial performance engineering: invalidation-driven layout/presentation, worker-based provider execution, transcript and text virtualization, retained measurement/highlight caches, asynchronous session loading, and bounded parallel tools. Replacing these systems wholesale is not justified.

The most valuable next work is to close gaps around those optimizations:

1. **Remove remaining synchronous catalog scans from the UI thread (F4).** Title rewrites now queue on the persist worker like session appends (F1). Sidebar reconciliation still does disk work synchronously.
2. **Make long-stream updates proportional to new work.** The runtime repeatedly scans growing text, reparses the whole Markdown document, and discards document-local caches. Throttling helps frequency, not growth in work per update.
3. **Finish transcript virtualization at the bookkeeping layer.** Message bodies are virtualized, but planning/revision/height operations still scan history; anchor queries repeatedly sum prefixes.
4. **Avoid unchanged input and background work.** Completion queries recur without edits, composer wrapping repeats across UI phases, skill discovery can run twice per request, and diff workers rebuild models before discovering nothing changed.
5. **Bound bytes as well as item counts.** Replay, queued persistence, diff capture, and shell spooling have memory or disk costs that are not bounded by their visible row/job limits.

### Priority map

**P1:** address first, especially UI blocking or long-session scaling. **P2:** meaningful targeted improvement; measure the indicated workload before a large redesign. **P3:** lower urgency/developer efficiency.

| ID | Priority | Issue | Primary trigger | Evidence confidence |
|---|---|---|---|---|
| F1 | ~~P1~~ **addressed** | Title persistence no longer blocks the frame thread | TODO task-title change; slow/contended storage | High |
| F2 | P1 | Growing stream text is scanned/reparsed repeatedly | Long streamed answers, especially rich Markdown | High |
| F3 | P1 | Virtualization still performs history-wide bookkeeping | Large history, scrolling, streaming, anchor preservation | High |
| F4 | P1 for large catalogs | Sidebar reconciliation performs synchronous storage work | Startup, catalog changes, periodic reconciliation | High |
| F5 | P2 | Completion rescans unchanged queries; token creation walks disk | Active `@` token in a large workspace | High; allocator impact conditional |
| F6 | P2 | Composer wrapping repeats across UI phases | Long pasted drafts; repeated redraws | High |
| F7 | P2 | Skills are rediscovered redundantly during request preparation | Requests offering `use_skill` | High |
| F8 | P2 | Diff polling pays full capture cost before deduplication | Dirty/untracked repositories; multiple workspaces | High |
| F9 | P2 | Replay retains transcript-sized temporary representations | Large saved sessions | High |
| F10 | P2 | Persistence coalescing has growing-prefix work and no byte cap | Slow writer plus bursty logging | High; copying impact conditional |
| F11 | P2 | HTTP requests discard connection state between rounds | Repeated short calls/tool follow-ups | High on lifecycle; latency impact unmeasured |
| F12 | P2 | Shell output cap does not cap disk spooling | Noisy/long-running shell commands | High |
| F13 | P3 | Build graph repeats work unnecessarily | No-op builds, relinks, broad test builds | High; unchanged build observed |

## Validation performed

### Environment and commands

Linux x86-64, AMD Ryzen 7 5800X3D, 16 logical CPUs, approximately 31.3 GiB RAM; GCC 15.3.0, CMake 4.3.4, Ninja 1.13.2 from the Nix development environment. Builds used four parallel jobs; tests ran serially. The machine was not isolated for benchmarking.

```sh
nix develop -c bash -lc '
  set -e
  export LC_ALL=C
  TIMEFORMAT="elapsed=%3R user=%3U sys=%3S"
  time cmake -S app --preset release \
    -DFETCHCONTENT_SOURCE_DIR_RAYLIB="$FETCHCONTENT_SOURCE_DIR_RAYLIB"
  time cmake --build app/build/release --parallel 4
  time ctest --test-dir app/build/release --output-on-failure -j 1
  time cmake --build app/build/release --parallel 4
'
```

| Check | Result | Observed wall time |
|---|---|---:|
| Release configuration | Passed | 0.417 s |
| Build in existing Release directory | Passed; 389 scheduled build steps | 45.340 s |
| Existing CTest suite | **43/43 passed** | 108.10 s (CTest total) |
| Immediate unchanged build | Runtime-file target still ran | 0.145 s |

These are single-run observations, **not clean-build benchmarks or app latency measurements**. Reconfiguration pointed Raylib at the Nix-provided source, and the pre-existing build state influenced the work scheduled. The unchanged-build result supports F13, but 0.145 s is the total build-command duration, not isolated overhead attributable to the runtime-file target.

Selected test durations: `pico_host_workspace_tests` 43.27 s, `pico_http_tests` 33.04 s, `pico_session_usage_tests` 15.34 s, `pico_agent_behavior_tests` 8.01 s. These suites include intentional waits, retries, timeout and lifecycle scenarios. In particular, HTTP tests use loopback fixtures; their duration is **not provider request latency**.

### Existing performance protections actually exercised

There is no dedicated, repeatable performance benchmark target in the reviewed CMake configuration, but it would be incorrect to say there are no performance tests:

- [`TestIdleFrameOnlyPresentsOnInvalidation`](app/tests/host_workspace_test.c#L11935) verifies that idle pumps continue while unchanged layout and presentation are skipped.
- [`TestStreamingReparseDebounced`](app/tests/host_workspace_test.c#L9241) verifies deferred mid-stream rendering and a complete final document.
- [`TestExpandedThinkRenderingIsCached`](app/tests/host_workspace_test.c#L12392) checks retained parsing/measurement behavior and cache invalidation.
- [`TestAsyncReplayLargeMessage`](app/tests/host_workspace_test.c#L10253) replays a 256 KiB message plus 150 fragments and rejects a pump exceeding 100 ms in that fixture. It does not measure peak RSS or cover arbitrary record sizes.
- [`TestHugeUnspacedWordWrapsInsideBudget`](app/tests/md_scroll_layout_test.c#L395) exercises a 256 KiB token with a 75 ms guard and a test measurement function. This is not a real-font/GPU benchmark.
- [`TestBottomFollowShellGeometryStable`](app/tests/host_workspace_test.c#L2111) protects repeated shell-layout geometry. Virtualization, scroll, diff, tool-concurrency and persistence correctness suites also passed.
- [`TestTitleRewriteDoesNotBlockOtherWorkspace`](app/tests/host_workspace_test.c) holds a session lock while a title rewrite is pending and checks that another workspace can still pump and accept cancellation.

## Detailed findings

### F1 — Session title changes can block the entire UI — **addressed**

**Original path (reviewed revision):** [`TodoWorkspaceOnFrame`](app/builtins/todo.c) → [`PicoSession_LogTitle`](app/session.c).

The TODO builtin still reconciles its task title during a main-thread frame callback, but `PicoSession_LogTitle` now enqueues on the persist worker when that worker is running. The calling thread no longer drains, takes the session lock, copies the transcript, or fsyncs. Distinct titles are not coalesced: each accepted change is its own header rewrite plus `title` event, stored as a continuation on that session's pending persist slot so a title burst cannot fill the global job queue. Completion and failure publish through the existing persist pump. Close/reset still drain queued work with a bound. Tests without a persist thread keep the synchronous rewrite fallback.

**Historical issue:** title persistence drained queued writes with a one-second deadline, then acquired a blocking `fcntl(..., F_SETLKW, ...)` lock and copied/fsynced the transcript on the frame thread. The drain deadline did not bound lock wait or rewrite I/O.

**Remaining measurement:** rewrite throughput versus transcript size is still unmeasured. Responsiveness is covered by `TestTitleRewriteDoesNotBlockOtherWorkspace` (lock held, other workspace pumps and accepts cancellation) and queued FIFO/failure tests in `session_usage_test.c`.

### F2 — Streaming performs growing-prefix work and invalidates Markdown caches

**Evidence:** [`AppendMessageText` and `ReparseMessage`](app/agent.c#L1754-L1805), [`PicoAgent_PumpBounded`](app/agent.c#L4471-L4537), [`MessageRevision`](app/builtins/chat.c#L2128-L2181).

There are three compounding costs:

- Each drained text batch calls `strlen` over the accumulated message and `realloc` to the exact new length. For K similarly sized batches ending at B bytes, cumulative length scanning is O(B × K); for fixed-size batches this is quadratic in final size. Reallocation **may** add the same growing-prefix copying, but in-place growth can avoid that copying.
- A dirty stream is reparsed at most every 100 ms, but each reparse frees and rebuilds the entire Markdown document. This destroys its wrapping/highlighting caches, so rendering subsequently repopulates affected caches. At a roughly constant stream rate, cumulative parsing still grows quadratically with response length; the timer reduces the coefficient.
- Each transcript layout hashes the full source of the last message, plus relevant last-message trace text, to detect in-place changes. This happens even when that last message has finished and its content is unchanged. It is **not** a full-source hash of every historical message.

**Existing protection:** provider-side accumulation uses capacity-aware buffers ([`DeltaCb`](app/agent.c#L786-L800)); stream parsing is throttled and the final document is flushed. Ordinary layouts are invalidation-gated. The issue is the cost of an update, not an assertion that all idle frames reparse text.

**Recommendation:** track message length/capacity and explicit content/layout revisions; retain finalized Markdown work where safe. Markdown can reinterpret earlier text after later input, so an incremental parser must identify safe reparse boundaries rather than assume only the final line changes. If parsing moves to a worker, use owned immutable snapshots and reject stale results; do not share mutable document arenas with rendering. Background agents should not need immediate display-document rebuilding merely to retain their text.

**Validation:** keep debounce/final-output tests. Measure pump time, parsed bytes, cache rebuild work and allocation volume for increasing response sizes and independently varying chunk/pump cadence. Include prose, code fences, tables and Unicode; measure total pump time with multiple streaming agents, not just md4c parsing. The source comment about parsing “~1 MB in ~1 ms” is not evidence for end-to-end frame cost.

### F3 — Transcript virtualization leaves linear scans and repeated prefix sums

**Evidence:** [`RenderTranscript`](app/builtins/chat.c#L2307-L2364), [`HarvestTranscriptHeights`](app/builtins/chat.c#L2382-L2414), [`PicoTranscriptVirtual_Begin/Plan`](app/transcript_virtual.c#L69-L205), [`SpanHeight/AnchorDelta`](app/transcript_virtual.c#L217-L262).

For each layout, Pico clears a full mounting array, calculates revisions across messages/traces, walks all message heights to choose visible rows, searches for dirty background rows, and walks message indices again to emit content/spacers. Height harvesting also scans all entries. With N messages and T trace entries, bookkeeping remains at least O(N + T), despite mounting only a small portion of the document.

When preserving a non-bottom scroll anchor, each mounted row's `AnchorDelta` computes its prefix height from index zero. With V mounted rows near the end of history, that adds O(V × N) height work. The prefix is calculated even if the eventual height delta is zero. Bottom-follow mode skips this anchor calculation; it still incurs the other scans.

The 32-row offscreen measurement batch bounds additional **message count**, not scan length or text/CPU cost per row. Dirty history can take many layouts to settle. Same-frame stabilization and scroll correction can multiply layout work ([`PicoHost_LayoutShell`](app/app.c#L3537-L3548), [relayout path](app/app.c#L4034-L4084)).

**Recommendation:** maintain explicit message/trace revisions and an indexed height structure. Plan and harvest the visible/forced range plus a bounded dirty queue, rather than repeatedly scanning N entries. A prefix-sum structure or Fenwick tree can make range-height and anchor queries cheap. Consider work/byte budgeting for unusually large offscreen rows as well as row counts.

**Validation:** benchmark history size independently of viewport and visible content size. Cover top/middle/bottom, stationary redraws, streaming, width/font changes, selection and search-forced mounting. Preserve visible behavior, not private data structures. Any shell nesting/viewport change must extend `TestBottomFollowShellGeometryStable` and run the repository-mandated debug host-workspace suite. Do not replace exact structural heights with vertical `FIT`/`GROW` as an optimization.

### F4 — Catalog refresh still performs potentially large synchronous I/O

**Path:** [`SidebarOnFrame`](app/builtins/sidebar.c#L2158-L2173) → [`SidebarRefresh`](app/builtins/sidebar.c#L320-L371) → catalog scan → [`ListSessionsInDir`](app/session.c#L474-L542).

The sidebar uses change tokens and a periodic reconciliation, which is an important optimization: it does **not** reread every session on every frame or every 0.5-second token poll. However, when a refresh is required, the full scan is synchronous on the UI thread. Initial discovery, dirty state, token changes and the 60-second reconciliation can trigger it.

Several costs remain:

- Every candidate JSONL is statted. Valid cache generations avoid parsing; misses call `ScanSessionFile`.
- Each file searches a linear cached-session array ([`CatalogFindSession`](app/session.c#L3625-L3640)). Lookup work is O(F × C), where F is file count and C is cached count.
- The cache holds **at most 256 sessions per checkout** ([limits](app/session.h#L31-L32), [scan caller](app/session.c#L4250-L4275)), while directory scanning and sorting occur before applying the result limit. Older files outside that cache can be reparsed again on subsequent refreshes. This is not unbounded O(F²) cache lookup: C is capped, but uncached file I/O keeps growing.
- Catalog metadata iteration uses `JsonArrayAt` from the beginning for each element ([catalog parser](app/session.c#L3730-L3736), [JSON helper](app/json.c#L689-L700)), adding O(C²) token traversal within the bounded cache.
- Catalog loading acquires an inter-process lock before scanning ([`CatalogScanDir`](app/session.c#L4240-L4275)); moving ordinary appends off-thread does not make this reader path nonblocking.

**Recommendation:** build immutable catalog snapshots on a worker and adopt them by generation. Retain change-token and cross-process correctness. Use indexed ID lookup and one-pass JSON traversal. Separate the visible/listing limit from metadata coverage so older files do not become permanent cache misses. Optimize hashing/lookups after removing the UI-thread storage dependency.

**Validation:** exercise initial/warm/changed scans with histories below and above the cache limit, multiple checkout catalogs, missing paths, and contended catalog locks. Assert that input/pumping remains responsive during a blocked scan and that external changes still become visible. Retain `TestCatalogListingCache`, `TestSessionListCompleteness`, and `TestSidebarCatalogChangeToken`.

### F5 — File completion repeats queries without edits and rebuilds on token creation

**Evidence:** [`ComposerFrame`](app/builtins/composer.c#L2415-L2435), [unconditional eligible refresh](app/builtins/composer.c#L1810), [`PicoComplete_Refresh`](app/builtins/complete.c#L258-L300), [`pico_files_complete`](app/builtins/files.c#L184-L253).

On ordinary eligible composer pumps, completion refresh invokes the selected completer even when the prefix did not change. For an active `@` token, sparse/no-match searches can inspect the cached file list twice. It stops early when enough results exist; it does not necessarily scan the whole index on every query.

A new eligible file-completion token triggers a synchronous filesystem rebuild with recursive `stat`/directory traversal. The per-token snapshot avoids repeated disk scans while typing within the same token, but does not cache query results. The index is capped at 8,000 files, depth at 12, and common generated/hidden directories are skipped. These bounds help but do not bound storage latency or all directory entries examined.

[`FilesAdd`](app/builtins/files.c#L61-L77) also reallocates its pointer array for each file. Worst-case pointer copying is quadratic if allocations move; the allocator may grow in place, so the filesystem walk is the more certain problem.

**Recommendation:** reuse results while token/prefix/workspace/index generation are unchanged; grow the pointer array geometrically. Prepare the index off-thread, preserving the existing stable-within-token snapshot semantics. Debounce or incremental search is secondary to eliminating identical repeated queries and blocking rebuilds.

**Validation:** retain token snapshot behavior in `files_test.c`. Measure first-token latency and no-match/rare-match query cost near the supported index size. Include an unchanged active token while another workspace streams, because composer callbacks run before layout's idle gate.

### F6 — Long drafts are wrapped repeatedly, bypassing the measurement cache

[`WrapComposer`](app/builtins/composer.c#L95-L177) measures individual UTF-8 codepoints with direct `MeasureTextEx` calls. This bypasses Pico's short-string measurement cache ([`Pico_MeasureTextUtf8`](app/theme.c#L425-L436)).

Independent paths wrap the draft to compute render height, after-layout geometry, and drawn selection/caret overlays: [`PicoComposer_Render`](app/builtins/composer.c#L2088-L2102), [`GetComposerView`](app/builtins/composer.c#L1187-L1220), [`DrawOverlay`](app/builtins/composer.c#L2234-L2243), [`ComposerAfterLayout`](app/builtins/composer.c#L2338-L2349). The wrapped-line limit bounds how much is processed, but there is no retained wrap result shared by these paths.

When optional spell checking is active, unchanged draws also compare the full field using `memcmp`, and edits recheck/copy the whole field ([spell cache](app/builtins/spell.c#L369-L421)). Dictionary calls are already cached when content is unchanged; they are not unconditionally repeated on every draw.

**Recommendation:** share line breaks/advances keyed by text revision, wrap width, font identity/generation and scale. Use them consistently for rendering, caret movement, hit testing, selection and scroll calculations. Prefer edit-generation invalidation for spell checking and later consider checking edited regions. Routing whole lines through the short-string cache alone will not solve the repeated wrapping.

**Validation:** compare unchanged redraws and single edits for long prose, Unicode, long tokens and drafts exceeding the visible composer height. Preserve selection, caret and width behavior with `composer_test.c`, `wrapped_text_test.c` and `spell_test.c`; benchmark with real fonts as well as test measurement functions.

### F7 — Skills discovery can run twice for one provider request

[`QueueLlm`](app/agent.c#L1523-L1553) calls `RunLlmHooks` before handing work to the provider worker. [`RunLlmHooks`](app/agent.c#L1000-L1063) invokes each hook once for filtering and again for instructions. On a non-compaction request where `use_skill` is offered, both invocations of [`SkillsLlmHook`](app/builtins/skills.c#L178-L203) call `SkillsRescan`.

The rescan clears/rebuilds a catalog from four roots while holding its mutex ([`SkillsRescan`](app/builtins/skills.c#L47-L79)). Eligible immediate child directories cause `SKILL.md` reads and parsing ([loader](app/builtins/skill_load.c#L987-L1040), [scanner](app/builtins/skill_load.c#L1067-L1153)). Each accepted-size file is read in full by `ReadFileLimited`; only metadata is included in the normal prompt, not all skill bodies. The 64-skill catalog limit and 512 KiB per-file limit do not stop scanning/parsing further candidates before discarding excess entries.

**Impact:** avoidable main-thread disk work before first-byte latency on initial requests and tool follow-ups. Small local catalogs may make it cheap; slow/shared storage or many skill directories amplify it.

**Recommendation:** refresh once per request/discovery generation and let both hook phases use the same immutable snapshot. Cache unchanged file metadata/content, with background refresh if necessary. Preserve discovery of new skills without an extension reload. If the solution changes public hook phases or callback contracts, update the extension hook documentation and `contracts.md`; do not silently change the API's meaning.

**Validation:** use a fake provider and local skill directories to measure request-preparation latency across repeated unchanged requests. Keep precedence, exclusion/allowlist, prompt metadata and dynamic discovery tests in the skills and host-workspace suites.

### F8 — Idle diff workers fully capture before checking whether anything changed

Every enabled diff workspace instance starts a worker at initialization ([`DiffWorkspaceInit`](app/builtins/diff.c#L788-L802)). [`DiffThreadMain`](app/builtins/diff.c#L77-L150) captures a model, compares its signature, then waits two seconds before repeating. It does not require the diff modal to be open or the workspace to be selected.

A normal repository capture runs five Git commands: repository detection, HEAD detection, numstat, unified patch, and untracked-file enumeration ([capture](app/diff_model.c#L599-L641), [Git helpers](app/diff_model.c#L352-L379), [untracked enumeration](app/diff_model.c#L582-L595)). Eligible untracked files are reread and converted into rows. Only **after** that work does signature equality skip highlighting/publication.

**Impact:** repeated Git process startup, repository traversal, patch allocation/parsing and file reads while visually idle. With W open repositories and fast captures, the default cadence implies roughly 5W Git commands per two-second interval; actual cadence is slower by capture duration. `GitRun` grows its output buffer without a total-byte ceiling ([implementation](app/diff_model.c#L294-L349)). Many individually small untracked files also have no aggregate content budget.

**Existing protection:** this is worker work, not normally direct UI blocking. Per-untracked-file content is limited to 1 MiB, highlighting has its own size cap, unchanged models avoid rehighlighting, rendering is virtualized, and shutdown does not wait for a blocked Git subprocess.

**Recommendation:** separate inexpensive footer/status freshness from full patch materialization. Load detailed patches when needed, cache per-file generations, and back off unchanged/background workspaces while maintaining reasonable freshness for external edits. Add aggregate bytes/rows and capture timeout/cancellation handling; show an explicit partial-result state instead of exhausting memory. Keep a fallback reconciliation if watchers are used.

**Validation:** measure idle CPU, child-process count, bytes read and peak RSS for clean, dirty and untracked-heavy repositories with one and multiple workspaces; repeat with the modal closed/open. Retain diff correctness and `TestDiffShutdownDoesNotWaitForGit`.

### F9 — Asynchronous replay still retains the whole serialized session and token trees

[`ReplayPrepareBefore`](app/session.c#L1422-L1568) first copies all nonempty JSONL lines, then parses and retains a `JsonDoc` token array for each. These representations coexist with the unpublished candidate's growing transcript/history and, during an async replacement, the old selected session.

This is O(file bytes + JSON tokens + reconstructed session state), not a leak claim. It nevertheless creates potentially substantial peak memory amplification. The preparation cap of 16 Markdown documents limits one contributor, not retained JSONL/token storage.

**Existing protection:** disk read/validation runs on a worker. Main-thread replay uses row/time budgets ([load pump](app/session.c#L5642-L5706)), and a large-message responsiveness regression test exists. Time budgets checked between records are not a preemption guarantee for arbitrarily expensive individual records or extension apply callbacks.

**Recommendation:** preserve validate-before-publish and chronological main-thread apply semantics while reducing redundant live representations. Options include validated file-offset indexes with lazy decoding, bounded prepared batches, and releasing consumed records. Any multi-pass approach must operate on a stable file snapshot so records cannot change between validation and replay. Avoid replaying effects from an incompletely validated file merely to save memory.

**Validation:** measure peak RSS and maximum pump time separately for many small records, a few huge records, tool-heavy sessions and grouped assistant fragments. Include cancellation and failed replay; verify the old selected session remains intact until publication and memory returns after cancellation.

### F10 — Persistence coalescing has unbounded pending bytes and repeated scans

[`QueueSessionLine`](app/session.c#L5281-L5390) merges records for a pending session into one string while holding `persist_mu`. Each merge scans the accumulated string with `strlen`, reallocates to the exact new size, and copies the new record.

If a writer is blocked and K fixed-size records accumulate, length scanning alone is quadratic in K; allocation movement can add quadratic copying. The queue capacity bounds pending **jobs**, not bytes inside one session's job. Large merges execute on the caller and lengthen the critical section shared with the persistence worker.

**Recommendation:** store length/capacity or queue owned record chunks so enqueue cost is proportional to the new record. Track total/per-session queued bytes and oldest pending age. Define an explicit overflow/backpressure policy that preserves FIFO ordering and reports persistence failure instead of silently dropping durable records. Do not block the UI indefinitely to enforce a memory budget.

**Validation:** delay a local writer, enqueue bursts for several sessions, and verify responsiveness, bounded resource behavior, ordered recovery and the existing failure contract. Benchmark bytes scanned/copied and mutex hold time; ordinary fast-disk test runs will rarely exercise a substantial backlog.

### F11 — HTTP requests do not reuse connection state across rounds

[`pico_http_post_sse`](app/http.c#L258-L311) creates a curl easy handle, performs one logical request, and cleans it up. There is no shared/multi connection cache in this path. Retries can reuse that handle within the same call, but later tool-follow-up requests start with a fresh handle and cannot reuse its connection/TLS-session cache.

**Impact:** potentially avoidable connection establishment and TLS setup between short calls to the same provider. This is separate from model inference, token throughput, provider queueing and server-side prompt caching. Actual benefit depends on protocol, network, proxy and server keep-alive behavior; no handshake saving was measured here.

**Recommendation:** use a concurrency-safe transport owner/pool with persistent connection state and bounded idle connections. Worker threads are per runtime, so an unrestricted handle-per-agent scheme could over-retain connections; conversely, a single globally locked handle would serialize agents. Preserve callback/request ownership, authentication isolation, cancellation, retries and shutdown/reload safety.

**Validation:** extend local HTTP fixtures to count accepted connections over sequential requests and concurrent agents. Measure setup and first-byte timing separately using curl timing information. Do not infer a regression from the existing HTTP suite's intentionally long retry tests.

### F12 — Shell output is RAM-bounded but can consume unbounded temporary storage

The shell tool retains a bounded head/tail capture. Once it exceeds that limit, [`ShellSpoolAppend`](app/builtins/shell.c#L175-L199) writes the complete output to a temporary file; the read loop continues appending until exit/timeout/cancellation ([loop](app/builtins/shell.c#L395-L414)). Successful large-output results return the retained path ([result](app/builtins/shell.c#L467-L479)); that successful-return path does not schedule automatic file cleanup.

There is no spool-byte ceiling in this path. The default timeout does not bound output bytes, and [`PicoSh_TimeoutFromDoc`](app/sh_timeout.h) accepts positive caller-supplied timeouts without a product maximum. A noisy command can consume significant disk capacity and I/O bandwidth, with potential pressure on unrelated session persistence.

**Recommendation:** define per-command and aggregate spool budgets plus a retention/cleanup policy. If a budget is exceeded, keep draining/discarding as needed or explicitly terminate according to policy; simply ceasing pipe reads can deadlock the child. Report truncation and the actual retained extent honestly. A maximum timeout may help but is not a substitute for a byte budget.

**Validation:** extend `shell_test.c` with an output-volume case that crosses the chosen resource policy, verifies usable bounded output/diagnostics and preserves cancellation/process-group cleanup. Background-tool logs already have a separate bounded-tail model; do not treat that as a cap on `sh` spool files.

### F13 — Development builds repeat runtime copying and compilation

Three distinct build costs are visible in [`app/CMakeLists.txt`](app/CMakeLists.txt):

1. [`pico_runtime_files`](app/CMakeLists.txt#L963-L999) is an `ALL` custom target without output tracking. Its copy commands run in an unchanged build. This was observed directly: the final build ran only that target and took 0.145 s.
2. [Eleven example extensions](app/CMakeLists.txt#L1002-L1010) are compiled in `pico`'s post-build step. Relinking the executable recompiles all examples regardless of whether their source or SDK dependencies changed.
3. Large source sets are repeated across test executables. For example, `agent.c` has five compile-command entries in the configured Release graph: the app and four test targets. See [agent behavior](app/CMakeLists.txt#L548-L591), [files/composer](app/CMakeLists.txt#L614-L690), and [host-workspace](app/CMakeLists.txt#L852-L940) target definitions.

**Recommendation:** use dependency/output-aware runtime-copy and example targets, retaining install/SDK smoke coverage. Share compiled objects only when include paths, definitions, test hooks, wrapping and compile options really match; the app and tests intentionally have differences. Do not indiscriminately merge targets just to reduce object count.

**Validation:** compare no-op builds, a one-source change, an SDK header change and a clean build. The existing 45.340 s build is not a clean-build baseline, and the unchanged-build observation alone does not justify prioritizing this over UI work; the runtime-file target’s incremental cost was not isolated. Profile build phases before pursuing LTO, compiler flags or a major CMake refactor.

## Additional issues to measure, not immediate redesigns

| Area | Source-based observation | Suggested next step |
|---|---|---|
| Extension polling | [`WalkExtTree`](app/plugin.c#L1011-L1051) caps depth and accepted C sources, not all visited entries. Poll requests recur at a 0.5-second minimum interval and coalesce ([scheduler](app/plugin.c#L1903-L1922)). A large non-C tree under an extension root can still cause repeated I/O. | Measure entries visited and idle scan cost; consider incremental discovery/backoff or a traversal budget that does not incorrectly report unvisited sources as deleted. Keep source/dependency work off the UI thread. |
| Extension snapshot allocation | [`PluginScan`](app/plugin.c#L1606-L1623) embeds fixed path arrays for all workspace slots; each snapshot is `calloc`ed ([creation](app/plugin.c#L1751-L1771)), roughly 1.2 MiB of addressable storage. | Measure actual allocation/page-touch cost before changing it. `calloc` does not prove every byte is eagerly written or resident. Reuse or compact snapshots if material. |
| Chat search | Query changes dirty all indexed messages; matching uses cached Unicode folds and a linear KMP scan, then assembles all matches ([search](app/chat_search.c#L127-L227)). Refresh occurs on the main-thread search UI path ([render](app/chat_find.c#L226-L230)). | Measure query latency and result memory for large indexed histories and very common queries. Consider cancellable incremental work if needed; do not replace a linear matcher with a complex index without evidence. |
| Request preparation | [`QueueLlm`](app/agent.c#L1523-L1553) duplicates the canonical input snapshot on the main thread each round. Provider mappers then parse/serialize input on a worker ([Responses](app/builtins/responses.c#L383-L426), [Completions](app/builtins/completions.c#L335-L445)). | Measure preparation versus network time and peak memory at large contexts. Immutable retained history chunks may remove copies, but full request serialization may still be protocol-required; preserve request-only context and turn-pinned ownership. |
| Release scroll diagnostics | Bottom pinning and relayout paths call `fprintf(stderr, ...)` without a debug guard ([frame path](app/app.c#L4059-L4070)). | Count logging during long streams and test a slow stderr sink. Gate/rate-limit normal diagnostics if they contribute; this is not established as a major hotspot. |
| Extension runtime code | The extension compiler command has no optimization flag ([compile command](app/plugin.c#L655-L658)); ordinary GCC/Clang invocations therefore use their default unoptimized mode. | Benchmark a representative expensive extension before choosing a development/release compile policy. Most builtins are already compiled as part of the optimized app; this is not a claim that Release Pico itself is unoptimized. |

### Startup-specific conclusions

Startup reaches Clay/window creation, font loading, host preferences/auth, workspace creation and plugin/catalog initialization before the normal frame loop ([`main`](app/main.c#L140-L190), [`PicoHost_Start`](app/app.c#L2679-L2755)). The first sidebar refresh adds the catalog work described in F4. Fonts are cached by face/size/device scale, with initial UI-size faces loaded by [`Pico_LoadFonts`](app/theme.c#L588-L598).

No interactive cold/warm startup measurement was taken, so there is no evidence to rank font rasterization, filesystem discovery, driver/window creation or extension activation as the dominant startup cost. Instrument **time to first presented frame** and **time to usable workspace** separately, with safe mode, no-workspace mode, populated catalogs and resume. Provider calls are not needed. Preserve asynchronous session resume and asynchronous extension compilation; do not describe startup as waiting synchronously for every compiler or transcript replay.

## Optimization order and measurement plan

### Phase 1 — Remove avoidable synchronous and duplicate work

- ~~Move title rewrites off the frame thread (F1).~~ **Done:** title rewrites queue on the persist worker; lock/copy/fsync stay off the UI thread.
- Move sidebar catalog scans off the frame thread (F4).
- Store stream lengths/capacity and explicit revisions (part of F2/F3).
- Cache unchanged completion results and composer wraps (F5/F6).
- Ensure the two LLM-hook phases share one skill discovery snapshot (F7).

These have narrow, explainable benefits without requiring an engine rewrite. Add timing/counters around the existing boundaries so savings can be attributed correctly.

### Phase 2 — Address scaling and resource ceilings

- Indexed transcript heights/dirty queues; investigate safe incremental Markdown work (F2/F3).
- Demand-driven/cached detailed diff capture and aggregate limits (F8).
- Replay representation lifetime and persistence byte-aware queues (F9/F10).
- Shell spool quotas/cleanup and connection reuse (F12/F11).

### Phase 3 — Optimize measured residual costs

Extension scanning, search, request-copying, startup font/asset work and build deduplication should follow measurements, not assumptions about which subsystem is usually expensive.

### Proposed workload matrix

The sizes here are **benchmark proposals**, not product limits or results observed in this review.

| Workload | Vary independently | Collect |
|---|---|---|
| Transcript | 100/1,000/10,000 messages; few/many traces; fixed viewport; top/middle/bottom | Layout/pump p50/p95/p99, mounted count, revisions checked, prefix queries, same-frame passes |
| Streaming | Final bytes, chunk size, cadence, Markdown structure; one/multiple agents | Main-thread latency, parsed bytes, allocations, cache rebuilds, time until final text is presented |
| Composer/completion | Draft length; Unicode; new token vs unchanged token; indexed file count | Keystroke-to-present latency, wrapping/measurement calls, directory/stat count |
| Session catalog | Files below/above cache coverage; cold/warm metadata; several checkout roots | UI stall time, files read/statted, lock wait, scan duration |
| Replay/persistence | Many small vs huge records; blocked writer/lock; concurrent agents | Peak RSS, pending bytes/age, enqueue cost, maximum pump time, cancellation latency |
| Idle workspaces | One/multiple repos; clean/dirty/untracked-heavy; modal closed/open | CPU time, wakeups, Git process count, bytes read, diff/extension scan work |
| HTTP | Sequential and concurrent local requests; keep-alive/TLS fixture | Connections established, connect/TLS/first-byte timings, cancellation and recovery |
| Shell output | Output bytes/rate, concurrent commands, successful retained files | Spool bytes, write bandwidth, cancellation latency, retained disk usage |
| Startup/build | Warm/cold caches; safe/default/resume; no-op/source/header build | First present/usable workspace time; phase times; rebuild count |

Use optimized builds for performance runs and separate sanitizer/debug validation. Debug enables SSE capture and Clay diagnostics ([build definitions](app/CMakeLists.txt#L243-L250)), so its I/O and timing should not be treated as production results. Run repeated samples after warmup, record machine/build/input details, and distinguish cold filesystem cost from warm CPU cost.

For interactive workloads, record the entire frame/pump critical path rather than just individual parser functions. A 60 Hz display allows about 16.7 ms per frame, but that is a planning budget, not proof of an existing regression or a suitable universal CI threshold. Track slow-frame distributions and missed input deadlines. Prefer durable responsiveness/ownership/ordering tests and a separate benchmark trend report over fragile machine-specific timing assertions.

## Contracts that optimizations must preserve

- **Geometry:** repeated bottom-follow layout bounds must remain stable, with and without the sidebar. Structural viewport heights remain exact; changing shell hierarchy is a scroll-layout change.
- **Threading:** extension callbacks documented as main-thread callbacks, especially tool apply/replay and UI callbacks, must not be moved to workers indiscriminately. Move I/O/preparation and publish owned snapshots instead.
- **Ownership/reload:** turns retain registration/model/tool snapshots; cancelled or stale generations cannot publish into replacement agents/workspaces. Background caches must obey those lifetimes.
- **Persistence:** preserve FIFO session records, atomic title replacement, inter-process coordination and explicit failure reporting. Performance is not a reason to silently lose durable state.
- **Tools:** keep policy-aware parallelism and serial barriers. Raising worker limits is not a substitute for fixing shared UI or storage bottlenecks.
- **Documentation:** if implementation changes public registration, lifecycle, threading, ownership, reload or hook behavior, update the matching `docs/extend/` topic and `contracts.md`, and keep examples aligned. No compatibility layer is needed.

**Bottom line:** title persistence no longer blocks the frame thread (F1). Remaining priority is other UI-thread storage work (especially F4 catalog scans) and repeated growing-history/text work. Existing tests provide a strong correctness base, but passing them does not establish smooth real-font rendering, low idle CPU, bounded peak memory or low provider round-trip overhead. Add measurements at those product boundaries before choosing larger architectural changes.
