#include "pico/app.h"
#include "pico/md_view.h"

#include "clay/clay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Clay_Raylib_Render(Clay_RenderCommandArray renderCommands, Font *fonts);

void pico_add_view(PicoApp *app, PicoUiSlot slot, PicoViewFn render)
{
    if (slot < 0 || slot >= PICO_SLOT_COUNT)
    {
        return;
    }
    app->views[slot] = render;
}

void PicoApp_AddMessage(PicoApp *app, PicoRole role, const char *markdown)
{
    if (app->message_count >= app->message_capacity)
    {
        int capacity = app->message_capacity == 0 ? 8 : app->message_capacity * 2;
        PicoMessage *next = (PicoMessage *)realloc(app->messages, (size_t)capacity * sizeof(PicoMessage));
        if (!next)
        {
            return;
        }
        app->messages = next;
        app->message_capacity = capacity;
    }
    PicoMessage *msg = &app->messages[app->message_count++];
    msg->role = role;
    size_t len = markdown ? strlen(markdown) : 0;
    msg->source = (char *)malloc(len + 1);
    if (msg->source)
    {
        memcpy(msg->source, markdown ? markdown : "", len + 1);
    }
    msg->doc = MdDocument_Parse(markdown ? markdown : "", len);
    app->chat_follow_bottom = true;
}

void PicoApp_Submit(PicoApp *app)
{
    PicoComposer *c = &app->composer;
    int start = 0;
    int end = c->length;
    while (start < end && (c->text[start] == ' ' || c->text[start] == '\n' || c->text[start] == '\t'))
    {
        start++;
    }
    while (end > start && (c->text[end - 1] == ' ' || c->text[end - 1] == '\n' || c->text[end - 1] == '\t'))
    {
        end--;
    }
    if (end <= start)
    {
        return;
    }

    char saved = c->text[end];
    c->text[end] = '\0';
    PicoApp_AddMessage(app, PICO_ROLE_USER, c->text + start);
    c->text[end] = saved;

    char reply[4096];
    snprintf(reply, sizeof(reply),
             "Got it — this slice has **no LLM**. Dummy reply to:\n\n> %s\n\n"
             "Enter more markdown in the composer to try the renderer. `Shift+Enter` adds a newline.",
             c->text + start);
    PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, reply);

    c->length = 0;
    c->cursor = 0;
    c->sel_anchor = 0;
    if (c->text)
    {
        c->text[0] = '\0';
    }
}

static const char *kDummyUser1 = "What is Pico?";

static const char *kDummyAssistant1 =
    "Pico is a **small C agent harness**. Clay is only the layout library.\n\n"
    "This first slice is just the shell:\n\n"
    "- Markdown chat bubbles (the renderer you already had)\n"
    "- A composer with readline-ish keys\n"
    "- A footer for status\n\n"
    "There is no model yet. Submit text to append a dummy reply.\n\n"
    "```c\n"
    "pico_add_view(app, PICO_SLOT_MAIN, PicoChat_Render);\n"
    "```";

static const char *kDummyUser2 = "Show me a list and a quote.";

static const char *kDummyAssistant2 =
    "> Extensions will do the interesting work. The core stays tiny.\n\n"
    "- **Ctrl+A / Ctrl+E** — line start / end\n"
    "- **Ctrl+W** — delete previous word\n"
    "- **Ctrl+K** — kill to end of line\n"
    "- **Ctrl+V** — paste (long clips go to `/tmp/pico-paste-…`)\n"
    "- **F3** — Clay debug overlay";

void PicoApp_Init(PicoApp *app, Font *fonts)
{
    memset(app, 0, sizeof(*app));
    app->fonts = fonts;
    app->model_name = "dummy-model";
    app->tokens_used = 0;
    app->tokens_limit = 128000;
    app->agent_state = PICO_AGENT_IDLE;
    app->selected_message = -1;
    app->chat_overflow = true;
    app->composer.capacity = 256;
    app->composer.text = (char *)malloc((size_t)app->composer.capacity);
    if (app->composer.text)
    {
        app->composer.text[0] = '\0';
    }

    pico_add_view(app, PICO_SLOT_MAIN, PicoChat_Render);
    pico_add_view(app, PICO_SLOT_COMPOSER, PicoComposer_Render);
    pico_add_view(app, PICO_SLOT_FOOTER, PicoFooter_Render);

    PicoApp_AddMessage(app, PICO_ROLE_USER, kDummyUser1);
    PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, kDummyAssistant1);
    PicoApp_AddMessage(app, PICO_ROLE_USER, kDummyUser2);
    PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, kDummyAssistant2);
}

