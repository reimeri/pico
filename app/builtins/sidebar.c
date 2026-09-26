#define _DEFAULT_SOURCE

#include "../agent_internal.h"
#define _POSIX_C_SOURCE 200809L
#include "host_internal.h"

#include "pico/plugin.h"
#include "agent.h"
#include "builtins/chat.h"
#include "docs_path.h"
#include "overlay.h"
#include "session.h"
#include "builtins/sidebar.h"
#include "tinyfiledialogs.h"
#include "text_range.h"
#include "text_field_ui.h"

#include "clay/clay.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PICO_SESSION_TEST_HOOKS
extern bool PicoSession_TestHook(const char *stage);
#endif

#define SIDEBAR_SCAN_SEC 0.5
#define SIDEBAR_RECONCILE_SEC 60.0
#define SIDEBAR_SESSION_PAGE 10
#define SIDEBAR_ROW_PAD_X 6
#define SIDEBAR_ROW_GAP 6
#define SIDEBAR_FOLDER_ICON 17
#define SIDEBAR_SESSION_DOT 8
#define SIDEBAR_SESSION_IDLE_DOT 4
#define SIDEBAR_DRAG_THRESHOLD 4.0f

typedef struct SidebarWsUi
{
    char path[4096];
    int shown;
} SidebarWsUi;

typedef struct SidebarState
{
    PicoHost *host;
    PicoCatalogWorkspace *workspaces;
    int workspace_count;
    SidebarWsUi *ui;
    int ui_count;
    bool want_folder;
    bool folder_painted;
    bool dirty;
    bool catalog_scanned;
    double last_scan;
    double last_reconcile;
    char catalog_change_token[PICO_CATALOG_CHANGE_TOKEN_MAX];
    bool catalog_change_token_valid;
    Texture2D folder_collapsed;
    Texture2D folder_expanded;
    Texture2D archive_open;
    Texture2D archive_closed;
    Texture2D pen_icon;
    bool stash_expanded;
    bool edit_open;
    bool delete_confirm;
    bool edit_pointer_latched;
    char edit_path[4096];
    char edit_name[PICO_CATALOG_NAME_MAX];
    PicoTextField edit_field;
    char delete_label[384];
    Texture2D settings_icon;
    Texture2D settings_icon_hover;
    Texture2D worktree_icon;
    bool icons_tried;
    bool drag_press_pending;
    int drag_source_index;
    Vector2 drag_press_pos;
    bool is_dragging;
    int drag_target_index;
    uint64_t order_persist_generation;
    bool order_unsaved;
} SidebarState;

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static int ClampShown(int shown, int total)
{
    if (total <= SIDEBAR_SESSION_PAGE)
    {
        return total;
    }
    if (shown < SIDEBAR_SESSION_PAGE)
    {
        shown = SIDEBAR_SESSION_PAGE;
    }
    if (shown > total)
    {
        shown = total;
    }
    return shown;
}

static int ShownForIndex(SidebarState *s, int index, int total)
{
    int shown;
    if (!s || index < 0 || index >= s->ui_count)
    {
        return ClampShown(SIDEBAR_SESSION_PAGE, total);
    }
    shown = ClampShown(s->ui[index].shown, total);
    s->ui[index].shown = shown;
    return shown;
}

static void AdjustShown(SidebarState *s, int index, int delta, int total)
{
    if (!s || index < 0 || index >= s->ui_count)
    {
        return;
    }
    s->ui[index].shown = ClampShown(s->ui[index].shown + delta, total);
}

static int PrevShownForPath(const SidebarWsUi *ui, int ui_count, const char *path)
{
    int i;
    if (!ui || !path || !path[0])
    {
        return SIDEBAR_SESSION_PAGE;
    }
    for (i = 0; i < ui_count; i++)
    {
        if (strcmp(ui[i].path, path) == 0)
        {
            return ui[i].shown;
        }
    }
    return SIDEBAR_SESSION_PAGE;
}

static int SidebarWorkspaceOrderCmp(const void *a, const void *b)
{
    const PicoCatalogWorkspace *x = (const PicoCatalogWorkspace *)a;
    const PicoCatalogWorkspace *y = (const PicoCatalogWorkspace *)b;
    if (x->order != y->order)
    {
        return x->order < y->order ? -1 : 1;
    }
    return strcmp(x->name, y->name);
}

static void SidebarPreserveWorkspaceOrder(PicoCatalogWorkspace *next, int next_count,
                                          const PicoCatalogWorkspace *previous,
                                          int previous_count)
{
    int i;
    int j;
    if (!next || next_count <= 0 || !previous || previous_count <= 0)
    {
        return;
    }
    for (i = 0; i < next_count; i++)
    {
        next[i].order = previous_count;
        for (j = 0; j < previous_count; j++)
        {
            if (strcmp(next[i].path, previous[j].path) == 0)
            {
                next[i].order = j;
                break;
            }
        }
    }
    qsort(next, (size_t)next_count, sizeof(*next), SidebarWorkspaceOrderCmp);
}

static void SidebarRefresh(SidebarState *s)
{
    char token_before[PICO_CATALOG_CHANGE_TOKEN_MAX];
    char token_after[PICO_CATALOG_CHANGE_TOKEN_MAX];
    PicoCatalogWorkspace *next = NULL;
    SidebarWsUi *next_ui = NULL;
    SidebarWsUi *prev_ui;
    int prev_ui_count;
    int n;
    int i;
    if (!s)
    {
        return;
    }
    bool token_before_valid = PicoCatalog_ReadChangeToken(token_before);
    n = PicoCatalog_ScanGrouped(&next);
    bool token_after_valid = PicoCatalog_ReadChangeToken(token_after);
    if (s->order_unsaved)
    {
        SidebarPreserveWorkspaceOrder(next, n, s->workspaces, s->workspace_count);
    }
    prev_ui = s->ui;
    prev_ui_count = s->ui_count;
    if (n > 0)
    {
        next_ui = (SidebarWsUi *)calloc((size_t)n, sizeof(SidebarWsUi));
        if (next_ui)
        {
            for (i = 0; i < n; i++)
            {
                snprintf(next_ui[i].path, sizeof(next_ui[i].path), "%s", next[i].path);
                next_ui[i].shown = PrevShownForPath(prev_ui, prev_ui_count, next[i].path);
            }
        }
    }
    PicoCatalog_Free(s->workspaces, s->workspace_count);
    free(prev_ui);
    s->workspaces = next;
    s->workspace_count = n;
    s->ui = next_ui;
    s->ui_count = next_ui ? n : 0;
    s->catalog_change_token_valid = token_after_valid;
    if (token_after_valid)
    {
        snprintf(s->catalog_change_token, sizeof(s->catalog_change_token), "%s", token_after);
    }
    s->dirty = token_after_valid &&
               (!token_before_valid || strcmp(token_before, token_after) != 0);
    s->catalog_scanned = true;
    s->last_scan = GetTime();
    s->last_reconcile = s->last_scan;
}

