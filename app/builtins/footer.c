#define _DEFAULT_SOURCE

#include "../agent_internal.h"
#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "agent.h"
#include "overlay.h"
#include "settings.h"
#include "tinyfiledialogs.h"
#include "usage.h"

#include "clay/clay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum FooterMenu {
    FOOTER_MENU_NONE = 0,
    FOOTER_MENU_MODEL,
    FOOTER_MENU_EFFORT,
} FooterMenu;

static char g_cwd[4096];
static char g_state[64];
static char g_tokens[128];
static char g_extra[64];
static char g_model[128];
static char g_effort[PICO_EFFORT_LEN];

static FooterMenu g_menu;
static int g_selected;
static bool g_want_folder;
static bool g_esc_block;

bool PicoFooter_MenuOpen(void)
{
    return g_menu != FOOTER_MENU_NONE || g_esc_block;
}

static const char *AgentStateName(const PicoApp *app)
{
    const PicoAgent *agent = PicoApp_ActiveAgentConst(app);
    switch (agent->state)
    {
        case PICO_AGENT_IDLE:
            return "idle";
        case PICO_AGENT_LLM_WAIT:
            return "waiting on model";
        case PICO_AGENT_TOOL_WAIT:
            return PicoAgent_AskUiOpen(agent) ? "waiting for you" : "running tool";
        case PICO_AGENT_COMPACT_WAIT:
            return "compacting";
        case PICO_AGENT_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

static void FormatCwd(const char *workspace, char *out, size_t cap)
{
    char real[4096];
    const char *src = workspace && workspace[0] ? workspace : ".";
    if (!realpath(src, real))
    {
        snprintf(real, sizeof(real), "%s", src);
    }

    const char *home = getenv("HOME");
    if (home && home[0])
    {
        size_t n = strlen(home);
        while (n > 1 && home[n - 1] == '/')
        {
            n--;
        }
        if (strncmp(real, home, n) == 0 && (real[n] == '\0' || real[n] == '/'))
        {
            snprintf(out, cap, "~%s", real + n);
            return;
        }
    }
    snprintf(out, cap, "%s", real);
}

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static bool Over(const char *id)
{
    return Clay_PointerOver(Clay_GetElementId(CStr(id)));
}

static void MutedText(const char *s)
{
    CLAY_TEXT(CStr(s), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = 13,
                                        .textColor = COLOR_MUTED,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
}

static void Sep(void)
{
    MutedText("  ·  ");
}

static int MenuCount(const PicoApp *app)
{
    if (g_menu == FOOTER_MENU_MODEL)
    {
        return app->model_count;
    }
    if (g_menu == FOOTER_MENU_EFFORT)
    {
        const PicoModel *m = PicoSettings_ActiveModelConst(app, PicoApp_ActiveAgentConst(app));
        return m ? m->effort_count : 0;
    }
    return 0;
}

static int HoveredItem(const PicoApp *app)
{
    int n = MenuCount(app);
    for (int i = 0; i < n; i++)
    {
        if (Clay_PointerOver(CLAY_IDI("FooterMenuItem", i)))
        {
            return i;
        }
    }
    return -1;
}

static void SelectHovered(PicoApp *app)
{
    Vector2 delta = GetMouseDelta();
    if (delta.x == 0.0f && delta.y == 0.0f)
    {
        return;
    }
    int hovered = HoveredItem(app);
    if (hovered >= 0)
    {
        g_selected = hovered;
    }
}

static void CloseMenu(void)
{
    g_menu = FOOTER_MENU_NONE;
    g_selected = 0;
}

static void OpenMenu(PicoApp *app, FooterMenu which)
{
    if (g_menu == which)
    {
        CloseMenu();
        return;
    }
    if (which == FOOTER_MENU_MODEL && app->model_count <= 0)
    {
        return;
    }
    if (which == FOOTER_MENU_EFFORT)
    {
        PicoModel *m = PicoSettings_ActiveModel(app, PicoApp_ActiveAgent(app));
        if (!m || m->effort_count <= 0)
        {
            return;
        }
    }

    g_menu = which;
    g_selected = 0;
    if (which == FOOTER_MENU_MODEL)
    {
        for (int i = 0; i < app->model_count; i++)
        {
            if (strcmp(app->models[i].id, app->settings.model) == 0)
            {
                g_selected = i;
                break;
            }
        }
    }
    else
    {
        PicoModel *m = PicoSettings_ActiveModel(app, PicoApp_ActiveAgent(app));
        const char *cur = PicoSettings_ActiveEffort(PicoApp_ActiveAgent(app));
        if (m && cur)
        {
            for (int i = 0; i < m->effort_count; i++)
            {
                if (strcmp(m->effort[i], cur) == 0)
                {
                    g_selected = i;
                    break;
                }
            }
        }
    }
}

static void Accept(PicoApp *app)
{
    if (g_menu == FOOTER_MENU_MODEL && g_selected >= 0 && g_selected < app->model_count)
    {
        PicoSettings_SetModel(app, PicoApp_ActiveAgent(app), app->models[g_selected].id);
    }
    else if (g_menu == FOOTER_MENU_EFFORT)
    {
        PicoModel *m = PicoSettings_ActiveModel(app, PicoApp_ActiveAgent(app));
        if (m && g_selected >= 0 && g_selected < m->effort_count)
        {
            PicoSettings_SetEffort(app, PicoApp_ActiveAgent(app), m->effort[g_selected]);
        }
    }
    CloseMenu();
}

static bool FolderDialogGraphic(void)
{
    tinyfd_forceConsole = 0;
    (void)tinyfd_selectFolderDialog("tinyfd_query", NULL);
    if (!tinyfd_response[0])
    {
        return false;
    }
    return strcmp(tinyfd_response, "dialog") != 0 && strcmp(tinyfd_response, "whiptail") != 0 &&
           strcmp(tinyfd_response, "basicinput") != 0 && strcmp(tinyfd_response, "no_solution") != 0;
}

static void RequestFolder(PicoApp *app)
{
    CloseMenu();
    if (PicoAgent_IsBusy(PicoApp_ActiveAgent(app)))
    {
        PicoOverlay_Notify(app, "Wait until the agent is idle before changing directory.");
        return;
    }
    g_want_folder = true;
}

static void RenderMenu(PicoApp *app)
{
    int n = MenuCount(app);
    if (n <= 0)
    {
        return;
    }
    SelectHovered(app);
    float row_h = 26.0f;
    float content_h = 12.0f + (float)n * row_h;
    bool scroll = content_h > 240.0f;
    float h = scroll ? 240.0f : content_h;

    CLAY(CLAY_ID("FooterMenu"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                       .zIndex = 25,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_BOTTOM,
                                        .parent = CLAY_ATTACH_POINT_RIGHT_TOP},
                       .offset = {.y = -6}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {6, 6, 6, 6},
                     .childGap = 2,
                     .sizing = {.width = CLAY_SIZING_FIT(180, 420),
                                .height = scroll ? CLAY_SIZING_FIXED(h) : CLAY_SIZING_FIT(0)}},
          .clip = {.vertical = scroll,
                   .horizontal = false,
                   .childOffset = scroll ? Clay_GetScrollOffset() : (Clay_Vector2){0}},
          .backgroundColor = COLOR_CONTENT_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        for (int i = 0; i < n; i++)
        {
            const char *label = "";
            const char *detail = "";
            if (g_menu == FOOTER_MENU_MODEL)
            {
                PicoModel *m = &app->models[i];
                label = m->name[0] ? m->name : m->id;
                detail = m->name[0] ? m->id : "";
            }
            else
            {
                PicoModel *m = PicoSettings_ActiveModel(app, PicoApp_ActiveAgent(app));
                label = m ? m->effort[i] : "";
            }
            Clay_Color bg = i == g_selected ? COLOR_CODE_BG : COLOR_CONTENT_BG;
            CLAY(CLAY_IDI("FooterMenuItem", i),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = 8,
                             .padding = {8, 8, 4, 4},
                             .sizing = {.width = CLAY_SIZING_GROW(0)}},
                  .backgroundColor = bg,
                  .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                                        .fontSize = 14,
                                                        .textColor = COLOR_TEXT,
                                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
                if (detail[0])
                {
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
                    CLAY_TEXT(CStr(detail), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                             .fontSize = 13,
                                                             .textColor = COLOR_MUTED,
                                                             .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
            }
        }
    }
}

static void Chip(Clay_ElementId id, const char *text, bool open, bool with_menu, PicoApp *app)
{
    bool hovered = Clay_PointerOver(id) || open;
    Clay_Color color = hovered ? COLOR_TEXT : COLOR_MUTED;
    CLAY(id, {.layout = {.sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0)}}})
    {
        CLAY_TEXT(CStr(text), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                               .fontSize = 13,
                                               .textColor = color,
                                               .wrapMode = CLAY_TEXT_WRAP_NONE}));
        if (with_menu && open)
        {
            RenderMenu(app);
        }
    }
}