void PicoApp_Free(PicoApp *app)
{
    for (int i = 0; i < app->message_count; i++)
    {
        free(app->messages[i].source);
        MdDocument_Free(&app->messages[i].doc);
    }
    free(app->messages);
    free(app->composer.text);
    memset(app, 0, sizeof(*app));
}

static Clay_RenderCommandArray CreateShellLayout(PicoApp *app)
{
    Clay_BeginLayout();
    MdView_BeginFrame();

    CLAY(CLAY_ID("Root"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                     .padding = {CONTENT_PADDING, 12, 16, 12},
                     .childGap = 12},
          .backgroundColor = COLOR_BG})
    {
        if (app->views[PICO_SLOT_SIDEBAR])
        {
            app->views[PICO_SLOT_SIDEBAR](app);
        }
        CLAY(CLAY_ID("MainColumn"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                         .childGap = 12}})
        {
            if (app->views[PICO_SLOT_MAIN])
            {
                app->views[PICO_SLOT_MAIN](app);
            }
            if (app->views[PICO_SLOT_COMPOSER])
            {
                app->views[PICO_SLOT_COMPOSER](app);
            }
        }
        if (app->views[PICO_SLOT_FOOTER])
        {
            app->views[PICO_SLOT_FOOTER](app);
        }
    }
    if (app->views[PICO_SLOT_OVERLAY])
    {
        app->views[PICO_SLOT_OVERLAY](app);
    }

    app->hovered_link = MdView_HoveredLink();
    return Clay_EndLayout(GetFrameTime());
}

static void UpdateChatScrollbarDrag(PicoApp *app, Clay_Vector2 mouse)
{
    PicoScrollbar *drag = &app->chat_scrollbar;
    if (!IsMouseButtonDown(0))
    {
        drag->mouse_down = false;
    }
    if (IsMouseButtonDown(0) && !drag->mouse_down &&
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScrollBarHandle"))))
    {
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
        if (data.found)
        {
            drag->click_origin = mouse;
            drag->position_origin = *data.scrollPosition;
            drag->mouse_down = true;
        }
    }
    else if (drag->mouse_down)
    {
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
        if (data.found && data.contentDimensions.height > 0)
        {
            float ratio = data.contentDimensions.height / data.scrollContainerDimensions.height;
            data.scrollPosition->y = drag->position_origin.y + (drag->click_origin.y - mouse.y) * ratio;
        }
    }
}

void PicoApp_Frame(PicoApp *app)
{
    Vector2 mouse_delta = GetMouseWheelMoveV();
    mouse_delta.x *= 5.0f;
    mouse_delta.y *= 5.0f;

    if (IsKeyPressed(KEY_F3))
    {
        app->debug_enabled = !app->debug_enabled;
        Clay_SetDebugModeEnabled(app->debug_enabled);
    }
    if (IsKeyPressed(KEY_F12))
    {
        TakeScreenshot("pico_screenshot.png");
    }

    PicoComposer_HandleInput(app);

    Clay_Vector2 mouse_position = {.x = GetMousePosition().x, .y = GetMousePosition().y};
    Clay_SetPointerState(mouse_position, IsMouseButtonDown(0) && !app->chat_scrollbar.mouse_down);
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()});

    UpdateChatScrollbarDrag(app, mouse_position);
    Clay_UpdateScrollContainers(true, (Clay_Vector2){mouse_delta.x, mouse_delta.y}, GetFrameTime());

    Clay_RenderCommandArray render_commands = CreateShellLayout(app);

    Clay_ScrollContainerData scroll_data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
    app->chat_overflow =
        scroll_data.found && scroll_data.contentDimensions.height > scroll_data.scrollContainerDimensions.height + 0.5f;

    PicoComposer_HandlePointer(app);
    PicoChat_HandlePointer(app);

    if (app->hovered_link)
    {
        SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    }
    else if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("Composer"))))
    {
        SetMouseCursor(MOUSE_CURSOR_IBEAM);
    }
    else
    {
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    }

    if (app->chat_follow_bottom)
    {
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
        if (data.found && data.scrollPosition &&
            data.contentDimensions.height > data.scrollContainerDimensions.height)
        {
            data.scrollPosition->y =
                data.scrollContainerDimensions.height - data.contentDimensions.height;
        }
        app->chat_follow_bottom = false;
    }

    if (app->hovered_link && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    {
        OpenURL(app->hovered_link);
    }

    BeginDrawing();
    ClearBackground((Color){(unsigned char)COLOR_BG.r, (unsigned char)COLOR_BG.g, (unsigned char)COLOR_BG.b, 255});
    Clay_Raylib_Render(render_commands, app->fonts);
    PicoComposer_DrawOverlay(app);
    EndDrawing();
}
