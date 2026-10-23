# Embedded browser: development handoff

## 1. Start here

**Implementation baseline:** `731c819` — `First part of integrated browser implementation`.

**Current status:** slices 1–4 are now implemented: human browsing,
model-visible composited observations, bounded agent input batches, developer
inspection/evaluation, and independent logical viewport control. The user
validated the original human-facing browser on native Wayland/niri; the new
agent loop is covered by real-helper and loopback-provider integration fixtures,
not yet by a live-model/native-Wayland manual acceptance run. Slice 4 adds a
same-page edit/hot-reload developer fixture and a non-vision loopback-provider
fixture. Slice 5 now has a bounded robustness/profiling increment: classified
helper/renderer failures, stopped-state presentation and explicit recovery,
fail-closed transport cleanup, and repeatable resource measurements. Native/manual
acceptance and the remaining optional slice-5 features are still pending. See
[the profiling procedure and baseline](browser-profiling.md).

The continuation preserves the original requirements below. See
[the current browser contract](extend/browser.md) for the implemented schemas,
readiness, cancellation, coordinate identity, complete output files, and limits.
Next work should verify real model usage and measured robustness before adding
more browser chrome.

Read these alongside this handoff:

- [Current browser behavior and contracts](extend/browser.md), also `/docs browser`.
- [Extension contracts](extend/contracts.md), especially threading, ownership,
  tool authorization, and reload.
- [Repository instructions](../AGENTS.md), especially the shell geometry rules.
- [Packaging instructions](../packaging/README.md).

This document distinguishes agreed requirements, implemented behavior, and
proposed follow-up work. Suggested tool names below are **not existing APIs**.

## 2. Agreed product requirements — preserve these

| Requirement | Decision/status |
| --- | --- |
| Embedded presentation | Browser content lives inside Pico's window, never in an external browser window. Implemented with CEF off-screen rendering. |
| Initial platforms | Linux, including native Wayland and X11. Cross-platform support was not requested for this phase. |
| Primary use | Developing and testing local web applications. A substantial Chromium dependency is acceptable. |
| Dock | Collapsible right-side browser pane. Collapse preserves the live page. Implemented. |
| Ownership | One independent browser per **main session**, not per workspace or selected UI tab. Sessions can browse concurrently. Implemented. |
| Subagents | No browser access for subagents or clarification helpers. Main-session instructions succinctly say not to delegate browser tasks. Implemented. |
| Lazy startup | Initialize only when the human opens the browser or an agent first uses it. Startup, selection, resume/replay, and reload must not start browsers. Implemented. |
| Persistence | Cookies and local storage persist separately by session. Do not automatically reopen pages or restore the last URL. Implemented. |
| Reclamation | Keep an initialized page until explicit close or session-runtime unload. No automatic idle eviction/suspension was requested. Implemented. |
| Hidden pages | Limit rendering costs without stopping application/agent work. Implemented with reduced paint cadence and discarded display textures. |
| Shared control | Explicit **User / Agent** mode. User takeover blocks new agent actions until control is released. Implemented for navigation/input; read-only observation is allowed in both modes. |
| New sessions | Before a session ID exists, show a non-blocking reminder to send a message first. Do not start Chromium or show a warning/error overlay. Implemented. |

Do not replace session isolation with a single global browser. Agent commands
must target their calling session, never whichever session happens to be visible.
Agent input must be browser-native injection, not OS-wide mouse/keyboard control.

## 3. What is implemented

### UI and agent surface

- Address entry, back, forward, reload, collapse, and explicit close.
- Human mouse movement/clicks, scrolling, keyboard events, and Unicode text input.
- Focus arbitration with Pico's composer and modal UI; capture cancellation on
  focus loss and releases outside the pane.
- Per-session expanded/collapsed presentation state.
- `browser_open({"url":"http://localhost:3000"})`, registered as a sequential
  tool for the calling main session. HTTP(S) and `about:blank` are accepted.
- Main-session-only tool filtering plus dispatch-time identity/generation/control
  checks. Guidance: “Browser access is main-session-only; do not delegate browser
  tasks to subagents.”

