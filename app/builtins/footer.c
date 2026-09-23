#define _DEFAULT_SOURCE

#include "../agent_internal.h"
#define _POSIX_C_SOURCE 200809L
#include "host_internal.h"

#include "pico/plugin.h"
#include "agent.h"
#include "overlay.h"
#include "settings.h"
#include "docs_path.h"
#include "scrollbar.h"
#include "tinyfiledialogs.h"
#include "usage.h"
#include "worktree.h"
#include "text_range.h"
#include "text_field_ui.h"

#include "clay/clay.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum FooterMenu
{
    FOOTER_MENU_NONE = 0,
    FOOTER_MENU_MODEL,
    FOOTER_MENU_EFFORT,
} FooterMenu;

typedef enum FooterStatusKind
{
    FOOTER_STATUS_IDLE = 0,
    FOOTER_STATUS_RUNNING,
    FOOTER_STATUS_WAITING_USER,
    FOOTER_STATUS_ERROR,
} FooterStatusKind;

typedef struct FooterState
{
    char cwd[4096];
    char state_str[64];
    char tokens[128];
    char cache[64];
    char cache_input[64];
    char cache_cached[64];
    char extra[64];
    char model[128];
    char effort[PICO_EFFORT_LEN];
    PicoHost *app;
    Texture2D fast_icon;
    bool fast_icon_tried;
    FooterMenu menu;
    int selected;
    bool want_folder;
    bool folder_painted;
    bool worktree_open;
    char worktree_name[129];
    char worktree_error[256];
    PicoTextField worktree_field;
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
#define g_cache (ActiveFooterState()->cache)
#define g_cache_input (ActiveFooterState()->cache_input)
#define g_cache_cached (ActiveFooterState()->cache_cached)
#define g_extra (ActiveFooterState()->extra)
#define g_model (ActiveFooterState()->model)
#define g_effort (ActiveFooterState()->effort)
#define g_app (ActiveFooterState()->app)
#define g_menu (ActiveFooterState()->menu)
#define g_selected (ActiveFooterState()->selected)
#define g_want_folder (ActiveFooterState()->want_folder)
#define g_folder_painted (ActiveFooterState()->folder_painted)
#define g_worktree_open (ActiveFooterState()->worktree_open)
#define g_worktree_name (ActiveFooterState()->worktree_name)
#define g_worktree_error (ActiveFooterState()->worktree_error)
#define g_worktree_field (ActiveFooterState()->worktree_field)
#define g_esc_block (ActiveFooterState()->esc_block)
#define g_scrollbar (ActiveFooterState()->scrollbar)

static void StartWorktreeCreation(PicoHost *app);

bool PicoFooter_MenuOpen(void)
{
    return g_menu != FOOTER_MENU_NONE || g_esc_block || g_want_folder || g_worktree_open;
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

static const char *FormatTokens(uint64_t tokens)
{
    static char buf[32];
    if (tokens < 1000)
    {
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)tokens);
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
        snprintf(buf, sizeof(buf), "%lluk", (unsigned long long)((tokens + 500) / 1000));
    }
    else if (tokens < 1000000000)
    {
        snprintf(buf, sizeof(buf), "%.2fM", tokens / 1000000.0);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%.2fB", tokens / 1000000000.0);
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

#define WORKTREE_NAME_PAD_X 10
#define WORKTREE_NAME_PAD_Y 8

static bool WorktreeNameHovered(void)
{
    Clay_ElementData box;
    Vector2 mouse;
    Clay_BoundingBox b;
    if (Over("WorktreeName"))
        return true;
    box = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("WorktreeName")));
    if (!box.found)
        return false;
    mouse = GetMousePosition();
    b = box.boundingBox;
    return mouse.x >= b.x && mouse.x <= b.x + b.width &&
           mouse.y >= b.y && mouse.y <= b.y + b.height;
}

static void HandleWorktreeKeys(PicoHost *app)
{
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))
    {
        StartWorktreeCreation(app);
        return;
    }
    PicoTextField_HandleKeys(&g_worktree_field);
}

