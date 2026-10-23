#define _GNU_SOURCE
#include "browser.h"
#include "browser/protocol.h"
#include "host_internal.h"
#include "agent_internal.h"
#include "agent.h"
#include "clarification.h"
#include "settings.h"
#include "overlay.h"
#include "text_field_ui.h"
#include "json.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
typedef struct BrowserSession {
    struct BrowserSession *next;
    PicoAgentId agent_id;
    uint64_t token;
    char session_id[40];
    bool user_control, ready, opened, visible, focus, address_focus;
    unsigned mouse_buttons;
    bool keys[512];
    int width, height;
    char url[PB_URL_CAP], address[PB_URL_CAP], error[512];
    PicoTextField field;
    Texture2D texture;
    /* Only the receiver and main thread share these, under service->mu. */
    unsigned char *pixels;
    int pixel_width, pixel_height;
    bool changed, received_ready, received_closed;
    char received_url[PB_URL_CAP], received_error[512];
} BrowserSession;
typedef struct BrowserRequest {
    struct BrowserRequest *next;
    PicoAgentId agent_id;
    uint64_t generation;
    bool done, cancelled, error;
    int refs;
    char url[PB_URL_CAP], result[512];
} BrowserRequest;
typedef struct PicoBrowserService {
    PicoHost *host;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    BrowserSession *sessions, *retired;
    BrowserRequest *requests;
    uint64_t next_token;
    int command_fd, event_fd;
    pid_t pid;
    pthread_t receiver;
    bool thread_started, disconnected;
    char error[512];
    PbCommand queue[256];
    unsigned queue_head, queue_count;
    int modifiers;
} PicoBrowserService;

static void Focus(PicoHost *host, BrowserSession *b, bool focus);
static int WindowsKey(int key);

