#define _DEFAULT_SOURCE

#include "../agent_internal.h"
#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "agent.h"
#include "agent_manager.h"
#include "overlay.h"
#include "session.h"
#include "tinyfiledialogs.h"
#include "workspace.h"

#include "clay/clay.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SIDEBAR_MAX_HITS 256
#define SIDEBAR_MAX_ROWS 64

typedef enum SidebarHitKind {
    HIT_ADD_WORKSPACE = 1,
    HIT_TOGGLE,
    HIT_NEW_SESSION,
    HIT_SELECT,
    HIT_RESUME,
    HIT_CLOSE,
    HIT_UNAVAILABLE,
    HIT_MODAL_BROWSE,
    HIT_MODAL_CONFIRM,
    HIT_MODAL_CANCEL,
    HIT_MODAL_DIM,
    HIT_MODAL_PATH,
    HIT_MODAL_NAME,
} SidebarHitKind;

typedef struct SidebarHit {
    SidebarHitKind kind;
    int workspace_index;
    PicoAgentId agent_id;
    char session_id[40];
} SidebarHit;

typedef struct SidebarRow {
    PicoAgentId live_id;
    char session_id[40];
    char title[256];
    PicoAgentPresentationStatus presentation;
    bool live;
    bool closeable;
} SidebarRow;

static bool g_modal;
static char g_path[4096];
static char g_name[PICO_WORKSPACE_NAME_MAX];
static int g_path_cursor;
static int g_name_cursor;
static int g_focus;
static bool g_want_browse;
static char g_path_display[4096];
static char g_name_display[PICO_WORKSPACE_NAME_MAX];
static SidebarHit g_hits[SIDEBAR_MAX_HITS];
static int g_hit_count;
static double g_last_refresh;

bool PicoSidebar_ModalOpen(void)
{
    return g_modal;
}

static Clay_String CStr(const char *s)
{
    if (!s)
    {
        s = "";
    }
    return (Clay_String){.length = (int32_t)strlen(s), .chars = s};
}

static int AddHit(SidebarHitKind kind, int workspace_index, PicoAgentId agent_id, const char *session_id)
{
    if (g_hit_count >= SIDEBAR_MAX_HITS)
    {
        return 0;
    }
    SidebarHit *hit = &g_hits[g_hit_count];
    memset(hit, 0, sizeof(*hit));
    hit->kind = kind;
    hit->workspace_index = workspace_index;
    hit->agent_id = agent_id;
    if (session_id)
    {
        snprintf(hit->session_id, sizeof(hit->session_id), "%s", session_id);
    }
    g_hit_count++;
    return g_hit_count;
}

static Clay_ElementId HitId(int id)
{
    return CLAY_IDI("SidebarHit", id);
}

static void CloseModal(void)
{
    g_modal = false;
    g_path[0] = '\0';
    g_name[0] = '\0';
    g_path_cursor = 0;
    g_name_cursor = 0;
    g_focus = 0;
    g_want_browse = false;
}

static void OpenModal(void)
{
    CloseModal();
    g_modal = true;
}

