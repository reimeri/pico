#include "pico/plugin.h"
#include "pico/md_view.h"
#include "agent.h"
#include "session.h"
#include "settings.h"
#include "auth.h"
#include "chat_sel.h"
#include "json.h"

#include "clay/clay.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Clay_Raylib_Render(Clay_RenderCommandArray renderCommands, Font *fonts);

void pico_add_view(PicoApp *app, PicoUiSlot slot, int z, PicoViewFn render)
{
    if (slot < 0 || slot >= PICO_SLOT_COUNT || !render)
    {
        return;
    }
    int n = app->view_count[slot];
    if (n >= PICO_MAX_SLOT_VIEWS)
    {
        return;
    }
    int i = n;
    while (i > 0 && app->views[slot][i - 1].z > z)
    {
        app->views[slot][i] = app->views[slot][i - 1];
        i--;
    }
    app->views[slot][i].render = render;
    app->views[slot][i].z = z;
    app->view_count[slot]++;
}

void pico_add_hook(PicoApp *app, PicoHook hook, PicoHookFn fn)
{
    if (!fn || app->hook_count >= PICO_MAX_HOOKS)
    {
        return;
    }
    app->hooks[app->hook_count].hook = hook;
    app->hooks[app->hook_count].fn = fn;
    app->hook_count++;
}

void pico_add_tool(PicoApp *app, const char *name, const char *description, const char *params_json, PicoToolFn run)
{
    if (!name || !run || app->tool_count >= PICO_MAX_TOOLS)
    {
        return;
    }
    app->tools[app->tool_count].name = name;
    app->tools[app->tool_count].description = description;
    app->tools[app->tool_count].params_json = params_json;
    app->tools[app->tool_count].run = run;
    app->tool_count++;
}

void pico_add_command(PicoApp *app, const char *name, const char *help, PicoCmdFn run)
{
    if (!name || !run || app->command_count >= PICO_MAX_COMMANDS)
    {
        return;
    }
    app->commands[app->command_count].name = name;
    app->commands[app->command_count].help = help;
    app->commands[app->command_count].run = run;
    app->command_count++;
}

void pico_add_completer(PicoApp *app, char trigger, bool bol_only, PicoCompleteQueryFn query,
                        PicoCompleteAcceptFn accept)
{
    if (!query || app->completer_count >= PICO_MAX_COMPLETERS)
    {
        return;
    }
    app->completers[app->completer_count].trigger = trigger;
    app->completers[app->completer_count].bol_only = bol_only;
    app->completers[app->completer_count].query = query;
    app->completers[app->completer_count].accept = accept;
    app->completer_count++;
}

void pico_add_provider(PicoApp *app, const PicoProvider *p)
{
    if (!app || !p || !p->name || !p->name[0] || !p->stream || app->provider_count >= PICO_MAX_PROVIDERS)
    {
        return;
    }
    app->providers[app->provider_count] = *p;
    app->provider_count++;
}

const PicoProvider *pico_find_provider(const PicoApp *app, const char *name)
{
    if (!app || !name || !name[0])
    {
        return NULL;
    }
    for (int i = 0; i < app->provider_count; i++)
    {
        if (app->providers[i].name && strcmp(app->providers[i].name, name) == 0)
        {
            return &app->providers[i];
        }
    }
    return NULL;
}

void pico_llm_result_free(PicoLlmResult *r)
{
    if (!r)
    {
        return;
    }
    free(r->error);
    free(r->assistant_text);
    free(r->think_text);
    for (int i = 0; i < r->call_count; i++)
    {
        free(r->calls[i].call_id);
        free(r->calls[i].name);
        free(r->calls[i].arguments);
    }
    free(r->calls);
    for (int i = 0; i < r->raw_count; i++)
    {
        free(r->raw_items[i]);
    }
    free(r->raw_items);
    memset(r, 0, sizeof(*r));
}

void pico_clear_registrations(PicoApp *app)
{
    memset(app->views, 0, sizeof(app->views));
    memset(app->view_count, 0, sizeof(app->view_count));
    memset(app->hooks, 0, sizeof(app->hooks));
    app->hook_count = 0;
    memset(app->tools, 0, sizeof(app->tools));
    app->tool_count = 0;
    memset(app->commands, 0, sizeof(app->commands));
    app->command_count = 0;
    memset(app->completers, 0, sizeof(app->completers));
    app->completer_count = 0;
    memset(app->providers, 0, sizeof(app->providers));
    app->provider_count = 0;
    memset(app->auths, 0, sizeof(app->auths));
    app->auth_count = 0;
}