static bool SidebarCatalogChanged(SidebarState *s)
{
    char token[PICO_CATALOG_CHANGE_TOKEN_MAX];
    if (!s || !PicoCatalog_ReadChangeToken(token))
    {
        return true;
    }
    return !s->catalog_change_token_valid ||
           strcmp(s->catalog_change_token, token) != 0;
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

static bool ClearFolderRequest(SidebarState *s)
{
    if (!s)
    {
        return false;
    }
    if (s->want_folder && s->host && !pico_ui_modal_pop(s->host, "sidebar-folder"))
    {
        return false;
    }
    s->want_folder = false;
    s->folder_painted = false;
    return true;
}

static void RequestAddWorkspace(PicoHost *host, SidebarState *s)
{
    if (!host || !s)
    {
        return;
    }
    if (!FolderDialogGraphic())
    {
        PicoOverlay_Notify(host, "Folder dialog unavailable. Install zenity or kdialog.");
        return;
    }
    if (!s->want_folder && !pico_ui_modal_push(host, "sidebar-folder"))
    {
        return;
    }
    s->want_folder = true;
    s->folder_painted = false;
}

static void NotifyOpenResult(PicoHost *host, PicoResult result)
{
    if (result == PICO_OK || result == PICO_ALREADY_OPEN)
    {
        return;
    }
    if (result == PICO_LIMIT)
    {
        PicoOverlay_Notify(host, "Too many workspaces are open.");
        return;
    }
    if (result == PICO_BUSY)
    {
        PicoOverlay_Notify(host, "That workspace is closing.");
        return;
    }
    PicoOverlay_Notify(host, "Could not open that workspace.");
}

static PicoWorkspaceId OpenLiveWorkspace(PicoHost *host, const char *path, PicoResult *out_opened)
{
    PicoWorkspaceId id = 0;
    PicoResult opened;
    if (out_opened)
    {
        *out_opened = PICO_INVALID;
    }
    if (!host || !path || !path[0])
    {
        return 0;
    }
    opened = pico_workspace_open(host, path, &id);
    if (out_opened)
    {
        *out_opened = opened;
    }
    if (opened != PICO_OK && opened != PICO_ALREADY_OPEN)
    {
        NotifyOpenResult(host, opened);
        return 0;
    }
    PicoCatalog_Ensure(path);
    return id;
}

static PicoAgentId LiveMainAgent(PicoHost *host, const char *ws_path, const char *session_id)
{
    int n;
    int i;
    if (!host || !ws_path || !ws_path[0])
    {
        return 0;
    }
    n = pico_agent_count(host);
    for (i = 0; i < n; i++)
    {
        PicoAgentInfo info;
        PicoAgent *agent;
        const char *agent_ws;
        if (!pico_agent_info(host, i, &info) || info.kind != PICO_AGENT_MAIN)
        {
            continue;
        }
        agent = PicoHost_FindAgent(host, info.id);
        agent_ws = PicoAgent_WorkspacePath(agent);
        if (!agent_ws || strcmp(agent_ws, ws_path) != 0)
        {
            continue;
        }
        if (session_id && session_id[0])
        {
            if (strcmp(info.session_id, session_id) == 0)
            {
                return info.id;
            }
        }
        else if (!info.session_id[0])
        {
            return info.id;
        }
    }
    return 0;
}

static bool CatalogHasSession(const PicoCatalogWorkspace *ws, const char *checkout_path,
                              const char *session_id)
{
    int i;
    if (!ws || !session_id || !session_id[0])
    {
        return false;
    }
    for (i = 0; i < ws->session_count; i++)
    {
        if (strcmp(ws->sessions[i].id, session_id) == 0 && checkout_path &&
            strcmp(ws->sessions[i].checkout_path, checkout_path) == 0)
        {
            return true;
        }
    }
    return false;
}

static void SelectAgent(PicoHost *host, PicoAgentId id)
{
    PicoChat_InspectClose();
    (void)pico_agent_select(host, id);
}

static void ExpandWorkspace(SidebarState *s, int index)
{
    PicoCatalogWorkspace *ws;
    if (!s || index < 0 || index >= s->workspace_count)
    {
        return;
    }
    ws = &s->workspaces[index];
    if (!ws->collapsed)
    {
        return;
    }
    ws->collapsed = false;
    PicoCatalog_SetCollapsed(ws->path, false);
}

static void NewSessionInWorkspace(PicoHost *host, SidebarState *s, int index)
{
    PicoCatalogWorkspace *ws;
    PicoWorkspaceId id;
    PicoAgentCreateOptions options;
    PicoAgentId agent_id = 0;
    PicoAgentId live;
    PicoResult created;
    if (!s || index < 0 || index >= s->workspace_count)
    {
        return;
    }
    ws = &s->workspaces[index];
    ExpandWorkspace(s, index);
    live = LiveMainAgent(host, ws->path, NULL);
    if (live)
    {
        SelectAgent(host, live);
        return;
    }
    id = OpenLiveWorkspace(host, ws->path, NULL);
    if (!id)
    {
        return;
    }
    memset(&options, 0, sizeof(options));
    options.kind = PICO_AGENT_MAIN;
    options.session_start = PICO_SESSION_NEW;
    options.select = true;
    PicoChat_InspectClose();
    created = pico_main_agent_create(host, id, &options, &agent_id);
    if (created != PICO_OK)
    {
        if (created == PICO_LIMIT)
        {
            PicoOverlay_Notify(host, "Too many agents are open.");
        }
        else
        {
            PicoOverlay_Notify(host, "Could not create a session.");
        }
        return;
    }
    s->dirty = true;
}

static void OpenCatalogSession(PicoHost *host, SidebarState *s, const char *path, const char *session_id)
{
    PicoAgentId live;
    PicoWorkspaceId id;
    PicoResult created;
    live = LiveMainAgent(host, path, session_id);
    if (live)
    {
        PicoSession_LoadCancel(host);
        SelectAgent(host, live);
        return;
    }
    id = OpenLiveWorkspace(host, path, NULL);
    if (!id)
    {
        return;
    }
    PicoChat_InspectClose();
    created = PicoSession_LoadAsync(host, id, 0, session_id, false, false, false, false);
    if (created != PICO_OK)
    {
        if (created == PICO_LIMIT)
        {
            PicoOverlay_Notify(host, "Too many agents are open.");
        }
        else if (created == PICO_SESSION_INVALID)
        {
            PicoOverlay_Notify(host, "Could not open that session.");
        }
        else
        {
            PicoOverlay_Notify(host, "Could not open that session.");
        }
        return;
    }
    if (s)
    {
        s->dirty = true;
    }
}

static void ToggleCollapsed(SidebarState *s, int index)
{
    PicoCatalogWorkspace *ws;
    if (!s || index < 0 || index >= s->workspace_count)
    {
        return;
    }
    ws = &s->workspaces[index];
    ws->collapsed = !ws->collapsed;
    PicoCatalog_SetCollapsed(ws->path, ws->collapsed);
}

static Clay_Color RowFill(bool selected, bool hovered)
{
    if (selected)
    {
        return (Clay_Color){52, 52, 62, 255};
    }
    if (hovered)
    {
        return (Clay_Color){42, 42, 50, 255};
    }
    return (Clay_Color){0, 0, 0, 0};
}

static void ResourcePath(const char *relative, char *out, size_t cap)
{
    if (!Pico_DataPath(relative, out, cap))
    {
        snprintf(out, cap, "%s", relative);
    }
}

static Texture2D LoadFolderIcon(const char *relative, Clay_Color tint)
{
    char path[4096];
    Image img;
    Texture2D tex = {0};
    ResourcePath(relative, path, sizeof(path));
    img = LoadImage(path);
    if (!img.data)
    {
        return tex;
    }
    /* Clay draws a filled rect for backgroundColor even on image elements. */
    ImageColorTint(&img, (Color){(unsigned char)tint.r, (unsigned char)tint.g,
                                 (unsigned char)tint.b, (unsigned char)tint.a});
    tex = LoadTextureFromImage(img);
    UnloadImage(img);
    if (tex.id != 0)
    {
        SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    }
    return tex;
}

static void EnsureFolderIcons(SidebarState *s)
{
    if (!s || s->icons_tried || !IsWindowReady())
    {
        return;
    }
    s->icons_tried = true;
    s->folder_collapsed = LoadFolderIcon("resources/folder-collapsed.png", COLOR_MUTED);
    s->folder_expanded = LoadFolderIcon("resources/folder-expanded.png", COLOR_MUTED);
    s->pen_icon = LoadFolderIcon("resources/pen.png", COLOR_TEXT);
    s->settings_icon = LoadFolderIcon("resources/settings.png", COLOR_MUTED);
    s->settings_icon_hover = LoadFolderIcon("resources/settings.png", COLOR_TEXT);
    s->worktree_icon = LoadFolderIcon("resources/worktree.png", COLOR_MUTED);
    s->archive_open = LoadFolderIcon("resources/archive_open.png", COLOR_MUTED);
    s->archive_closed = LoadFolderIcon("resources/archive_closed.png", COLOR_MUTED);
}

static void UnloadFolderIcons(SidebarState *s)
{
    if (!s)
    {
        return;
    }
    if (s->folder_collapsed.id != 0)
    {
        UnloadTexture(s->folder_collapsed);
        memset(&s->folder_collapsed, 0, sizeof(s->folder_collapsed));
    }
    if (s->folder_expanded.id != 0)
    {
        UnloadTexture(s->folder_expanded);
        memset(&s->folder_expanded, 0, sizeof(s->folder_expanded));
    }
    if (s->pen_icon.id != 0)
        UnloadTexture(s->pen_icon);
    if (s->settings_icon.id != 0)
    {
        UnloadTexture(s->settings_icon);
        memset(&s->settings_icon, 0, sizeof(s->settings_icon));
    }
    if (s->settings_icon_hover.id != 0)
    {
        UnloadTexture(s->settings_icon_hover);
        memset(&s->settings_icon_hover, 0, sizeof(s->settings_icon_hover));
    }
    if (s->worktree_icon.id != 0)
    {
        UnloadTexture(s->worktree_icon);
        memset(&s->worktree_icon, 0, sizeof(s->worktree_icon));
    }
    if (s->archive_open.id != 0)
    {
        UnloadTexture(s->archive_open);
        memset(&s->archive_open, 0, sizeof(s->archive_open));
    }
    if (s->archive_closed.id != 0)
    {
        UnloadTexture(s->archive_closed);
        memset(&s->archive_closed, 0, sizeof(s->archive_closed));
    }
}

static void RenderGlyph(const char *glyph, Clay_Color color)
{
    CLAY_TEXT(CStr(glyph), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                             .fontSize = PICO_FONT_UI,
                                             .textColor = color,
                                             .wrapMode = CLAY_TEXT_WRAP_NONE}));
}

