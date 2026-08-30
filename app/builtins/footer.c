#define _DEFAULT_SOURCE

#include "../agent_internal.h"
#define _POSIX_C_SOURCE 200809L
#include "host_internal.h"

#include "pico/plugin.h"
#include "agent.h"
#include "overlay.h"
#include "settings.h"
#include "scrollbar.h"
#include "tinyfiledialogs.h"
#include "usage.h"

#include "clay/clay.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum FooterMenu {
    FOOTER_MENU_NONE = 0,
    FOOTER_MENU_MODEL,
    FOOTER_MENU_EFFORT,
} FooterMenu;

typedef enum FooterStatusKind {
    FOOTER_STATUS_IDLE = 0,
    FOOTER_STATUS_RUNNING,
    FOOTER_STATUS_WAITING_USER,
    FOOTER_STATUS_ERROR,
} FooterStatusKind;

typedef struct FooterState {
    char cwd[4096];
    char state_str[64];
    char tokens[128];
    char extra[64];
    char model[128];
    char effort[PICO_EFFORT_LEN];
    PicoHost *app;
    FooterMenu menu;
    int selected;
    bool want_folder;
    bool folder_painted;
    bool esc_block;
    PicoScrollbar scrollbar;
} FooterState;

static __thread FooterState *s_active_footer_state = NULL;

static FooterState *ActiveFooterState(void)
{
    return s_active_footer_state;
}

#define g_cwd (ActiveFooterState()->cwd)
#define g_state (ActiveFooterState()->state_str)
#define g_tokens (ActiveFooterState()->tokens)
#define g_extra (ActiveFooterState()->extra)
#define g_model (ActiveFooterState()->model)
#define g_effort (ActiveFooterState()->effort)
#define g_app (ActiveFooterState()->app)
#define g_menu (ActiveFooterState()->menu)
#define g_selected (ActiveFooterState()->selected)
#define g_want_folder (ActiveFooterState()->want_folder)
#define g_folder_painted (ActiveFooterState()->folder_painted)
#define g_esc_block (ActiveFooterState()->esc_block)
#define g_scrollbar (ActiveFooterState()->scrollbar)

bool PicoFooter_MenuOpen(void)
{
    return g_menu != FOOTER_MENU_NONE || g_esc_block || g_want_folder;
}

static bool UnclaimMenu(void)
{
    return g_menu == FOOTER_MENU_NONE || !g_app ||
           pico_ui_modal_pop(g_app, "footer-menu");
}

static bool UnclaimFolder(void)
{
    return !g_want_folder || !g_app || pico_ui_modal_pop(g_app, "folder");
}

static bool ClearFolderRequest(void)
{
    if (!UnclaimFolder())
    {
        return false;
    }
    g_want_folder = false;
    g_folder_painted = false;
    return true;
}

