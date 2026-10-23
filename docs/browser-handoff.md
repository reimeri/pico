# Embedded browser: development handoff

## 1. Start here

**Implementation baseline:** `731c819` — `First part of integrated browser implementation`.

**Current status:** the first vertical slice is implemented. The user has tested
it on their native Wayland setup (niri) and confirmed that the embedded browser
works well after the rendering and link-navigation fixes described below.

**The original goal is not yet complete:** humans can browse inside Pico and an
agent can open a URL, but the agent cannot yet receive a screenshot or operate
mouse/keyboard tools. The next milestone should close that observe–act–verify
loop, not add more browser chrome first.

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
| Shared control | Explicit **User / Agent** mode. User takeover blocks new agent actions until control is released. Implemented for navigation; extend the same rule to future actions. |
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
page loaded. It neither selects another session nor opens its dock. There is no
model-visible screenshot, DOM, console, or agent input result yet.

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
- All sessions share one helper: a helper failure affects all live browser
  instances. The UI currently asks the user to close/reopen to retry.
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
| `app/browser/protocol.h` | Private command/event structures. Already has manual mouse/key/text input commands; no observation/DevTools protocol yet. |
| `app/browser/CMakeLists.txt` | Isolated C++20/CEF target, wrapper library, runtime assets and install layout. |
| `app/builtins/browser.c` | `browser_open`, tool filtering, no-delegation guidance. |
| `app/app.c` | Browser pump and shutdown, shell layout, input routing, layout/render boundaries. |
| `app/main.c` | GLFW key/character interception and host lifetime. Modifier/release state must still reach Raylib appropriately. |
| `app/agent.c`, `app/agent_internal.h` | Browser cleanup on agent destruction; session-local dock expansion flag. |
| `app/builtins/composer.c` | Prevent browser input/file drops from leaking into the composer. |
| `app/host_internal.h`, `app/plugin.c` | Core service ownership and builtin registration. |
| `app/tests/browser_integration.py` | Real helper/Chromium test without a Pico window. |
| `app/tests/browser_ui_integration.py` | Real Pico window, address-bar navigation/reload, and screenshot-pixel assertions under Xvfb. |
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

### Slice 2 — model-visible observations (next)

**Status: not implemented; prerequisite for reliable visual automation.**

Pico's `PicoToolResult` currently contains text, optional JSON details, and an
error flag. Canonical tool results and provider adapters serialize text. Images
are already supported in user input, but that does not make an image returned
by a browser tool visible to the model.

Work:

- Design an explicit multimodal tool-result/observation contract. Inspect
  `app/include/pico/app.h`, `app/canonical.c`, `app/agent.c`, `app/session.c`,
  `app/builtins/responses.c`, and `app/builtins/completions.c`.
- Define screenshot asset ownership, persistence, history replay, compaction,
  cleanup, and provider capability/error behavior. Do not merely put a path or
  base64 string into text/`details_json` and call that vision support.
- Add an explicit on-demand observation request to the **same session browser**.
  It must work when the dock is hidden and no Pico texture is retained.
- Return page metadata with the image: URL, viewport dimensions, coordinate
  mapping, and sufficient navigation/viewport identity to detect stale actions.
- Define what “capture complete” means. A newly queued navigation and a completed
  page observation are different events; avoid arbitrary sleeps as readiness.
- Choose whether screenshots reflect CEF's composited OSR surface or a page
  capture via DevTools; document popup composition and scaling differences.

**Acceptance:** a vision-capable model receives the actual page image, can report
its visible content, and gets a clear failure for unsupported providers. Capture
must not require selecting/expanding the session or mix up concurrent sessions.

### Slice 3 — agent mouse/keyboard action loop

**Status: not implemented as agent tools; helper-side primitives already exist.**

A possible surface is `browser_observe` plus `browser_act`, or several focused
input tools. Settle the schema before implementation.

Work:

- Expose clicks/movement, scrolling, key down/up or key chords, text insertion,
  and drag sequences through the existing browser service.
- Keep text input separate from physical keys; handle modifiers and balanced
  press/release pairs deliberately.
- Extend explicit User/Agent ownership to every action. Taking control blocks
  new agent actions; define the treatment of an already-started action batch.
- Serialize mutations within a session while allowing different sessions to
  progress independently. Do not infer global safety from a per-agent sequential
  tool registration alone if future APIs allow sharing a target.
- Define coordinate semantics against observations, resize/navigation staleness,
  cancellation, action completion, and cleanup of held buttons/keys.
- Bound action batches and prevent stale/cancelled generations from acting on a
  resumed or replaced session. Never redirect automation to the selected pane.

**Acceptance:** the agent observes a local app, clicks/types/scrolls, observes the
result, and verifies it. Two sessions can run that loop independently. Human
takeover, collapse, selection changes, cancellation, and close are safe.

### Slice 4 — developer-oriented inspection and viewport control

**Status: not implemented; useful after the visual control loop.**

- Console messages and uncaught JavaScript exceptions.
- Failed network requests and bounded diagnostic details.
- Optional DOM/accessibility observations and, if approved, page evaluation.
- Page viewport dimensions independent of dock size: fixed/mobile viewport
  presets with documented scaling or letterboxing.

Prefer CEF's DevTools messaging to the same embedded browser, rather than a
second headless browser with separate state. A remote debugging port is not
necessary. Treat page/DOM/console content as untrusted tool data, not instructions.

**Acceptance:** reproduce a local UI bug, associate it with browser diagnostics,
edit the app, and verify the result after hot reload in the same browser.

### Slice 5 — robustness, resource profiling, and UX polish

**Status: partial foundations only; prioritize measured problems.**

- Profile CPU, memory, pixel-copy/upload bandwidth, and WebGL behavior with
  visible, hidden, and several concurrent session browsers. Resource usage has
  not been comprehensively benchmarked; user fan noise was reported during the
  crash investigation, not independently diagnosed as a performance defect.
- Consider dirty-region transport/coalescing or GPU sharing only after profiling.
  Do not reintroduce tab suspension or idle page eviction without user approval.
- Improve helper/renderer failure reporting and recovery: distinguish process
  exit, signal, renderer failure, protocol errors, and queue exhaustion; avoid a
  misleading “Starting browser...” state after the helper has exited.
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

## 7. Decisions the next developer should clarify

Do not silently turn these proposed details into public contracts:

1. Observation/tool-result schema and supported provider behavior; how image
   assets are retained and compacted.
2. Exact action-tool shape, coordinate units, batch boundaries, readiness rules,
   and stale-observation handling.
3. Whether takeover cancels an in-flight batch or only rejects subsequent work.
4. Desired viewport presets/resize behavior and screenshot/display scaling.
5. Whether multiple Pico windows/processes must share a browser service.
6. Scope and permission policies for clipboard, files, downloads, tabs, page
   evaluation, and other browser capabilities.

The per-main-session ownership, subagent exclusion, lazy startup, persistence,
and explicit control-mode requirements in section 2 are already settled.

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

Recorded validation before this documentation handoff:

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

**Recommended next deliverable:** a model-visible screenshot from the calling
main session, with explicit image ownership and coordinate metadata, followed by
one safe click/type/observe loop. That is the smallest slice that advances the
remaining original goal.