static void RenderFolderIcon(Texture2D *tex, const char *fallback)
{
    float size = Pico_FontPx(SIDEBAR_FOLDER_ICON);
    if (tex && tex->id != 0)
    {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(size),
                                            .height = CLAY_SIZING_FIXED(size)}},
                      .image = {.imageData = tex}})
        {
        }
        return;
    }
    RenderGlyph(fallback, COLOR_MUTED);
}

static void RenderSettingsIcon(SidebarState *s, bool hovered)
{
    Texture2D *tex = hovered && s->settings_icon_hover.id != 0 ? &s->settings_icon_hover
                                                               : &s->settings_icon;
    float size = Pico_FontPx(SIDEBAR_FOLDER_ICON);
    if (tex->id != 0)
    {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(size),
                                            .height = CLAY_SIZING_FIXED(size)}},
                      .image = {.imageData = tex}})
        {
        }
        return;
    }
    RenderGlyph("*", hovered ? COLOR_TEXT : COLOR_MUTED);
}

typedef enum SidebarDotKind
{
    SIDEBAR_DOT_IDLE = 0,
    SIDEBAR_DOT_RUNNING,
    SIDEBAR_DOT_WAITING_USER,
    SIDEBAR_DOT_DONE,
    SIDEBAR_DOT_ERROR,
} SidebarDotKind;

static SidebarDotKind SessionDotKind(PicoHost *host, const char *ws_path, const char *session_id,
                                     PicoAgentId live_id, bool catalog_unseen)
{
    PicoAgent *agent;
    if (!live_id)
    {
        live_id = LiveMainAgent(host, ws_path, session_id);
    }
    if (!live_id)
    {
        return catalog_unseen ? SIDEBAR_DOT_DONE : SIDEBAR_DOT_IDLE;
    }
    agent = PicoHost_FindAgent(host, live_id);
    if (!agent)
    {
        return catalog_unseen ? SIDEBAR_DOT_DONE : SIDEBAR_DOT_IDLE;
    }
    switch (agent->state)
    {
    case PICO_AGENT_ERROR:
        return SIDEBAR_DOT_ERROR;
    case PICO_AGENT_TOOL_WAIT:
        if (PicoAgent_AskUiOpen(agent))
        {
            return SIDEBAR_DOT_WAITING_USER;
        }
        return SIDEBAR_DOT_RUNNING;
    case PICO_AGENT_LLM_WAIT:
    case PICO_AGENT_COMPACT_WAIT:
        return SIDEBAR_DOT_RUNNING;
    case PICO_AGENT_IDLE:
    default:
        return agent->unseen_complete ? SIDEBAR_DOT_DONE : SIDEBAR_DOT_IDLE;
    }
}

static void RenderSessionSpinner(int row_id, float slot)
{
    const int segments = 8;
    const float two_pi = 6.28318530718f;
    float theta = (float)GetTime() * two_pi * 1.5f;
    float size = slot * 0.23f;
    /* Floating segments may extend beyond the slot without changing row layout. */
    float diameter = Pico_FontPx(11);
    float radius = (diameter - size) * 0.5f;
    CLAY(CLAY_IDI("SidebarSessSpinner", row_id),
         {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(slot),
                                .height = CLAY_SIZING_FIXED(slot)}}})
    {
        for (int i = 0; i < segments; i++)
        {
            float angle = theta - (float)i * two_pi / (float)segments;
            Clay_Color color = COLOR_STATUS_OFF;
            color.a *= 1.0f - 0.85f * (float)i / (float)(segments - 1);
            CLAY(CLAY_IDI("SidebarSessSpinSegment", row_id * segments + i),
                 {.floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                               .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                               .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_CENTER,
                                                .parent = CLAY_ATTACH_POINT_CENTER_CENTER},
                               .offset = {.x = radius * cosf(angle), .y = radius * sinf(angle)}},
                  .layout = {.sizing = {.width = CLAY_SIZING_FIXED(size),
                                         .height = CLAY_SIZING_FIXED(size)}},
                  .backgroundColor = color,
                  .cornerRadius = CLAY_CORNER_RADIUS(size * 0.5f)}) {}
        }
    }
}

static void RenderSessionDot(SidebarState *s, SidebarDotKind kind, int row_id, bool worktree,
                             bool loading, bool unloaded)
{
    float gutter = Pico_FontPx(SIDEBAR_FOLDER_ICON);
    float slot = Pico_FontPx(SIDEBAR_SESSION_DOT);
    float size = Pico_FontPx(kind == SIDEBAR_DOT_IDLE ? SIDEBAR_SESSION_IDLE_DOT
                                                      : SIDEBAR_SESSION_DOT);
    Clay_Color color = COLOR_STATUS_OFF;
    switch (kind)
    {
    case SIDEBAR_DOT_ERROR:
        color = COLOR_STATUS_ERR;
        break;
    case SIDEBAR_DOT_WAITING_USER:
        color = COLOR_STATUS_RUN;
        break;
    case SIDEBAR_DOT_RUNNING:
    {
        float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 6.28318530718f * 1.25f);
        color = COLOR_STATUS_ON;
        color.a = 90.0f + 165.0f * pulse;
        break;
    }
    case SIDEBAR_DOT_DONE:
        color = COLOR_STATUS_DONE;
        break;
    case SIDEBAR_DOT_IDLE:
    default:
        break;
    }
    if (unloaded) color.a *= 0.65f;
    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(gutter),
                                        .height = CLAY_SIZING_FIXED(slot)},
                             .childAlignment = {.x = CLAY_ALIGN_X_CENTER,
                                                .y = CLAY_ALIGN_Y_CENTER}}})
    {
        if (loading)
        {
            RenderSessionSpinner(row_id, slot);
        }
        else if (worktree && kind == SIDEBAR_DOT_IDLE && s && s->worktree_icon.id != 0)
        {
            CLAY(CLAY_IDI("SidebarSessDot", row_id),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(gutter),
                                        .height = CLAY_SIZING_FIXED(gutter)}},
                  .image = {.imageData = &s->worktree_icon}}) {}
        }
        else
        {
            CLAY(CLAY_IDI("SidebarSessDot", row_id),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(size),
                                        .height = CLAY_SIZING_FIXED(size)}},
                  .backgroundColor = color,
                  .cornerRadius = CLAY_CORNER_RADIUS(size * 0.5f)}) {}
        }
    }
}

static bool SessionIsSelected(PicoHost *host, const char *ws_path, const char *session_id,
                              PicoAgentId live_id)
{
    PicoAgentId selected = pico_agent_active(host);
    PicoAgentInfo info;
    const char *agent_ws;
    if (!pico_agent_find(host, selected, &info) || info.kind != PICO_AGENT_MAIN)
    {
        return false;
    }
    agent_ws = PicoAgent_WorkspacePath(PicoHost_FindAgentConst(host, selected));
    if (!agent_ws || strcmp(agent_ws, ws_path) != 0)
    {
        return false;
    }
    if (live_id)
    {
        return selected == live_id;
    }
    return session_id && session_id[0] && strcmp(info.session_id, session_id) == 0;
}

static int SessionRowId(int ws_index, int session_index)
{
    return ws_index * 20000 + session_index;
}

bool PicoSidebar_DragMoved(float press_x, float press_y, float mouse_x, float mouse_y)
{
    float dx = mouse_x - press_x;
    float dy = mouse_y - press_y;
    return dx * dx + dy * dy >= SIDEBAR_DRAG_THRESHOLD * SIDEBAR_DRAG_THRESHOLD;
}

int PicoSidebar_DragTarget(const float *midpoints, int count, int source, float mouse_y)
{
    int target = source;
    int k;
    if (!midpoints || count <= 0 || source < 0 || source >= count)
    {
        return -1;
    }
    if (mouse_y < midpoints[source])
    {
        for (k = source - 1; k >= 0 && mouse_y < midpoints[k]; k--)
        {
            target = k;
        }
    }
    else
    {
        for (k = source + 1; k < count && mouse_y > midpoints[k]; k++)
        {
            target = k;
        }
    }
    return target;
}

int PicoSidebar_DropTarget(const float *midpoints, int count, int source, float mouse_y,
                           bool pointer_over_list)
{
    return pointer_over_list
               ? PicoSidebar_DragTarget(midpoints, count, source, mouse_y)
               : -1;
}