static const char *AgentStateName(const PicoHost *app)
{
    const PicoAgent *agent = PicoHost_SelectedAgentConst(app);
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

static FooterStatusKind StatusKind(const PicoHost *app)
{
    const PicoAgent *agent = PicoHost_SelectedAgentConst(app);
    switch (agent->state)
    {
        case PICO_AGENT_ERROR:
            return FOOTER_STATUS_ERROR;
        case PICO_AGENT_TOOL_WAIT:
            return PicoAgent_AskUiOpen(agent) ? FOOTER_STATUS_WAITING_USER : FOOTER_STATUS_RUNNING;
        case PICO_AGENT_LLM_WAIT:
        case PICO_AGENT_COMPACT_WAIT:
            return FOOTER_STATUS_RUNNING;
        case PICO_AGENT_IDLE:
        default:
            return FOOTER_STATUS_IDLE;
    }
}

static Clay_Color StatusDotColor(FooterStatusKind kind)
{
    switch (kind)
    {
        case FOOTER_STATUS_WAITING_USER:
            return COLOR_STATUS_RUN;
        case FOOTER_STATUS_ERROR:
            return COLOR_STATUS_ERR;
        case FOOTER_STATUS_RUNNING:
            return COLOR_STATUS_ON;
        case FOOTER_STATUS_IDLE:
        default:
            return COLOR_STATUS_OFF;
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


static const char *FormatTokens(int tokens)
{
    static char buf[32];
    if (tokens < 1000)
    {
        snprintf(buf, sizeof(buf), "%d", tokens);
    }
    else if (tokens < 10000)
    {
        snprintf(buf, sizeof(buf), "%.2fk", tokens / 1000.0);
    }
    else if (tokens < 100000)
    {
        snprintf(buf, sizeof(buf), "%.1fk", tokens / 1000.0);
    }
    else if (tokens < 1000000)
    {
        snprintf(buf, sizeof(buf), "%dk", (int)((tokens + 500) / 1000));
    }
    else
    {
        snprintf(buf, sizeof(buf), "%.2fM", tokens / 1000000.0);
    }
    return buf;
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
                                        .fontSize = PICO_FONT_CAPTION,
                                        .textColor = COLOR_MUTED,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
}

static void Sep(void)
{
    MutedText("  ·  ");
}

static int MenuCount(const PicoHost *app)
{
    if (g_menu == FOOTER_MENU_MODEL)
    {
        const PicoWorkspace *ws = PicoHost_SelectedWorkspaceConst(app);
        return ws ? ws->model_count : 0;
    }
    if (g_menu == FOOTER_MENU_EFFORT)
    {
        const PicoModel *m = PicoSettings_ActiveModelConst(PicoHost_SelectedAgentConst(app));
        return m ? m->effort_count : 0;
    }
    return 0;
}

static int HoveredItem(const PicoHost *app)
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

static void SelectHovered(PicoHost *app)
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

static bool CloseMenu(void)
{
    if (!UnclaimMenu())
    {
        return false;
    }
    g_menu = FOOTER_MENU_NONE;
    g_selected = 0;
    return true;
}

static void OpenMenu(PicoHost *app, FooterMenu which)
{
    if (g_menu == which)
    {
        CloseMenu();
        return;
    }
    PicoWorkspace *ws = PicoHost_SelectedWorkspace(app);
    if (which == FOOTER_MENU_MODEL && (!ws || ws->model_count <= 0))
    {
        return;
    }
    if (which == FOOTER_MENU_EFFORT)
    {
        PicoModel *m = PicoSettings_ActiveModel(PicoHost_SelectedAgent(app));
        if (!m || m->effort_count <= 0)
        {
            return;
        }
    }

    if (g_menu == FOOTER_MENU_NONE &&
        (!g_app || !pico_ui_modal_push(g_app, "footer-menu")))
    {
        return;
    }
    g_menu = which;
    g_selected = 0;
    if (which == FOOTER_MENU_MODEL)
    {
        PicoAgent *agent = PicoHost_SelectedAgent(app);
        const char *active_id = agent ? agent->model : "";
        for (int i = 0; ws && i < ws->model_count; i++)
        {
            if (strcmp(ws->models[i].id, active_id) == 0)
            {
                g_selected = i;
                break;
            }
        }
    }
    else
    {
        PicoModel *m = PicoSettings_ActiveModel(PicoHost_SelectedAgent(app));
        const char *cur = PicoSettings_ActiveEffort(PicoHost_SelectedAgent(app));
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

static void Accept(PicoHost *app)
{
    PicoAgent *agent = PicoHost_SelectedAgent(app);
    PicoWorkspace *ws = PicoHost_SelectedWorkspace(app);
    if (g_menu == FOOTER_MENU_MODEL && ws && g_selected >= 0 && g_selected < ws->model_count)
    {
        PicoSettings_SetModel(agent, ws->models[g_selected].id);
    }
    else if (g_menu == FOOTER_MENU_EFFORT)
    {
        PicoModel *m = PicoSettings_ActiveModel(agent);
        if (m && g_selected >= 0 && g_selected < m->effort_count)
        {
            PicoSettings_SetEffort(agent, m->effort[g_selected]);
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

static void RequestFolder(PicoHost *app)
{
    if (!CloseMenu())
    {
        return;
    }
    if (PicoAgent_IsBusy(PicoHost_SelectedAgent(app)))
    {
        PicoOverlay_Notify(app, "Wait until the agent is idle before changing directory.");
        return;
    }
    if (!FolderDialogGraphic())
    {
        PicoOverlay_Notify(app, "Folder dialog unavailable. Install zenity or kdialog.");
        return;
    }
    if (!g_want_folder && (!g_app || !pico_ui_modal_push(g_app, "folder")))
    {
        return;
    }
    g_want_folder = true;
    g_folder_painted = false;
}

static void RenderFolderModal(PicoHost *app, void *state)
{
    s_active_footer_state = state ? (FooterState *)state : (FooterState *)PicoPlugins_HostState(app, "footer");
    if (!s_active_footer_state || !g_want_folder)
    {
        return;
    }

    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
    float card_w = sw < 520.0f ? sw - 48.0f : 420.0f;
    if (card_w < 260.0f)
    {
        card_w = 260.0f;
    }

    CLAY(CLAY_ID("FolderModalDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 43,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 140}})
    {
        CLAY(CLAY_ID("FolderModalCard"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {20, 20, 16, 16},
                         .childGap = 8,
                         .sizing = {.width = CLAY_SIZING_FIXED(card_w)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            CLAY_TEXT(CLAY_STRING("Select a workspace folder"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_TITLE, .textColor = COLOR_TEXT}));
            CLAY_TEXT(CLAY_STRING("To continue choose a workspace folder in the opened file dialog."),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = PICO_FONT_UI,
                                        .textColor = COLOR_TEXT,
                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
        }
    }
    g_folder_painted = true;
}

static void RenderMenu(PicoHost *app)
{
    int n = MenuCount(app);
    if (n <= 0)
    {
        return;
    }
    SelectHovered(app);
    float row_h = 30.0f;
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
          .backgroundColor = COLOR_CONTENT_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY(CLAY_ID("FooterMenuRow"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childGap = SCROLLBAR_GAP,
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}})
        {
            CLAY(CLAY_ID("FooterMenuScroll"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 2,
                             .sizing = {.width = CLAY_SIZING_GROW(0),
                                        .height = scroll ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIT(0)}},
                  .clip = {.vertical = scroll,
                           .horizontal = true,
                           .childOffset = scroll ? Clay_GetScrollOffset() : (Clay_Vector2){0}}})
            {
                for (int i = 0; i < n; i++)
                {
                    const char *label = "";
                    const char *detail = "";
                    if (g_menu == FOOTER_MENU_MODEL)
                    {
                        PicoWorkspace *ws = PicoHost_SelectedWorkspace(app);
                        PicoModel *m = ws ? &ws->models[i] : NULL;
                        label = m && m->name[0] ? m->name : (m ? m->id : "");
                        detail = m ? m->provider : "";
                    }
                    else
                    {
                        PicoModel *m = PicoSettings_ActiveModel(PicoHost_SelectedAgent(app));
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
                                                                .fontSize = PICO_FONT_UI,
                                                                .textColor = COLOR_TEXT,
                                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        if (detail[0])
                        {
                            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
                            CLAY_TEXT(CStr(detail), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                     .fontSize = PICO_FONT_CAPTION,
                                                                     .textColor = COLOR_MUTED,
                                                                     .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        }
                    }
                }
            }
            if (scroll)
            {
                PicoScrollbar_Render(CLAY_STRING("FooterMenuScroll"), CLAY_STRING("FooterMenuScrollTrack"),
                                     CLAY_STRING("FooterMenuScrollHandle"));
            }
        }
    }
}

static void Chip(Clay_ElementId id, const char *text, bool open, bool with_menu, PicoHost *app)
{
    bool hovered = Clay_PointerOver(id) || open;
    Clay_Color color = hovered ? COLOR_TEXT : COLOR_MUTED;
    CLAY(id, {.layout = {.sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0)}}})
    {
        CLAY_TEXT(CStr(text), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                               .fontSize = PICO_FONT_CAPTION,
                                               .textColor = color,
                                               .wrapMode = CLAY_TEXT_WRAP_NONE}));
        if (with_menu && open)
        {
            RenderMenu(app);
        }
    }
}

static void RenderStatus(PicoHost *app)
{
    FooterStatusKind kind = StatusKind(app);
    Clay_ElementId id = CLAY_ID("FooterStatus");
    CLAY(id, {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(16), .height = CLAY_SIZING_FIXED(16)},
                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}})
    {
        if (kind == FOOTER_STATUS_RUNNING)
        {
            const float radius = 5.0f;
            const float two_pi = 6.28318530718f;
            float theta = (float)GetTime() * two_pi * 1.75f;
            static const float alphas[3] = {1.0f, 0.5f, 0.22f};
            for (int i = 0; i < 3; i++)
            {
                float angle = theta + (float)i * (two_pi / 3.0f);
                Clay_Color color = COLOR_STATUS_ON;
                color.a *= alphas[i];
                CLAY(CLAY_IDI("FooterRunDot", i),
                     {.floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                                   .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                                   .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_CENTER,
                                                    .parent = CLAY_ATTACH_POINT_CENTER_CENTER},
                                   .offset = {.x = radius * cosf(angle), .y = radius * sinf(angle)}},
                      .layout = {.sizing = {.width = CLAY_SIZING_FIXED(4), .height = CLAY_SIZING_FIXED(4)}},
                      .backgroundColor = color,
                      .cornerRadius = CLAY_CORNER_RADIUS(2)})
                {
                }
            }
        }
        else
        {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(8), .height = CLAY_SIZING_FIXED(8)}},
                          .backgroundColor = StatusDotColor(kind),
                          .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
            }
        }

        if (Clay_PointerOver(id))
        {
            CLAY(CLAY_ID("FooterStatusTip"),
                 {.floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                               .zIndex = 26,
                               .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                               .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_BOTTOM,
                                                .parent = CLAY_ATTACH_POINT_LEFT_TOP},
                               .offset = {.y = -6}},
                  .layout = {.padding = {8, 8, 4, 4}},
                  .backgroundColor = COLOR_CONTENT_BG,
                  .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                CLAY_TEXT(CStr(g_state), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                          .fontSize = PICO_FONT_CAPTION,
                                                          .textColor = COLOR_TEXT,
                                                          .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
        }
    }
}