static void FooterDrawWorktreeOverlay(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)event;
    s_active_footer_state = state ? (FooterState *)state : (FooterState *)PicoPlugins_HostState(app, "footer");
    if (!s_active_footer_state || !g_worktree_open || !pico_ui_modal_is_top(app, "worktree-create"))
        return;
    PicoTextField_Draw(&g_worktree_field, CLAY_ID("WorktreeName"), (float)WORKTREE_NAME_PAD_X,
                       (float)WORKTREE_NAME_PAD_Y, FONT_MONO);
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
        const PicoModel *m = PicoSettings_SelectedModelConst(PicoHost_SelectedAgentConst(app));
        bool fast = PicoSettings_FastAvailable(PicoHost_SelectedAgentConst(app));
        return m ? m->effort_count + (fast ? 1 : 0) : 0;
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
        PicoModel *m = PicoSettings_SelectedModel(PicoHost_SelectedAgent(app));
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
        PicoModel *m = PicoSettings_SelectedModel(PicoHost_SelectedAgent(app));
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
        PicoModel *m = PicoSettings_SelectedModel(agent);
        if (m && g_selected >= 0 && g_selected < m->effort_count)
        {
            PicoSettings_SetEffort(agent, m->effort[g_selected]);
        }
        else if (m && g_selected == m->effort_count && PicoSettings_FastAvailable(agent))
        {
            PicoSettings_SetFast(agent, !agent->fast);
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

static void CloseWorktreeModal(PicoHost *app)
{
    if (!g_worktree_open)
        return;
    if (app && !pico_ui_modal_pop(app, "worktree-create"))
        return;
    g_worktree_open = false;
    g_worktree_error[0] = '\0';
    PicoTextField_Unbind(&g_worktree_field);
}

static void OpenWorktreeModal(PicoHost *app)
{
    PicoAgent *agent = PicoHost_SelectedAgent(app);
    PicoWorkspace *ws = agent ? agent->workspace : NULL;
    if (!agent || !ws || !ws->checkout_root)
        return;
    if (!ws->can_create_worktree)
    {
        PicoOverlay_Notify(app, "Worktree creation requires a normal Git checkout with at least one commit.");
        return;
    }
    if (agent->accepted_submit || agent->message_count > 0)
    {
        PicoOverlay_Notify(app, "A session's checkout is fixed after its first message.");
        return;
    }
    if (PicoAgent_IsBusy(agent) || PicoWorktree_Pending(app))
    {
        PicoOverlay_Notify(app, "Wait for the current operation to finish.");
        return;
    }
    if (!pico_ui_modal_push(app, "worktree-create"))
        return;
    g_worktree_open = true;
    g_worktree_error[0] = '\0';
    (void)PicoWorktree_SuggestName(ws->project_path, g_worktree_name, sizeof(g_worktree_name));
    PicoTextField_Bind(&g_worktree_field, g_worktree_name, sizeof(g_worktree_name));
    g_worktree_field.blink_at = GetTime();
}

static void StartWorktreeCreation(PicoHost *app)
{
    PicoAgent *agent = PicoHost_SelectedAgent(app);
    if (!agent)
        return;
    char error[256] = {0};
    PicoResult result = PicoWorktree_Request(app, agent->id, g_worktree_name, error, sizeof(error));
    if (result == PICO_OK)
    {
        CloseWorktreeModal(app);
        PicoOverlay_Notify(app, "Creating worktree…");
    }
    else
    {
        snprintf(g_worktree_error, sizeof(g_worktree_error), "%s",
                 error[0] ? error : (result == PICO_BUSY ? "The session is busy or already locked." : "Could not start worktree creation."));
    }
}

static void RenderWorktreeButton(Clay_ElementId id, const char *label, bool primary)
{
    bool hover = Clay_PointerOver(id);
    Clay_Color bg = primary ? (Clay_Color){74, 104, 180, 255} : COLOR_FOOTER_BG;
    if (hover)
        bg = primary ? (Clay_Color){92, 126, 210, 255} : COLOR_CODE_BG;
    CLAY(id, {.layout = {.padding = {14, 14, 8, 8}}, .backgroundColor = bg, .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                 .fontSize = PICO_FONT_UI,
                                                 .textColor = COLOR_TEXT,
                                                 .wrapMode = CLAY_TEXT_WRAP_NONE}));
    }
}