static void FolderName(const char *path, char *out, size_t cap)
{
    const char *slash = path ? strrchr(path, '/') : NULL;
    const char *name = slash && slash[1] ? slash + 1 : path;
    if (!name || !name[0] || strcmp(name, "/") == 0)
    {
        name = "Workspace";
    }
    if (!out || cap == 0)
    {
        return;
    }
    size_t n = strlen(name);
    if (n >= cap)
    {
        n = cap - 1;
    }
    memcpy(out, name, n);
    out[n] = '\0';
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

static void FieldInsert(char *buf, size_t cap, int *cursor, const char *bytes, int n)
{
    int len = (int)strlen(buf);
    if (n <= 0 || *cursor < 0 || *cursor > len || len + n >= (int)cap)
    {
        return;
    }
    memmove(buf + *cursor + n, buf + *cursor, (size_t)(len - *cursor) + 1);
    memcpy(buf + *cursor, bytes, (size_t)n);
    *cursor += n;
}

static void FieldBackspace(char *buf, int *cursor)
{
    if (*cursor <= 0)
    {
        return;
    }
    int len = (int)strlen(buf);
    int from = *cursor - 1;
    while (from > 0 && ((unsigned char)buf[from] & 0xC0) == 0x80)
    {
        from--;
    }
    memmove(buf + from, buf + *cursor, (size_t)(len - *cursor) + 1);
    *cursor = from;
}

static void TruncateTitle(char *out, size_t cap, const char *src)
{
    if (!src)
    {
        src = "";
    }
    while (*src == ' ' || *src == '\t')
    {
        src++;
    }
    size_t n = 0;
    while (src[n] && src[n] != '\n' && src[n] != '\r' && n + 1 < cap)
    {
        n++;
    }
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t'))
    {
        n--;
    }
    if (n == 0)
    {
        snprintf(out, cap, "Untitled");
        return;
    }
    memcpy(out, src, n);
    out[n] = '\0';
}

static void LiveTitle(PicoApp *app, PicoAgentId id, char *out, size_t cap)
{
    int count = pico_agent_message_count(app, id);
    for (int i = 0; i < count; i++)
    {
        const PicoMessage *msg = pico_agent_message(app, id, i);
        if (msg && msg->role == PICO_ROLE_USER && msg->source && msg->source[0])
        {
            TruncateTitle(out, cap, msg->source);
            return;
        }
    }
    snprintf(out, cap, "Untitled");
}

static PicoAgent *WorkspaceAgent(PicoApp *app, const char *key)
{
    return PicoAgentManager_MostRecentInWorkspace(app->agents, key);
}

static void NotifyResult(PicoApp *app, PicoAgentResult result, const char *fallback)
{
    const char *msg = fallback;
    if (result == PICO_AGENT_RESULT_LIMIT)
    {
        msg = "Agent limit reached.";
    }
    else if (result == PICO_AGENT_RESULT_BUSY)
    {
        msg = "That session is still running.";
    }
    else if (result == PICO_AGENT_RESULT_SESSION_IN_USE)
    {
        msg = "That session is already open.";
    }
    PicoOverlay_Notify(app, msg);
}

static void CreateWorkspaceSession(PicoApp *app, const PicoWorkspace *workspace)
{
    if (!workspace || !workspace->available)
    {
        PicoOverlay_Notify(app, "That workspace is unavailable until its directory is restored.");
        return;
    }
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_NORMAL,
        .workspace_key = workspace->key,
        .session_start = PICO_SESSION_NEW,
        .select = true,
    };
    PicoAgentResult result = pico_agent_create(app, &options, NULL);
    if (result != PICO_AGENT_RESULT_OK)
    {
        NotifyResult(app, result, "Could not create a session in that workspace.");
    }
}

static void ResumeHistorical(PicoApp *app, const PicoWorkspace *workspace, const char *session_id)
{
    if (!workspace || !workspace->available)
    {
        PicoOverlay_Notify(app, "That workspace is unavailable until its directory is restored.");
        return;
    }
    PicoAgent *live = WorkspaceAgent(app, workspace->key);
    PicoAgentResult result;
    if (live)
    {
        result = PicoAgentManager_OpenSession(app, live, session_id, false, true);
    }
    else
    {
        PicoAgentCreateOptions options = {
            .kind = PICO_AGENT_NORMAL,
            .workspace_key = workspace->key,
            .session_start = PICO_SESSION_RESUME,
            .session_id = session_id,
            .select = true,
        };
        result = pico_agent_create(app, &options, NULL);
    }
    if (result != PICO_AGENT_RESULT_OK)
    {
        NotifyResult(app, result, "Could not open that session.");
    }
}

static void ConfirmAddWorkspace(PicoApp *app)
{
    char trimmed[4096];
    snprintf(trimmed, sizeof(trimmed), "%s", g_path);
    size_t len = strlen(trimmed);
    while (len > 0 && isspace((unsigned char)trimmed[len - 1]))
    {
        trimmed[--len] = '\0';
    }
    char *start = trimmed;
    while (*start && isspace((unsigned char)*start))
    {
        start++;
    }
    if (!start[0])
    {
        PicoOverlay_Notify(app, "Choose a workspace folder.");
        return;
    }
    struct stat st;
    if (stat(start, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        PicoOverlay_Notify(app, "Not a directory.");
        return;
    }
    const char *name = g_name[0] ? g_name : NULL;
    char key[4096];
    if (PicoWorkspaceRegistry_Register(app->workspaces, start, name, key, sizeof(key)) !=
        PICO_WORKSPACE_OK)
    {
        PicoOverlay_Notify(app, "Could not register the workspace.");
        return;
    }
    (void)PicoWorkspaceRegistry_SetCollapsed(app->workspaces, key, false);
    CloseModal();
    if (!PicoApp_ChangeWorkspace(app, start))
    {
        PicoOverlay_Notify(app, "Workspace registered.");
    }
}

static Clay_Color StatusColor(PicoAgentPresentationStatus status)
{
    switch (status)
    {
        case PICO_AGENT_PRESENT_ERROR:
            return COLOR_STATUS_ERR;
        case PICO_AGENT_PRESENT_WAITING_USER:
            return COLOR_STATUS_WAIT;
        case PICO_AGENT_PRESENT_RUNNING: {
            float pulse = 0.45f + 0.55f * (0.5f + 0.5f * sinf((float)GetTime() * 6.0f));
            Clay_Color color = COLOR_STATUS_RUN;
            color.a = pulse * 255.0f;
            return color;
        }
        case PICO_AGENT_PRESENT_COMPLETED:
            return COLOR_STATUS_DONE;
        default:
            return (Clay_Color){0, 0, 0, 0};
    }
}

static void RenderStatusDot(PicoAgentPresentationStatus status)
{
    if (status == PICO_AGENT_PRESENT_IDLE)
    {
        return;
    }
    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(8), .height = CLAY_SIZING_FIXED(8)}},
                  .backgroundColor = StatusColor(status),
                  .cornerRadius = CLAY_CORNER_RADIUS(4)})
    {
    }
}