static BrowserSession *Session(PicoHost *host, PicoAgentId id)
{
    PicoBrowserService *s = host ? host->browser : NULL;
    for (BrowserSession *b = s ? s->sessions : NULL; b; b = b->next)
        if (b->agent_id == id) return b;
    return NULL;
}
static BrowserSession *Selected(PicoHost *host)
{
    PicoAgent *a = PicoHost_SelectedAgent(host);
    return PicoAgent_IsUserMain(a) ? Session(host, a->id) : NULL;
}
void PicoBrowser_Init(PicoHost *host)
{
    if (!host || host->browser) return;
    PicoBrowserService *s = calloc(1, sizeof(*s));
    if (!s) return;
    s->host = host; s->command_fd = s->event_fd = -1;
    pthread_mutex_init(&s->mu, NULL); pthread_cond_init(&s->cv, NULL);
    host->browser = s;
}
static bool ReadAll(int fd, void *data, size_t n)
{
    char *p = data;
    while (n) {
        ssize_t got = recv(fd, p, n, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        p += got; n -= (size_t)got;
    }
    return true;
}
static void *Receive(void *arg)
{
    PicoBrowserService *s = arg;
    PbEvent event;
    while (ReadAll(s->event_fd, &event, sizeof(event))) {
        bool frame = event.type == PB_FRAME;
        if (frame ? (!event.width || !event.height || event.width > PB_MAX_DIMENSION ||
                     event.height > PB_MAX_DIMENSION || event.length != event.width * event.height * 4)
                  : event.length >= PB_URL_CAP) break;
        unsigned char *data = malloc((size_t)event.length + 1);
        if (!data || !ReadAll(s->event_fd, data, event.length)) { free(data); break; }
        data[event.length] = 0;
        pthread_mutex_lock(&s->mu);
        BrowserSession *b = s->sessions;
        while (b && b->token != event.token) b = b->next;
        if (!event.token && event.type == PB_ERROR)
            snprintf(s->error, sizeof(s->error), "%s", data);
        if (b) {
            if (frame) {
                free(b->pixels); b->pixels = data; data = NULL;
                b->pixel_width = event.width; b->pixel_height = event.height;
            } else if (event.type == PB_READY) b->received_ready = true;
            else if (event.type == PB_CLOSED) b->received_closed = true;
            else if (event.type == PB_URL) snprintf(b->received_url, sizeof(b->received_url), "%s", data);
            else if (event.type == PB_ERROR) snprintf(b->received_error, sizeof(b->received_error), "%s", data);
            b->changed = true;
        }
        pthread_mutex_unlock(&s->mu);
        free(data);
        pico_host_wakeup(s->host);
    }
    pthread_mutex_lock(&s->mu); s->disconnected = true;
    pthread_mutex_unlock(&s->mu);
    pico_host_wakeup(s->host);
    return NULL;
}
static void Flush(PicoBrowserService *s)
{
    while (s->queue_count && s->command_fd >= 0) {
        PbCommand *c = &s->queue[s->queue_head];
        ssize_t n = send(s->command_fd, c, sizeof(*c), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n != sizeof(*c)) break;
        s->queue_head = (s->queue_head + 1) % 256; s->queue_count--;
    }
}
static bool Send(PicoBrowserService *s, PbCommand command)
{
    if (!s || s->command_fd < 0) return false;
    Flush(s);
    if (s->queue_count == 256) {
        /* Fail closed rather than silently lose a key-up or browser close. */
        pthread_mutex_lock(&s->mu);
        snprintf(s->error, sizeof(s->error), "Browser stopped: command queue exhausted.");
        pthread_mutex_unlock(&s->mu);
        if (s->pid > 0) kill(-s->pid, SIGTERM);
        return false;
    }
    s->queue[(s->queue_head + s->queue_count) % 256] = command;
    s->queue_count++;
    Flush(s);
    return true;
}
static bool MakeDirs(const char *path)
{
    char buf[4096];
    if (!path || strlen(path) >= sizeof(buf)) return false;
    strcpy(buf, path);
    for (char *p = buf + 1;; ++p) {
        if (*p && *p != '/') continue;
        char saved = *p; *p = 0;
        if (mkdir(buf, 0700) && errno != EEXIST) return false;
        *p = saved;
        if (!saved) return true;
    }
}
static bool Start(PicoBrowserService *s)
{
    if (s->pid > 0) {
        pthread_mutex_lock(&s->mu);
        bool connected = !s->disconnected && s->command_fd >= 0;
        pthread_mutex_unlock(&s->mu);
        return connected;
    }
    char helper[4096], root[4096], config[4096];
    if (!IsWindowReady()) { snprintf(s->error, sizeof(s->error), "Browser requires Pico's graphics window."); return false; }
    if (!Pico_ConfigDir(config, sizeof(config)) ||
        snprintf(root, sizeof(root), "%s/browser", config) >= sizeof(root) || !MakeDirs(root) ||
        snprintf(helper, sizeof(helper), "%sbrowser/runtime/pico_browser_helper", GetApplicationDirectory()) >= sizeof(helper)) {
        snprintf(s->error, sizeof(s->error), "Cannot prepare browser storage."); return false;
    }
    if (access(helper, X_OK) != 0)
        snprintf(helper, sizeof(helper), "%s../libexec/pico/browser/pico_browser_helper", GetApplicationDirectory());
#ifdef PICO_BROWSER_HELPER
    if (access(helper, X_OK) != 0) snprintf(helper, sizeof(helper), "%s", PICO_BROWSER_HELPER);
#endif
    int commands[2] = {-1,-1}, events[2] = {-1,-1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, commands) ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, events)) goto fail;
    /* Relocate sources above the fixed child descriptors before dup2. Even a
     * host with hundreds of open fds must not overwrite the second source. */
    int command_source = fcntl(commands[1], F_DUPFD_CLOEXEC, 200);
    if (command_source < 0) goto fail;
    close(commands[1]); commands[1] = command_source;
    int event_source = fcntl(events[1], F_DUPFD_CLOEXEC, 200);
    if (event_source < 0) goto fail;
    close(events[1]); events[1] = event_source;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, commands[1], 198);
    posix_spawn_file_actions_adddup2(&actions, events[1], 199);
    char *argv[] = {helper, "198", "199", root, NULL};
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    int rc = posix_spawn(&s->pid, helper, &actions, &attributes, argv, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (rc) { errno = rc; s->pid = 0; goto fail; }
    close(commands[1]); close(events[1]);
    s->command_fd = commands[0]; s->event_fd = events[0];
    pthread_mutex_lock(&s->mu);
    s->disconnected = false; s->error[0] = 0;
    pthread_mutex_unlock(&s->mu);
    s->queue_count = s->queue_head = 0;
    if (pthread_create(&s->receiver, NULL, Receive, s)) {
        kill(-s->pid, SIGTERM); waitpid(s->pid, NULL, 0); s->pid = 0;
        close(s->command_fd); close(s->event_fd); s->command_fd = s->event_fd = -1;
        snprintf(s->error, sizeof(s->error), "Cannot start browser receiver."); return false;
    }
    s->thread_started = true;
    return true;
fail:
    snprintf(s->error, sizeof(s->error), "Cannot launch browser helper: %s", strerror(errno));
    for (int i = 0; i < 2; ++i) { if (commands[i] >= 0) close(commands[i]); if (events[i] >= 0) close(events[i]); }
    return false;
}
static bool ValidUrl(const char *url)
{
    return url && strlen(url) < PB_URL_CAP &&
        (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8) || !strcmp(url, "about:blank"));
}
static bool Open(PicoHost *host, PicoAgent *agent, const char *url, bool user, char *error, size_t cap)
{
    if (!PicoAgent_IsUserMain(agent)) { snprintf(error, cap, "Browser access is limited to main sessions."); return false; }
    if (!agent->session_id[0]) { snprintf(error, cap, "Session has no persistent identity."); return false; }
    if (!ValidUrl(url)) { snprintf(error, cap, "Use an http:// or https:// URL (or about:blank)."); return false; }
    PicoBrowser_Init(host);
    PicoBrowserService *s = host->browser;
    if (!s) { snprintf(error, cap, "Out of memory."); return false; }
    BrowserSession *b = Session(host, agent->id);
    if (b && !user && b->user_control) { snprintf(error, cap, "Browser is under User control. Ask the user to select Agent control."); return false; }
    if (!Start(s)) { snprintf(error, cap, "%s", s->error); return false; }
    if (!b) {
        b = calloc(1, sizeof(*b));
        if (!b) { snprintf(error, cap, "Out of memory."); return false; }
        b->agent_id = agent->id; b->token = ++s->next_token;
        b->user_control = user;
        snprintf(b->session_id, sizeof(b->session_id), "%s", agent->session_id);
        PicoTextField_Bind(&b->field, b->address, sizeof(b->address));
        pthread_mutex_lock(&s->mu); b->next = s->sessions; s->sessions = b; pthread_mutex_unlock(&s->mu);
    }
    PbCommand command = {.type = PB_OPEN, .token = b->token};
    snprintf(command.text, sizeof(command.text), "%s", url);
    snprintf(command.profile, sizeof(command.profile), "%s", agent->session_id);
    if (!Send(s, command)) { snprintf(error, cap, "Browser command queue unavailable; retry."); return false; }
    if (!b->opened) {
        b->opened = true; b->visible = false; b->width = b->height = 0;
        b->error[0] = 0;
    }
    if (user) agent->browser_expanded = true;
    PicoTextField_SetText(&b->field, url);
    pico_host_request_redraw(host);
    return true;
}
void PicoBrowser_CloseAgent(PicoHost *host, PicoAgentId id)
{
    PicoBrowserService *s = host ? host->browser : NULL;
    if (!s) return;
    pthread_mutex_lock(&s->mu);
    BrowserSession **link = &s->sessions;
    while (*link && (*link)->agent_id != id) link = &(*link)->next;
    BrowserSession *b = *link;
    if (b) *link = b->next;
    pthread_mutex_unlock(&s->mu);
    if (!b) return;
    Send(s, (PbCommand){.type = PB_CLOSE, .token = b->token});
    free(b->pixels); b->pixels = NULL;
    /* Clay commands borrow text and &texture. Agent close is legal even in an
     * AFTER_LAYOUT hook: retire, never free resources behind pending commands. */
    b->next = s->retired; s->retired = b;
    pico_host_request_redraw(host);
}
void PicoBrowser_BeginLayout(PicoHost *host)
{
    PicoBrowserService *s = host ? host->browser : NULL;
    while (s && s->retired) {
        BrowserSession *b = s->retired; s->retired = b->next;
        if (b->texture.id) UnloadTexture(b->texture);
        PicoTextField_Unbind(&b->field); free(b);
    }
}
static void ReleaseRequest(BrowserRequest *r) { if (!--r->refs) free(r); }
char *PicoBrowser_ToolOpen(PicoAgentContext *ctx, const char *url, bool *error)
{
    *error = true;
    PicoWorkspace *ws = PicoAgentContext_Workspace(ctx);
    PicoBrowserService *s = ws && ws->host ? ws->host->browser : NULL;
    if (!s || !ValidUrl(url)) return strdup("browser_open: provide an http:// or https:// URL.");
    BrowserRequest *r = calloc(1, sizeof(*r));
    if (!r) return strdup("browser_open: out of memory.");
    r->refs = 2; r->agent_id = pico_agent_context_id(ctx);
    r->generation = pico_agent_context_generation(ctx);
    snprintf(r->url, sizeof(r->url), "%s", url);
    pthread_mutex_lock(&s->mu);
    BrowserRequest **tail = &s->requests; while (*tail) tail = &(*tail)->next; *tail = r;
    pico_host_wakeup(s->host);
    while (!r->done && !pico_agent_context_cancelled(ctx)) {
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 50000000;
        if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&s->cv, &s->mu, &until);
    }
    char *result;
    if (!r->done) { r->cancelled = true; result = strdup("browser_open cancelled."); }
    else { *error = r->error; result = strdup(r->result); }
    ReleaseRequest(r); pthread_mutex_unlock(&s->mu);
    return result;
}
void PicoBrowser_Pump(PicoHost *host)
{
    PicoBrowserService *s = host ? host->browser : NULL;
    if (!s) return;
    Flush(s);
    for (;;) {
        pthread_mutex_lock(&s->mu);
        BrowserRequest *r = s->requests;
        if (!r) { pthread_mutex_unlock(&s->mu); break; }
        s->requests = r->next;
        bool cancelled = r->cancelled;
        pthread_mutex_unlock(&s->mu);
        char result[512] = "browser_open cancelled."; bool error = true;
        PicoAgent *a = PicoHost_FindAgent(host, r->agent_id);
        if (!cancelled && a && a->runtime_generation == r->generation) {
            error = !Open(host, a, r->url, false, result, sizeof(result));
            if (!error) snprintf(result, sizeof(result), "Browser navigation requested for this main session. Page load is asynchronous. Screenshot and agent input tools are not available yet.");
        }
        pthread_mutex_lock(&s->mu);
        r->done = true; r->error = error; snprintf(r->result, sizeof(r->result), "%s", result);
        pthread_cond_broadcast(&s->cv); ReleaseRequest(r);
        pthread_mutex_unlock(&s->mu);
    }
    pthread_mutex_lock(&s->mu);
    bool disconnected = s->disconnected;
    pthread_mutex_unlock(&s->mu);
    if (disconnected && s->thread_started) {
        pthread_join(s->receiver, NULL); s->thread_started = false;
        close(s->command_fd); close(s->event_fd); s->command_fd = s->event_fd = -1;
        s->queue_count = 0;
        for (BrowserSession *b = s->sessions; b; b = b->next) {
            b->opened = b->ready = b->visible = b->focus = b->address_focus = false;
            b->mouse_buttons = 0;
            b->width = b->height = 0;
            if (b->texture.id) { UnloadTexture(b->texture); b->texture = (Texture2D){0}; }
        }
        pico_host_request_redraw(host);
    }
    if (s->pid > 0 && !s->thread_started && waitpid(s->pid, NULL, WNOHANG) == s->pid) s->pid = 0;
    for (BrowserSession *b = s->sessions, *next; b; b = next) {
        next = b->next;
        PicoAgent *a = PicoHost_FindAgent(host, b->agent_id);
        if (!a || strcmp(a->session_id, b->session_id)) { PicoBrowser_CloseAgent(host, b->agent_id); continue; }
        bool visible = host->selected_agent_id == a->id && a->browser_expanded && !host->window_minimized;
        if (b->opened && visible != b->visible) {
            if (Send(s, (PbCommand){.type = PB_VISIBLE, .token = b->token, .a = visible})) b->visible = visible;
            if (!visible) { Focus(host, b, false); b->address_focus = false; }
        }
        pthread_mutex_lock(&s->mu);
        bool changed = b->changed; b->changed = false;
        unsigned char *pixels = b->pixels; b->pixels = NULL;
        int w = b->pixel_width, h = b->pixel_height;
        if (b->received_ready) { b->ready = true; b->received_ready = false; }
        if (b->received_url[0]) { snprintf(b->url, sizeof(b->url), "%s", b->received_url); b->received_url[0] = 0; }
        if (b->received_error[0]) { snprintf(b->error, sizeof(b->error), "%s", b->received_error); b->received_error[0] = 0; }
        if (changed && !b->address_focus && b->url[0]) PicoTextField_SetText(&b->field, b->url);
        bool closed = b->received_closed;
        if (disconnected && !b->error[0]) snprintf(b->error, sizeof(b->error), "%s", s->error[0] ? s->error : "Browser helper exited. Close and reopen the browser to retry; see browser/cef.log.");
        pthread_mutex_unlock(&s->mu);
        if (closed) { PicoBrowser_CloseAgent(host, b->agent_id); pico_host_request_redraw(host); free(pixels); continue; }
        if (pixels && visible && IsWindowReady()) {
            // CEF BGRA -> Raylib RGBA. Only this thread owns graphics resources.
            for (size_t i = 0; i < (size_t)w * h * 4; i += 4) { unsigned char c = pixels[i]; pixels[i] = pixels[i+2]; pixels[i+2] = c; }
            if (b->texture.id && (b->texture.width != w || b->texture.height != h)) { UnloadTexture(b->texture); b->texture = (Texture2D){0}; }
            if (!b->texture.id) b->texture = LoadTextureFromImage((Image){.data = pixels, .width = w, .height = h, .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8});
            else UpdateTexture(b->texture, pixels);
            pico_host_request_redraw(host);
        }
        if (!visible && b->texture.id) { UnloadTexture(b->texture); b->texture = (Texture2D){0}; }
        free(pixels);
        if (changed && visible) pico_host_request_redraw(host);
    }
}
void PicoBrowser_Shutdown(PicoHost *host, const struct timespec *deadline)
{
    PicoBrowserService *s = host ? host->browser : NULL;
    if (!s) return;
    while (s->sessions) PicoBrowser_CloseAgent(host, s->sessions->agent_id);
    PicoBrowser_BeginLayout(host);
    Send(s, (PbCommand){.type = PB_STOP});
    // Preserve graceful CEF storage flushing, with a bounded process shutdown.
    while (s->pid > 0) {
        Flush(s);
        if (waitpid(s->pid, NULL, WNOHANG) == s->pid) { s->pid = 0; break; }
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (!deadline || now.tv_sec > deadline->tv_sec ||
            (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) break;
        usleep(1000);
    }
    if (s->pid > 0) { kill(-s->pid, SIGKILL); waitpid(s->pid, NULL, 0); }
    if (s->command_fd >= 0) shutdown(s->command_fd, SHUT_RDWR);
    if (s->event_fd >= 0) shutdown(s->event_fd, SHUT_RDWR);
    if (s->thread_started) pthread_join(s->receiver, NULL);
    if (s->command_fd >= 0) close(s->command_fd);
    if (s->event_fd >= 0) close(s->event_fd);
    while (s->requests) { BrowserRequest *r = s->requests; s->requests = r->next; ReleaseRequest(r); }
    pthread_mutex_destroy(&s->mu); pthread_cond_destroy(&s->cv);
    free(s); host->browser = NULL;
}

float PicoBrowser_ShellWidth(const PicoHost *host)
{
    const PicoAgent *a = PicoHost_SelectedAgentConst(host);
    if (!PicoAgent_IsUserMain(a)) return 0;
    float width = Clay_GetLayoutDimensions().width;
    if (!a->browser_expanded) return 28;
    float pane = width * 0.44f;
    if (pane > 720) pane = 720;
    if (pane > width - 300) pane = width - 300;
    return pane > 28 ? pane : 28;
}
static void Label(const char *text)
{
    CLAY_TEXT(((Clay_String){.length = (int)strlen(text), .chars = text}),
        CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI,
                          .textColor = COLOR_TEXT, .wrapMode = CLAY_TEXT_WRAP_NONE}));
}
static void Button(const char *id, const char *text, float width)
{
    CLAY(Clay_GetElementId((Clay_String){.length = (int)strlen(id), .chars = id}),
        {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(width), .height = CLAY_SIZING_FIXED(30)},
                    .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
         .backgroundColor = COLOR_CONTENT_BG, .cornerRadius = CLAY_CORNER_RADIUS(4)}) { Label(text); }
}
void PicoBrowser_Layout(PicoHost *host)
{
    PicoAgent *a = PicoHost_SelectedAgent(host);
    if (!PicoAgent_IsUserMain(a)) return;
    BrowserSession *b = Selected(host);
    CLAY(CLAY_ID("BrowserPane"),
        {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .sizing = {.width = CLAY_SIZING_FIXED(PicoBrowser_ShellWidth(host)),
                               .height = CLAY_SIZING_PERCENT(1)}, .childGap = 4},
         .backgroundColor = COLOR_CONTENT_BG, .cornerRadius = CLAY_CORNER_RADIUS(6)}) {
        if (!a->browser_expanded) { Button("BrowserExpand", "<", 28); }
        else {
            CLAY(CLAY_ID("BrowserToolbar"),
                {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(34)},
                            .childGap = 4, .padding = {4,4,2,2}}}) {
                Button("BrowserBack", "<", 28); Button("BrowserForward", ">", 28);
                Button("BrowserReload", "R", 28);
                Button("BrowserControl", b && b->user_control ? "User" : "Agent", 70);
                CLAY(CLAY_ID("BrowserToolbarSpace"), {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(30)}}}) {}
                Button("BrowserClose", "x", 28); Button("BrowserCollapse", ">", 28);
            }
            if (b) {
                CLAY(CLAY_ID("BrowserAddress"),
                    {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(32)},
                                .padding = {8,8,5,5}},
                     .clip = {.horizontal = true, .childOffset = {.x = -b->field.scroll_x}},
                     .backgroundColor = COLOR_BG}) { Label(b->address); }
                if (b->error[0]) {
                    CLAY(CLAY_ID("BrowserError"), {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(26)}},
                         .clip = {.horizontal = true}}) { Label(b->error); }
                }
                CLAY(CLAY_ID("BrowserSurface"),
                    {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                     /* This Clay version emits the background after the image.
                      * Leave image-backed surfaces transparent, or an opaque
                      * rectangle covers every successfully rendered page. */
                     .backgroundColor = b->texture.id ? (Clay_Color){0} : COLOR_BG,
                     .image = {.imageData = b->texture.id ? &b->texture : NULL}}) {
                    if (!b->texture.id) Label(b->ready ? "Waiting for page paint..." : "Starting browser...");
                }
            } else { Button("BrowserStart", "Open browser", 150); }
        }
    }
}
static bool Over(const char *id)
{
    Clay_ElementData e = Clay_GetElementData(Clay_GetElementId((Clay_String){.length = (int)strlen(id), .chars = id}));
    Vector2 p = GetMousePosition();
    return e.found && p.x >= e.boundingBox.x && p.y >= e.boundingBox.y &&
        p.x < e.boundingBox.x + e.boundingBox.width && p.y < e.boundingBox.y + e.boundingBox.height;
}
static int Modifiers(PicoHost *host)
{
    return (host->browser ? host->browser->modifiers : 0) |
           (IsMouseButtonDown(MOUSE_BUTTON_LEFT) ? 16 : 0) |
           (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ? 32 : 0) |
           (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ? 64 : 0);
}
static void Focus(PicoHost *host, BrowserSession *b, bool focus)
{
    if (!focus) {
        for (int i = 0; i < 3; ++i)
            if (b->mouse_buttons & (1u << i)) Send(host->browser, (PbCommand){.type=PB_MOUSE, .token=b->token, .a=i, .b=1});
        b->mouse_buttons = 0;
        for (int key = 0; key < 512; ++key) {
            if (b->keys[key]) Send(host->browser, (PbCommand){.type=PB_KEY, .token=b->token, .a=WindowsKey(key), .b=1});
            b->keys[key] = false;
        }
    }
    if (b->focus == focus) return;
    if (Send(host->browser, (PbCommand){.type = PB_FOCUS, .token = b->token, .a = focus})) b->focus = focus;
}
bool PicoBrowser_BlocksKeyboard(const PicoHost *host)
{
    BrowserSession *b = Selected((PicoHost *)host);
    const PicoAgent *a = PicoHost_SelectedAgentConst(host);
    return b && a->browser_expanded && b->user_control && (b->focus || b->address_focus);
}
void PicoBrowser_Input(PicoHost *host)
{
    PicoAgent *a = PicoHost_SelectedAgent(host);
    if (!PicoAgent_IsUserMain(a)) return;
    BrowserSession *b = Selected(host);
    if (PicoUi_ModalOpen(host) || !host->window_focused) {
        if (b) { Focus(host, b, false); b->address_focus = false; }
        return;
    }
    bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (click && (Over("BrowserExpand") || Over("BrowserStart"))) {
        if (!a->session_id[0]) {
            PicoOverlay_Notify(host, "Send a message to create this session before opening its browser.");
            return;
        }
        a->browser_expanded = true;
        if (!b) {
            char error[512];
            if (!Open(host, a, "about:blank", true, error, sizeof(error))) pico_status_warn(host, error);
            b = Selected(host);
            if (b) { b->address_focus = true; PicoTextField_SelectAll(&b->field); }
        }
        pico_host_request_redraw(host);
        return;
    }
    if (click && Over("BrowserCollapse")) {
        a->browser_expanded = false;
        if (b) { Focus(host, b, false); b->address_focus = false; }
        pico_host_request_redraw(host);
    }
    if (click && Over("BrowserClose")) {
        PicoBrowser_CloseAgent(host, a->id); pico_host_request_redraw(host); return;
    }
    if (!b || !a->browser_expanded) return;
    if (click && Over("BrowserControl")) {
        b->user_control = !b->user_control; b->address_focus = false; Focus(host, b, false);
        pico_host_request_redraw(host);
    }
    if (!b->user_control) return;
    if (click) {
        if (Over("BrowserBack")) Send(host->browser, (PbCommand){.type=PB_BACK, .token=b->token});
        if (Over("BrowserForward")) Send(host->browser, (PbCommand){.type=PB_FORWARD, .token=b->token});
        if (Over("BrowserReload")) Send(host->browser, (PbCommand){.type=PB_RELOAD, .token=b->token});
        b->address_focus = Over("BrowserAddress");
        Focus(host, b, Over("BrowserSurface") || b->mouse_buttons);
    }
    if (b->address_focus) {
        PicoTextField_HandlePointer(&b->field, CLAY_ID("BrowserAddress"), 8, FONT_REGULAR);
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            char error[512];
            if (!Open(host, a, b->address, true, error, sizeof(error))) snprintf(b->error, sizeof(b->error), "%s", error);
            else b->error[0] = 0;
        } else PicoTextField_HandleKeys(&b->field);
        Clay_ElementData e = Clay_GetElementData(CLAY_ID("BrowserAddress"));
        if (e.found) PicoTextField_KeepCaretVisible(&b->field, e.boundingBox.width - 16, FONT_REGULAR);
        pico_host_request_redraw_after(host, PicoTextField_NextBlink(&b->field));
    }
    bool over = Over("BrowserSurface");
    if (!over && !b->mouse_buttons) return;
    Clay_ElementData e = Clay_GetElementData(CLAY_ID("BrowserSurface"));
    if (!e.found || e.boundingBox.width <= 0 || e.boundingBox.height <= 0) return;
    Vector2 p = GetMousePosition();
    PbCommand c = {.type = PB_MOUSE, .token = b->token, .a = -1,
        .x = (int)((p.x - e.boundingBox.x) * b->width / e.boundingBox.width),
        .y = (int)((p.y - e.boundingBox.y) * b->height / e.boundingBox.height), .modifiers = Modifiers(host)};
    if (GetMouseDelta().x != 0 || GetMouseDelta().y != 0) Send(host->browser, c);
    const int buttons[] = {MOUSE_BUTTON_LEFT, MOUSE_BUTTON_MIDDLE, MOUSE_BUTTON_RIGHT};
    for (int i = 0; i < 3; ++i) {
        if (IsMouseButtonPressed(buttons[i]) || IsMouseButtonReleased(buttons[i])) {
            bool released = IsMouseButtonReleased(buttons[i]);
            if (released && !(b->mouse_buttons & (1u << i))) continue;
            if (!released && !over) continue;
            if (released) b->mouse_buttons &= ~(1u << i); else b->mouse_buttons |= 1u << i;
            c.a = i; c.b = released; Send(host->browser, c);
        }
    }
    Vector2 wheel = GetMouseWheelMoveV();
    if (wheel.x != 0 || wheel.y != 0) { c.type=PB_WHEEL; c.a=(int)(wheel.x*100); c.b=(int)(wheel.y*100); Send(host->browser, c); }
}
void PicoBrowser_AfterLayout(PicoHost *host)
{
    BrowserSession *b = Selected(host);
    if (!b || !b->opened) return;
    Clay_ElementData e = Clay_GetElementData(CLAY_ID("BrowserSurface"));
    if (!e.found) return;
    int w = (int)e.boundingBox.width, h = (int)e.boundingBox.height;
    if (w < 1 || h < 1) return;
    if (w > PB_MAX_DIMENSION) w = PB_MAX_DIMENSION;
    if (h > PB_MAX_DIMENSION) h = PB_MAX_DIMENSION;
    if ((w != b->width || h != b->height) &&
        Send(host->browser, (PbCommand){.type=PB_RESIZE, .token=b->token, .x=w, .y=h})) { b->width=w; b->height=h; }
}
void PicoBrowser_AfterRender(PicoHost *host)
{
    BrowserSession *b = Selected(host);
    if (b && b->address_focus) PicoTextField_Draw(&b->field, CLAY_ID("BrowserAddress"), 8, 5, FONT_REGULAR);
}
static int WindowsKey(int key)
{
    if ((key >= KEY_A && key <= KEY_Z) || (key >= KEY_ZERO && key <= KEY_NINE)) return key;
    if (key >= KEY_F1 && key <= KEY_F12) return 0x70 + key - KEY_F1;
    switch (key) {
    case KEY_SPACE: return 32; case KEY_ESCAPE: return 27; case KEY_ENTER: case KEY_KP_ENTER: return 13;
    case KEY_TAB: return 9; case KEY_BACKSPACE: return 8; case KEY_INSERT: return 45;
    case KEY_DELETE: return 46; case KEY_RIGHT: return 39; case KEY_LEFT: return 37;
    case KEY_DOWN: return 40; case KEY_UP: return 38; case KEY_HOME: return 36; case KEY_END: return 35;
    case KEY_PAGE_UP: return 33; case KEY_PAGE_DOWN: return 34;
    case KEY_LEFT_SHIFT: case KEY_RIGHT_SHIFT: return 16;
    case KEY_LEFT_CONTROL: case KEY_RIGHT_CONTROL: return 17;
    case KEY_LEFT_ALT: case KEY_RIGHT_ALT: return 18;
    case KEY_SEMICOLON: return 186; case KEY_EQUAL: return 187; case KEY_COMMA: return 188;
    case KEY_MINUS: return 189; case KEY_PERIOD: return 190; case KEY_SLASH: return 191;
    case KEY_GRAVE: return 192; case KEY_LEFT_BRACKET: return 219; case KEY_BACKSLASH: return 220;
    case KEY_RIGHT_BRACKET: return 221; case KEY_APOSTROPHE: return 222;
    default: return 0;
    }
}
bool PicoBrowser_Key(PicoHost *host, int key, int scan, int action, int mods)
{
    int flags = ((mods & 1) ? 2 : 0) | ((mods & 2) ? 4 : 0) | ((mods & 4) ? 8 : 0);
    if (host && host->browser) host->browser->modifiers = flags;
    BrowserSession *b = Selected(host);
    if (!b || !b->focus || !PicoBrowser_BlocksKeyboard(host) || PicoUi_ModalOpen(host)) return false;
    if (key >= 0 && key < 512) b->keys[key] = action != 0;
    int vk = WindowsKey(key);
    if (vk) Send(host->browser, (PbCommand){.type=PB_KEY, .token=b->token, .a=vk, .b=action==0, .x=scan, .modifiers=flags});
    // Enter/Tab have no GLFW character callback, but editable HTML needs CHAR.
    if (action != 0 && (vk == 13 || vk == 9)) {
        PbCommand c = {.type=PB_TEXT, .token=b->token, .modifiers=flags}; c.text[0] = (char)vk; Send(host->browser,c);
    }
    return true;
}
bool PicoBrowser_Char(PicoHost *host, unsigned int codepoint)
{
    BrowserSession *b = Selected(host);
    if (!b || !b->focus || !PicoBrowser_BlocksKeyboard(host) || PicoUi_ModalOpen(host)) return false;
    int length = 0; const char *text = CodepointToUTF8((int)codepoint, &length);
    PbCommand c = {.type=PB_TEXT, .token=b->token};
    if (length > 0 && length < (int)sizeof(c.text)) { memcpy(c.text, text, length); Send(host->browser,c); }
    return true;
}