/* Reorderable (non-stashed) workspace indices in display order. */
static int SidebarDraggableSlots(const SidebarState *s, int *slots)
{
    int n = 0;
    for (int i = 0; i < s->workspace_count && n < PICO_MAX_CATALOG_WORKSPACES; i++)
        if (!s->workspaces[i].stashed)
            slots[n++] = i;
    return n;
}

static int SidebarComputeDragTarget(SidebarState *s, float mouse_y, bool pointer_over_list)
{
    float mids[PICO_MAX_CATALOG_WORKSPACES];
    int slots[PICO_MAX_CATALOG_WORKSPACES];
    int n, source = -1;
    if (!s || s->drag_source_index < 0 || s->drag_source_index >= s->workspace_count ||
        s->workspaces[s->drag_source_index].stashed)
        return -1;
    n = SidebarDraggableSlots(s, slots);
    for (int k = 0; k < n; k++)
    {
        Clay_ElementData el = Clay_GetElementData(CLAY_IDI("SidebarWs", slots[k]));
        if (!el.found)
            return s->drag_source_index;
        mids[k] = el.boundingBox.y + el.boundingBox.height * 0.5f;
        if (slots[k] == s->drag_source_index)
            source = k;
    }
    int target = PicoSidebar_DropTarget(mids, n, source, mouse_y, pointer_over_list);
    return target < 0 ? -1 : slots[target];
}

static void SidebarReorderWorkspaces(SidebarState *s, int from, int to)
{
    PicoCatalogWorkspace moved_ws;
    SidebarWsUi moved_ui;
    bool has_ui;
    int k;
    if (!s || from < 0 || to < 0 || from >= s->workspace_count || to >= s->workspace_count || from == to)
    {
        return;
    }
    if (s->workspaces[from].stashed || s->workspaces[to].stashed)
        return;
    int slots[PICO_MAX_CATALOG_WORKSPACES];
    int n = SidebarDraggableSlots(s, slots);
    int from_slot = -1, to_slot = -1;
    for (int i = 0; i < n; i++)
    {
        if (slots[i] == from)
            from_slot = i;
        if (slots[i] == to)
            to_slot = i;
    }
    if (from_slot < 0 || to_slot < 0)
        return;
    moved_ws = s->workspaces[from];
    memset(&moved_ui, 0, sizeof(moved_ui));
    has_ui = s->ui && from < s->ui_count && to < s->ui_count;
    if (has_ui)
        moved_ui = s->ui[from];
    if (from_slot < to_slot)
    {
        for (int i = from_slot; i < to_slot; i++)
        {
            s->workspaces[slots[i]] = s->workspaces[slots[i + 1]];
            if (has_ui)
                s->ui[slots[i]] = s->ui[slots[i + 1]];
        }
    }
    else
    {
        for (int i = from_slot; i > to_slot; i--)
        {
            s->workspaces[slots[i]] = s->workspaces[slots[i - 1]];
            if (has_ui)
                s->ui[slots[i]] = s->ui[slots[i - 1]];
        }
    }
    s->workspaces[to] = moved_ws;
    if (has_ui)
        s->ui[to] = moved_ui;
    for (k = 0; k < s->workspace_count; k++)
    {
        s->workspaces[k].order = k;
    }
    s->order_persist_generation = PicoCatalog_EnqueueOrder(s->host, s->workspaces,
                                                           s->workspace_count);
    if (s->order_persist_generation == 0)
    {
        s->order_unsaved = true;
    }
}

static void RenderDropIndicator(void)
{
    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_PERCENT(1),
                                        .height = CLAY_SIZING_FIXED(2)}},
                  .backgroundColor = (Clay_Color){80, 140, 255, 255},
                  .cornerRadius = CLAY_CORNER_RADIUS(1)})
    {
    }
}

static void RenderWorkspaceRow(PicoHost *host, SidebarState *s, const PicoCatalogWorkspace *ws, int index)
{
    Clay_ElementId row_id = CLAY_IDI("SidebarWs", index);
    Clay_ElementId plus_id = CLAY_IDI("SidebarPlus", index);
    Clay_ElementId edit_id = CLAY_IDI("SidebarEdit", index);
    bool edit_hovered = Clay_PointerOver(edit_id);
    bool is_dragged = s && s->is_dragging && s->drag_source_index == index;
    bool plus_hovered = !is_dragged && Clay_PointerOver(plus_id);
    bool hovered = !is_dragged && (Clay_PointerOver(row_id) || plus_hovered);
    Clay_Color fill = is_dragged ? (Clay_Color){35, 35, 42, 100} : RowFill(false, hovered);
    Clay_Color text_col = is_dragged ? (Clay_Color){120, 120, 135, 120} : COLOR_TEXT;

    CLAY(row_id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .padding = {SIDEBAR_ROW_PAD_X, SIDEBAR_ROW_PAD_X, 4, 4},
                             .childGap = SIDEBAR_ROW_GAP,
                             .sizing = {.width = CLAY_SIZING_PERCENT(1)}},
                  .backgroundColor = fill,
                  .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY(edit_id, {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(Pico_FontPx(SIDEBAR_FOLDER_ICON)),
                                             .height = CLAY_SIZING_FIXED(Pico_FontPx(SIDEBAR_FOLDER_ICON))}}})
        {
            RenderFolderIcon(edit_hovered ? &s->pen_icon : ws->collapsed ? &s->folder_collapsed
                                                                         : &s->folder_expanded,
                             edit_hovered ? "*" : ws->collapsed ? ">"
                                                                : "v");
        }
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}},
                      .clip = {.horizontal = true}})
        {
            CLAY_TEXT(CStr(ws->name),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = PICO_FONT_UI,
                                        .textColor = text_col,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        if (hovered && !is_dragged)
        {
            CLAY(plus_id, {.layout = {.padding = {4, 4, 0, 0}}})
            {
                RenderGlyph("+", plus_hovered ? COLOR_TEXT : COLOR_MUTED);
            }
        }
    }
    (void)host;
}