static void RenderButton(int hit, const char *label, bool primary)
{
    bool hover = Clay_PointerOver(HitId(hit));
    Clay_Color bg = primary ? (hover ? COLOR_LINK : COLOR_CODE_BG) : (hover ? COLOR_CODE_BG : COLOR_COMPOSER_BG);
    CLAY(HitId(hit),
         {.layout = {.padding = {10, 10, 6, 6}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}},
          .backgroundColor = bg,
          .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                .fontSize = 13,
                                                .textColor = COLOR_TEXT,
                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
    }
}

static int CollectRows(PicoApp *app, const PicoWorkspace *workspace, SidebarRow *rows, int max)
{
    int n = 0;
    int normals = 0;
    for (int i = 0; i < pico_agent_count(app); i++)
    {
        PicoAgentInfo info;
        normals += pico_agent_info(app, i, &info) && info.kind == PICO_AGENT_NORMAL;
    }
    for (int i = 0; i < pico_agent_count(app) && n < max; i++)
    {
        PicoAgentInfo info;
        if (!pico_agent_info(app, i, &info) || info.kind != PICO_AGENT_NORMAL ||
            strcmp(info.workspace_key, workspace->key) != 0)
        {
            continue;
        }
        SidebarRow *row = &rows[n++];
        memset(row, 0, sizeof(*row));
        row->live_id = info.id;
        snprintf(row->session_id, sizeof(row->session_id), "%s", info.session_id);
        LiveTitle(app, info.id, row->title, sizeof(row->title));
        row->presentation = info.presentation;
        row->live = true;
        row->closeable = !info.busy && normals > 1;
    }
    for (int i = 0; i < n; i++)
    {
        for (int j = i + 1; j < n; j++)
        {
            PicoAgentInfo left;
            PicoAgentInfo right;
            if (!pico_agent_find(app, rows[i].live_id, &left) ||
                !pico_agent_find(app, rows[j].live_id, &right) ||
                left.last_selected_seq >= right.last_selected_seq)
            {
                continue;
            }
            SidebarRow tmp = rows[i];
            rows[i] = rows[j];
            rows[j] = tmp;
        }
    }
    PicoSessionInfo *listed = NULL;
    int listed_n = PicoSession_ListWorkspace(app, workspace->key, &listed, true);
    for (int i = 0; i < listed_n && n < max; i++)
    {
        bool live = false;
        for (int r = 0; r < n; r++)
        {
            if (rows[r].live && rows[r].session_id[0] &&
                strcmp(rows[r].session_id, listed[i].id) == 0)
            {
                live = true;
                break;
            }
        }
        if (live)
        {
            continue;
        }
        SidebarRow *row = &rows[n++];
        memset(row, 0, sizeof(*row));
        snprintf(row->session_id, sizeof(row->session_id), "%s", listed[i].id);
        TruncateTitle(row->title, sizeof(row->title), listed[i].title);
        row->presentation = PICO_AGENT_PRESENT_IDLE;
    }
    free(listed);
    return n;
}

