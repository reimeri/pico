#define _DEFAULT_SOURCE

#include "../agent_internal.h"
#define _POSIX_C_SOURCE 200809L
#include "host_internal.h"

#include "pico/plugin.h"
#include "agent.h"
#include "builtins/chat.h"
#include "overlay.h"
#include "session.h"
#include "tinyfiledialogs.h"

#include "clay/clay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIDEBAR_SCAN_SEC 0.5
#define SIDEBAR_SESSION_PAGE 10
#define SIDEBAR_FOLDER_ICON 14

typedef struct SidebarWsUi {
    char path[4096];
    int shown;
} SidebarWsUi;

typedef struct SidebarState {
    PicoHost *host;
    PicoCatalogWorkspace *workspaces;
    int workspace_count;
    SidebarWsUi *ui;
    int ui_count;
    bool want_folder;
    bool folder_painted;
    bool dirty;
    double last_scan;
    Texture2D folder_collapsed;
    Texture2D folder_expanded;
    bool icons_tried;
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

static void SidebarRefresh(SidebarState *s)
{
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
    n = PicoCatalog_Scan(&next);
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
    s->dirty = false;
    s->last_scan = GetTime();
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

static bool CatalogHasSession(const PicoCatalogWorkspace *ws, const char *session_id)
{
    int i;
    if (!ws || !session_id || !session_id[0])
    {
        return false;
    }
    for (i = 0; i < ws->session_count; i++)
    {
        if (strcmp(ws->sessions[i].id, session_id) == 0)
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
    PicoAgentCreateOptions options;
    PicoAgentId agent_id = 0;
    PicoResult created;
    live = LiveMainAgent(host, path, session_id);
    if (live)
    {
        SelectAgent(host, live);
        return;
    }
    id = OpenLiveWorkspace(host, path, NULL);
    if (!id)
    {
        return;
    }
    memset(&options, 0, sizeof(options));
    options.kind = PICO_AGENT_MAIN;
    options.session_start = PICO_SESSION_RESUME;
    options.session_id = session_id;
    options.select = true;
    PicoChat_InspectClose();
    created = pico_main_agent_create(host, id, &options, &agent_id);
    if (created == PICO_SESSION_IN_USE)
    {
        live = LiveMainAgent(host, path, session_id);
        if (live)
        {
            SelectAgent(host, live);
            return;
        }
    }
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
    const char *directory = GetApplicationDirectory();
    size_t length = directory ? strlen(directory) : 0;
    if (length > 0)
    {
        snprintf(out, cap, "%s%s%s", directory, directory[length - 1] == '/' ? "" : "/", relative);
    }
    else
    {
        snprintf(out, cap, "%s", relative);
    }
}

static Texture2D LoadFolderIcon(const char *relative)
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
    ImageColorTint(&img, (Color){(unsigned char)COLOR_MUTED.r, (unsigned char)COLOR_MUTED.g,
                                 (unsigned char)COLOR_MUTED.b, (unsigned char)COLOR_MUTED.a});
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
    s->folder_collapsed = LoadFolderIcon("resources/folder-collapsed.png");
    s->folder_expanded = LoadFolderIcon("resources/folder-expanded.png");
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
}

static void RenderGlyph(const char *glyph, Clay_Color color)
{
    CLAY_TEXT(CStr(glyph), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                            .fontSize = 13,
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
    return ws_index * 512 + session_index;
}

static void RenderWorkspaceRow(PicoHost *host, SidebarState *s, const PicoCatalogWorkspace *ws, int index)
{
    Clay_ElementId row_id = CLAY_IDI("SidebarWs", index);
    Clay_ElementId plus_id = CLAY_IDI("SidebarPlus", index);
    bool plus_hovered = Clay_PointerOver(plus_id);
    bool hovered = Clay_PointerOver(row_id) || plus_hovered;
    CLAY(row_id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .padding = {6, 6, 4, 4},
                             .childGap = 6,
                             .sizing = {.width = CLAY_SIZING_PERCENT(1)}},
                  .backgroundColor = RowFill(false, hovered),
                  .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        RenderFolderIcon(ws->collapsed ? &s->folder_collapsed : &s->folder_expanded,
                         ws->collapsed ? ">" : "v");
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}},
                      .clip = {.horizontal = true}})
        {
            CLAY_TEXT(CStr(ws->name),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = 13,
                                        .textColor = COLOR_TEXT,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        if (hovered)
        {
            CLAY(plus_id, {.layout = {.padding = {4, 4, 0, 0}}})
            {
                RenderGlyph("+", plus_hovered ? COLOR_TEXT : COLOR_MUTED);
            }
        }
    }
    (void)host;
}

static void RenderSessionRow(PicoHost *host, const char *ws_path, const char *title,
                             const char *session_id, PicoAgentId live_id, int row_id)
{
    bool selected = SessionIsSelected(host, ws_path, session_id, live_id);
    Clay_ElementId id = CLAY_IDI("SidebarSess", row_id);
    bool hovered = Clay_PointerOver(id);
    CLAY(id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                         .padding = {6, 6, 3, 3},
                         .childGap = 6,
                         .sizing = {.width = CLAY_SIZING_PERCENT(1)}},
              .backgroundColor = RowFill(selected, hovered),
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        RenderGlyph("o", COLOR_MUTED);
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}},
                      .clip = {.horizontal = true}})
        {
            CLAY_TEXT(CStr(title && title[0] ? title : "Untitled"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = 13,
                                        .textColor = selected ? COLOR_TEXT : COLOR_MUTED,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
    }
}

static PicoAgentId LiveExtraAt(PicoHost *host, const PicoCatalogWorkspace *ws, int extra_index);
static int CountLiveExtras(PicoHost *host, const PicoCatalogWorkspace *ws);

typedef struct SidebarPin {
    bool found;
    PicoAgentId live_id;
    int row_id;
    const char *title;
    const char *session_id;
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
        if (!SessionIsSelected(host, ws->path, info.session_id, extra))
        {
            continue;
        }
        pin.found = true;
        pin.live_id = extra;
        pin.row_id = SessionRowId(ws_index, j);
        pin.title = info.session_id[0] ? "Untitled" : "New session";
        pin.session_id = "";
        return pin;
    }
    for (j = 0; j < ws->session_count; j++)
    {
        if (!SessionIsSelected(host, ws->path, ws->sessions[j].id, 0))
        {
            continue;
        }
        pin.found = true;
        pin.live_id = 0;
        pin.row_id = SessionRowId(ws_index, extras + j);
        pin.title = ws->sessions[j].title;
        pin.session_id = ws->sessions[j].id;
        return pin;
    }
    return pin;
}