static void RenderSessionRow(PicoHost *host, SidebarState *s, const char *ws_path,
                             const char *title, const char *session_id, PicoAgentId live_id,
                             int row_id, bool catalog_unseen, bool worktree,
                             bool missing_checkout)
{
    bool selected = SessionIsSelected(host, ws_path, session_id, live_id);
    PicoAgentId row_live_id = live_id ? live_id : LiveMainAgent(host, ws_path, session_id);
    const char *target_ws = NULL;
    const char *target_id = NULL;
    bool loading = PicoSession_LoadTarget(host, &target_ws, &target_id) &&
                   ws_path && session_id && session_id[0] &&
                   strcmp(ws_path, target_ws) == 0 && strcmp(session_id, target_id) == 0;
    Clay_ElementId id = CLAY_IDI("SidebarSess", row_id);
    bool hovered = Clay_PointerOver(id);
    if (loading)
    {
        host->session_load_row_rendered = host->session_load_row_id != row_id ||
                                          host->session_load_row_was_visible;
        host->session_load_row_id = row_id;
        host->session_load_row_known = true;
    }
    CLAY(id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                         .padding = {SIDEBAR_ROW_PAD_X, SIDEBAR_ROW_PAD_X, 3, 3},
                         .childGap = SIDEBAR_ROW_GAP,
                         .sizing = {.width = CLAY_SIZING_PERCENT(1)}},
              .backgroundColor = RowFill(selected, hovered),
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        RenderSessionDot(s, SessionDotKind(host, ws_path, session_id, row_live_id, catalog_unseen),
                         row_id, worktree, loading, !row_live_id);
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}},
                      .clip = {.horizontal = true}})
        {
            CLAY_TEXT(CStr(title && title[0] ? title : "Untitled"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = PICO_FONT_UI,
                                        .textColor = missing_checkout ? COLOR_STATUS_ERR
                                                                      : (selected ? COLOR_TEXT : COLOR_MUTED),
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
    }
}

static bool AgentInCatalogProject(const PicoAgent *agent, const PicoCatalogWorkspace *ws);
static PicoAgentId LiveExtraAt(PicoHost *host, const PicoCatalogWorkspace *ws, int extra_index);
static int CountLiveExtras(PicoHost *host, const PicoCatalogWorkspace *ws);

typedef struct SidebarPin
{
    bool found;
    PicoAgentId live_id;
    int row_id;
    const char *title;
    const char *session_id;
    const char *checkout_path;
    bool worktree;
    bool missing_checkout;
    bool unseen_complete;
} SidebarPin;

static SidebarPin FindSelectedSidebarRow(PicoHost *host, const PicoCatalogWorkspace *ws, int ws_index)
{
    SidebarPin pin = {0};
    PicoAgentInfo info;
    int extras;
    int j;
    if (!host || !ws)
    {
        return pin;
    }
    extras = CountLiveExtras(host, ws);
    for (j = 0; j < extras; j++)
    {
        PicoAgentId extra = LiveExtraAt(host, ws, j);
        if (!pico_agent_find(host, extra, &info))
        {
            continue;
        }
        PicoAgent *selected_agent = PicoHost_FindAgent(host, extra);
        if (!SessionIsSelected(host, PicoAgent_WorkspacePath(selected_agent), info.session_id, extra))
        {
            continue;
        }
        pin.found = true;
        pin.live_id = extra;
        pin.row_id = SessionRowId(ws_index, j);
        pin.title = info.session_id[0] ? "Untitled" : "New session";
        pin.session_id = "";
        PicoAgent *live_agent = PicoHost_FindAgent(host, extra);
        pin.checkout_path = PicoAgent_WorkspacePath(live_agent);
        pin.worktree = live_agent && live_agent->workspace && live_agent->workspace->worktree;
        pin.unseen_complete = false;
        return pin;
    }
    for (j = 0; j < ws->session_count; j++)
    {
        if (!SessionIsSelected(host, ws->sessions[j].checkout_path, ws->sessions[j].id, 0))
        {
            continue;
        }
        pin.found = true;
        pin.live_id = 0;
        pin.row_id = SessionRowId(ws_index, extras + j);
        pin.title = ws->sessions[j].title;
        pin.session_id = ws->sessions[j].id;
        pin.checkout_path = ws->sessions[j].checkout_path;
        pin.worktree = ws->sessions[j].worktree;
        pin.missing_checkout = ws->sessions[j].missing_checkout;
        pin.unseen_complete = ws->sessions[j].unseen_complete;
        return pin;
    }
    return pin;
}

static void RenderPinnedSelected(PicoHost *host, SidebarState *s, const PicoCatalogWorkspace *ws, int ws_index)
{
    SidebarPin pin = FindSelectedSidebarRow(host, ws, ws_index);
    if (!pin.found)
    {
        return;
    }
    RenderSessionRow(host, s, pin.checkout_path, pin.title, pin.session_id, pin.live_id,
                     pin.row_id, pin.unseen_complete, pin.worktree, pin.missing_checkout);
}

static bool OpenPinnedSelected(PicoHost *host, SidebarState *s, const PicoCatalogWorkspace *ws,
                               int ws_index)
{
    SidebarPin pin = FindSelectedSidebarRow(host, ws, ws_index);
    if (!pin.found || !Clay_PointerOver(CLAY_IDI("SidebarSess", pin.row_id)))
    {
        return false;
    }
    if (pin.live_id)
    {
        SelectAgent(host, pin.live_id);
        return true;
    }
    OpenCatalogSession(host, s, pin.checkout_path, pin.session_id);
    return true;
}

static void RenderMoreLessLabel(Clay_ElementId id, Clay_String label)
{
    bool hovered = Clay_PointerOver(id);
    CLAY(id, {.layout = {.padding = {6, 6, 2, 2}},
              .backgroundColor = RowFill(false, hovered),
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                           .fontSize = PICO_FONT_CAPTION,
                                           .textColor = COLOR_MUTED,
                                           .wrapMode = CLAY_TEXT_WRAP_NONE}));
    }
}

static void RenderMoreLessRow(int ws_index, int shown, int total)
{
    bool more = shown < total;
    bool less = shown > SIDEBAR_SESSION_PAGE;
    if (!more && !less)
    {
        return;
    }
    CLAY(CLAY_IDI("SidebarMoreLess", ws_index),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                     .padding = {SIDEBAR_ROW_PAD_X - 6, SIDEBAR_ROW_PAD_X - 6, 2, 2},
                     .childGap = SIDEBAR_ROW_GAP,
                     .sizing = {.width = CLAY_SIZING_PERCENT(1)}}})
    {
        if (more)
        {
            float gutter = Pico_FontPx(SIDEBAR_FOLDER_ICON);
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(gutter)}}}) {}
            RenderMoreLessLabel(CLAY_IDI("SidebarMore", ws_index), CLAY_STRING("More"));
        }
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
        if (less)
        {
            RenderMoreLessLabel(CLAY_IDI("SidebarLess", ws_index), CLAY_STRING("Less"));
        }
    }
}

static void RenderLiveExtras(PicoHost *host, SidebarState *s, const PicoCatalogWorkspace *ws,
                             int ws_index, int max_extras)
{
    int n = pico_agent_count(host);
    int extra = 0;
    int i;
    for (i = 0; i < n; i++)
    {
        PicoAgentInfo info;
        PicoAgent *agent;
        const char *agent_ws;
        if (!pico_agent_info(host, i, &info) || info.kind != PICO_AGENT_MAIN)
        {
            continue;
        }
        agent = PicoHost_FindAgent(host, info.id);
        agent_ws = PicoAgent_WorkspacePath(agent);
        if (!agent_ws || !AgentInCatalogProject(agent, ws))
        {
            continue;
        }
        if (info.session_id[0] && CatalogHasSession(ws, agent_ws, info.session_id))
        {
            continue;
        }
        if (extra >= max_extras)
        {
            break;
        }
        RenderSessionRow(host, s, agent_ws, info.session_id[0] ? "Untitled" : "New session",
                         info.session_id, info.id, SessionRowId(ws_index, extra), false,
                         agent && agent->workspace && agent->workspace->worktree, false);
        extra++;
    }
}