static void RenderSessionRow(PicoApp *app, const PicoWorkspace *workspace, int workspace_index,
                             const SidebarRow *row)
{
    bool active = row->live && row->live_id == pico_agent_active(app);
    int select_id = AddHit(row->live ? HIT_SELECT : HIT_RESUME, workspace_index, row->live_id,
                           row->session_id);
    int close_id = 0;
    bool hover = Clay_PointerOver(HitId(select_id));
    if (row->live && row->closeable && hover)
    {
        close_id = AddHit(HIT_CLOSE, workspace_index, row->live_id, row->session_id);
    }
    Clay_Color bg = active ? COLOR_SIDEBAR_ACTIVE : (hover ? COLOR_CODE_BG : (Clay_Color){0, 0, 0, 0});
    CLAY(HitId(select_id),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .childGap = 8,
                     .padding = {8, 8, 6, 6},
                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_GROW(0)}},
          .backgroundColor = bg,
          .cornerRadius = CLAY_CORNER_RADIUS(8)})
    {
        RenderStatusDot(row->presentation);
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            CLAY_TEXT(CStr(row->title),
                      CLAY_TEXT_CONFIG({.fontId = active ? FONT_BOLD : FONT_REGULAR,
                                        .fontSize = 13,
                                        .textColor = workspace->available ? COLOR_TEXT : COLOR_MUTED,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        if (close_id)
        {
            bool close_hover = Clay_PointerOver(HitId(close_id));
            CLAY(HitId(close_id),
                 {.layout = {.padding = {4, 4, 2, 2}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}},
                  .backgroundColor = close_hover ? COLOR_ERROR_BG : (Clay_Color){0, 0, 0, 0},
                  .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                CLAY_TEXT(CLAY_STRING("×"), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                              .fontSize = 14,
                                                              .textColor = COLOR_MUTED,
                                                              .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
        }
    }
}

static void RenderWorkspace(PicoApp *app, int index, const PicoWorkspace *workspace)
{
    int toggle_id = AddHit(HIT_TOGGLE, index, 0, NULL);
    int add_id = workspace->available ? AddHit(HIT_NEW_SESSION, index, 0, NULL) : 0;
    int unavailable_id = workspace->available ? 0 : AddHit(HIT_UNAVAILABLE, index, 0, NULL);
    bool row_hover = Clay_PointerOver(HitId(toggle_id)) || (add_id && Clay_PointerOver(HitId(add_id)));
    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = 6,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
    {
        CLAY(HitId(unavailable_id ? unavailable_id : toggle_id),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childGap = 6,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                         .sizing = {.width = CLAY_SIZING_GROW(0)}},
              .backgroundColor = row_hover ? COLOR_CODE_BG : (Clay_Color){0, 0, 0, 0},
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
        {
            CLAY_TEXT(CStr(workspace->collapsed ? "›" : "▾"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = 12,
                                        .textColor = COLOR_MUTED,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                CLAY_TEXT(CStr(workspace->name[0] ? workspace->name : workspace->path),
                          CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                            .fontSize = 13,
                                            .textColor = workspace->available ? COLOR_TEXT : COLOR_MUTED,
                                            .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
        }
        if (add_id)
        {
            bool add_hover = Clay_PointerOver(HitId(add_id));
            CLAY(HitId(add_id),
                 {.layout = {.padding = {6, 6, 2, 2}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}},
                  .backgroundColor = add_hover ? COLOR_USER_BG : (Clay_Color){0, 0, 0, 0},
                  .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                              .fontSize = 14,
                                                              .textColor = COLOR_MUTED,
                                                              .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
        }
    }
    if (workspace->collapsed || !workspace->available)
    {
        return;
    }
    SidebarRow rows[SIDEBAR_MAX_ROWS];
    int n = CollectRows(app, workspace, rows, SIDEBAR_MAX_ROWS);
    for (int i = 0; i < n; i++)
    {
        RenderSessionRow(app, workspace, index, &rows[i]);
    }
}

static void RenderField(int hit, const char *label, const char *value, bool focused)
{
    CLAY_TEXT(CStr(label), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                            .fontSize = 12,
                                            .textColor = COLOR_MUTED,
                                            .wrapMode = CLAY_TEXT_WRAP_NONE}));
    CLAY(HitId(hit),
         {.layout = {.padding = {8, 8, 6, 6}, .sizing = {.width = CLAY_SIZING_GROW(0)}},
          .backgroundColor = COLOR_COMPOSER_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        const char *shown = focused && !value[0] ? "" : value;
        CLAY_TEXT(CStr(shown[0] ? shown : " "),
                  CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                    .fontSize = 13,
                                    .textColor = value[0] ? COLOR_TEXT : COLOR_MUTED,
                                    .wrapMode = CLAY_TEXT_WRAP_NONE}));
    }
}

static void RenderModal(PicoApp *app)
{
    (void)app;
    if (!g_modal)
    {
        return;
    }
    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
    int dim = AddHit(HIT_MODAL_DIM, -1, 0, NULL);
    int path_id = AddHit(HIT_MODAL_PATH, -1, 0, NULL);
    int name_id = AddHit(HIT_MODAL_NAME, -1, 0, NULL);
    int browse = AddHit(HIT_MODAL_BROWSE, -1, 0, NULL);
    int cancel = AddHit(HIT_MODAL_CANCEL, -1, 0, NULL);
    int confirm = AddHit(HIT_MODAL_CONFIRM, -1, 0, NULL);
    snprintf(g_path_display, sizeof(g_path_display), "%s", g_path);
    snprintf(g_name_display, sizeof(g_name_display), "%s", g_name);
    CLAY(HitId(dim),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 42,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 140}})
    {
        CLAY(CLAY_ID("SidebarAddCard"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = 10,
                         .padding = {18, 18, 16, 16},
                         .sizing = {.width = CLAY_SIZING_FIXED(sw < 520 ? sw - 48 : 420)}},
              .backgroundColor = COLOR_CONTENT_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(10)})
        {
            CLAY_TEXT(CLAY_STRING("Add workspace"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                        .fontSize = 16,
                                        .textColor = COLOR_TEXT,
                                        .wrapMode = CLAY_TEXT_WRAP_NONE}));
            RenderField(path_id, "Path", g_path_display, g_focus == 0);
            RenderButton(browse, "Browse…", false);
            RenderField(name_id, "Name", g_name_display, g_focus == 1);
            CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                     .childGap = 8,
                                     .childAlignment = {.x = CLAY_ALIGN_X_RIGHT},
                                     .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                RenderButton(cancel, "Cancel", false);
                RenderButton(confirm, "Add", true);
            }
        }
    }
}