void PicoFooter_Render(PicoHost *app, void *state)
{
    s_active_footer_state = state ? (FooterState *)state : (FooterState *)PicoPlugins_HostState(app, "footer");
    if (!s_active_footer_state)
    {
        return;
    }
    const char *extra = "";
    if (PicoAgent_IsBusy(PicoHost_SelectedAgent(app)) && PicoAgent_CancelRequested(PicoHost_SelectedAgent(app)))
    {
        extra = "Esc again to force";
    }
    else if (PicoAgent_IsBusy(PicoHost_SelectedAgent(app)))
    {
        extra = "Esc to cancel";
    }
    else if (PicoHost_SelectedAgent(app)->state == PICO_AGENT_ERROR)
    {
        extra = "Esc to dismiss";
    }

    {
        const char *root = PicoAgent_WorkspacePath(PicoHost_SelectedAgentConst(app));
        if (!root[0])
        {
            root = PicoWorkspace_Path(PicoHost_PrimaryWorkspaceConst(app));
        }
        FormatCwd(root[0] ? root : ".", g_cwd, sizeof(g_cwd));
    }
    snprintf(g_state, sizeof(g_state), "%s", AgentStateName(app));
    snprintf(g_extra, sizeof(g_extra), "%s", extra);
    const PicoAgent *agent = PicoHost_SelectedAgentConst(app);
    char used[32];
    char limit[32];
    snprintf(used, sizeof(used), "%s", FormatTokens(agent->tokens_used));
    snprintf(limit, sizeof(limit), "%s", FormatTokens(agent->context_limit));
    int cache_percent = 0;
    if (PicoUsage_SessionPercent(PicoHost_SelectedAgent(app), &cache_percent))
    {
        snprintf(g_tokens, sizeof(g_tokens), "%s / %s tokens  ·  %d%% cache", used, limit, cache_percent);
    }
    else
    {
        snprintf(g_tokens, sizeof(g_tokens), "%s / %s tokens", used, limit);
    }

    PicoModel *active = PicoSettings_ActiveModel(PicoHost_SelectedAgent(app));
    bool show_effort = active && active->effort_count > 0;
    const char *model = PicoHost_SelectedAgent(app)->model_name ? PicoHost_SelectedAgent(app)->model_name : "?";
    snprintf(g_model, sizeof(g_model), "%s", model);
    if (show_effort)
    {
        const char *effort = PicoSettings_ActiveEffort(PicoHost_SelectedAgent(app));
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
          .backgroundColor = {0, 0, 0, 0}})
    {
        RenderStatus(app);
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(10), .height = CLAY_SIZING_FIXED(1)}}}) {}
        Chip(CLAY_ID("FooterCwd"), g_cwd, false, false, app);
        PicoDiff_RenderChip(app);
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