A successful `browser_open` result means **navigation was queued**, not that the
page loaded. It neither selects another session nor opens its dock.
`browser_observe({})` supplies a PNG plus URL/viewport/coordinate identity;
`browser_act({"observation_id":"…","actions":[…]})` executes native input and
returns a fresh observation. Both require an existing browser and a capable
vision model/provider.

Slice 4 adds `browser_diagnostics`, `browser_inspect` (accessibility and
selector-scoped main-document DOM), `browser_evaluate`, and `browser_viewport`.
They require an already-open browser but not vision. Diagnostics/inspection work
in both control modes; evaluation and agent viewport changes require Agent mode.
Evaluation needs no extra confirmation and can mutate the page. A pending
evaluation interrupted by takeover/cancellation closes the page to prevent late
execution; already performed effects are not undone.

Diagnostics append complete collected records across navigation to private
`/tmp/pico-browser-diagnostics-*` journals. Reads are non-destructive JSONL
snapshots with browser-scoped cursors. Large text results (all inspection kinds)
return complete `/tmp/pico-browser-output-*` files and byte sizes, not truncated
previews. Returned files/journals survive close/exit until external temp cleanup.
Console objects are Chromium descriptions, not promised deep snapshots. Storage
failures explicitly report incomplete collection.

The toolbar viewport field accepts `dock`, `desktop`, `tablet`, `mobile`, or
`WIDTHxHEIGHT`. The tool supports the same presets plus custom dimensions.
Presets are dimensions only, not mobile device emulation. Fixed pages fit down,
centered/letterboxed, with mapped human input; screenshots keep full logical
resolution. Settings are live-browser-local, not restored on restart.

Human-created instances start in User mode; agent-created instances start in
Agent mode. The toolbar label is the current mode and toggles on click. Disabling
the workspace `browser` extension removes agent access, not the core human dock.

### Runtime architecture

```text
Pico main/graphics thread
  ├─ per-session dock, focus, texture lifetime
  ├─ validates worker requests against current session/control state
  └─ bounded command queue → private SOCK_SEQPACKET socket
                                  ↓
                     lazy pico_browser_helper process
                       ├─ one CEF runtime
                       ├─ independent browser/request context per session
                       └─ Chromium renderer/GPU/network subprocesses
                                  ↓
                       framed events + raw BGRA pixels
                                  ↓
Pico receiver thread → mutex-protected latest snapshots → host wakeup
                                  ↓
Pico graphics thread → BGRA/RGBA conversion → texture upload → Clay/Raylib
```

- `PicoBrowser_Init` allocates service metadata; it does **not** launch Chromium.
- The helper and browser instances belong to core, not hot-reloaded extension
  state. Extension reload does not tear them down.
- The C++ helper uses Chromium's headless off-screen platform. Pico's existing
  GLFW window owns native Wayland/X11 presentation.
- The helper reader posts work onto CEF's UI thread. Browser callbacks serialize
  events back to Pico. The receiver never calls Clay or graphics APIs.
- Frames replace pending frames rather than accumulating an unbounded video
  history. The main thread alone creates, updates, or unloads Raylib textures.
- Clay commands borrow browser text/texture storage. Closed browser objects are
  retired until the next layout boundary; do not free them from underneath an
  `AFTER_LAYOUT` callback's pending render commands.
- IPC is private and same-build, not a public extension API. No debugging server
  or remote DevTools port is exposed.
- Chromium sandboxing stays enabled. Never add an automatic `--no-sandbox` retry.

Profiles live at `$XDG_CONFIG_HOME/pico/browser/<session-id>/`, normally
`~/.config/pico/browser/<session-id>/`. They are separate from ordinary Chrome
profiles and are not automatically deleted. A resumed session uses its durable
ID, not its new in-memory agent ID, to retain storage.

### Resource behavior and present limits

- The shared helper stays warm until Pico exits, even after all pages close.
- Hidden pages currently paint at a lower frame rate, but their frames are not
  sent to Pico and their Pico textures are released. JavaScript/network activity
  continues; this is **not** suspension or a CPU/RAM limit.