static void SidebarRender(PicoApp *app)
{
    g_hit_count = 0;
    if (!app || !app->workspaces)
    {
        return;
    }
    int add_id = AddHit(HIT_ADD_WORKSPACE, -1, 0, NULL);
    CLAY(CLAY_ID("SidebarInner"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 8,
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}})
    {
        RenderButton(add_id, "Add workspace", false);
        CLAY(CLAY_ID("SidebarScroll"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .childGap = 8,
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
              .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
        {
            int n = PicoWorkspaceRegistry_Count(app->workspaces);
            for (int i = 0; i < n; i++)
            {
                const PicoWorkspace *workspace = PicoWorkspaceRegistry_Get(app->workspaces, i);
                if (workspace)
                {
                    RenderWorkspace(app, i, workspace);
                }
            }
        }
    }
}

static bool OtherModal(PicoApp *app)
{
    return PicoExts_IsOpen() || PicoPrompt_IsOpen() || PicoFooter_MenuOpen() ||
           PicoAgentManager_TreeHasAsk(app ? app->agents : NULL, pico_agent_active(app));
}

static const SidebarHit *HitAtPointer(void)
{
    const SidebarHit *close = NULL;
    const SidebarHit *any = NULL;
    for (int i = 0; i < g_hit_count; i++)
    {
        if (!Clay_PointerOver(HitId(i + 1)))
        {
            continue;
        }
        if (g_hits[i].kind == HIT_CLOSE)
        {
            close = &g_hits[i];
        }
        else
        {
            any = &g_hits[i];
        }
    }
    return close ? close : any;
}

static void HandleHit(PicoApp *app, const SidebarHit *hit)
{
    const PicoWorkspace *workspace =
        hit->workspace_index >= 0 ? PicoWorkspaceRegistry_Get(app->workspaces, hit->workspace_index)
                                  : NULL;
    switch (hit->kind)
    {
        case HIT_ADD_WORKSPACE:
            OpenModal();
            break;
        case HIT_TOGGLE:
            if (workspace)
            {
                PicoWorkspaceRegistry_SetCollapsed(app->workspaces, workspace->key, !workspace->collapsed);
            }
            break;
        case HIT_NEW_SESSION:
            CreateWorkspaceSession(app, workspace);
            break;
        case HIT_SELECT:
            if (!pico_agent_select(app, hit->agent_id))
            {
                PicoOverlay_Notify(app, "Could not switch to that session.");
            }
            break;
        case HIT_RESUME:
            ResumeHistorical(app, workspace, hit->session_id);
            break;
        case HIT_CLOSE: {
            PicoAgentResult result = pico_agent_close(app, hit->agent_id);
            if (result != PICO_AGENT_RESULT_OK)
            {
                NotifyResult(app, result, "Could not close that session.");
            }
            break;
        }
        case HIT_UNAVAILABLE:
            PicoOverlay_Notify(app, "That workspace is unavailable until its directory is restored.");
            break;
        case HIT_MODAL_BROWSE:
            g_want_browse = true;
            break;
        case HIT_MODAL_CONFIRM:
            ConfirmAddWorkspace(app);
            break;
        case HIT_MODAL_CANCEL:
        case HIT_MODAL_DIM:
            CloseModal();
            break;
        case HIT_MODAL_PATH:
            g_focus = 0;
            break;
        case HIT_MODAL_NAME:
            g_focus = 1;
            break;
        default:
            break;
    }
}

static void SidebarAfterLayout(PicoApp *app, const PicoHookEvent *event)
{
    (void)event;
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return;
    }
    if (g_modal)
    {
        const SidebarHit *hit = HitAtPointer();
        if (hit)
        {
            HandleHit(app, hit);
        }
        else
        {
            CloseModal();
        }
        return;
    }
    if (OtherModal(app))
    {
        return;
    }
    const SidebarHit *hit = HitAtPointer();
    if (hit)
    {
        HandleHit(app, hit);
    }
}