static void PicoSidebar_Render(PicoHost *host, void *state)
{
    SidebarState *s = state ? (SidebarState *)state : (SidebarState *)PicoPlugins_HostState(host, "sidebar");
    int i;
    int j;
    int extras;
    int total;
    int shown;
    bool add_hover;
    if (!s)
    {
        return;
    }
    EnsureFolderIcons(s);

    add_hover = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarAddWs")));
    CLAY(CLAY_ID("SidebarRoot"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 6,
                     .sizing = {.width = CLAY_SIZING_PERCENT(1), .height = CLAY_SIZING_GROW(0)}}})
    {
        CLAY(CLAY_ID("SidebarProjectsHeader"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                         .padding = {SIDEBAR_ROW_PAD_X, SIDEBAR_ROW_PAD_X, 4, 4},
                         .childGap = SIDEBAR_ROW_GAP,
                         .sizing = {.width = CLAY_SIZING_PERCENT(1)}}})
        {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}},
                          .clip = {.horizontal = true}})
            {
                CLAY_TEXT(CLAY_STRING("Projects"),
                          CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                            .fontSize = PICO_FONT_UI,
                                            .textColor = COLOR_MUTED,
                                            .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
            CLAY(CLAY_ID("SidebarAddWs"), {.layout = {.padding = {4, 4, 0, 0}}})
            {
                RenderGlyph("+", add_hover ? COLOR_TEXT : COLOR_MUTED);
            }
        }

        CLAY(CLAY_ID("SidebarScroll"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = 2,
                         .sizing = {.width = CLAY_SIZING_PERCENT(1), .height = CLAY_SIZING_GROW(0)}},
              .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
        {
            for (int section = 0; section < 2; section++)
            {
                if (section == 1)
                {
                    bool any = false;
                    for (int k = 0; k < s->workspace_count; k++)
                        if (s->workspaces[k].stashed)
                            any = true;
                    if (!any)
                        break;
                    Clay_ElementId stash_id = CLAY_ID("SidebarStashedHeader");
                    CLAY(stash_id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                               .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                               .childGap = SIDEBAR_ROW_GAP,
                                               .padding = {SIDEBAR_ROW_PAD_X, 4, 8, 4},
                                               .sizing = {.width = CLAY_SIZING_PERCENT(1)}}})
                    {
                        RenderFolderIcon(s->stash_expanded ? &s->archive_open : &s->archive_closed,
                                         s->stash_expanded ? "v" : ">");
                        CLAY_TEXT(CLAY_STRING("Stashed"),
                                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI, .textColor = Clay_PointerOver(stash_id) ? COLOR_TEXT : COLOR_MUTED}));
                    }
                    if (!s->stash_expanded)
                        break;
                }
                for (i = 0; i < s->workspace_count; i++)
                {
                    if (s->workspaces[i].stashed != (section == 1))
                        continue;
                    if (s->is_dragging && s->drag_target_index != s->drag_source_index)
                    {
                        if (s->drag_target_index < s->drag_source_index && s->drag_target_index == i)
                        {
                            RenderDropIndicator();
                        }
                    }
                    const PicoCatalogWorkspace *ws = &s->workspaces[i];
                    RenderWorkspaceRow(host, s, ws, i);
                    if (ws->collapsed)
                    {
                        RenderPinnedSelected(host, s, ws, i);
                    }
                    else
                    {
                        extras = CountLiveExtras(host, ws);
                        total = extras + ws->session_count;
                        shown = ShownForIndex(s, i, total);
                        RenderLiveExtras(host, s, ws, i, shown < extras ? shown : extras);
                        for (j = 0; j < shown - extras && j < ws->session_count; j++)
                        {
                            RenderSessionRow(host, s, ws->sessions[j].checkout_path, ws->sessions[j].title,
                                             ws->sessions[j].id, 0, SessionRowId(i, extras + j),
                                             ws->sessions[j].unseen_complete, ws->sessions[j].worktree,
                                             ws->sessions[j].missing_checkout);
                        }
                        RenderMoreLessRow(i, shown, total);
                    }
                    if (s->is_dragging && s->drag_target_index != s->drag_source_index)
                    {
                        if (s->drag_target_index > s->drag_source_index && s->drag_target_index == i)
                        {
                            RenderDropIndicator();
                        }
                    }
                }
            }
        }

        if (s->is_dragging && s->drag_source_index >= 0 && s->drag_source_index < s->workspace_count)
        {
            Vector2 mouse = GetMousePosition();
            const PicoCatalogWorkspace *drag_ws = &s->workspaces[s->drag_source_index];
            CLAY(CLAY_ID("SidebarDragPreview"),
                 {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                               .offset = {.x = mouse.x + 12.0f, .y = mouse.y - 12.0f},
                               .zIndex = 50,
                               .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH},
                  .layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .padding = {SIDEBAR_ROW_PAD_X, SIDEBAR_ROW_PAD_X, 4, 4},
                             .childGap = SIDEBAR_ROW_GAP,
                             .sizing = {.width = CLAY_SIZING_FIXED(180)}},
                  .backgroundColor = (Clay_Color){42, 42, 50, 230},
                  .cornerRadius = CLAY_CORNER_RADIUS(6),
                  .border = {.width = {1, 1, 1, 1}, .color = (Clay_Color){80, 140, 255, 200}}})
            {
                RenderFolderIcon(drag_ws->collapsed ? &s->folder_collapsed : &s->folder_expanded,
                                 drag_ws->collapsed ? ">" : "v");
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}},
                              .clip = {.horizontal = true}})
                {
                    CLAY_TEXT(CStr(drag_ws->name),
                              CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                .fontSize = PICO_FONT_UI,
                                                .textColor = COLOR_TEXT,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
            }
        }

        {
            bool settings_hover = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarSettings")));
            float icon = Pico_FontPx(SIDEBAR_FOLDER_ICON);
            float row_h = icon + 12.0f;
            CLAY(CLAY_ID("SidebarSettingsRow"),
                 {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER},
                             .sizing = {.width = CLAY_SIZING_PERCENT(1), .height = CLAY_SIZING_FIXED(row_h)}}})
            {
                CLAY(CLAY_ID("SidebarSettings"),
                     {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                                 .padding = {6, 6, 4, 4},
                                 .sizing = {.width = CLAY_SIZING_FIXED(icon + 12.0f),
                                            .height = CLAY_SIZING_FIXED(icon + 8.0f)}}})
                {
                    RenderSettingsIcon(s, settings_hover);
                }
            }
        }
    }
}

static int EditWorkspaceIndex(const SidebarState *s)
{
    if (!s || !s->edit_open)
        return -1;
    for (int i = 0; i < s->workspace_count; i++)
        if (!strcmp(s->workspaces[i].path, s->edit_path))
            return i;
    return -1;
}

static bool CloseWorkspaceEdit(SidebarState *s)
{
    if (!s || !s->edit_open)
        return true;
    if (!pico_ui_modal_pop(s->host, "sidebar-workspace-edit"))
        return false; /* buried under another modal; retry once it is top again */
    s->edit_open = false;
    s->delete_confirm = false;
    PicoTextField_Unbind(&s->edit_field);
    return true;
}

static void OpenWorkspaceEdit(SidebarState *s, int index)
{
    if (!s || index < 0 || index >= s->workspace_count ||
        !pico_ui_modal_push(s->host, "sidebar-workspace-edit"))
        return;
    s->edit_open = true;
    s->delete_confirm = false;
    s->edit_pointer_latched = true; /* opening press must not close the new popup */
    snprintf(s->edit_path, sizeof(s->edit_path), "%s", s->workspaces[index].path);
    snprintf(s->edit_name, sizeof(s->edit_name), "%s", s->workspaces[index].name);
    PicoTextField_Bind(&s->edit_field, s->edit_name, sizeof(s->edit_name));
    snprintf(s->delete_label, sizeof(s->delete_label),
             "Delete saved sessions from %s and ALL its checkouts and worktrees? Project folders remain untouched.",
             s->workspaces[index].name);
}

static void RenderEditAction(Clay_ElementId id, const char *label, bool danger)
{
    CLAY(id, {.layout = {.padding = {7, 7, 5, 5},
                         .sizing = {.width = CLAY_SIZING_PERCENT(1)}},
              .backgroundColor = Clay_PointerOver(id) ? (Clay_Color){52, 52, 62, 255} : (Clay_Color){0, 0, 0, 0},
              .cornerRadius = CLAY_CORNER_RADIUS(4)})
    {
        CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                 .fontSize = PICO_FONT_UI,
                                                 .textColor = danger ? (Clay_Color){235, 105, 105, 255} : COLOR_TEXT,
                                                 .wrapMode = CLAY_TEXT_WRAP_WORDS}));
    }
}

static void RenderWorkspaceEdit(PicoHost *host, void *state)
{
    SidebarState *s = state ? state : PicoPlugins_HostState(host, "sidebar");
    int i = EditWorkspaceIndex(s);
    Clay_ElementData row;
    float x, y, height, expected;
    bool short_view;
    if (i < 0 || !pico_ui_modal_is_top(host, "sidebar-workspace-edit"))
        return;
    row = Clay_GetElementData(CLAY_IDI("SidebarWs", i));
    if (!row.found)
        return;
    x = row.boundingBox.x;
    y = row.boundingBox.y + row.boundingBox.height + 3;
    if (x + 244 > GetScreenWidth())
        x = GetScreenWidth() - 244;
    if (x < 4)
        x = 4;
    /* The confirmation is taller and fonts can be scaled by the user. */
    expected = (s->delete_confirm ? 300.0f : 200.0f) * Pico_FontPx(PICO_FONT_UI) / PICO_FONT_UI;
    height = expected;
    short_view = height > GetScreenHeight() - 8;
    if (short_view)
        height = GetScreenHeight() - 8;
    if (y + height > GetScreenHeight())
        y = row.boundingBox.y - height - 3;
    if (y < 4)
        y = 4;
    CLAY(CLAY_ID("SidebarEditPopup"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {.x = x, .y = y}, .zIndex = 55},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 5, .padding = {10, 10, 10, 10}, .sizing = {.width = CLAY_SIZING_FIXED(240), .height = short_view ? CLAY_SIZING_FIXED(height) : CLAY_SIZING_FIT(0)}},
          .clip = {.vertical = short_view, .childOffset = Clay_GetScrollOffset()},
          .backgroundColor = COLOR_CONTENT_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(7),
          .border = {.width = {1, 1, 1, 1}, .color = COLOR_MUTED}})
    {
        CLAY_TEXT(CLAY_STRING("Project name "), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                  .fontSize = PICO_FONT_UI,
                                                                  .textColor = COLOR_MUTED}));
        CLAY(CLAY_ID("SidebarEditName"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .padding = {7, 7, 6, 6},
                         .sizing = {.width = CLAY_SIZING_PERCENT(1)}},
              .clip = {.horizontal = true, .childOffset = {.x = -s->edit_field.scroll_x}},
              .backgroundColor = (Clay_Color){35, 35, 42, 255},
              .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            Clay_TextElementConfig field_config = PicoTextField_Config(FONT_REGULAR);
            if (!s->edit_name[0])
                field_config.textColor = COLOR_MUTED;
            CLAY_TEXT(CStr(s->edit_name[0] ? s->edit_name : " "),
                      CLAY_TEXT_CONFIG(field_config));
        }
        RenderEditAction(CLAY_ID("SidebarEditSave"), "Save name", false);
        RenderEditAction(CLAY_ID("SidebarEditStash"), s->workspaces[i].stashed ? "Restore to Projects" : "Stash project", false);
        if (!s->delete_confirm)
            RenderEditAction(CLAY_ID("SidebarEditDelete"), "Delete workspace...", true);
        else
        {
            CLAY_TEXT(CStr(s->delete_label),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_CAPTION, .textColor = COLOR_TEXT, .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            RenderEditAction(CLAY_ID("SidebarEditConfirm"), "Delete all saved history", true);
            RenderEditAction(CLAY_ID("SidebarEditCancelDelete"), "Cancel", false);
        }
    }
}