static void RenderPinnedSelected(PicoHost *host, const PicoCatalogWorkspace *ws, int ws_index)
{
    SidebarPin pin = FindSelectedSidebarRow(host, ws, ws_index);
    if (!pin.found)
    {
        return;
    }
    RenderSessionRow(host, ws->path, pin.title, pin.session_id, pin.live_id, pin.row_id);
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
    OpenCatalogSession(host, s, ws->path, pin.session_id);
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
                                          .fontSize = 12,
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
                     .padding = {6, 6, 2, 2},
                     .sizing = {.width = CLAY_SIZING_PERCENT(1)}}})
    {
        if (more)
        {
            RenderMoreLessLabel(CLAY_IDI("SidebarMore", ws_index), CLAY_STRING("More"));
        }
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
        if (less)
        {
            RenderMoreLessLabel(CLAY_IDI("SidebarLess", ws_index), CLAY_STRING("Less"));
        }
    }
}

static void RenderLiveExtras(PicoHost *host, const PicoCatalogWorkspace *ws, int ws_index,
                             int max_extras)
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
        if (!agent_ws || strcmp(agent_ws, ws->path) != 0)
        {
            continue;
        }
        if (info.session_id[0] && CatalogHasSession(ws, info.session_id))
        {
            continue;
        }
        if (extra >= max_extras)
        {
            break;
        }
        RenderSessionRow(host, ws->path, info.session_id[0] ? "Untitled" : "New session",
                         info.session_id, info.id, SessionRowId(ws_index, extra));
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
        CLAY(CLAY_ID("SidebarAddWs"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                         .padding = {8, 8, 6, 6},
                         .sizing = {.width = CLAY_SIZING_PERCENT(1)}},
              .backgroundColor = add_hover ? (Clay_Color){42, 42, 50, 255} : COLOR_COMPOSER_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
        {
            CLAY_TEXT(CLAY_STRING("Add workspace"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = 13, .textColor = COLOR_TEXT}));
        }

        CLAY(CLAY_ID("SidebarScroll"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = 2,
                         .sizing = {.width = CLAY_SIZING_PERCENT(1), .height = CLAY_SIZING_GROW(0)}},
              .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
        {
            for (i = 0; i < s->workspace_count; i++)
            {
                const PicoCatalogWorkspace *ws = &s->workspaces[i];
                RenderWorkspaceRow(host, s, ws, i);
                if (ws->collapsed)
                {
                    RenderPinnedSelected(host, ws, i);
                    continue;
                }
                extras = CountLiveExtras(host, ws);
                total = extras + ws->session_count;
                shown = ShownForIndex(s, i, total);
                RenderLiveExtras(host, ws, i, shown < extras ? shown : extras);
                for (j = 0; j < shown - extras && j < ws->session_count; j++)
                {
                    RenderSessionRow(host, ws->path, ws->sessions[j].title, ws->sessions[j].id, 0,
                                     SessionRowId(i, extras + j));
                }
                RenderMoreLessRow(i, shown, total);
            }
        }
    }
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
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 18, .textColor = COLOR_TEXT}));
            CLAY_TEXT(CLAY_STRING("To continue choose a workspace folder in the opened file dialog."),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = 14,
                                        .textColor = COLOR_TEXT,
                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
        }
    }
    s->folder_painted = true;
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
        if (!agent_ws || strcmp(agent_ws, ws->path) != 0)
        {
            continue;
        }
        if (info.session_id[0] && CatalogHasSession(ws, info.session_id))
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
    if (s->want_folder || PicoUi_ModalOpen(host))
    {
        return;
    }
    if (Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarAddWs"))) ||
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("SidebarScroll"))))
    {
        host->hovered_clickable = true;
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
    for (i = 0; i < s->workspace_count; i++)
    {
        PicoCatalogWorkspace *ws = &s->workspaces[i];
        if (Clay_PointerOver(CLAY_IDI("SidebarPlus", i)))
        {
            NewSessionInWorkspace(host, s, i);
            return;
        }
        if (Clay_PointerOver(CLAY_IDI("SidebarWs", i)))
        {
            ToggleCollapsed(s, i);
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
                OpenCatalogSession(host, s, ws->path, ws->sessions[j].id);
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
    if (s->dirty || s->last_scan <= 0.0 || GetTime() - s->last_scan >= SIDEBAR_SCAN_SEC)
    {
        SidebarRefresh(s);
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
    pico_host_add_hook(host, PICO_HOOK_AFTER_LAYOUT, SidebarAfterLayout);
    return 0;
}

static void SidebarShutdown(PicoHost *host, void *state)
{
    SidebarState *s = (SidebarState *)state;
    (void)host;
    if (!s)
    {
        return;
    }
    (void)ClearFolderRequest(s);
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