static void SidebarOnFrame(PicoApp *app, float dt)
{
    (void)dt;
    if (app && app->workspaces && GetTime() - g_last_refresh > 0.5)
    {
        PicoWorkspaceRegistry_Refresh(app->workspaces);
        g_last_refresh = GetTime();
    }
    if (g_want_browse)
    {
        g_want_browse = false;
        if (!FolderDialogGraphic())
        {
            PicoOverlay_Notify(app, "Folder dialog unavailable. Install zenity or kdialog.");
        }
        else
        {
            const char *start = g_path[0] ? g_path : (app->workspace[0] ? app->workspace : NULL);
            char *path = tinyfd_selectFolderDialog("Workspace", start);
            if (path && path[0])
            {
                snprintf(g_path, sizeof(g_path), "%s", path);
                g_path_cursor = (int)strlen(g_path);
                FolderName(g_path, g_name, sizeof(g_name));
                g_name_cursor = (int)strlen(g_name);
                g_focus = 1;
            }
        }
    }
    if (!g_modal)
    {
        return;
    }
    if (IsKeyPressed(KEY_ESCAPE))
    {
        CloseModal();
        return;
    }
    if (IsKeyPressed(KEY_TAB))
    {
        g_focus = g_focus == 0 ? 1 : 0;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))
    {
        ConfirmAddWorkspace(app);
        return;
    }
    char *buf = g_focus == 0 ? g_path : g_name;
    int *cursor = g_focus == 0 ? &g_path_cursor : &g_name_cursor;
    size_t cap = g_focus == 0 ? sizeof(g_path) : sizeof(g_name);
    int len = (int)strlen(buf);
    if (*cursor > len)
    {
        *cursor = len;
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE))
    {
        FieldBackspace(buf, cursor);
    }
    int cp;
    while ((cp = GetCharPressed()) > 0)
    {
        if (cp < 32)
        {
            continue;
        }
        char bytes[8];
        int n = 0;
        if (cp < 0x80)
        {
            bytes[n++] = (char)cp;
        }
        else if (cp < 0x800)
        {
            bytes[n++] = (char)(0xC0 | (cp >> 6));
            bytes[n++] = (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            bytes[n++] = (char)(0xE0 | (cp >> 12));
            bytes[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            bytes[n++] = (char)(0x80 | (cp & 0x3F));
        }
        FieldInsert(buf, cap, cursor, bytes, n);
    }
}

static void SidebarInit(PicoApp *app)
{
    pico_add_view(app, PICO_SLOT_SIDEBAR, 0, SidebarRender);
    pico_add_view(app, PICO_SLOT_OVERLAY, 20, RenderModal);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, SidebarAfterLayout);
}

static void SidebarShutdown(PicoApp *app)
{
    (void)app;
    CloseModal();
    g_hit_count = 0;
}

PicoExt pico_ext_sidebar(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "sidebar",
        .description = "Workspace and session sidebar",
        .init = SidebarInit,
        .shutdown = SidebarShutdown,
        .on_frame = SidebarOnFrame,
    };
}