void PicoFooter_Render(PicoApp *app)
{
    const char *extra = "";
    if (PicoAgent_IsBusy(PicoApp_ActiveAgent(app)) && PicoAgent_CancelRequested(PicoApp_ActiveAgent(app)))
    {
        extra = "Esc again to force";
    }
    else if (PicoAgent_IsBusy(PicoApp_ActiveAgent(app)))
    {
        extra = "Esc to cancel";
    }
    else if (PicoApp_ActiveAgent(app)->state == PICO_AGENT_ERROR)
    {
        extra = "Esc to dismiss";
    }

    FormatCwd(app->workspace, g_cwd, sizeof(g_cwd));
    snprintf(g_state, sizeof(g_state), "%s", AgentStateName(app));
    snprintf(g_extra, sizeof(g_extra), "%s", extra);
    int cache_percent = 0;
    if (PicoUsage_SessionPercent(PicoApp_ActiveAgent(app), &cache_percent))
    {
        snprintf(g_tokens, sizeof(g_tokens), "%d / %d tokens  ·  %d%% cache", PicoApp_ActiveAgent(app)->tokens_used,
                 PicoApp_ActiveAgent(app)->context_limit, cache_percent);
    }
    else
    {
        snprintf(g_tokens, sizeof(g_tokens), "%d / %d tokens", PicoApp_ActiveAgent(app)->tokens_used, PicoApp_ActiveAgent(app)->context_limit);
    }

    PicoModel *active = PicoSettings_ActiveModel(app, PicoApp_ActiveAgent(app));
    bool show_effort = active && active->effort_count > 0;
    const char *model = PicoApp_ActiveAgent(app)->model_name ? PicoApp_ActiveAgent(app)->model_name : "?";
    snprintf(g_model, sizeof(g_model), "%s", model);
    if (show_effort)
    {
        const char *effort = PicoSettings_ActiveEffort(PicoApp_ActiveAgent(app));
        snprintf(g_effort, sizeof(g_effort), "%s", effort ? effort : "none");
    }
    else
    {
        g_effort[0] = '\0';
        if (g_menu == FOOTER_MENU_EFFORT)
        {
            CloseMenu();
        }
    }

    CLAY(CLAY_ID("Footer"),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .padding = {14, 14, 8, 8},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}},
          .backgroundColor = COLOR_FOOTER_BG})
    {
        Chip(CLAY_ID("FooterCwd"), g_cwd, false, false, app);
        Sep();
        MutedText(g_state);
        Sep();
        MutedText(g_tokens);
        if (g_extra[0])
        {
            Sep();
            MutedText(g_extra);
        }
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
        Chip(CLAY_ID("FooterModel"), g_model, g_menu == FOOTER_MENU_MODEL, true, app);
        if (show_effort)
        {
            Sep();
            Chip(CLAY_ID("FooterEffort"), g_effort, g_menu == FOOTER_MENU_EFFORT, true, app);
        }
    }
}