void pico_run_hooks(PicoApp *app, PicoHook hook)
{
    for (int i = 0; i < app->hook_count; i++)
    {
        if (app->hooks[i].hook == hook && app->hooks[i].fn)
        {
            app->hooks[i].fn(app);
        }
    }
}

static char LayoutLetter(int key)
{
    const char *name = glfwGetKeyName(key, 0);
    if (name && name[0] && (unsigned char)name[0] < 128 && name[1] == '\0')
    {
        char c = name[0];
        if (c >= 'A' && c <= 'Z')
        {
            c = (char)(c - 'A' + 'a');
        }
        if (c >= 'a' && c <= 'z')
        {
            return c;
        }
    }
    if (key >= KEY_A && key <= KEY_Z)
    {
        return (char)(key - KEY_A + 'a');
    }
    return 0;
}

static bool LayoutKeyHit(char letter, bool include_repeat)
{
    if (letter >= 'A' && letter <= 'Z')
    {
        letter = (char)(letter - 'A' + 'a');
    }
    if (letter < 'a' || letter > 'z')
    {
        return false;
    }
    for (int key = KEY_A; key <= KEY_Z; key++)
    {
        if (!IsKeyPressed(key) && !(include_repeat && IsKeyPressedRepeat(key)))
        {
            continue;
        }
        if (LayoutLetter(key) == letter)
        {
            return true;
        }
    }
    return false;
}

bool Pico_ShortcutPressed(char letter)
{
    return LayoutKeyHit(letter, false);
}

bool Pico_ShortcutRepeat(char letter)
{
    return LayoutKeyHit(letter, true);
}

static void RunSlot(PicoApp *app, PicoUiSlot slot)
{
    for (int i = 0; i < app->view_count[slot]; i++)
    {
        if (app->views[slot][i].render)
        {
            app->views[slot][i].render(app);
        }
    }
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
    memset(msg, 0, sizeof(*msg));
    msg->role = role;
    size_t len = markdown ? strlen(markdown) : 0;
    msg->source = (char *)malloc(len + 1);
    if (msg->source)
    {
        memcpy(msg->source, markdown ? markdown : "", len + 1);
    }
    msg->doc = MdDocument_ParseEx(markdown ? markdown : "", len,
                                  role == PICO_ROLE_USER ? MD_PARSE_PRESERVE_NEWLINES : MD_PARSE_DEFAULT);
    app->chat_follow_bottom = true;
    pico_run_hooks(app, PICO_HOOK_ON_MESSAGE);
}

void PicoApp_AppendAssistant(PicoApp *app, const char *text)
{
    if (!text)
    {
        text = "";
    }
    if (app->message_count <= 0 || app->messages[app->message_count - 1].role != PICO_ROLE_ASSISTANT)
    {
        PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, text);
        return;
    }
    if (!text[0])
    {
        return;
    }
    PicoMessage *m = &app->messages[app->message_count - 1];
    size_t old = m->source ? strlen(m->source) : 0;
    size_t n = strlen(text);
    char *next = (char *)realloc(m->source, old + n + 1);
    if (!next)
    {
        return;
    }
    memcpy(next + old, text, n + 1);
    m->source = next;
    MdDocument_Free(&m->doc);
    m->doc = MdDocument_ParseEx(m->source, old + n, MD_PARSE_DEFAULT);
    app->chat_follow_bottom = true;
}

void PicoApp_AddToolCall(PicoApp *app, const char *name, const char *args)
{
    if (app->message_count <= 0 || app->messages[app->message_count - 1].role != PICO_ROLE_ASSISTANT)
    {
        PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, "");
    }
    PicoMessage *m = &app->messages[app->message_count - 1];
    PicoTraceLine *next =
        (PicoTraceLine *)realloc(m->trace, (size_t)(m->trace_count + 1) * sizeof(PicoTraceLine));
    if (!next)
    {
        return;
    }
    m->trace = next;
    PicoTraceLine *line = &m->trace[m->trace_count++];
    memset(line, 0, sizeof(*line));
    line->is_tool = true;
    line->tool_name = JsonDup(name && name[0] ? name : "tool");
    line->tool_args = JsonDup(args ? args : "");
}