static void RenderWorktreeModal(PicoHost *app, void *state)
{
    s_active_footer_state = state ? state : PicoPlugins_HostState(app, "footer");
    if (!s_active_footer_state || !g_worktree_open)
        return;
    PicoWorkspace *ws = PicoHost_SelectedWorkspace(app);
    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
    float card_w = sw < 580.0f ? sw - 48.0f : 500.0f;
    CLAY(CLAY_ID("WorktreeModalDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 44, .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 140}})
    {
        CLAY(CLAY_ID("WorktreeModalCard"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {20, 20, 18, 18},
                         .childGap = 10,
                         .sizing = {.width = CLAY_SIZING_FIXED(card_w)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            CLAY_TEXT(CLAY_STRING("New worktree"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_TITLE, .textColor = COLOR_TEXT}));
            CLAY_TEXT(CLAY_STRING("Create a new worktree for this session."),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI, .textColor = COLOR_MUTED, .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            CLAY(CLAY_ID("WorktreeName"),
                 {.layout = {.padding = {WORKTREE_NAME_PAD_X, WORKTREE_NAME_PAD_X, WORKTREE_NAME_PAD_Y, WORKTREE_NAME_PAD_Y},
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .sizing = {.width = CLAY_SIZING_GROW(0),
                                        .height = CLAY_SIZING_FIT((float)PICO_FONT_UI_LINE)}},
                  .clip = {.horizontal = true, .vertical = true,
                           .childOffset = {.x = -g_worktree_field.scroll_x}},
                  .backgroundColor = COLOR_CODE_BG,
                  .cornerRadius = CLAY_CORNER_RADIUS(5),
                  .border = {.width = {1, 1, 1, 1}, .color = COLOR_MUTED}})
            {
                CLAY_TEXT(CStr(g_worktree_name),
                          CLAY_TEXT_CONFIG(PicoTextField_Config(FONT_MONO)));
            }
            CLAY(CLAY_ID("WorktreeUseLocal"),
                 {.layout = {.padding = {10, 10, 7, 7},
                             .sizing = {.width = CLAY_SIZING_PERCENT(1)}},
                  .backgroundColor = Clay_PointerOver(CLAY_ID("WorktreeUseLocal"))
                                         ? COLOR_CODE_BG
                                         : COLOR_CONTENT_BG,
                  .cornerRadius = CLAY_CORNER_RADIUS(5)})
            {
                CLAY_TEXT(CStr(ws && ws->worktree ? "Local" : "✓ Local"),
                          CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI, .textColor = COLOR_TEXT}));
            }
            if (g_worktree_error[0])
                CLAY_TEXT(CStr(g_worktree_error), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                    .fontSize = PICO_FONT_CAPTION,
                                                                    .textColor = COLOR_STATUS_ERR,
                                                                    .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            CLAY_AUTO_ID({.layout = {.sizing = {.height = CLAY_SIZING_FIXED(2)}}}) {}
            CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                     .childGap = 8,
                                     .sizing = {.width = CLAY_SIZING_PERCENT(1)}}})
            {
                RenderWorktreeButton(CLAY_ID("WorktreeCancel"), "Cancel", false);
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
                RenderWorktreeButton(CLAY_ID("WorktreeCreate"), "Create worktree", true);
            }
        }
    }
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
                        PicoModel *m = PicoSettings_SelectedModel(PicoHost_SelectedAgent(app));
                        bool fast_row = m && i == m->effort_count;
                        label = fast_row ? "Fast mode" : (m ? m->effort[i] : "");
                        if (fast_row)
                            detail = PicoHost_SelectedAgent(app)->fast ? "On" : "Off";
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

static void EnsureFastIcon(FooterState *s)
{
    if (!s || s->fast_icon_tried || !IsWindowReady())
        return;
    s->fast_icon_tried = true;
    char path[4096];
    if (!Pico_DataPath("resources/fast.png", path, sizeof(path)))
        snprintf(path, sizeof(path), "resources/fast.png");
    Image image = LoadImage(path);
    if (!image.data)
        return;
    ImageColorTint(&image, (Color){(unsigned char)COLOR_MUTED.r, (unsigned char)COLOR_MUTED.g,
                                   (unsigned char)COLOR_MUTED.b, (unsigned char)COLOR_MUTED.a});
    s->fast_icon = LoadTextureFromImage(image);
    UnloadImage(image);
    if (s->fast_icon.id)
        SetTextureFilter(s->fast_icon, TEXTURE_FILTER_BILINEAR);
}

static void Chip(Clay_ElementId id, const char *text, bool open, bool with_menu, PicoHost *app)
{
    bool hovered = Clay_PointerOver(id) || open;
    Clay_Color color = hovered ? COLOR_TEXT : COLOR_MUTED;
    CLAY(id, {.layout = {.childAlignment = {.y = CLAY_ALIGN_Y_CENTER}, .childGap = 4,
                         .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0)}}})
    {
        const PicoAgent *agent = PicoHost_SelectedAgentConst(app);
        if (id.id == CLAY_ID("FooterEffort").id && agent && agent->fast)
        {
            FooterState *s = ActiveFooterState();
            EnsureFastIcon(s);
            float size = Pico_FontPx(PICO_FONT_CAPTION);
            CLAY(CLAY_ID("FooterFastIcon"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(size),
                                       .height = CLAY_SIZING_FIXED(size)}},
                  .image = {.imageData = s->fast_icon.id ? &s->fast_icon : NULL}}) {}
        }
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