- The current path copies raw pixels over IPC and uploads CPU buffers. It does
  not implement dirty-region transport or GPU-buffer sharing.
- The command queue is bounded. Exhaustion currently stops the helper rather
  than silently dropping lifecycle commands or key/button releases.
- All sessions share one helper: helper failure stops its pages; a renderer
  failure stops each affected page without stopping independent sessions. Causes
  distinguish process exit/signal, transport/protocol failure, queue exhaustion,
  and renderer termination. Stopped docks no longer show a startup placeholder.
  Close/reopen remains explicit; there is no automatic recreation or command
  replay. Runtime/page initialization watchdogs and process-group cleanup are
  bounded, including escalation for unresponsive failed helpers.
- CEF's profile root is a singleton: **one Pico process per config root** may own
  the browser runtime. Multiple sessions inside that process are supported.
  A second process is rejected explicitly; default Chrome relaunch behavior is
  suppressed so it cannot create an external window.

## 4. Code map

Paths in this table are relative to the repository root.

| Path | Responsibility |
| --- | --- |
| `app/browser.c`, `app/browser.h` | Host service, lazy process launch, request broker, per-session state, IPC receiver/queue, dock, focus/input, textures, shutdown. Private core API. |
| `app/browser/helper.cc` | CEF lifecycle and request contexts, OSR painting/popup composition, event/input dispatch, feature configuration, private profile-root lock. |
| `app/browser/inspection.h` | Helper-side diagnostic journal/resource completion handlers, asynchronous snapshot copying, DevTools inspection/evaluation, and complete temporary results. |
| `app/browser/protocol.h` | Private command/event structures. Private observation/request metadata, bounded native input batches, cancellation and control messages. |
| `app/browser/CMakeLists.txt` | Isolated C++20/CEF target, wrapper library, runtime assets and install layout. |
| `app/builtins/browser.c` | Visual and developer browser tools, main-session filtering and guidance. |
| `app/app.c` | Browser pump and shutdown, shell layout, input routing, layout/render boundaries. |
| `app/main.c` | GLFW key/character interception and host lifetime. Modifier/release state must still reach Raylib appropriately. |
| `app/agent.c`, `app/agent_internal.h` | Browser cleanup on agent destruction; session-local dock expansion flag. |
| `app/builtins/composer.c` | Prevent browser input/file drops from leaking into the composer. |
| `app/host_internal.h`, `app/plugin.c` | Core service ownership and builtin registration. |
| `app/tests/browser_integration.py` | Real helper/Chromium test without a Pico window. |
| `app/tests/browser_agent_integration.py` | Real Pico and loopback Responses fixture: model-visible PNGs/input/media plus `--developer` non-vision tools, lazy startup and complete output-file handoff/lifetime. |
| `app/tests/browser_developer_integration.py` | Real-helper diagnostics, accessibility/DOM, evaluation control, complete large output, storage failures, fixed viewport and same-page source edit/hot reload. |
| `app/tests/browser_failure_integration.py` | Real renderer-kill cause, pending evaluation failure, independent session progress, explicit reopen, and malformed command reporting. |
| `app/tests/browser_profile.py` | Repeatable static/animated/WebGL visible/hidden/concurrent CPU/RSS/IPC and actual host conversion/upload measurements. |
| `app/tests/browser_ui_integration.py` | Real Pico window, navigation/reload pixels, fixed viewport/letterboxing, mapped input/margins, resized dock and repeated bottom-follow presentation under Xvfb. |
| `app/tests/host_workspace_test.c` | Browser eligibility, new-session notification, repeated shell geometry. |
| `app/tests/install_smoke.cmake` | Checks installed helper and required CEF assets. |
| `flake.nix`, `packaging/cef.json`, `packaging/fetch-cef.sh` | Nix development dependency and pinned native-release download. |
| `.github/workflows/release.yml`, `packaging/appimage/build.sh` | Release dependency setup and helper/CEF dependency scanning. |

## 5. Bugs already fixed — do not reintroduce

### Browser loaded pages but displayed solid white

The host received valid frames and the server received navigation/reload
requests. However, this repository's Clay implementation emits a background
rectangle **after** an element's image. The browser image element's opaque white
background covered the page.