void PicoApp_SetLastToolOutput(PicoApp *app, const char *output)
{
    if (app->message_count <= 0)
    {
        return;
    }
    PicoMessage *m = &app->messages[app->message_count - 1];
    for (int t = m->trace_count - 1; t >= 0; t--)
    {
        if (m->trace[t].is_tool)
        {
            free(m->trace[t].tool_output);
            m->trace[t].tool_output = JsonDup(output ? output : "");
            return;
        }
    }
}

void pico_session_log_custom(PicoApp *app, const char *ext, const char *data_json)
{
    PicoSession_LogCustom(app, ext, data_json);
}

void PicoApp_Submit(PicoApp *app)
{
    if (app->agent_state == PICO_AGENT_LLM_WAIT || app->agent_state == PICO_AGENT_TOOL_WAIT ||
        app->agent_state == PICO_AGENT_COMPACT_WAIT)
    {
        return;
    }

    PicoComposer *c = &app->composer;
    if (!c->text || c->length <= 0)
    {
        return;
    }
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
    if (start > 0)
    {
        memmove(c->text, c->text + start, (size_t)(end - start));
        end -= start;
    }
    c->length = end;
    c->text[c->length] = '\0';
    c->cursor = c->length;
    c->sel_anchor = c->length;

    free(app->agent_input);
    app->agent_input = NULL;
    app->submit_cancel = false;
    pico_run_hooks(app, PICO_HOOK_BEFORE_SUBMIT);
    if (app->submit_cancel)
    {
        free(app->agent_input);
        app->agent_input = NULL;
        return;
    }

    const char *display = c->text;
    const char *agent = app->agent_input && app->agent_input[0] ? app->agent_input : display;
    PicoApp_AddMessage(app, PICO_ROLE_USER, display);
    PicoSession_LogUser(app, agent, display);
    PicoAgent_StartTurn(app, agent);

    c->length = 0;
    c->cursor = 0;
    c->sel_anchor = 0;
    if (c->text)
    {
        c->text[0] = '\0';
    }
    free(app->agent_input);
    app->agent_input = NULL;
    pico_run_hooks(app, PICO_HOOK_ON_SUBMIT);
}

void PicoApp_Cancel(PicoApp *app)
{
    PicoAgent_Cancel(app);
}

void PicoApp_Init(PicoApp *app, Font *fonts, const char *workspace, bool safe_mode,
                 PicoSessionStart session_start, const char *session_file)
{
    memset(app, 0, sizeof(*app));
    app->fonts = fonts;
    app->agent_state = PICO_AGENT_IDLE;
    app->chat_sel.msg = -1;
    app->chat_overflow = true;
    app->safe_mode = safe_mode;
    if (workspace && workspace[0])
    {
        snprintf(app->workspace, sizeof(app->workspace), "%s", workspace);
    }
    else
    {
        snprintf(app->workspace, sizeof(app->workspace), ".");
    }
    app->composer.capacity = 256;
    app->composer.text = (char *)malloc((size_t)app->composer.capacity);
    if (app->composer.text)
    {
        app->composer.text[0] = '\0';
    }

    app->session_ephemeral = true;
    PicoSettings_Load(app);
    PicoAuth_Load(app);
    PicoAgent_Init(app);
    PicoPlugins_Load(app);
    PicoSession_Start(app, session_start, session_file);
}

void PicoApp_RequestReload(PicoApp *app)
{
    PicoPlugins_Reload(app);
}

void PicoApp_ClearMessages(PicoApp *app)
{
    if (!app)
    {
        return;
    }
    for (int i = 0; i < app->message_count; i++)
    {
        free(app->messages[i].source);
        for (int t = 0; t < app->messages[i].trace_count; t++)
        {
            free(app->messages[i].trace[t].text);
            free(app->messages[i].trace[t].tool_name);
            free(app->messages[i].trace[t].tool_args);
            free(app->messages[i].trace[t].tool_output);
        }
        free(app->messages[i].trace);
        MdDocument_Free(&app->messages[i].doc);
        memset(&app->messages[i], 0, sizeof(app->messages[i]));
    }
    app->message_count = 0;
    PicoChatSel_Clear(app);
}

