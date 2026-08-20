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
- `PICO_SLOT_FOOTER` — status line.
- `PICO_SLOT_OVERLAY` — drawn after the shell (warnings, popups, modals). Builtin `/extensions` is an overlay modal.

`z` sorts views in a slot: lower `z` runs first, higher `z` later. Max 16 views per slot (`PICO_MAX_SLOT_VIEWS`).

## Contract

- Render callbacks run on the **main thread** inside Clay layout. Use Clay macros; fonts/colors from `pico/theme.h` (`FONT_*`, `COLOR_*`).
- Unique `CLAY_ID(...)` per element. Colliding IDs break layout.
- Pointer handling belongs in `PICO_HOOK_AFTER_LAYOUT`; extra drawing after Clay in `PICO_HOOK_AFTER_RENDER` (see `hooks.md`).
- Do not call Clay from a tool or provider callback (worker thread).