static void RenderCacheChip(void)
{
    Clay_ElementId id = CLAY_ID("FooterCache");
    CLAY(id, {.layout = {.sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0)}}})
    {
        CLAY_TEXT(CStr(g_cache), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                   .fontSize = PICO_FONT_CAPTION,
                                                   .textColor = COLOR_MUTED,
                                                   .wrapMode = CLAY_TEXT_WRAP_NONE}));
        if (Clay_PointerOver(id))
        {
            CLAY(CLAY_ID("FooterCacheTip"),
                 {.floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                               .zIndex = 26,
                               .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                               .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_BOTTOM,
                                                .parent = CLAY_ATTACH_POINT_LEFT_TOP},
                               .offset = {.y = -6}},
                  .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 2,
                             .padding = {8, 8, 4, 4},
                             .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0)}},
                  .backgroundColor = COLOR_CONTENT_BG,
                  .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                CLAY_TEXT(CStr(g_cache_input), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                 .fontSize = PICO_FONT_CAPTION,
                                                                 .textColor = COLOR_TEXT,
                                                                 .wrapMode = CLAY_TEXT_WRAP_NONE}));
                CLAY_TEXT(CStr(g_cache_cached), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                  .fontSize = PICO_FONT_CAPTION,
                                                                  .textColor = COLOR_TEXT,
                                                                  .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
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
    if (!s_active_footer_state || !PicoHost_SelectedAgent(app))
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
        const PicoWorkspace *selected_ws = PicoHost_SelectedWorkspaceConst(app);
        const char *root = selected_ws && selected_ws->project_path[0]
                               ? selected_ws->project_path
                               : PicoAgent_WorkspacePath(PicoHost_SelectedAgentConst(app));
        if (!root[0])
            root = PicoWorkspace_Path(PicoHost_PrimaryWorkspaceConst(app));
        FormatCwd(root[0] ? root : ".", g_cwd, sizeof(g_cwd));
    }
    snprintf(g_state, sizeof(g_state), "%s", AgentStateName(app));
    snprintf(g_extra, sizeof(g_extra), "%s", extra);
    const PicoAgent *agent = PicoHost_SelectedAgentConst(app);
    char used[32];
    char limit[32];
    snprintf(used, sizeof(used), "%s", FormatTokens((uint64_t)agent->tokens_used));
    snprintf(limit, sizeof(limit), "%s", FormatTokens((uint64_t)agent->context_limit));
    int cache_percent = 0;
    bool show_cache = PicoUsage_SessionPercent(agent, &cache_percent);
    snprintf(g_tokens, sizeof(g_tokens), "%s / %s tokens", used, limit);
    if (show_cache)
    {
        char total_input[32];
        char total_cached[32];
        snprintf(total_input, sizeof(total_input), "%s", FormatTokens(agent->session_input_tokens));
        snprintf(total_cached, sizeof(total_cached), "%s", FormatTokens(agent->session_cached_tokens));
        snprintf(g_cache, sizeof(g_cache), "%d%% cache", cache_percent);
        snprintf(g_cache_input, sizeof(g_cache_input), "Total input: %s", total_input);
        snprintf(g_cache_cached, sizeof(g_cache_cached), "Cached input: %s", total_cached);
    }

    PicoModel *active = PicoSettings_SelectedModel(PicoHost_SelectedAgent(app));
    bool show_effort = active && active->effort_count > 0;
    const char *model = PicoHost_SelectedAgent(app)->model_name;
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
        PicoWorkspace *footer_ws = PicoHost_SelectedWorkspace(app);
        if (footer_ws && footer_ws->checkout_root)
        {
            Sep();
            const PicoAgent *footer_agent = PicoHost_SelectedAgentConst(app);
            const char *checkout = footer_agent && PicoWorktree_PendingFor(app, footer_agent->id)
                                       ? "creating…"
                                       : (footer_ws->worktree && footer_ws->checkout_name[0]
                                              ? footer_ws->checkout_name
                                              : "local");
            Chip(CLAY_ID("FooterWorktree"), checkout, false, false, app);
            if (Over("FooterWorktree"))
            {
                CLAY(CLAY_ID("FooterWorktreeTip"),
                     {.floating = {.attachTo = CLAY_ATTACH_TO_PARENT, .zIndex = 26, .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH, .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_BOTTOM, .parent = CLAY_ATTACH_POINT_LEFT_TOP}, .offset = {.y = -6}},
                      .layout = {.padding = {8, 8, 4, 4}},
                      .backgroundColor = COLOR_CONTENT_BG,
                      .cornerRadius = CLAY_CORNER_RADIUS(4)})
                {
                    CLAY_TEXT(CStr(footer_ws->path), CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                                                       .fontSize = PICO_FONT_CAPTION,
                                                                       .textColor = COLOR_TEXT,
                                                                       .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
            }
        }
        PicoDiff_RenderChip(app);
        Sep();
        MutedText(g_tokens);
        if (show_cache)
        {
            Sep();
            RenderCacheChip();
        }
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
    if (!s_active_footer_state || !PicoHost_SelectedAgent(app))
    {
        app->hovered_clickable = false;
        return;
    }
    bool own_menu_top = g_menu != FOOTER_MENU_NONE &&
                        pico_ui_modal_is_top(app, "footer-menu");
    bool own_worktree_top = g_worktree_open && pico_ui_modal_is_top(app, "worktree-create");
    if (own_worktree_top)
    {
        bool over_name = WorktreeNameHovered();
        app->hovered_text = over_name;
        app->hovered_clickable = Over("WorktreeUseLocal") || Over("WorktreeCancel") ||
                                 Over("WorktreeCreate");
        PicoTextField_HandlePointer(&g_worktree_field, CLAY_ID("WorktreeName"),
                                    (float)WORKTREE_NAME_PAD_X, FONT_MONO);
        {
            Clay_ElementData name_box = Clay_GetElementData(CLAY_ID("WorktreeName"));
            if (name_box.found)
                PicoTextField_KeepCaretVisible(&g_worktree_field,
                                               name_box.boundingBox.width - 2.0f * (float)WORKTREE_NAME_PAD_X,
                                               FONT_MONO);
        }
        if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            return;
        if (over_name)
            return;
        if (Over("WorktreeCancel") || (Over("WorktreeModalDim") && !Over("WorktreeModalCard")))
        {
            CloseWorktreeModal(app);
            return;
        }
        if (Over("WorktreeUseLocal"))
        {
            PicoAgent *agent = PicoHost_SelectedAgent(app);
            PicoWorkspace *ws = agent ? agent->workspace : NULL;
            if (ws && !ws->worktree)
                CloseWorktreeModal(app);
            else if (agent && PicoHost_StartLocalSession(app, agent->id))
                CloseWorktreeModal(app);
            else
                snprintf(g_worktree_error, sizeof(g_worktree_error), "Could not start a local session.");
            return;
        }
        if (Over("WorktreeCreate"))
        {
            StartWorktreeCreation(app);
            return;
        }
        return;
    }
    if ((PicoUi_ModalOpen(app) && !own_menu_top && !own_worktree_top) || g_want_folder)
    {
        app->hovered_clickable = false;
        return;
    }

    int hovered = HoveredItem(app);
    app->hovered_clickable = Over("FooterCwd") || Over("FooterWorktree") || Over("FooterModel") || Over("FooterEffort") || hovered >= 0 ||
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
    else if (Over("FooterWorktree"))
    {
        OpenWorktreeModal(app);
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
    if (!PicoHost_SelectedAgent(app))
    {
        CloseMenu();
        if (g_want_folder)
            ClearFolderRequest();
        if (g_worktree_open)
            CloseWorktreeModal(app);
        return;
    }
    g_esc_block = false;
    if (g_worktree_open && pico_ui_modal_is_top(app, "worktree-create"))
    {
        if (IsKeyPressed(KEY_ESCAPE))
        {
            CloseWorktreeModal(app);
            g_esc_block = true;
            return;
        }
        HandleWorktreeKeys(app);
        return;
    }
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
    pico_host_add_view(app, PICO_SLOT_OVERLAY, 41, RenderWorktreeModal);
    pico_host_add_hook(app, PICO_HOOK_AFTER_LAYOUT, FooterAfterLayout);
    pico_host_add_hook(app, PICO_HOOK_AFTER_RENDER, FooterDrawWorktreeOverlay);
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
    CloseWorktreeModal(app);
    if (s->fast_icon.id)
        UnloadTexture(s->fast_icon);
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