**Fix:** use a transparent background once the browser texture exists; retain a
background only for the loading placeholder. The real-window test checks
presented website pixels, not merely receipt of a frame or successful HTTP.

### Opening a browser in an uncreated session looked like an application error

A new session can have no durable session ID yet.

**Fix:** the human open path checks before expansion/initialization and shows:
“Send a message to create this session before opening its browser.” The host
regression checks the toast, absence of warnings/modals, and no profile creation.

### Helper crashed several seconds after clicking a client-side link

Direct URL entry worked, but the user's local app crashed after login → Terms or
other SPA navigation. Native Wayland and isolated X11 crash dumps matched:
`SIGSEGV` in Chromium's `ReadAnythingSoftNavigationObserver::OnSoftNavigation`,
which called `TabInterface::GetFromContents` without a normal Chrome tab.

**Fix:** disable the unused `ImmersiveReadAnything` feature in the browser-process
command line, preserving other `disable-features` entries. See
[CEF #4234](https://github.com/chromiumembedded/cef/issues/4234). No queue-limit
increase or sandbox relaxation was used.

The regression uses an actual mouse click, `history.pushState`, replacement page
content, a delayed application update, and a subsequent resize/new frame. It
crashes with the old helper and passes with the fix. Checking only the immediate
URL event would miss this asynchronous failure.

The user initially recalled queue exhaustion, but the captured reproduction
had an empty queue and a helper segmentation fault. Do not assume every future
exhaustion report is this same issue. Temporary diagnostic logging was removed.

## 6. Remaining implementation plan

The order below is the recommended continuation, not a commitment to every
suggested API name or optional browser feature.

### Slice 2 — model-visible observations

**Status: implemented, with live-model verification still pending.**

- ABI 22 adds `PicoToolImage`, image-bearing `PicoToolResult`, provider
  `tool_images` capability, and a worker capability accessor. Core validates PNGs
  and owns persisted assets; canonical results gain image `parts` references.
- Browser assets live in `.pico/media/browser/<session-id>/`, separate from
  `.pico/media/composer/`. Generic image tools default to `tools/<session-id>/`.
  Replay never recaptures. Compaction removes summarized images from active
  model context but keeps saved history/assets; history deletion cleans images.
- OpenAI Responses (including Codex), Hyper, and xAI project actual image parts
  into labeled untrusted observation messages after tool results. Missing media
  or unsupported model/provider capability fails explicitly.
- Capture works hidden and under User control, without selecting/expanding a
  pane. It primes a surface, fences via an internal compositor snapshot, then
  captures fresh composited OSR view/popup pixels at logical scale 1. Readiness is
  navigation load plus fresh paint, **not** network-idle or SPA completion.

### Slice 3 — agent mouse/keyboard action loop

**Status: implemented, with native/live-model acceptance still pending.**

`browser_act` supports bounded move, click, scroll, named key chords, Unicode text
insertion, and drag paths; validates the whole batch before dispatch; and returns
an observation automatically. Coordinates are image pixels. Older observations
remain valid within the same browser/navigation/viewport identity, even after
input, but ordinary DOM changes are not detectable as staleness.

CEF's private DevTools Input methods deliver native input to the same embedded
page and acknowledge each event. Takeover/cancellation interrupts remaining
work, reconciles pending input, and releases held buttons/keys. Results distinguish
completed actions, delivered events, partial actions, and uncertain delivery;
nothing is rolled back or automatically retried. Cancellation/runtime retirement
cannot retarget a replacement session. Capture/input timeouts are bounded;
unacknowledged input/release failure may close the page.

No browser is created by observe/act alone, and no new viewport presets, tabs,
clipboard/file/download tools, page evaluation, or cross-process sharing were
added. Current initial/hidden viewport behavior is preserved.

### Slice 4 — developer-oriented inspection and viewport control

**Status: implemented; native/live-model acceptance remains pending.**

- `browser_diagnostics`: console messages, uncaught exceptions, failed network
  requests and HTTP 4xx/5xx, with URL/method/status/error details but no headers,
  cookies or request/response bodies. Runtime diagnostics enable before initial
  requested navigation; CEF resource callbacks supply network completion details
  without retaining an unbounded in-memory request-ID map.
- `browser_inspect`: the main page's Chromium accessibility tree or first
  selector-matched element's outer HTML. Uses read-only DevTools methods, not
  JavaScript. No cross-frame/shadow selector traversal or durable action handles.
- `browser_evaluate`: arbitrary main-page JavaScript with by-value results and
  promise awaiting, Agent-only, no extra confirmation. A synchronous 10-second
  engine limit and 30-second helper deadline apply. Interrupted pending evaluation
  closes its page; side effects are not rolled back or automatically retried.
- `browser_viewport` and human toolbar editing: dock-size mode, desktop 1280×720,
  tablet 768×1024, mobile-sized 390×844, or custom 1–4096 dimensions. Dimensions
  only; no UA/touch/DPR emulation. Fixed pages scale down only, centered with
  letterboxing; observations and agent coordinates retain full logical dimensions.
  Only actual page dimension changes invalidate observation coordinate identity.

User-approved output policy supersedes the original bounded-history proposal:
**never discard overflow or truncate returned content.** Diagnostic journals are
disk-backed, retained across navigation, and read non-destructively using opaque
browser-scoped cursors. All large text results spill to complete private `/tmp`
files with paths/byte sizes. Returned files and journals remain after close/exit;
external temp cleanup owns their eventual deletion. Console object descriptors
are not deep object snapshots. Resource/capture/storage failures are explicit,
including incomplete collection; a file-size quota does not kill the helper with
SIGXFSZ. Diagnostic snapshots copy an immutable journal prefix on a CEF file
worker, without holding its append lock or blocking the UI for the bulk copy.

These tools reuse the same embedded page, request broker and private DevTools
connection. They never create/select/expand a browser and need no vision support.
Read-only tools work in both control modes. Per-main-session ownership, runtime
generation validation, subagent exclusion, lazy startup and sandboxing remain.
All page-derived content/files are untrusted data, not instructions. No public
extension ABI change was needed.

**Automated acceptance:** the real-helper fixture reproduces a broken label,
finds its console diagnostic, edits its source module, and verifies the hot-reload
DOM and composited pixels without navigating/replacing the page. A real-Pico
non-vision loopback provider also exercises the actual registered developer tools,
large-file handoff and viewport control. Neither is live-model certification.

### Slice 5 — robustness, resource profiling, and UX polish

**Status: bounded robustness/profiling increment implemented; remaining optional
work below stays deferred.**

Implemented in this increment:

- Helper exit status/signals, command/event transport and protocol causes, queue
  exhaustion, and renderer killed/crashed/OOM/launch classification are retained
  in dock/tool errors. Stopped pages never claim to be starting. Pending mutating
  operations report effect uncertainty, without automatic replay/retry.
- Helper failure stops all its pages; renderer failure stops each affected page
  while independent session pages continue. Recovery requires explicit close and
  reopen. Runtime/page startup watchdogs are independent of queued-open success.
- Terminal queue/transport state prevents later flushing of queued input. Failed
  helpers and surviving Chromium process groups are cleaned up with bounded
  escalation, even after the helper leader is reaped. Queue capacity is unchanged.
- A maintained matrix covers static, animated and WebGL pages, visible/hidden and
  several concurrent session browsers, sampled process-tree CPU/RSS, raw pixel
  IPC bandwidth and actual host conversion/upload API wall times. Minimal private
  opt-in counters use `PICO_BROWSER_PROFILE`; no public extension ABI change.
  See [procedure, limitations and baseline](browser-profiling.md). WebGL backend
  unavailability is explicit, not silently counted as a successful measurement.
- Host fault fixtures cover exit/signal/protocol/queue/stopped-state/reopen;
  real-helper renderer kill checks pending evaluation and independent sessions;
  the real-Pico loopback provider has a helper-failure mode.

Remaining scope and measured follow-up:

- Profile CPU, memory, pixel-copy/upload bandwidth, and WebGL behavior with
  visible, hidden, and several concurrent session browsers. The synthetic matrix
  is now repeatable, but native GPU/Wayland, large observations, long-soak memory
  and realistic dev-app activity remain unbenchmarked; user fan noise was reported
  during the crash investigation, not independently diagnosed as a performance defect.
- Consider dirty-region transport/coalescing or GPU sharing only after profiling.
  Do not reintroduce tab suspension or idle page eviction without user approval.
- Failure classification and explicit stopped-state recovery are now implemented.
  Continue measuring failures/cleanup under realistic native loads rather than
  adding automatic restart or replay.
- Review backpressure/fairness, large observations, and action bursts. Never
  silently drop close commands or key/button releases to hide queue pressure.
- Consider releasing the shared helper when unused, and decide whether
  multi-Pico-process service/profile sharing is required. Both need explicit
  lifecycle/profile-locking designs, not disabling CEF's singleton protection.
- Complete native keyboard/IME, cursor, clipboard, and HiDPI rasterization work.
- Decide scope/policies for tabs, popups, JavaScript dialogs, permissions,
  uploads/downloads, and context menus. They are not all approved requirements.
- Verify real archive/AppImage browser startup, runtime assets, sandboxing,
  shutdown, and clean-machine dependencies; install presence checks alone do not
  establish portable-release correctness.
- Keep Chromium security updates part of release maintenance. Re-evaluate the
  reader-mode policy against the actual pinned CEF source before changing it.

## 7. Settled continuation decisions and future scope

The user approved slices 2 and 3 together, all builtin vision adapters,
composited OSR observations after navigation load/fresh paint, durable
`.pico/media/browser/<session-id>/` assets, observations in both control modes,
batched `browser_act` with automatic observations, older observation reuse while
browser/navigation/viewport identity matches, and interruption on takeover.
Provider-specific labeled observation-message projection is allowed; canonical
history must remain tool output, not human input.

Slice 4's scope and permissions are now approved and implemented as above,
including no extra evaluation confirmation and complete file output rather than
truncation. Cross-process sharing, clipboard/files/downloads/tabs, mobile device
emulation and a visual developer-console panel remain deferred. The
per-main-session ownership, subagent exclusion, lazy startup, persistence, and
explicit control-mode requirements remain settled.

## 8. Build, test, and validation status

Use the Nix development environment. The pinned native-release manifest currently
specifies **CEF 151.3.16 / Chromium 151.0.7922.109**. Nix obtains CEF from the
pinned nixpkgs package; keep `packaging/cef.json` aligned when updating that lock.

```sh
nix develop
cmake -S app --preset debug
cmake --build app/build/debug
./app/build/debug/pico
```

Restart Pico for core/helper changes; F5 only reloads extensions. Development
helper: `app/build/debug/browser/runtime/pico_browser_helper`. Installed helper
and assets: `libexec/pico/browser/`. ICU/V8 data must resolve beside the loaded
runtime; the helper uses `$ORIGIN`. See `app/browser/CMakeLists.txt` before changing
asset locations. CEF adds a C++20 helper, not a conversion of Pico's C99 core.

Run ordinary tests inside `nix develop`:

```sh
cmake --build app/build/debug --target pico_host_workspace_tests
app/build/debug/pico_host_workspace_tests
ctest --test-dir app/build/debug --output-on-failure -j 1

timeout -k 5s 90s python3 app/tests/browser_integration.py \
  app/build/debug/browser/runtime/pico_browser_helper
```

Real-window test, using its optional tooling shell and an isolated display:

```sh
nix develop .#browser-tests -c xvfb-run -a -s '-screen 0 1600x1200x24' \
  timeout -k 5s 90s python3 app/tests/browser_ui_integration.py app/build/debug/pico
```

Historical validation before the original documentation handoff:

- The first slice passed the full 48-test CTest suite in a serial run.
- After the subsequent fixes, the required host/workspace suite, real helper
  integration test, and real-window navigation/reload test passed.
- Both development and installed helpers were exercised during the first slice.
- The real local login → Terms flow was reproduced before the fix and remained
  rendered/interactive after a 15-second wait with the fix, at a 720 × 1256 browser
  viewport in isolated X11.
- The user subsequently confirmed navigation was fixed and the browser works
  well on their native Wayland setup. This is not exhaustive native-platform,
  performance, IME, or release-artifact certification.

Recorded continuation validation:

- Full 48-test CTest suite passed serially, including the host/workspace geometry
  suite and install smoke test. The harness's documented zombie-reaping issue
  required an external test-parent reaper; no test assertions were weakened.
- Real helper regression passed with hidden PNG captures, native input,
  stale identities, cancellation/takeover, and concurrent independent requests.
- Real-window navigation/reload presentation regression passed under Xvfb.
- Real-Pico loopback-provider agent regression passed, checking actual image
  payload pixels, click/Unicode text/scroll, and session-owned media persistence.
- New SDK image example compiles in the ordinary build.
- Native Wayland **agent** usage and live OpenAI/Codex/Hyper/xAI model acceptance
  have not been manually tested. Existing human Wayland validation predates
  these changes.

Continuation validation commands (in addition to the original helper/UI tests):

```sh
nix develop .#browser-tests -c env -u WAYLAND_DISPLAY xvfb-run -a \
  -s '-screen 0 1600x1200x24' timeout -k 5s 120s \
  python3 app/tests/browser_agent_integration.py app/build/debug/pico
```

Slice-4 validation commands:

```sh
nix develop -c timeout -k 5s 120s python3 app/tests/browser_developer_integration.py \
  app/build/debug/browser/runtime/pico_browser_helper

nix develop .#browser-tests -c env -u WAYLAND_DISPLAY xvfb-run -a \
  -s '-screen 0 1600x1200x24' timeout -k 5s 120s \
  python3 app/tests/browser_agent_integration.py app/build/debug/pico --developer
```

Slice-4 validation has passed the full 48-test serial CTest suite, required
host/workspace geometry suite, existing helper and vision-agent regressions,
new developer helper and non-vision agent fixtures, and extended real-window
viewport/input regression. Native Wayland/live-model validation remains manual.
One initial existing helper capture timed out with Chromium subprocess
stack-smashing messages; a repeat passed without weakening assertions or changing
sandbox policy. This is not a measured robustness certification.

The host suite also exposed an unrelated optional Enchant/Aspell loader/exit race
under the inherited development `LD_LIBRARY_PATH` (confirmed by a debugger stack
in `SpellLoaderMain` / `libaspell` during retained process shutdown). The successful
serial run omitted the optional Enchant path and used the documented orphan
reaper. No production spell behavior or test assertions were changed. Treat this
as a separate shutdown issue rather than hiding it in browser logic. A later
standalone host rerun also hit the existing session-reference completion-scope
fixture; its retry was clean. No browser code executes in that failing fixture,
and its assertions were not changed.

The core tests now cover image payload mapping, unsupported providers, image
ownership/transcript copies, malformed/missing media, replay/compaction retention,
and browser-owned history deletion. Helper coverage includes hidden observations,
input acknowledgments, stale identity, and interrupted batches. The real Pico
fixture verifies the open → observe → click/type → observe → scroll loop and the
actual PNG contents delivered to a loopback Responses endpoint. No real API
credentials are used. Hyper/xAI wire projection is covered by serializer tests;
live service/model acceptance, including Codex authentication, remains a manual
check.

Slice-5 final validation passed the required host/workspace suite, the full
48-test serial CTest suite (including geometry and install smoke), existing
helper and developer fixtures, the new renderer-failure fixture, real-window
navigation/viewport/input regression, and all three real-Pico loopback modes
(vision, non-vision developer, and helper failure during evaluation). The final
serial run used the already documented external orphan reaper and omitted the
optional Enchant library path; no production spell behavior or assertions were
changed. The resource matrix was measured separately, without concurrent test
runs. Native/manual and packaged-artifact acceptance remain pending.

Slice-5 validation and profiling commands:

```sh
nix develop -c app/build/debug/pico_host_workspace_tests --browser-failures

nix develop -c timeout -k 5s 90s python3 app/tests/browser_failure_integration.py \
  app/build/debug/browser/runtime/pico_browser_helper

nix develop .#browser-tests -c env -u WAYLAND_DISPLAY xvfb-run -a \
  -s '-screen 0 1600x1200x24' timeout -k 5s 120s \
  python3 app/tests/browser_agent_integration.py app/build/debug/pico --failure
```

The initial measured baseline includes 21 software-X11 helper/host trials and
five native-driver, engine-only WebGL trials, with 2-second warmup and 3-second
sampled intervals. Animated visible 1280×720 pages transported approximately
106 MiB/s each, and three helper-visible pages approximately 318–320 MiB/s.
Hidden fixtures kept progressing without display-frame transport. Forced-software
WebGL was unavailable (Chromium blocklist); native-driver WebGL completed with
ANGLE/radeonsi. These are short Debug-build observations, not production/native
Wayland performance or long-soak certification. Full procedure, exact counters,
CPU/RSS limitations and structured results are in
[browser-profiling.md](browser-profiling.md) and
[browser-profile-baseline.json](browser-profile-baseline.json).

The robustness increment retains the current sandbox, helper warmth, per-session
profiles and explicit recovery policy. Packaged archive/AppImage execution tests,
automatic helper shutdown/restart, cross-process sharing, clipboard/files/tabs,
native IME/HiDPI, and live-model acceptance remain deferred. Run the ordinary
host/workspace and serial CTest suites plus existing helper/developer/UI/agent
fixtures after core/helper changes, not just the new fault subset.

Test-environment notes:

- Chromium integration tests require usable sandbox/user namespaces; do not
  bypass this with `--no-sandbox`.
- Under the Pico harness, a background-process test once stalled because its
  killed fixture grandchild was retained as a zombie by the outer subreaper.
  A wrapper that reaped orphaned children restored normal test behavior without
  changing production/test code. Run from a normal terminal or an appropriate
  reaping parent if encountering this environment issue.
- Concurrent runs showed intermittent host fixture failures; the final
  serial run was clean. Investigate failures rather than weakening assertions.
- No regression should depend on temporary diagnostic scripts/files from the
  original implementation session; maintained tests are in `app/tests/`.

## 9. Safety rails and debugging checklist

- Follow `AGENTS.md`: no backward-compatibility layer is required for this
  development-stage project.
- Preserve exact shell viewport bounds. Extend
  `TestBottomFollowShellGeometryStable` for hierarchy/pane changes and run the
  required host/workspace tests. The browser shares `ChatBrowserRow` with chat
  and composer; the footer spans beneath both.
- Do not touch Clay/Raylib from browser or tool workers, retain borrowed Clay
  pointers across layouts, or free textures referenced by pending commands.
- Public tool-result/provider/lifecycle changes require matching extension docs,
  `contracts.md`, examples where applicable, and the appropriate ABI update.
  The current private browser IPC/header is not an extension API.
- Keep transient browser activity out of replay side effects. Replaying a
  transcript must not re-execute old navigation or input.
- Never use the human's normal browser profile or OS-wide input for automation.

When debugging:

1. Separate command delivery, page loading, frame receipt, texture upload, and
   actual presentation. A server request or `OnPaint` callback alone is not proof
   that the user or model sees the page.
2. Check `$XDG_CONFIG_HOME/pico/browser/cef.log`, defaulting to
   `~/.config/pico/browser/cef.log`, and capture Pico's stdout/stderr.
3. For process exits, inspect recent core dumps with `coredumpctl list` and select
   the PID. The process name may be truncated; filtering only on the full helper
   name previously missed dumps. Core dumps/profiles may contain sensitive data.
4. Reproduce with isolated profiles and loopback fixtures. Distinguish hard
   navigation from SPA `pushState`/DOM replacement and allow delayed events to
   run. Check the user's viewport dimensions as well as a small test window.
5. Measure outbound command types/depth before attributing an exit to exhaustion.
   Do not raise limits or disable security features without identifying the cause.

**Recommended next deliverable:** manually verify the implemented observe–act
loop with a real vision-capable model on native Wayland, including concurrent
sessions and takeover. Then prioritize measured robustness rather than additional chrome. Loopback payload validation proves
image transport and input behavior, not model comprehension or every backend's
acceptance.
