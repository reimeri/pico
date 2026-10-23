# Embedded browser (first slice)

Pico renders Chromium inside a right-side dock. There is no external browser
window. This is a core-owned service, not a general pane/texture extension API.

## Using it

Select a main session and click the right-edge `<` to expand its browser. A new,
uncreated session shows a non-blocking reminder to send a message first; it does
not initialize Chromium or show an error overlay. On first use it opens `about:blank`; enter a full URL such as
`http://localhost:5173` in the address field. The toolbar offers back, forward,
reload (`R`), control mode, close (`x`), and collapse (`>`). The footer stays
below the chat/browser content row at its full shell width.

- Collapse hides the pane without discarding the page.
- Close destroys that session's live page, not its cookies or local storage.
- Switching sessions selects that session's dock and browser; it does not
  navigate or close other sessions' browsers.
- **User / Agent** is the current control mode; click it to switch. User mode
  blocks agent navigation. Agent mode blocks human page/navigation input but
  always permits takeover, collapse, and close. A UI-created browser starts in
  User mode; an agent-created browser starts in Agent mode.
- Click the page to focus its keyboard input; click outside to return keyboard
  input to Pico. Clicking the address field edits the URL, not the composer.

The builtin `browser` extension registers the agent tool; disabling that workspace
extension removes agent access, not the core human-operated dock.

The agent tool `browser_open({"url":"http://localhost:5173"})` lazily opens or
navigates **the calling main session's** browser. It does not switch the selected
session or expand another session's pane. A successful result means navigation
was queued, not that the page finished loading. This first slice does not yet
provide screenshots, DOM inspection, or agent mouse/keyboard tools. Do not
pretend that opening a page gives the model visual access to it.

Subagents and clarification helpers have no browser access. Main-session system
instructions include: “Browser access is main-session-only; do not delegate
browser tasks to subagents.”

## Lifecycle, storage, and resource use

Neither app startup, session creation/selection, layout, transcript replay, nor
extension reload starts Chromium. First explicit use starts one helper process;
each used main session gets its own CEF browser and request context. Chromium
also creates sandboxed renderer/GPU/network subprocesses as needed.

Cookies and local storage persist in
`$XDG_CONFIG_HOME/pico/browser/<session-id>/` (normally
`~/.config/pico/browser/<session-id>/`). Pages and their last URL are not restored
automatically. No normal Chrome profile is imported. Close or session-runtime
unload releases the page; extension reload does not. The shared helper stays
warm until Pico exits. Disk profiles are not automatically deleted.

Hidden pages keep running, including dev-server updates and JavaScript timers.
Their paint cadence is reduced, frames are not sent to Pico, and Pico releases
their display textures. This is not tab suspension or idle eviction: CPU-heavy
JavaScript and multiple live renderer processes still consume resources.

CEF currently owns one singleton profile root. **Only one Pico process can use
the embedded browser with a given config root at a time.** Multiple main sessions
within that process work independently. A second process fails explicitly; it
does not open an external Chrome window. Sharing a browser service between Pico
processes is not part of this slice.

## Implementation contracts

- Browser lifetime belongs to core, outside hot-reloaded extension state.
- Tool workers submit copied requests. The main thread validates current agent
  identity, generation, main-session eligibility, and control ownership before
  dispatch. Requests never target “the selected browser.”
- Private local sockets carry commands and off-screen frames; there is no
  listening DevTools/debug port. Text events and frame dimensions are bounded.
- The receiver publishes owned pending snapshots under a mutex and wakes Pico.
  Only the graphics thread uploads/unloads textures or touches Clay.
- Browser view callbacks are declarative. Retired texture/text storage survives
  pending Clay commands until the next layout boundary.
- The content row is bounded by the shell; browser and main panes take its exact
  inner height. Repeated bottom-follow layouts must not move their bounds.
- Chromium sandboxing stays enabled. Startup failure does not retry unsandboxed.
- Chrome's immersive reader-mode feature is disabled: this Alloy/off-screen
  browser has no Chrome tab or reader side panel. The pinned Chromium 151 reader
  observer otherwise crashes after client-side navigation
  ([CEF #4234](https://github.com/chromiumembedded/cef/issues/4234)). This does not
  disable page accessibility or change the browser's command-queue limits.

## Current limitations and manual checks

This is a development slice, not a complete browser UI. One page per session;
no tabs, browser popups, native context menus, file dialogs, download UI, IME
composition integration, or permission UI. Popup windows are blocked and
JavaScript dialogs are suppressed. Unicode character input is supported, but
platform keyboard/IME polish still needs native testing. Browser content is
rendered at logical pane resolution; HiDPI browser rasterization and a viewport
size independent of dock size are follow-up work.

Build with `nix develop`, then:

```sh
cmake -S app --preset debug
cmake --build app/build/debug
./app/build/debug/pico
```

For the real, windowless engine test (requires working Chromium sandbox/user
namespaces):

```sh
python3 app/tests/browser_integration.py \
  app/build/debug/browser/runtime/pico_browser_helper
```

That test exercises frames, mouse/text input, hidden-page progress, profile
isolation, resize, persistence across helper restart, and continued rendering
after a client-side link updates history and replaces page content. The latter
waits for a delayed application update to catch asynchronous navigation crashes. It is separate from
headless unit tests because locked-down build sandboxes may prohibit Chromium's
sandbox setup. Failures must not be worked around with `--no-sandbox`.

A separate UI regression drives the actual Pico window on an isolated X11
display, enters a loopback URL, and checks screenshot pixels before and after
reload. This catches presentation failures that a helper-only frame test cannot:

```sh
nix develop .#browser-tests -c xvfb-run -a -s '-screen 0 1600x1200x24' \
  timeout 90s python3 app/tests/browser_ui_integration.py app/build/debug/pico
```

On **native Wayland and X11**, manually check:

1. Open a local dev app, navigate, type Unicode, use shortcuts, scroll, and drag
   beyond the pane before releasing. The composer must not receive page input.
2. Collapse/expand; switch between two main sessions with different local
   storage/login state; verify both pages keep progressing in the background.
3. In User mode, ask the agent to navigate; verify the tool refuses. Switch to
   Agent mode and retry. Taking control must not affect another session.
4. Close/reopen; restart Pico and resume the same session. Storage should
   persist, but the browser must not start until explicitly requested.
5. Resize and move Pico between monitors. Watch for bounds drift, image scaling,
   popup positioning, and stuck keys/buttons after focus changes.

CEF uses a headless off-screen platform internally. Pico's native GLFW window
still controls Wayland/X11 presentation; passing the windowless engine test does
not establish native window/input correctness on either display system.
