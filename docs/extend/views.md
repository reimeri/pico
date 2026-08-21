# Views

Add Clay UI into a fixed slot. You cannot remove or replace builtins; you add alongside them.

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

Full file: `examples/hello.c`.

## Slots

- `PICO_SLOT_SIDEBAR` — left column. The sidebar exists only if at least one view is registered here.
- `PICO_SLOT_MAIN` — chat column (builtin `chat` already fills this).
- `PICO_SLOT_COMPOSER` — input box.
- `PICO_SLOT_FOOTER` — status line. Builtin footer: click cwd for a folder picker, model/effort for dropdowns.
- `PICO_SLOT_OVERLAY` — drawn after the shell (warnings, popups, modals). Builtin `/extensions` is an overlay modal.

`z` sorts views in a slot: lower `z` runs first, higher `z` later. Max 16 views per slot (`PICO_MAX_SLOT_VIEWS`).

## Contract

- Render callbacks run on the **main thread** inside Clay layout. Use Clay macros; fonts/colors from `pico/theme.h` (`FONT_*`, `COLOR_*`).
- Unique `CLAY_ID(...)` per element. Colliding IDs break layout.
- Pointer handling belongs in `PICO_HOOK_AFTER_LAYOUT`; extra drawing after Clay in `PICO_HOOK_AFTER_RENDER` (see `hooks.md`).
- Do not call Clay from a tool or provider callback (worker thread).
- `PicoUi_ModalOpen` is true while `/extensions` is open, a footer menu is open, or a tool ask is pending. Builtin composer, chat, and footer skip input then. A custom ask overlay still receives pointer hits; it may consume `GetCharPressed()` from `on_frame` after the composer has skipped.
- Answer a pending ask with `pico_tool_answer(app, ask.id, json)` from the main thread. Bind buttons to the `id` from `pico_tool_pending_ask`. The request string stays valid through Clay render of this frame even if you answer in `AFTER_LAYOUT`.