static void FooterAfterLayout(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)event;
    s_active_footer_state = state ? (FooterState *)state : (FooterState *)PicoPlugins_HostState(app, "footer");
    if (!s_active_footer_state)
    {
        return;
    }
    bool own_menu_top = g_menu != FOOTER_MENU_NONE &&
                        pico_ui_modal_is_top(app, "footer-menu");
    if (PicoAgent_AskUiOpen(PicoHost_SelectedAgent(app)) ||
        (pico_ui_modal_claimed(app) && !own_menu_top) || g_want_folder)
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

static void FooterOnFrame(PicoHost *app, void *state, float dt)
{
    (void)dt;
    s_active_footer_state = state ? (FooterState *)state : (FooterState *)PicoPlugins_HostState(app, "footer");
    if (!s_active_footer_state)
    {
        return;
    }
    g_esc_block = false;
    if (g_menu != FOOTER_MENU_NONE)
    {
        if (!pico_ui_modal_is_top(app, "footer-menu"))
        {
            return;
        }
        PicoScrollbar_UpdateDrag(&g_scrollbar, CLAY_STRING("FooterMenuScroll"),
                                 CLAY_STRING("FooterMenuScrollHandle"));
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

    if (!g_want_folder || !g_folder_painted || !pico_ui_modal_is_top(app, "folder"))
    {
        return;
    }
    if (!FolderDialogGraphic())
    {
        ClearFolderRequest();
        PicoOverlay_Notify(app, "Folder dialog unavailable. Install zenity or kdialog.");
        return;
    }
    const char *start = PicoAgent_WorkspacePath(PicoHost_SelectedAgentConst(app));
    if (!start[0])
    {
        start = PicoWorkspace_Path(PicoHost_PrimaryWorkspaceConst(app));
    }
    if (!start[0])
    {
        start = NULL;
    }
    char *path = tinyfd_selectFolderDialog("Workspace", start);
    ClearFolderRequest();
    if (path && path[0])
    {
        PicoHost_ChangeWorkspace(app, PicoHost_SelectedWorkspace(app), path);
    }
}

static int FooterInit(PicoHost *app, void **state_out)
{
    FooterState *s = (FooterState *)calloc(1, sizeof(FooterState));
    if (!s)
    {
        return 1;
    }
    s->app = app;
    if (state_out)
    {
        *state_out = s;
    }
    s_active_footer_state = s;
    pico_host_add_view(app, PICO_SLOT_FOOTER, 0, PicoFooter_Render);
    pico_host_add_view(app, PICO_SLOT_OVERLAY, 40, RenderFolderModal);
    pico_host_add_hook(app, PICO_HOOK_AFTER_LAYOUT, FooterAfterLayout);
    return 0;
}

static void FooterShutdown(PicoHost *app, void *state)
{
    (void)app;
    FooterState *s = (FooterState *)state;
    if (!s)
    {
        return;
    }
    s_active_footer_state = s;
    (void)CloseMenu();
    (void)ClearFolderRequest();
    free(s);
    s_active_footer_state = NULL;
}

PicoExt pico_ext_footer(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "footer",
        .description = "Status bar",
        .host_init = FooterInit,
        .host_shutdown = FooterShutdown,
        .host_on_frame = FooterOnFrame,
    };
}