static void WorkspaceEditInput(PicoHost *host, SidebarState *s)
{
    int i = EditWorkspaceIndex(s);
    if (i < 0)
    {
        CloseWorkspaceEdit(s);
        return;
    }
    if (!pico_ui_modal_is_top(host, "sidebar-workspace-edit"))
        return;
    host->hovered_text = Clay_PointerOver(CLAY_ID("SidebarEditName"));
    if (IsKeyPressed(KEY_ESCAPE))
    {
        CloseWorkspaceEdit(s);
        return;
    }
    if (!s->delete_confirm)
    {
        PicoTextField_HandleKeys(&s->edit_field);
    }
    else
    {
        while (GetCharPressed() != 0)
        {
        } /* the queue is global; discard typing while confirming */
    }
    {
        Clay_ElementData name_box = Clay_GetElementData(CLAY_ID("SidebarEditName"));
        if (name_box.found)
            PicoTextField_KeepCaretVisible(&s->edit_field, name_box.boundingBox.width - 14.0f,
                                           FONT_REGULAR);
    }
    if (s->edit_pointer_latched)
    {
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            s->edit_pointer_latched = false;
        return;
    }
    PicoTextField_HandlePointer(&s->edit_field, CLAY_ID("SidebarEditName"), 7.0f, FONT_REGULAR);
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        return;
    s->edit_pointer_latched = true;
    if (Clay_PointerOver(CLAY_ID("SidebarEditName")))
        return; /* the field consumed the press */
    if (Clay_PointerOver(CLAY_ID("SidebarEditSave")) && !s->delete_confirm)
    {
        if (PicoCatalog_SetProjectName(s->edit_path, s->edit_name) != 0)
            PicoOverlay_Notify(host, "Enter a name to rename the project.");
        else
        {
            s->dirty = true;
            CloseWorkspaceEdit(s);
        }
    }
    else if (Clay_PointerOver(CLAY_ID("SidebarEditStash")) && !s->delete_confirm)
    {
        if (PicoCatalog_SetProjectStashed(s->edit_path, !s->workspaces[i].stashed) != 0)
            PicoOverlay_Notify(host, "Could not update the project.");
        else
        {
            s->dirty = true;
            CloseWorkspaceEdit(s);
        }
    }
    else if (Clay_PointerOver(CLAY_ID("SidebarEditDelete")) && !s->delete_confirm)
        s->delete_confirm = true;
    else if (Clay_PointerOver(CLAY_ID("SidebarEditCancelDelete")))
        s->delete_confirm = false;
    else if (Clay_PointerOver(CLAY_ID("SidebarEditConfirm")) && s->delete_confirm)
    {
        bool live = false;
        for (int k = 0; k < pico_agent_count(host); k++)
        {
            PicoAgentInfo info;
            if (pico_agent_info(host, k, &info) &&
                AgentInCatalogProject(PicoHost_FindAgent(host, info.id), &s->workspaces[i]))
                live = true;
        }
        if (live)
            PicoOverlay_Notify(host, "Close all agents in this project before deleting its history.");
        else if (PicoCatalog_DeleteProject(host, s->edit_path) != 0)
            PicoOverlay_Notify(host, "Could not delete all saved project history.");
        else
        {
            s->dirty = true;
            CloseWorkspaceEdit(s);
        }
    }
    else if (!Clay_PointerOver(CLAY_ID("SidebarEditPopup")))
        CloseWorkspaceEdit(s);
}

static void SidebarDrawEditOverlay(PicoHost *host, const PicoHookEvent *event, void *state)
{
    SidebarState *s = state ? (SidebarState *)state : (SidebarState *)PicoPlugins_HostState(host, "sidebar");
    (void)event;
    if (!s || !s->edit_open || !pico_ui_modal_is_top(host, "sidebar-workspace-edit"))
        return;
    PicoTextField_Draw(&s->edit_field, CLAY_ID("SidebarEditName"), 7.0f, 6.0f, FONT_REGULAR);
}

