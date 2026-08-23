# Views

Add Clay UI into a fixed slot. You cannot remove or replace builtins; you add alongside them. The empty-state (no messages yet) is the exception: `pico_add_empty_view` can replace it or stack panels around the builtin cards. See [Empty state](#empty-state).

```c
#include "pico/plugin.h"
#include "clay/clay.h"

static void HelloRender(PicoApp *app)
{
    (void)app;
    CLAY(CLAY_ID("HelloExt"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 6}})
    {
        CLAY_TEXT(CLAY_STRING("hello"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 16, .textColor = COLOR_TEXT}));
    }
}

static void HelloInit(PicoApp *app)
{
    pico_add_view(app, PICO_SLOT_SIDEBAR, 0, HelloRender);
}
```

Full file: [`../../examples/hello.c`](../../examples/hello.c).

## Slots

- `PICO_SLOT_SIDEBAR` — left column. The sidebar exists only if at least one view is registered here.
- `PICO_SLOT_MAIN` — chat column (builtin `chat` already fills this).
- `PICO_SLOT_COMPOSER` — input box.
- `PICO_SLOT_FOOTER` — status line. Builtin footer: click cwd for a folder picker (native dialog; Pico dims the window with a “select a folder” modal until it closes), model/effort for dropdowns. In a git workspace with uncommitted changes the footer also shows a `+adds -dels` chip (including untracked files) that opens the unified diff modal.
- `PICO_SLOT_OVERLAY` — drawn after the shell (warnings, popups, modals). Builtin `/extensions`, `/show-prompt`, the `ask_user` questionnaire, the workspace folder-picker wait modal, the subagent inspect chat, and the diff modal are overlay modals.

`z` sorts views in a slot: lower `z` runs first, higher `z` later. Max 16 views per slot (`PICO_MAX_SLOT_VIEWS`).

## Empty state

When the chat has no messages, builtin `chat` shows Tools / Context / Skills cards. Extensions register extra Clay with `pico_add_empty_view`, not a shell slot — the empty state is inside the chat column.

```c
pico_add_empty_view(app, PICO_EMPTY_ABOVE, 0, BannerRender);
```

Kinds:

- `PICO_EMPTY_ABOVE` — stacked above the three cards.
- `PICO_EMPTY_BELOW` — stacked below the three cards.
- `PICO_EMPTY_REPLACE` — takes over the empty state. If any REPLACE view is registered, Pico skips the builtin cards and every ABOVE/BELOW view. Only REPLACE views run (sorted by `z`).

`z` sorts within a kind the same way as slots. Max 16 empty views total (`PICO_MAX_EMPTY_VIEWS`). Invalid kind is a silent no-op.

ABOVE/BELOW sit in the same max-900 column as the cards. REPLACE callbacks are children of `ChatContent` (full width); you own the layout.

Full file for a banner: [`../../examples/empty_banner.c`](../../examples/empty_banner.c).

Replace the whole empty state:

```c
static void CustomEmpty(PicoApp *app)
{
    (void)app;
    CLAY(CLAY_ID("MyEmpty"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 8,
                     .sizing = {.width = CLAY_SIZING_GROW(0, 900)}}})
    {
        CLAY_TEXT(CLAY_STRING("Ready when you are"),
                  CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 18, .textColor = COLOR_TEXT}));
    }
}

static void CustomEmptyInit(PicoApp *app)
{
    pico_add_empty_view(app, PICO_EMPTY_REPLACE, 0, CustomEmpty);
}
```

## Contract

- Render callbacks run on the **main thread** inside Clay layout. Use Clay macros; fonts/colors from `pico/theme.h` (`FONT_*`, `COLOR_*`). `fontSize` values are design pixels; Pico multiplies them by `font_scale` from `settings.json` (default 1.0) at measure and draw. Direct `MeasureTextEx` / `DrawTextEx` must use `Pico_FontPx`. Explicit `lineHeight` must use `Pico_FontPxU16`.
- Unique `CLAY_ID(...)` per element. Colliding IDs break layout.
- Pointer handling belongs in `PICO_HOOK_AFTER_LAYOUT`; extra drawing after Clay in `PICO_HOOK_AFTER_RENDER` (see `hooks.md`).
- Do not call Clay from a tool or provider callback (worker thread).
- `PicoUi_ModalOpen` is true while `/extensions` or `/show-prompt` is open, a footer menu or workspace folder picker is open, a subagent inspect chat is open, the diff modal is open, or a tool ask is pending. Builtin composer, chat, and footer skip input then. A custom ask overlay still receives pointer hits; it may consume `GetCharPressed()` from `on_frame` after the composer has skipped.
- Answer a pending ask with `pico_tool_answer(app, ask.id, json)` from the main thread. `PicoToolAsk` also identifies `agent_id`, `profile`, and `purpose`; show these when the target may not be the visible agent. Bind buttons to the globally unique ask ID. The request string stays valid through Clay render of this frame even if you answer in `AFTER_LAYOUT`.
- The builtin `ask_user` overlay owns custom requests with `{"type":"questionnaire","ui":"custom",…}`. Use a different `type` for an extension-defined ask UI.
- A queued reload or workspace change keeps ask/cancel UI responsive while blocking new submits. Do not cache active-agent transcript indices or workspace-derived UI snapshots across workspace replacement; IDs and borrowed messages become stale.
