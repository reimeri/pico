# Views

Add Clay UI into a fixed slot. You cannot remove or replace builtins; you add alongside them. The empty-state (no messages yet) is the exception: `pico_workspace_add_empty_view` can replace it or stack panels around the builtin cards. See [Empty state](#empty-state).

Views are scoped:
- **Host views** (`pico_host_add_view`) — registered in `host_init`. Callback signature: `void (*PicoHostViewFn)(PicoHost *host, void *state)`.
- **Workspace views** (`pico_workspace_add_view`) — registered in `workspace_init`. Callback signature: `void (*PicoWorkspaceViewFn)(PicoWorkspace *workspace, PicoAgentId selected_agent_id, void *state)`. Called only when the selected agent belongs to that workspace.
- **Empty-state views** (`pico_workspace_add_empty_view`) — registered in `workspace_init`. Callback signature: `PicoWorkspaceViewFn`.

```c
#include "pico/plugin.h"
#include "clay/clay.h"

static void HelloRender(PicoHost *host, void *state)
{
    (void)host;
    (void)state;
    CLAY(CLAY_ID("HelloExt"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 6}})
    {
        CLAY_TEXT(CLAY_STRING("hello"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_UI, .textColor = COLOR_TEXT}));
    }
}

static int HelloInit(PicoHost *host, void **state_out)
{
    (void)state_out;
    pico_host_add_view(host, PICO_SLOT_SIDEBAR, 0, HelloRender);
    return 0;
}
```

Full file: [`../../examples/hello.c`](../../examples/hello.c).

## Slots

- `PICO_SLOT_SIDEBAR` — left column, **fixed 200px**, full content height. Builtin `sidebar` owns this slot: it lists disk workspaces under `~/.config/pico/sessions/` (not only live runtimes; entries whose path is not an existing directory are omitted), with Add workspace (native folder picker and a wait modal), expand/collapse (latest 10 sessions, More/Less to page by 10; the selected session stays visible when its workspace is collapsed), `+` for a new main agent, and click-to-resume or select. The column appears whenever at least one view is registered here; the builtin always registers, so the sidebar is always on. Extra host views in this slot stack with it (`z` order).
- `PICO_SLOT_MAIN` — chat column (builtin `chat` already fills this).
- `PICO_SLOT_COMPOSER` — input box.
- `PICO_SLOT_FOOTER` — status line in the main column (from the sidebar's right edge to the view's right edge, not under the sidebar). Builtin footer: click cwd for a folder picker (native dialog; Pico dims the window with a “select a folder” modal until it closes), model/effort for dropdowns. In a git workspace with uncommitted changes the footer also shows a `+adds -dels` chip (including untracked files) that opens the unified diff modal.
- `PICO_SLOT_OVERLAY` — drawn after the shell (warnings, popups, modals). Builtin `/extensions`, `/show-prompt`, the `ask_user` questionnaire, the workspace folder-picker wait modal (footer cwd and sidebar Add workspace), the subagent inspect chat, the composer image preview, and the diff modal are overlay modals.

`z` sorts views in a slot: lower `z` runs first, higher `z` later. Max 16 views per slot (`PICO_MAX_SLOT_VIEWS`). Host views in a slot are process-global. Workspace views in a slot render only for the selected workspace.

## Empty state

When the chat has no messages, builtin `chat` shows Tools / Context / Skills cards. Extensions register extra Clay with `pico_workspace_add_empty_view` during `workspace_init`, not a shell slot — the empty state is inside the chat column.

```c
pico_workspace_add_empty_view(workspace, PICO_EMPTY_ABOVE, 0, BannerRender);
```

Kinds:

- `PICO_EMPTY_ABOVE` — stacked above the three cards.
- `PICO_EMPTY_BELOW` — stacked below the three cards.
- `PICO_EMPTY_REPLACE` — takes over the empty state. If any REPLACE view is registered, Pico skips the builtin cards and every ABOVE/BELOW view. Only REPLACE views run (sorted by `z`).

`z` sorts within a kind the same way as slots. Max 16 empty views total (`PICO_MAX_EMPTY_VIEWS`). Invalid kind is a silent no-op.

ABOVE/BELOW sit in the same `chat_width` column as the cards (user-global `settings.json`, default 90 characters). REPLACE callbacks are children of `ChatContent` (that same column); you own the layout.

Full file for a banner: [`../../examples/empty_banner.c`](../../examples/empty_banner.c).

Replace the whole empty state:

```c
static void CustomEmpty(PicoWorkspace *workspace, PicoAgentId selected_agent_id, void *state)
{
    (void)workspace;
    (void)selected_agent_id;
    (void)state;
    CLAY(CLAY_ID("MyEmpty"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 8,
                     .sizing = {.width = CLAY_SIZING_GROW(0)}}})
    {
        CLAY_TEXT(CLAY_STRING("Ready when you are"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_TITLE, .textColor = COLOR_TEXT}));
    }
}

static int CustomEmptyInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_workspace_add_empty_view(workspace, PICO_EMPTY_REPLACE, 0, CustomEmpty);
    return 0;
}
```

## Contract

- Render callbacks run on the **main thread** inside Clay layout. They are declarative and may run more than once per displayed frame when Pico performs a same-frame reflow; do not mutate durable state, perform I/O, or consume input in them. Put those effects in `on_frame` or a notification hook. Use Clay macros; fonts/colors from `pico/theme.h` (`FONT_*`, `COLOR_*`, and the type scale `PICO_FONT_CAPTION` 15 / `PICO_FONT_UI` 16 / `PICO_FONT_BODY` 18 / `PICO_FONT_TITLE` 20). Do not set `fontSize` below 14. Those values are design pixels; Pico multiplies them by user-global `settings.json` `font_scale` (default 1.0) at measure and draw. Direct `MeasureTextEx` / `DrawTextEx` must use `Pico_FontPx`. Explicit `lineHeight` must use `Pico_FontPxU16` (`PICO_FONT_CAPTION_LINE` 20 / `PICO_FONT_UI_LINE` 22 / `PICO_FONT_BODY_LINE` 26).
- Unique `CLAY_ID(...)` per element. Colliding IDs break layout.
- Pointer handling belongs in `PICO_HOOK_AFTER_LAYOUT`; extra drawing after Clay in `PICO_HOOK_AFTER_RENDER` (see `hooks.md`). Those hooks are host-scoped.
- Do not call Clay from a tool, provider, or workspace `on_frame` callback. Workspace `on_frame` runs for every `OPEN` or `RELOADING` workspace and must not draw.
- `PicoUi_ModalOpen` is true while the named modal stack is non-empty or a tool ask is showing. Builtin composer, chat, and footer skip input then. Core does not close your overlay on Esc during normal frames; it only suppresses turn-cancel while a claim is already on the stack at the start of the frame. Check `pico_ui_modal_is_top(host, name)` before consuming input, then close the top claim with `pico_ui_modal_pop`. A custom ask overlay still receives pointer hits; it may consume `GetCharPressed()` from `on_frame` after the composer has skipped.
- Answer a pending ask with `pico_tool_answer(host, ask.id, json)` from the main thread. `PicoToolAsk` also identifies `agent_id`, `profile`, and `purpose`; show these when the target may not be the visible agent. Bind buttons to the globally unique ask ID. The request string stays valid through Clay render of this frame even if you answer in `AFTER_LAYOUT`.
- The builtin `ask_user` overlay owns custom requests with `{"type":"questionnaire","ui":"custom",…}`. Use a different `type` for an extension-defined ask UI.
- A queued workspace reload keeps ask/cancel UI responsive while blocking new submits in that workspace. Do not cache selected-agent transcript indices or workspace-derived UI snapshots across selection or workspace close; IDs and borrowed messages become stale.

## Named modals

Claim input ownership from the main thread:

```c
if (pico_ui_modal_push(host, "my-modal"))
{
    /* PicoUi_ModalOpen is now true; draw your overlay in PICO_SLOT_OVERLAY. */
}
```

- `pico_ui_modal_push` copies a globally unique `name` (non-empty, shorter than `PICO_UI_MODAL_NAME`). It fails when that name is already claimed or the stack is full (`PICO_MAX_UI_MODALS`). Prefix extension modal names to avoid collisions with builtins.
- `pico_ui_modal_pop(host, name)` succeeds only when `name` is the current top.
- `pico_ui_modal_top` / `pico_ui_modal_is_top` / `pico_ui_modal_count` / `pico_ui_modal_has` / `pico_ui_modal_claimed` inspect the stack. Ask UI is not a named claim; `PicoUi_ModalOpen` ORs it in separately.
- Host-extension replacement closes every named modal before rebuilding host view/hook registrations. Do not attempt to preserve modal-local state across host reload.
- `pico_host_init` / `pico_host_free` also clear the stack and modal-local builtin state. Builtin inspect, extensions, prompt, diff, preview, footer menu, and folder picker use this same stack.
- Draw your own dimmer and card with unique `CLAY_ID`s. Do not reuse builtin IDs such as `SubagentModalDim`. Handle Esc and dimmer clicks yourself.

Full file: [`../../examples/modal.c`](../../examples/modal.c).

## Named mailbox

Tools cannot touch Clay. To stream into a named overlay, post from the worker and read the published snapshot on the main thread. Storage is workspace-owned and keyed by `(agent_id, runtime_generation, name)`:

```c
PicoUiPost post;
PicoAgentId id = pico_agent_active(host);
if (pico_agent_ui_latest(host, id, "web_search", &post))
{
    /* post.status / post.text are borrowed until the next pump or clear. */
}
pico_agent_ui_clear(host, id, "web_search");
```

`pico_ui_latest` / `pico_ui_clear` are the same lookup for the UI-selected agent. `pico_ui_post` copies immediately and publishes on the next pump. TEXT appends; STATUS replaces. Force-cancelled leftovers and retired generations are dropped. Two agents may use the same name without collision; each `(agent_id, runtime_generation, name)` occupies one of at most `PICO_MAX_UI_POSTS` (16) slots in that workspace. The snapshot survives the tool returning and extension reload; workspace close, force-cancel, generation retirement, and `pico_agent_ui_clear` drop it. See [tools](tools.md#streaming-into-a-modal) and [`../../examples/stream_modal.c`](../../examples/stream_modal.c).