static void RenderFolderModal(PicoHost *host, void *state)
{
    SidebarState *s = state ? (SidebarState *)state : (SidebarState *)PicoPlugins_HostState(host, "sidebar");
    float sw;
    float sh;
    float card_w;
    if (!s || !s->want_folder)
    {
        return;
    }
    sw = (float)GetScreenWidth();
    sh = (float)GetScreenHeight();
    card_w = sw < 520.0f ? sw - 48.0f : 420.0f;
    if (card_w < 260.0f)
    {
        card_w = 260.0f;
    }
    CLAY(CLAY_ID("SidebarFolderDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 43,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 140}})
    {
        CLAY(CLAY_ID("SidebarFolderCard"),
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
    s->folder_painted = true;
}

static bool AgentInCatalogProject(const PicoAgent *agent, const PicoCatalogWorkspace *ws)
{
    if (!agent || !agent->workspace || !ws)
        return false;
    const char *project = agent->workspace->project_path[0]
                              ? agent->workspace->project_path
                              : agent->workspace->path;
    return strcmp(project, ws->path) == 0;
}

static PicoAgentId LiveExtraAt(PicoHost *host, const PicoCatalogWorkspace *ws, int extra_index)
{
    int n = pico_agent_count(host);
    int extra = 0;
    int i;
    for (i = 0; i < n; i++)
    {
        PicoAgentInfo info;
        PicoAgent *agent;
        const char *agent_ws;
        if (!pico_agent_info(host, i, &info) || info.kind != PICO_AGENT_MAIN)
        {
            continue;
        }
        agent = PicoHost_FindAgent(host, info.id);
        agent_ws = PicoAgent_WorkspacePath(agent);
        if (!agent_ws || !AgentInCatalogProject(agent, ws))
        {
            continue;
        }
        if (info.session_id[0] && CatalogHasSession(ws, agent_ws, info.session_id))
        {
            continue;
        }
        if (extra == extra_index)
        {
            return info.id;
        }
        extra++;
    }
    return 0;
}

static int CountLiveExtras(PicoHost *host, const PicoCatalogWorkspace *ws)
{
    int extra = 0;
    while (LiveExtraAt(host, ws, extra))
    {
        extra++;
    }
    return extra;
}

static bool SidebarPointerOverClickable(PicoHost *host, SidebarState *s)
{
    int i;
    int j;
    int extras;
    int total;
    int shown;
    if (Clay_PointerOver(CLAY_ID("SidebarStashedHeader")) ||
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarAddWs"))) ||
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarSettings"))))
    {
        return true;
    }
    for (i = 0; i < s->workspace_count; i++)
    {
        const PicoCatalogWorkspace *ws = &s->workspaces[i];
        if (Clay_PointerOver(CLAY_IDI("SidebarEdit", i)) ||
            Clay_PointerOver(CLAY_IDI("SidebarPlus", i)) || Clay_PointerOver(CLAY_IDI("SidebarWs", i)))
        {
            return true;
        }
        if (ws->collapsed)
        {
            SidebarPin pin = FindSelectedSidebarRow(host, ws, i);
            if (pin.found && Clay_PointerOver(CLAY_IDI("SidebarSess", pin.row_id)))
            {
                return true;
            }
            continue;
        }
        extras = CountLiveExtras(host, ws);
        total = extras + ws->session_count;
        shown = ShownForIndex(s, i, total);
        if (Clay_PointerOver(CLAY_IDI("SidebarMore", i)) || Clay_PointerOver(CLAY_IDI("SidebarLess", i)))
        {
            return true;
        }
        for (j = 0; j < extras && j < shown; j++)
        {
            if (Clay_PointerOver(CLAY_IDI("SidebarSess", SessionRowId(i, j))))
            {
                return true;
            }
        }
        for (j = 0; j < ws->session_count && extras + j < shown; j++)
        {
            if (Clay_PointerOver(CLAY_IDI("SidebarSess", SessionRowId(i, extras + j))))
            {
                return true;
            }
        }
    }
    return false;
}

static void SidebarAfterLayout(PicoHost *host, const PicoHookEvent *event, void *state)
{
    SidebarState *s = state ? (SidebarState *)state : (SidebarState *)PicoPlugins_HostState(host, "sidebar");
    int i;
    int j;
    int extras;
    int total;
    int shown;
    (void)event;
    if (!s)
    {
        return;
    }
    if (s->edit_open)
    {
        WorkspaceEditInput(host, s);
        return;
    }
    if (s->want_folder || PicoUi_ModalOpen(host) || IsKeyPressed(KEY_ESCAPE))
    {
        s->drag_press_pending = false;
        s->is_dragging = false;
        s->drag_source_index = -1;
        s->drag_target_index = -1;
        host->ui_drag_active = false;
        return;
    }

    if (s->is_dragging)
    {
        host->hovered_drag = true;
    }
    else if (SidebarPointerOverClickable(host, s))
    {
        host->hovered_clickable = true;
    }

    if (s->drag_press_pending)
    {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        {
            Vector2 mouse = GetMousePosition();
            if (!s->is_dragging &&
                PicoSidebar_DragMoved(s->drag_press_pos.x, s->drag_press_pos.y,
                                      mouse.x, mouse.y))
            {
                s->is_dragging = true;
            }
            if (s->is_dragging)
            {
                host->hovered_drag = true;
                s->drag_target_index = SidebarComputeDragTarget(
                    s, mouse.y,
                    Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarScroll"))));
                return;
            }
        }
        else
        {
            if (s->is_dragging)
            {
                Vector2 mouse = GetMousePosition();
                s->drag_target_index = SidebarComputeDragTarget(
                    s, mouse.y,
                    Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarScroll"))));
                if (s->drag_target_index >= 0 && s->drag_target_index < s->workspace_count &&
                    s->drag_target_index != s->drag_source_index)
                {
                    SidebarReorderWorkspaces(s, s->drag_source_index, s->drag_target_index);
                }
                s->is_dragging = false;
                s->drag_press_pending = false;
                s->drag_source_index = -1;
                s->drag_target_index = -1;
                host->ui_drag_active = false;
                return;
            }
            else
            {
                int clicked = s->drag_source_index;
                s->drag_press_pending = false;
                s->drag_source_index = -1;
                s->drag_target_index = -1;
                host->ui_drag_active = false;
                if (clicked >= 0 && clicked < s->workspace_count &&
                    Clay_PointerOver(CLAY_IDI("SidebarWs", clicked)))
                {
                    ToggleCollapsed(s, clicked);
                    return;
                }
            }
        }
    }

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarAddWs"))))
    {
        RequestAddWorkspace(host, s);
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarSettings"))))
    {
        PicoSettingsUi_Open(host);
        return;
    }
    if (Clay_PointerOver(CLAY_ID("SidebarStashedHeader")))
    {
        s->stash_expanded = !s->stash_expanded;
        return;
    }
    for (i = 0; i < s->workspace_count; i++)
    {
        PicoCatalogWorkspace *ws = &s->workspaces[i];
        if (Clay_PointerOver(CLAY_IDI("SidebarEdit", i)))
        {
            OpenWorkspaceEdit(s, i);
            return;
        }
        if (Clay_PointerOver(CLAY_IDI("SidebarPlus", i)))
        {
            NewSessionInWorkspace(host, s, i);
            return;
        }
        if (Clay_PointerOver(CLAY_IDI("SidebarWs", i)))
        {
            if (ws->stashed)
            {
                ToggleCollapsed(s, i);
                return;
            }
            s->drag_press_pending = true;
            s->drag_source_index = i;
            s->drag_press_pos = GetMousePosition();
            s->is_dragging = false;
            s->drag_target_index = i;
            host->ui_drag_active = true;
            return;
        }
        if (ws->collapsed)
        {
            if (OpenPinnedSelected(host, s, ws, i))
            {
                return;
            }
            continue;
        }
        extras = CountLiveExtras(host, ws);
        total = extras + ws->session_count;
        shown = ShownForIndex(s, i, total);
        if (Clay_PointerOver(CLAY_IDI("SidebarMore", i)))
        {
            AdjustShown(s, i, SIDEBAR_SESSION_PAGE, total);
            return;
        }
        if (Clay_PointerOver(CLAY_IDI("SidebarLess", i)))
        {
            AdjustShown(s, i, -SIDEBAR_SESSION_PAGE, total);
            return;
        }
        for (j = 0; j < extras && j < shown; j++)
        {
            PicoAgentId extra = LiveExtraAt(host, ws, j);
            if (Clay_PointerOver(CLAY_IDI("SidebarSess", SessionRowId(i, j))))
            {
                SelectAgent(host, extra);
                return;
            }
        }
        for (j = 0; j < ws->session_count && extras + j < shown; j++)
        {
            if (Clay_PointerOver(CLAY_IDI("SidebarSess", SessionRowId(i, extras + j))))
            {
                OpenCatalogSession(host, s, ws->sessions[j].checkout_path, ws->sessions[j].id);
                return;
            }
        }
    }
}

static void SidebarOnFrame(PicoHost *host, void *state, float dt)
{
    SidebarState *s = state ? (SidebarState *)state : (SidebarState *)PicoPlugins_HostState(host, "sidebar");
    const PicoAgent *selected;
    (void)dt;
    if (!s)
    {
        return;
    }
    selected = PicoHost_SelectedAgentConst(host);
    if (s->order_persist_generation != 0)
    {
        PicoCatalogPersistStatus status = PicoCatalog_OrderPersistStatus(
            host, s->order_persist_generation);
        if (status == PICO_CATALOG_PERSIST_SUCCEEDED)
        {
            s->order_persist_generation = 0;
            s->order_unsaved = false;
            s->dirty = true;
        }
        else if (status == PICO_CATALOG_PERSIST_FAILED)
        {
            s->order_persist_generation = 0;
            s->order_unsaved = true;
        }
    }
    double now = GetTime();
    bool poll_due = !s->catalog_scanned || now - s->last_scan >= SIDEBAR_SCAN_SEC;
#ifdef PICO_SESSION_TEST_HOOKS
    poll_due = poll_due || PicoSession_TestHook("sidebar_poll_due");
#endif
    bool reconcile_due = !s->catalog_scanned ||
                         now - s->last_reconcile >= SIDEBAR_RECONCILE_SEC;
    if (!s->is_dragging && !s->drag_press_pending &&
        s->order_persist_generation == 0 &&
        (s->dirty || (poll_due && (reconcile_due || SidebarCatalogChanged(s)))))
    {
        SidebarRefresh(s);
    }
    else if (poll_due)
    {
        s->last_scan = now;
    }
    if (!s->want_folder || !s->folder_painted || !pico_ui_modal_is_top(host, "sidebar-folder"))
    {
        return;
    }
    if (!FolderDialogGraphic())
    {
        ClearFolderRequest(s);
        PicoOverlay_Notify(host, "Folder dialog unavailable. Install zenity or kdialog.");
        return;
    }
    {
        const char *start = PicoAgent_WorkspacePath(selected);
        char *path;
        if (!start || !start[0])
        {
            start = NULL;
        }
        path = tinyfd_selectFolderDialog("Add workspace", start);
        ClearFolderRequest(s);
        if (path && path[0])
        {
            if (PicoCatalog_Ensure(path) != 0)
            {
                PicoOverlay_Notify(host, "Could not add that workspace.");
            }
            else
            {
                PicoHost_ChangeWorkspace(host, PicoHost_SelectedWorkspace(host), path);
                s->dirty = true;
            }
        }
    }
}

static int SidebarInit(PicoHost *host, void **state_out)
{
    SidebarState *s = (SidebarState *)calloc(1, sizeof(SidebarState));
    if (!s)
    {
        return 1;
    }
    s->host = host;
    s->dirty = true;
    if (state_out)
    {
        *state_out = s;
    }
    pico_host_add_view(host, PICO_SLOT_SIDEBAR, 0, PicoSidebar_Render);
    pico_host_add_view(host, PICO_SLOT_OVERLAY, 41, RenderFolderModal);
    pico_host_add_view(host, PICO_SLOT_OVERLAY, 42, RenderWorkspaceEdit);
    pico_host_add_hook(host, PICO_HOOK_AFTER_LAYOUT, SidebarAfterLayout);
    pico_host_add_hook(host, PICO_HOOK_AFTER_RENDER, SidebarDrawEditOverlay);
    return 0;
}

static void SidebarShutdown(PicoHost *host, void *state)
{
    SidebarState *s = (SidebarState *)state;
    if (!s)
    {
        return;
    }
    (void)ClearFolderRequest(s);
    (void)CloseWorkspaceEdit(s);
    if (host)
    {
        host->ui_drag_active = false;
    }
    UnloadFolderIcons(s);
    PicoCatalog_Free(s->workspaces, s->workspace_count);
    free(s->ui);
    free(s);
}

PicoExt pico_ext_sidebar(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "sidebar",
        .description = "Workspace and session list",
        .host_init = SidebarInit,
        .host_shutdown = SidebarShutdown,
        .host_on_frame = SidebarOnFrame,
    };
}