void PicoApp_Free(PicoApp *app)
{
    /* A detached agent worker outlives us and can still reach the auth store to
     * refresh a token, so leave the store and the struct itself alone in that case;
     * the process is exiting anyway. Everything freed below is reached only through
     * the agent runtime, which the detach path already leaks. */
    bool agent_reaped = PicoAgent_Shutdown(app);
    PicoPlugins_Shutdown(app);
    if (agent_reaped)
    {
        PicoAuth_Free(app);
    }
    PicoApp_ClearMessages(app);
    free(app->messages);
    free(app->composer.text);
    free(app->status_warn);
    free(app->agent_error);
    free(app->compact_summary);
    free(app->models);
    free(app->agent_input);
    if (agent_reaped)
    {
        memset(app, 0, sizeof(*app));
    }
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
        CLAY(CLAY_ID("Body"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childGap = 12,
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}})
        {
            if (app->view_count[PICO_SLOT_SIDEBAR] > 0)
            {
                CLAY(CLAY_ID("Sidebar"),
                     {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .childGap = 8,
                                 .padding = {8, 8, 8, 8},
                                 .sizing = {.width = CLAY_SIZING_FIT(120, 280), .height = CLAY_SIZING_GROW(0)}},
                      .backgroundColor = COLOR_CONTENT_BG,
                      .cornerRadius = CLAY_CORNER_RADIUS(8)})
                {
                    RunSlot(app, PICO_SLOT_SIDEBAR);
                }
            }
            CLAY(CLAY_ID("MainColumn"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                             .childGap = 12}})
            {
                RunSlot(app, PICO_SLOT_MAIN);
                RunSlot(app, PICO_SLOT_COMPOSER);
            }
        }
        RunSlot(app, PICO_SLOT_FOOTER);
    }
    RunSlot(app, PICO_SLOT_OVERLAY);

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
    if (IsKeyPressed(KEY_F5))
    {
        PicoApp_RequestReload(app);
    }

    PicoPlugins_Poll(app);
    PicoAgent_Pump(app);
    if (app->reload_queued && !PicoAgent_BlocksReload(app))
    {
        PicoPlugins_Reload(app);
    }

    bool had_warn = app->status_warn != NULL;
    bool had_complete = PicoComplete_IsOpen();
    PicoPlugins_OnFrame(app, GetFrameTime());
    if (!had_warn && !had_complete && IsKeyPressed(KEY_ESCAPE))
    {
        if (PicoAgent_IsBusy(app))
        {
            if (PicoAgent_CancelRequested(app))
            {
                PicoAgent_ForceCancel(app);
            }
            else
            {
                PicoApp_Cancel(app);
            }
        }
        else if (app->agent_state == PICO_AGENT_ERROR)
        {
            PicoAgent_DismissError(app);
        }
    }

    Clay_Vector2 mouse_position = {.x = GetMousePosition().x, .y = GetMousePosition().y};
    bool composer_bar_drag = app->composer_scrollbar.mouse_down;
    bool over_composer = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("Composer")));
    bool over_chat = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScroll")));
    Clay_SetPointerState(mouse_position,
                         IsMouseButtonDown(0) && !app->chat_scrollbar.mouse_down && !composer_bar_drag);
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()});

    UpdateChatScrollbarDrag(app, mouse_position);
    Clay_UpdateScrollContainers(!over_composer && !over_chat && !composer_bar_drag &&
                                    !app->chat_sel.mouse_selecting,
                                (Clay_Vector2){mouse_delta.x, mouse_delta.y}, GetFrameTime());

    Clay_RenderCommandArray render_commands = CreateShellLayout(app);

    Clay_ScrollContainerData scroll_data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
    app->chat_overflow =
        scroll_data.found && scroll_data.contentDimensions.height > scroll_data.scrollContainerDimensions.height + 0.5f;

    pico_run_hooks(app, PICO_HOOK_AFTER_LAYOUT);

    if (app->hovered_link || app->hovered_tool)
    {
        SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    }
    else if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("CompScrollBarHandle"))) ||
             Clay_PointerOver(Clay_GetElementId(CLAY_STRING("CompScrollTrack"))) ||
             Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ChatScrollBarHandle"))))
    {
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    }
    else if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("Composer"))) || PicoChatSel_PointerOverText() ||
             app->chat_sel.mouse_selecting)
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
            data.scrollPosition->y = data.scrollContainerDimensions.height - data.contentDimensions.height;
        }
        app->chat_follow_bottom = false;
    }

    if (app->hovered_link && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !app->chat_sel.dragging &&
        !PicoChatSel_HasSelection(app))
    {
        OpenURL(app->hovered_link);
    }

    BeginDrawing();
    ClearBackground((Color){(unsigned char)COLOR_BG.r, (unsigned char)COLOR_BG.g, (unsigned char)COLOR_BG.b, 255});
    Clay_Raylib_Render(render_commands, app->fonts);
    pico_run_hooks(app, PICO_HOOK_AFTER_RENDER);
    EndDrawing();
}