static void FooterAfterLayout(PicoApp *app, const PicoHookEvent *event)
{
    (void)event;
    if (PicoExts_IsOpen() || PicoAgent_AskUiOpen(PicoApp_ActiveAgent(app)))
    {
        app->hovered_clickable = false;
        return;
    }

    int hovered = HoveredItem(app);
    app->hovered_clickable = Over("FooterCwd") || Over("FooterModel") || Over("FooterEffort") || hovered >= 0 ||
                             (g_menu != FOOTER_MENU_NONE && Over("FooterMenu"));

    if (g_menu != FOOTER_MENU_NONE)
    {
        SelectHovered(app);
    }

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return;
    }

    if (g_menu != FOOTER_MENU_NONE)
    {
        if (hovered >= 0)
        {
            g_selected = hovered;
            Accept(app);
            return;
        }
        if (Over("FooterMenu"))
        {
            return;
        }
        if (Over("FooterModel"))
        {
            OpenMenu(app, FOOTER_MENU_MODEL);
            return;
        }
        if (Over("FooterEffort"))
        {
            OpenMenu(app, FOOTER_MENU_EFFORT);
            return;
        }
        if (Over("FooterCwd"))
        {
            RequestFolder(app);
            return;
        }
        CloseMenu();
        return;
    }

    if (Over("FooterCwd"))
    {
        RequestFolder(app);
    }
    else if (Over("FooterModel"))
    {
        OpenMenu(app, FOOTER_MENU_MODEL);
    }
    else if (Over("FooterEffort"))
    {
        OpenMenu(app, FOOTER_MENU_EFFORT);
    }
}

static void FooterOnFrame(PicoApp *app, float dt)
{
    (void)dt;
    g_esc_block = false;
    if (g_menu != FOOTER_MENU_NONE)
    {
        int n = MenuCount(app);
        if (IsKeyPressed(KEY_ESCAPE))
        {
            CloseMenu();
            g_esc_block = true;
            return;
        }
        if ((IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) && g_selected > 0)
        {
            g_selected--;
        }
        if ((IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) && g_selected + 1 < n)
        {
            g_selected++;
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_TAB))
        {
            Accept(app);
            return;
        }
    }

    if (!g_want_folder)
    {
        return;
    }
    g_want_folder = false;
    if (PicoAgent_IsBusy(PicoApp_ActiveAgent(app)))
    {
        PicoOverlay_Notify(app, "Wait until the agent is idle before changing directory.");
        return;
    }
    if (!FolderDialogGraphic())
    {
        PicoOverlay_Notify(app, "Folder dialog unavailable. Install zenity or kdialog.");
        return;
    }
    const char *start = app->workspace[0] ? app->workspace : NULL;
    char *path = tinyfd_selectFolderDialog("Workspace", start);
    if (path && path[0])
    {
        PicoApp_ChangeWorkspace(app, path);
    }
}

static void FooterInit(PicoApp *app)
{
    pico_add_view(app, PICO_SLOT_FOOTER, 0, PicoFooter_Render);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, FooterAfterLayout);
}

PicoExt pico_ext_footer(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "footer",
        .description = "Status bar",
        .init = FooterInit,
        .on_frame = FooterOnFrame,
    };
}
