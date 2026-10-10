#define _GNU_SOURCE

#include "clarification.h"
#include "pico/plugin.h"
#include "canonical.h"
#include "render_scale.h"
#include "complete_internal.h"
#include "composer_internal.h"
#include "json.h"
#include "path.h"
#include "settings.h"
#include "text_range.h"
#include "spell_internal.h"
#include "scrollbar.h"
#include "host_internal.h"
#include "agent.h"

#include "clay/clay.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__linux__)
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <spawn.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif
#include <unistd.h>

#define PASTE_TEMP_THRESHOLD 4096
#define COMPOSER_PAD_X 14
#define COMPOSER_PAD_Y 10
#define COMPOSER_FONT_SIZE PICO_FONT_BODY
#define COMPOSER_MAX_LINES 256
#define COMPOSER_MIN_HEIGHT 60
#define COMPOSER_MAX_GROW_LINES 10
#define COMPOSER_MAX_ATTACH 32
#define ATTACH_THUMB 56
#define ATTACH_REMOVE 18
#define ATTACH_GAP 8.0f
#define CLIP_IMAGE_MAX (32 * 1024 * 1024)
#define CLIP_PROCESS_TIMEOUT_SECONDS 1.5
#define CARET_BLINK_HZ 2.0

static Font ComposerFont(void)
{
    return Pico_FontAt(FONT_REGULAR, COMPOSER_FONT_SIZE);
}

static float ComposerPx(void)
{
    return Pico_FontPx(COMPOSER_FONT_SIZE);
}

typedef struct CompLine {
    int start;
    int length;
} CompLine;

static bool IsCtrlDown(void)
{
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

static bool IsShiftDown(void)
{
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}





static int LineStart(const char *s, int pos)
{
    while (pos > 0 && s[pos - 1] != '\n')
    {
        pos--;
    }
    return pos;
}

static int LineEnd(const char *s, int length, int pos)
{
    while (pos < length && s[pos] != '\n')
    {
        pos++;
    }
    return pos;
}

static float MeasureSlice(Font font, const char *s, int start, int length, float font_size)
{
    if (length <= 0)
    {
        return 0;
    }
    char saved = ((char *)s)[start + length];
    ((char *)s)[start + length] = '\0';
    Vector2 size = MeasureTextEx(font, s + start, font_size, 0);
    ((char *)s)[start + length] = saved;
    return size.x;
}

static int WrapComposer(const PicoComposer *c, Font font, float max_width, CompLine *lines, int max_lines,
                        float *line_height)
{
    Vector2 sample = MeasureTextEx(font, "Hg", ComposerPx(), 0);
    *line_height = sample.y > 1 ? sample.y : ComposerPx();
    if (!c->text || c->length == 0)
    {
        lines[0].start = 0;
        lines[0].length = 0;
        return 1;
    }

    int line_count = 0;
    int i = 0;
    while (i < c->length && line_count < max_lines)
    {
        int line_start = i;
        if (c->text[i] == '\n')
        {
            lines[line_count].start = line_start;
            lines[line_count].length = 0;
            line_count++;
            i++;
            continue;
        }

        float width = 0;
        int break_at = -1;
        int break_resume = -1;
        int wrapped = 0;
        while (i < c->length && c->text[i] != '\n')
        {
            int next = PicoText_Utf8Next(c->text, c->length, i);
            float ch_w = MeasureSlice(font, c->text, i, next - i, ComposerPx());
            if (width + ch_w > max_width && i > line_start)
            {
                if (break_at > line_start)
                {
                    lines[line_count].start = line_start;
                    lines[line_count].length = break_at - line_start;
                    line_count++;
                    i = break_resume;
                }
                else
                {
                    lines[line_count].start = line_start;
                    lines[line_count].length = i - line_start;
                    line_count++;
                }
                wrapped = 1;
                break;
            }
            width += ch_w;
            if (c->text[i] == ' ' || c->text[i] == '\t')
            {
                break_at = i;
                break_resume = next;
            }
            i = next;
        }
        if (!wrapped)
        {
            lines[line_count].start = line_start;
            lines[line_count].length = i - line_start;
            line_count++;
            if (i < c->length && c->text[i] == '\n')
            {
                i++;
            }
        }
    }
    if (c->length > 0 && c->text[c->length - 1] == '\n' && line_count < max_lines)
    {
        lines[line_count].start = c->length;
        lines[line_count].length = 0;
        line_count++;
    }
    if (line_count == 0)
    {
        lines[0].start = 0;
        lines[0].length = c->length;
        return 1;
    }
    return line_count;
}

typedef struct ComposerView {
    CompLine lines[COMPOSER_MAX_LINES];
    int line_count;
    float line_height;
    float wrap_width;
    float origin_x;
    float origin_y;
    float scroll_y;
    Clay_BoundingBox clip;
    bool found;
} ComposerView;

typedef struct ComposerAttach {
    char path[4096];
    Texture2D thumb;
    bool loaded;
    bool owned;
} ComposerAttach;

#if defined(__linux__)
typedef struct ClipboardPaste {
    atomic_bool cancelled;
    bool wayland;
    bool fallback;
    bool failed;
    unsigned char *bytes;
    size_t length;
    const char *image_ext;
    double deadline;
} ClipboardPaste;
#endif

typedef struct ComposerWrapCache {
    uint64_t text_revision;
    uint64_t font_generation;
    float font_scale;
    unsigned int font_texture_id;
    int text_length;
    float wrap_width;
    float line_height;
    CompLine lines[COMPOSER_MAX_LINES];
    int line_count;
    bool valid;
} ComposerWrapCache;

typedef struct ComposerState {
    float wrap_width;
    float composer_width;
    int seen_cursor;
    int seen_length;
    float goal_x;
    ComposerWrapCache wrap_cache;
    double caret_blink_at;
    PicoHost *app;
    ComposerAttach attach[COMPOSER_MAX_ATTACH];
    int attach_n;
    int preview;
    Texture2D preview_tex;
    Image preview_src;
    bool preview_loaded;
#if defined(__linux__)
    ClipboardPaste *clipboard_paste; /* Borrowed until core task completion. */
#endif
} ComposerState;

static __thread ComposerState *s_active_composer_state = NULL;

static ComposerState *ActiveComposerState(void)
{
    return s_active_composer_state;
}

static int WrapComposerCached(ComposerState *s, const PicoComposer *c, Font font, float max_width,
                              CompLine *lines, int max_lines, float *line_height)
{
    if (s && s->wrap_cache.valid && c &&
        s->wrap_cache.text_revision == c->revision &&
        s->wrap_cache.text_length == c->length &&
        s->wrap_cache.font_generation == Pico_FontGeneration() &&
        s->wrap_cache.font_scale == Pico_FontScale() &&
        s->wrap_cache.font_texture_id == font.texture.id &&
        s->wrap_cache.wrap_width == max_width &&
        s->wrap_cache.line_count <= max_lines)
    {
        memcpy(lines, s->wrap_cache.lines, (size_t)s->wrap_cache.line_count * sizeof(*lines));
        *line_height = s->wrap_cache.line_height;
        return s->wrap_cache.line_count;
    }
    int n = WrapComposer(c, font, max_width, lines, max_lines, line_height);
    if (s && n > 0 && n <= COMPOSER_MAX_LINES)
    {
        s->wrap_cache.valid = true;
        s->wrap_cache.text_revision = c ? c->revision : 0;
        s->wrap_cache.text_length = c ? c->length : 0;
        s->wrap_cache.font_generation = Pico_FontGeneration();
        s->wrap_cache.font_scale = Pico_FontScale();
        s->wrap_cache.font_texture_id = font.texture.id;
        s->wrap_cache.wrap_width = max_width;
        s->wrap_cache.line_height = *line_height;
        s->wrap_cache.line_count = n;
        memcpy(s->wrap_cache.lines, lines, (size_t)n * sizeof(*lines));
    }
    return n;
}

#define s_wrap_width (ActiveComposerState()->wrap_width)
#define s_composer_width (ActiveComposerState()->composer_width)
#define s_seen_cursor (ActiveComposerState()->seen_cursor)
#define s_seen_length (ActiveComposerState()->seen_length)
#define s_caret_blink_at (ActiveComposerState()->caret_blink_at)
#define g_app (ActiveComposerState()->app)
#define g_attach (ActiveComposerState()->attach)
#define g_attach_n (ActiveComposerState()->attach_n)
#define g_preview (ActiveComposerState()->preview)
#define g_preview_tex (ActiveComposerState()->preview_tex)
#define g_preview_src (ActiveComposerState()->preview_src)
#define g_preview_loaded (ActiveComposerState()->preview_loaded)

static float ComposerFallbackWrap(PicoHost *app)
{
    float width = (float)GetScreenWidth() - 80.0f;
    float column = Pico_ChatColumnMaxPx(app);
    if (column > 0.0f)
    {
        float inner = column - (float)(COMPOSER_PAD_X * 2);
        if (inner > 10.0f && inner < width)
        {
            width = inner;
        }
    }
    if (width < 10.0f)
    {
        width = 10.0f;
    }
    return width;
}

static float ComposerWrapWidth(PicoHost *app)
{
    return s_wrap_width > 10 ? s_wrap_width : ComposerFallbackWrap(app);
}

static void ResetPreview(void)
{
    if (g_preview_loaded)
    {
        UnloadTexture(g_preview_tex);
    }
    if (g_preview_src.data)
    {
        UnloadImage(g_preview_src);
    }
    memset(&g_preview_tex, 0, sizeof(g_preview_tex));
    memset(&g_preview_src, 0, sizeof(g_preview_src));
    g_preview_loaded = false;
    g_preview = -1;
}

static bool ClosePreview(void)
{
    if (g_preview < 0)
    {
        return true;
    }
    if (g_app && !pico_ui_modal_pop(g_app, "preview"))
    {
        return false;
    }
    ResetPreview();
    return true;
}

static void PreviewFitSize(int src_w, int src_h, int screen_w, int screen_h, int *out_w, int *out_h)
{
    if (src_w < 1)
    {
        src_w = 1;
    }
    if (src_h < 1)
    {
        src_h = 1;
    }
    if (screen_w < 1)
    {
        screen_w = 1;
    }
    if (screen_h < 1)
    {
        screen_h = 1;
    }
    if (src_w <= screen_w && src_h <= screen_h)
    {
        *out_w = src_w;
        *out_h = src_h;
        return;
    }
    int max_w = screen_w - 48;
    int max_h = screen_h - 48;
    if (max_w < 32)
    {
        max_w = 32;
    }
    if (max_h < 32)
    {
        max_h = 32;
    }
    long long dest_w = src_w;
    long long dest_h = src_h;
    if (dest_w > max_w)
    {
        dest_w = max_w;
        dest_h = (dest_w * src_h + src_w / 2) / src_w;
        if (dest_h < 1)
        {
            dest_h = 1;
        }
    }
    if (dest_h > max_h)
    {
        dest_h = max_h;
        dest_w = (dest_h * src_w + src_h / 2) / src_h;
        if (dest_w < 1)
        {
            dest_w = 1;
        }
    }
    *out_w = (int)dest_w;
    *out_h = (int)dest_h;
}

static void UpdatePreviewDisplay(void)
{
    if (g_preview < 0 || !g_preview_src.data || g_preview_src.width < 1 || g_preview_src.height < 1)
    {
        return;
    }
    int dest_w = 0;
    int dest_h = 0;
    PreviewFitSize(g_preview_src.width, g_preview_src.height, GetScreenWidth(), GetScreenHeight(), &dest_w,
                   &dest_h);
    if (g_preview_loaded && g_preview_tex.width == dest_w && g_preview_tex.height == dest_h)
    {
        return;
    }
    Image copy = ImageCopy(g_preview_src);
    if (!copy.data)
    {
        return;
    }
    if (copy.width != dest_w || copy.height != dest_h)
    {
        ImageResize(&copy, dest_w, dest_h);
    }
    if (g_preview_loaded)
    {
        UnloadTexture(g_preview_tex);
        memset(&g_preview_tex, 0, sizeof(g_preview_tex));
        g_preview_loaded = false;
    }
    g_preview_tex = LoadTextureFromImage(copy);
    UnloadImage(copy);
    if (g_preview_tex.id != 0)
    {
        SetTextureFilter(g_preview_tex, TEXTURE_FILTER_POINT);
        g_preview_loaded = true;
    }
}

static void LoadThumb(ComposerAttach *a)
{
    a->loaded = false;
    memset(&a->thumb, 0, sizeof(a->thumb));
    if (!IsWindowReady())
    {
        return;
    }
    Image img = LoadImage(a->path);
    if (!img.data)
    {
        return;
    }
    int max_side = img.width > img.height ? img.width : img.height;
    if (max_side > ATTACH_THUMB)
    {
        float scale = (float)ATTACH_THUMB / (float)max_side;
        int w = (int)((float)img.width * scale + 0.5f);
        int h = (int)((float)img.height * scale + 0.5f);
        if (w < 1)
        {
            w = 1;
        }
        if (h < 1)
        {
            h = 1;
        }
        ImageResize(&img, w, h);
    }
    a->thumb = LoadTextureFromImage(img);
    UnloadImage(img);
    a->loaded = a->thumb.id != 0;
}

static void UnloadAttach(ComposerAttach *a, bool delete_owned)
{
    if (a->loaded)
    {
        UnloadTexture(a->thumb);
    }
    if (delete_owned && a->owned && a->path[0])
    {
        unlink(a->path);
    }
    memset(a, 0, sizeof(*a));
}

static void ClearAttachments(bool delete_owned)
{
    if (!ClosePreview())
    {
        return;
    }
    for (int i = 0; i < g_attach_n; i++)
    {
        UnloadAttach(&g_attach[i], delete_owned);
    }
    g_attach_n = 0;
}

void PicoComposer_ReleaseAttachments(void)
{
    ClearAttachments(false);
}

void PicoComposer_DiscardAttachments(void)
{
    ClearAttachments(true);
}

bool PicoComposer_HasAttachments(const PicoHost *app)
{
    ComposerState *s = (ComposerState *)PicoPlugins_HostState(app, "composer");
    return s && s->attach_n > 0;
}

int pico_composer_attachment_count(void)
{
    return g_attach_n;
}

bool pico_composer_attach_path(const char *path, bool owned)
{
    if (!path || !path[0] || g_attach_n >= COMPOSER_MAX_ATTACH)
    {
        return false;
    }
    char resolved[4096];
    if (!realpath(path, resolved) || !pico_canonical_is_image_path(resolved))
    {
        return false;
    }
    for (int i = 0; i < g_attach_n; i++)
    {
        if (strcmp(g_attach[i].path, resolved) == 0)
        {
            return true;
        }
    }
    ComposerAttach *a = &g_attach[g_attach_n];
    memset(a, 0, sizeof(*a));
    snprintf(a->path, sizeof(a->path), "%s", resolved);
    a->owned = owned;
    LoadThumb(a);
    g_attach_n++;
    return true;
}

bool pico_composer_remove_at(int index)
{
    if (index < 0 || index >= g_attach_n)
    {
        return false;
    }
    if (g_preview == index)
    {
        if (!ClosePreview())
        {
            return false;
        }
    }
    else if (g_preview > index)
    {
        g_preview--;
    }
    UnloadAttach(&g_attach[index], true);
    if (index < g_attach_n - 1)
    {
        memmove(&g_attach[index], &g_attach[index + 1],
                (size_t)(g_attach_n - index - 1) * sizeof(g_attach[0]));
    }
    g_attach_n--;
    memset(&g_attach[g_attach_n], 0, sizeof(g_attach[0]));
    return true;
}

bool pico_composer_submit_ready(const char *text, int length)
{
    if (g_attach_n > 0)
    {
        return true;
    }
    if (!text || length <= 0)
    {
        return false;
    }
    int start = 0;
    int end = length;
    while (start < end && (text[start] == ' ' || text[start] == '\n' || text[start] == '\t'))
    {
        start++;
    }
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\n' || text[end - 1] == '\t'))
    {
        end--;
    }
    return end > start;
}

static bool PathInParts(const PicoLlmPart *parts, int n, const char *path)
{
    for (int i = 0; i < n; i++)
    {
        if (parts[i].path && path && strcmp(parts[i].path, path) == 0)
        {
            return true;
        }
    }
    return false;
}

char *pico_composer_merge_parts(const char *text, const char *existing_parts)
{
    PicoLlmPart *parts = NULL;
    int n = 0;
    if (existing_parts)
    {
        if (existing_parts[0] != '[')
        {
            return NULL;
        }
        JsonBuf wrap;
        JsonBuf_Init(&wrap);
        JsonBuf_Puts(&wrap, "{\"parts\":");
        JsonBuf_Puts(&wrap, existing_parts);
        JsonBuf_Putc(&wrap, '}');
        char *obj = JsonBuf_Steal(&wrap);
        JsonDoc doc;
        memset(&doc, 0, sizeof(doc));
        bool parsed = obj && JsonParse(&doc, obj, strlen(obj)) == 0 &&
                      pico_canonical_parse_parts(&doc, 0, &parts, &n) && n > 0;
        if (doc.toks)
        {
            JsonFree(&doc);
        }
        free(obj);
        if (!parsed)
        {
            pico_canonical_free_parts(parts, n);
            return NULL;
        }
    }
    else
    {
        parts = (PicoLlmPart *)calloc(1, sizeof(PicoLlmPart));
        if (!parts)
        {
            return NULL;
        }
        parts[0].kind = PICO_LLM_PART_TEXT;
        parts[0].text = JsonDup(text ? text : "");
        if (!parts[0].text)
        {
            free(parts);
            return NULL;
        }
        n = 1;
    }
    int extra = 0;
    for (int i = 0; i < g_attach_n; i++)
    {
        if (!PathInParts(parts, n, g_attach[i].path))
        {
            extra++;
        }
    }
    if (extra > 0)
    {
        PicoLlmPart *grown = (PicoLlmPart *)realloc(parts, (size_t)(n + extra) * sizeof(PicoLlmPart));
        if (!grown)
        {
            pico_canonical_free_parts(parts, n);
            return NULL;
        }
        parts = grown;
        memset(parts + n, 0, (size_t)extra * sizeof(PicoLlmPart));
        for (int i = 0; i < g_attach_n; i++)
        {
            if (PathInParts(parts, n, g_attach[i].path))
            {
                continue;
            }
            parts[n].kind = PICO_LLM_PART_IMAGE;
            parts[n].path = JsonDup(g_attach[i].path);
            parts[n].mime = JsonDup(pico_canonical_mime_for_path(g_attach[i].path));
            if (!parts[n].path || !parts[n].mime)
            {
                pico_canonical_free_parts(parts, n + 1);
                return NULL;
            }
            n++;
        }
    }
    char *json = pico_canonical_parts_json(parts, n);
    pico_canonical_free_parts(parts, n);
    return json;
}

char *pico_composer_display_message(const char *text)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    if (text && text[0])
    {
        JsonBuf_Puts(&b, text);
    }
    for (int i = 0; i < g_attach_n; i++)
    {
        if (b.len > 0)
        {
            JsonBuf_Puts(&b, "\n\n");
        }
        JsonBuf_Puts(&b, "![image](");
        JsonBuf_Puts(&b, g_attach[i].path);
        JsonBuf_Putc(&b, ')');
    }
    return JsonBuf_Steal(&b);
}

bool PicoComposer_ApplyAttachments(PicoHost *app)
{
    s_active_composer_state = (ComposerState *)PicoPlugins_HostState(app, "composer");
    if (!app || g_attach_n <= 0)
    {
        return true;
    }
    const char *text = app->agent_input && app->agent_input[0] ? app->agent_input
                                                              : (app->composer.text ? app->composer.text : "");
    char *merged = pico_composer_merge_parts(text, app->agent_parts);
    if (!merged)
    {
        return false;
    }
    free(app->agent_parts);
    app->agent_parts = merged;
    return true;
}

static bool ComposerMediaDir(const PicoHost *app, char *out, size_t cap)
{
    const char *ws = PicoWorkspace_Path(PicoHost_SelectedWorkspaceConst(app));
    if (!ws[0])
    {
        ws = "/tmp";
    }
    return PicoPath_Format(out, cap, "%s/.pico/media/composer", ws);
}

#if defined(__linux__)
static const char *ImageExtFromBytes(const unsigned char *b, size_t n)
{
    if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0)
    {
        return "png";
    }
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
    {
        return "jpg";
    }
    if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0)
    {
        return "webp";
    }
    if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0))
    {
        return "gif";
    }
    if (n >= 2 && b[0] == 'B' && b[1] == 'M')
    {
        return "bmp";
    }
    return NULL;
}



static double MonotonicSeconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

/* Only the worker owns helper processes and byte buffers. It never reads the
 * host, Raylib, or composer state. posix_spawn avoids post-fork work in this
 * multithreaded process. All discovery/fetch attempts share one deadline. */
typedef enum ClipboardReadResult {
    CLIPBOARD_READ_OK,
    CLIPBOARD_READ_UNAVAILABLE,
    CLIPBOARD_READ_FAILED,
} ClipboardReadResult;

static ClipboardReadResult ClipboardRead(ClipboardPaste *paste, const char *type,
                          unsigned char **out, size_t *out_n)
{
    extern char **environ;
    const char *wl[] = {"wl-paste", "--no-newline", "--type", type, NULL};
    const char *wl_types[] = {"wl-paste", "--list-types", NULL};
    const char *x11[] = {"xclip", "-selection", "clipboard", "-o", "-t",
                         type ? type : "TARGETS", NULL};
    const char **argv = paste->wayland ? (type ? wl : wl_types) : x11;
    int fds[2];
    if (atomic_load(&paste->cancelled) || MonotonicSeconds() >= paste->deadline ||
        pipe2(fds, O_CLOEXEC) != 0)
        return CLIPBOARD_READ_FAILED;
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0)
    {
        close(fds[0]);
        close(fds[1]);
        return CLIPBOARD_READ_FAILED;
    }
    rc = posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&actions, fds[0]);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&actions, fds[1]);
    pid_t pid = 0;
    if (rc == 0) rc = posix_spawnp(&pid, argv[0], &actions, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0)
    {
        close(fds[0]);
        return rc == ENOENT || rc == ENOTDIR || rc == EACCES
                   ? CLIPBOARD_READ_UNAVAILABLE : CLIPBOARD_READ_FAILED;
    }
    bool ok = fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0;
    unsigned char *bytes = NULL;
    size_t n = 0, capacity = 0;
    bool eof = false, exited = false;
    int status = 0;
    while (ok && !(eof && exited))
    {
        if (atomic_load(&paste->cancelled) || MonotonicSeconds() >= paste->deadline)
        {
            ok = false;
            break;
        }
        unsigned char chunk[8192];
        ssize_t got = eof ? -1 : read(fds[0], chunk, sizeof(chunk));
        if (got > 0)
        {
            if ((size_t)got > CLIP_IMAGE_MAX - n)
            {
                ok = false;
                break;
            }
            if (n + (size_t)got + 1 > capacity)
            {
                size_t needed = n + (size_t)got + 1;
                capacity = capacity ? capacity * 2 : 8192;
                if (capacity < needed) capacity = needed;
                if (capacity > CLIP_IMAGE_MAX + 1) capacity = CLIP_IMAGE_MAX + 1;
                unsigned char *grown = realloc(bytes, capacity);
                if (!grown)
                {
                    ok = false;
                    break;
                }
                bytes = grown;
            }
            memcpy(bytes + n, chunk, (size_t)got);
            n += (size_t)got;
            continue;
        }
        if (!eof && got == 0) eof = true;
        else if (!eof && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            ok = false;
            break;
        }
        if (!exited)
        {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) exited = true;
            else if (waited < 0 && errno != EINTR) ok = false;
        }
        if (ok && !(eof && exited))
        {
            struct pollfd fd = {.fd = eof ? -1 : fds[0], .events = POLLIN};
            /* A closed stdout can precede process exit. Avoid spinning on HUP. */
            (void)poll(&fd, 1, 10);
        }
    }
    close(fds[0]);
    if (!exited)
    {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok)
    {
        free(bytes);
        return CLIPBOARD_READ_FAILED;
    }
    if (!bytes) bytes = malloc(1);
    if (!bytes) return CLIPBOARD_READ_FAILED;
    bytes[n] = '\0';
    *out = bytes;
    *out_n = n;
    return CLIPBOARD_READ_OK;
}

static bool ClipboardOffers(const unsigned char *types, const char *type)
{
    const char *line = (const char *)types;
    size_t length = strlen(type);
    while (*line)
    {
        size_t n = strcspn(line, "\r\n");
        if (n == length && memcmp(line, type, n) == 0) return true;
        line += n;
        while (*line == '\r' || *line == '\n') line++;
    }
    return false;
}

/* ICCCM STRING is Latin-1, unlike UTF8_STRING and UTF-8 MIME targets. */
static bool ClipboardLatin1ToUtf8(ClipboardPaste *paste)
{
    unsigned char *utf8 = malloc(paste->length * 2 + 1);
    if (!utf8) return false;
    size_t n = 0;
    for (size_t i = 0; i < paste->length; i++)
    {
        unsigned char cp = paste->bytes[i];
        if (cp < 0x80) utf8[n++] = cp;
        else
        {
            utf8[n++] = 0xc0 | (cp >> 6);
            utf8[n++] = 0x80 | (cp & 0x3f);
        }
    }
    utf8[n] = '\0';
    free(paste->bytes);
    paste->bytes = utf8;
    paste->length = n;
    return true;
}

static void *ClipboardPasteRun(void *arg)
{
    ClipboardPaste *paste = arg;
    unsigned char *types = NULL;
    size_t types_n = 0;
    ClipboardReadResult result = ClipboardRead(paste, NULL, &types, &types_n);
    if (result != CLIPBOARD_READ_OK)
    {
        paste->fallback = result == CLIPBOARD_READ_UNAVAILABLE;
        paste->failed = !paste->fallback;
        return NULL;
    }
    const char *images[] = {"image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp"};
    for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++)
    {
        if (!ClipboardOffers(types, images[i])) continue;
        result = ClipboardRead(paste, images[i], &paste->bytes, &paste->length);
        paste->fallback = result == CLIPBOARD_READ_UNAVAILABLE;
        paste->failed = result == CLIPBOARD_READ_FAILED;
        if (result == CLIPBOARD_READ_OK)
        {
            paste->image_ext = ImageExtFromBytes(paste->bytes, paste->length);
            paste->failed = paste->image_ext == NULL;
        }
        free(types);
        return NULL;
    }
    const char *texts[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING"};
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++)
    {
        if (!ClipboardOffers(types, texts[i])) continue;
        result = ClipboardRead(paste, texts[i], &paste->bytes, &paste->length);
        paste->fallback = result == CLIPBOARD_READ_UNAVAILABLE;
        paste->failed = result == CLIPBOARD_READ_FAILED;
        if (result == CLIPBOARD_READ_OK && strcmp(texts[i], "STRING") == 0)
            paste->failed = !ClipboardLatin1ToUtf8(paste);
        break;
    }
    free(types);
    return NULL;
}

static void ClipboardPasteCancel(void *arg)
{
    atomic_store(&((ClipboardPaste *)arg)->cancelled, true);
}

static void ClipboardPasteDestroy(void *arg)
{
    ClipboardPaste *paste = arg;
    free(paste->bytes);
    free(paste);
}

void PicoComposer_CancelClipboardPaste(void)
{
    ComposerState *s = ActiveComposerState();
    if (s && s->clipboard_paste)
    {
        ClipboardPasteCancel(s->clipboard_paste);
        s->clipboard_paste = NULL;
    }
}

#endif

static unsigned char *ClipboardImageBytes(size_t *out_n, const char **out_ext, bool *raylib_alloc)
{
    *out_n = 0;
    *out_ext = NULL;
    *raylib_alloc = false;
#if !defined(__linux__)
    Image img = GetClipboardImage();
    if (img.data && img.width > 0 && img.height > 0)
    {
        int size = 0;
        unsigned char *png = ExportImageToMemory(img, ".png", &size);
        UnloadImage(img);
        if (png && size > 0)
        {
            *out_n = (size_t)size;
            *out_ext = "png";
            *raylib_alloc = true;
            return png;
        }
        if (png)
        {
            MemFree(png);
        }
    }
    else if (img.data)
    {
        UnloadImage(img);
    }
#endif
    return NULL;
}

static bool PersistClipboardImage(PicoHost *app, const unsigned char *bytes, size_t n, const char *ext)
{
    char dir[4096];
    if (!ComposerMediaDir(app, dir, sizeof(dir)))
    {
        return false;
    }
    char *path = pico_canonical_persist_bytes(dir, ext && ext[0] ? ext : "png", bytes, n);
    if (!path)
    {
        return false;
    }
    bool ok = pico_composer_attach_path(path, true);
    free(path);
    return ok;
}

static bool PasteClipboardImage(PicoHost *app)
{
    if (g_attach_n >= COMPOSER_MAX_ATTACH)
    {
        pico_status_warn(app, "Too many attached images.");
        return true;
    }
    size_t n = 0;
    const char *ext = NULL;
    bool raylib_alloc = false;
    unsigned char *bytes = ClipboardImageBytes(&n, &ext, &raylib_alloc);
    if (!bytes)
    {
        return false;
    }
    bool ok = PersistClipboardImage(app, bytes, n, ext);
    if (raylib_alloc)
    {
        MemFree(bytes);
    }
    else
    {
        free(bytes);
    }
    if (!ok)
    {
        pico_status_warn(app, "Could not attach the pasted image.");
    }
    return true;
}

static void PasteClipboard(PicoComposer *c);

#if defined(__linux__)
static void PasteClipboardText(PicoComposer *c, const char *clip);

static void ClipboardPasteCompleted(PicoHost *app, void *arg)
{
    ClipboardPaste *paste = arg;
    ComposerState *s = PicoPlugins_HostState(app, "composer");
    /* Reload/cancel invalidates the borrowed identity without waiting. */
    if (!s || s->clipboard_paste != paste) return;
    s_active_composer_state = s;
    s->clipboard_paste = NULL;
    if (atomic_load(&paste->cancelled)) return;
    if (paste->image_ext && PicoClarification_View(app)) return;
    if (paste->image_ext)
    {
        if (!PersistClipboardImage(app, paste->bytes, paste->length, paste->image_ext))
            pico_status_warn(app, "Could not attach the pasted image.");
    }
    else if (paste->fallback)
        PasteClipboard(&app->composer);
    else if (paste->failed)
        pico_status_warn(app, "Could not read the clipboard.");
    else if (paste->bytes)
        PasteClipboardText(&app->composer, (const char *)paste->bytes);
    pico_host_request_redraw(app);
}

bool PicoComposer_ClipboardPasteBusy(void)
{
    ComposerState *s = ActiveComposerState();
    return s && s->clipboard_paste != NULL;
}

void PicoComposer_BeginClipboardPaste(PicoHost *app)
{
    s_active_composer_state = PicoPlugins_HostState(app, "composer");
    ComposerState *s = ActiveComposerState();
    if (!s || s->clipboard_paste) return;
    if (g_attach_n >= COMPOSER_MAX_ATTACH)
    {
        pico_status_warn(app, "Too many attached images.");
        return;
    }
    int platform = glfwGetPlatform();
    if (platform != GLFW_PLATFORM_WAYLAND && platform != GLFW_PLATFORM_X11)
    {
        PasteClipboard(&app->composer);
        pico_host_request_redraw(app);
        return;
    }
    ClipboardPaste *paste = calloc(1, sizeof(*paste));
    if (!paste) return;
    atomic_init(&paste->cancelled, false);
    paste->wayland = platform == GLFW_PLATFORM_WAYLAND;
    paste->deadline = MonotonicSeconds() + CLIP_PROCESS_TIMEOUT_SECONDS;
    if (!PicoHost_StartTaskCompleted(app, ClipboardPasteRun, paste, ClipboardPasteCancel,
                                     ClipboardPasteCompleted, ClipboardPasteDestroy))
    {
        ClipboardPasteDestroy(paste);
        return;
    }
    s->clipboard_paste = paste;
}

#endif

static void MoveCursor(PicoComposer *c, int pos, bool extend);

static void NoteCaretActivity(void)
{
    ComposerState *s = ActiveComposerState();
    if (s)
    {
        s->caret_blink_at = GetTime();
    }
}

static ComposerView GetComposerView(PicoHost *app)
{
    ComposerView v = {0};
    PicoComposer *c = &app->composer;
    Clay_ElementData scroll_box = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("ComposerScroll")));
    Clay_ElementData composer_box = Clay_GetElementData(Clay_GetElementId(CLAY_STRING("Composer")));
    v.found = scroll_box.found || composer_box.found;
    v.clip = scroll_box.found ? scroll_box.boundingBox : composer_box.boundingBox;
    if (scroll_box.found)
    {
        v.origin_x = scroll_box.boundingBox.x;
        v.origin_y = scroll_box.boundingBox.y;
        v.wrap_width = scroll_box.boundingBox.width;
    }
    else if (composer_box.found)
    {
        v.origin_x = composer_box.boundingBox.x + COMPOSER_PAD_X;
        v.origin_y = composer_box.boundingBox.y + COMPOSER_PAD_Y;
        v.wrap_width = composer_box.boundingBox.width - COMPOSER_PAD_X * 2;
    }
    else
    {
        v.wrap_width = s_wrap_width;
    }
    if (v.wrap_width < 10)
    {
        v.wrap_width = ComposerWrapWidth(app);
    }
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ComposerScroll")));
    if (scroll.found && scroll.scrollPosition)
    {
        v.scroll_y = scroll.scrollPosition->y;
    }
    v.line_count = WrapComposerCached(ActiveComposerState(), c, ComposerFont(), v.wrap_width, v.lines, COMPOSER_MAX_LINES, &v.line_height);
    return v;
}

static int CaretLineIndex(const ComposerView *v, int cursor)
{
    int line_i = 0;
    for (int i = 0; i < v->line_count; i++)
    {
        if (cursor >= v->lines[i].start)
        {
            line_i = i;
        }
    }
    return line_i;
}

static int OffsetAtXOnLine(Font font, const PicoComposer *c, CompLine line, float target_x)
{
    if (target_x <= 0 || line.length <= 0 || !c->text)
    {
        return line.start;
    }
    float width = 0;
    int pos = line.start;
    int end = line.start + line.length;
    while (pos < end)
    {
        int next = PicoText_Utf8Next(c->text, c->length, pos);
        float ch_w = MeasureSlice(font, c->text, pos, next - pos, ComposerPx());
        if (width + ch_w * 0.5f >= target_x)
        {
            return pos;
        }
        width += ch_w;
        pos = next;
    }
    return end;
}

static void MoveVertical(PicoHost *app, int dir, bool extend)
{
    PicoComposer *c = &app->composer;
    CompLine lines[COMPOSER_MAX_LINES];
    float line_height = ComposerPx();
    float wrap = ComposerWrapWidth(app);
    int line_count = WrapComposerCached(ActiveComposerState(), c, ComposerFont(), wrap, lines, COMPOSER_MAX_LINES, &line_height);
    int line_i = 0;
    for (int i = 0; i < line_count; i++)
    {
        if (c->cursor >= lines[i].start)
        {
            line_i = i;
        }
    }
    int start = lines[line_i].start;
    int take = c->cursor - start;
    if (take > lines[line_i].length)
    {
        take = lines[line_i].length;
    }
    if (take < 0)
    {
        take = 0;
    }
    float x = MeasureSlice(ComposerFont(), c->text ? c->text : "", start, take, ComposerPx());
    ComposerState *s = ActiveComposerState();
    float goal = (s && s->goal_x >= 0) ? s->goal_x : x;
    int next = line_i + dir;
    int pos;
    if (next < 0)
    {
        pos = 0;
    }
    else if (next >= line_count)
    {
        pos = c->length;
    }
    else
    {
        pos = OffsetAtXOnLine(ComposerFont(), c, lines[next], goal);
    }
    MoveCursor(c, pos, extend);
    if (s)
    {
        s->goal_x = goal;
    }
}

static int OffsetAtPoint(PicoHost *app, float x, float y)
{
    PicoComposer *c = &app->composer;
    ComposerView v = GetComposerView(app);
    if (!v.found)
    {
        return c->cursor;
    }
    float local_x = x - v.origin_x;
    float local_y = y - v.origin_y - v.scroll_y;
    if (local_y < 0)
    {
        return 0;
    }
    int line_i = (int)(local_y / v.line_height);
    int line_count = v.line_count;
    if (line_i >= line_count)
    {
        return c->length;
    }
    if (line_i < 0)
    {
        line_i = 0;
    }
    CompLine line = v.lines[line_i];
    if (local_x <= 0)
    {
        return line.start;
    }
    float width = 0;
    int pos = line.start;
    int end = line.start + line.length;
    while (pos < end)
    {
        int next = PicoText_Utf8Next(c->text, c->length, pos);
        float ch_w = MeasureSlice(ComposerFont(), c->text, pos, next - pos, ComposerPx());
        if (width + ch_w * 0.5f >= local_x)
        {
            return pos;
        }
        width += ch_w;
        pos = next;
    }
    if (line_i < line_count - 1)
    {
        return end;
    }
    return end;
}

static void CaretPos(PicoHost *app, float *out_x, float *out_y, float *out_h)
{
    PicoComposer *c = &app->composer;
    ComposerView v = GetComposerView(app);
    *out_x = v.origin_x;
    *out_y = v.origin_y;
    *out_h = v.line_height > 1 ? v.line_height : ComposerPx();
    if (!v.found)
    {
        return;
    }
    int line_i = CaretLineIndex(&v, c->cursor);
    int start = v.lines[line_i].start;
    int take = c->cursor - start;
    if (take > v.lines[line_i].length)
    {
        take = v.lines[line_i].length;
    }
    if (take < 0)
    {
        take = 0;
    }
    *out_y = v.origin_y + (float)line_i * v.line_height + v.scroll_y;
    *out_x = v.origin_x +
             MeasureSlice(ComposerFont(), c->text ? c->text : "", start, take, ComposerPx());
}

static void EnsureCaretVisible(PicoHost *app)
{
    PicoComposer *c = &app->composer;
    if (c->cursor == s_seen_cursor && c->length == s_seen_length)
    {
        return;
    }
    s_seen_cursor = c->cursor;
    s_seen_length = c->length;

    ComposerView v = GetComposerView(app);
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ComposerScroll")));
    if (!scroll.found || !scroll.scrollPosition || v.line_height < 1)
    {
        return;
    }
    int line_i = CaretLineIndex(&v, c->cursor);
    float caret_top = (float)line_i * v.line_height;
    float caret_bot = caret_top + v.line_height;
    float view_h = scroll.scrollContainerDimensions.height;
    float vis_top = -scroll.scrollPosition->y;
    float vis_bot = vis_top + view_h;
    if (caret_top < vis_top)
    {
        scroll.scrollPosition->y = -caret_top;
    }
    else if (caret_bot > vis_bot)
    {
        scroll.scrollPosition->y = -(caret_bot - view_h);
    }
}

static bool ComposerReserve(PicoComposer *c, int extra)
{
    /* size_t math: length/extra are int and their sum can overflow near INT_MAX. */
    size_t needed = (size_t)c->length + (size_t)extra + 1;
    if (needed <= (size_t)c->capacity)
    {
        return true;
    }
    if (needed > (size_t)INT_MAX)
    {
        return false;
    }
    size_t capacity = c->capacity == 0 ? 256 : (size_t)c->capacity;
    while (capacity < needed)
    {
        size_t doubled = capacity * 2;
        capacity = doubled > (size_t)INT_MAX ? needed : doubled;
    }
    char *next = (char *)realloc(c->text, capacity);
    if (!next)
    {
        return false; /* keep the old buffer and capacity; caller must not insert */
    }
    c->text = next;
    c->capacity = (int)capacity;
    return true;
}

static int SelFrom(const PicoComposer *c)
{
    return c->sel_anchor < c->cursor ? c->sel_anchor : c->cursor;
}

static int SelTo(const PicoComposer *c)
{
    return c->sel_anchor > c->cursor ? c->sel_anchor : c->cursor;
}

bool PicoComposer_HasSelection(const PicoHost *app)
{
    return app->composer.sel_anchor != app->composer.cursor;
}

static void ComposerDeleteRange(PicoComposer *c, int from, int to);
static void ComposerInsert(PicoComposer *c, const char *bytes, int nbytes);

static void ComposerDeleteRange(PicoComposer *c, int from, int to)
{
    if (from < 0)
    {
        from = 0;
    }
    if (to > c->length)
    {
        to = c->length;
    }
    if (to <= from)
    {
        return;
    }
    PicoComplete_BeforeEdit(from, to);
    memmove(c->text + from, c->text + to, (size_t)(c->length - to));
    c->length -= (to - from);
    c->cursor = from;
    c->sel_anchor = from;
    c->text[c->length] = '\0';
    c->revision++;
    ComposerState *s = ActiveComposerState();
    if (s)
    {
        s->goal_x = -1;
        s->wrap_cache.valid = false;
    }
    NoteCaretActivity();
}

void PicoComposer_ReplaceRange(PicoHost *app, int from, int to, const char *text)
{
    s_active_composer_state = (ComposerState *)PicoPlugins_HostState(app, "composer");
    PicoComposer *c = &app->composer;
    c->sel_anchor = c->cursor;
    if (from > to)
    {
        int tmp = from;
        from = to;
        to = tmp;
    }
    ComposerDeleteRange(c, from, to);
    if (text && text[0])
    {
        ComposerInsert(c, text, (int)strlen(text));
    }
}

void PicoComposer_SetText(PicoHost *app, const char *text)
{
    s_active_composer_state = (ComposerState *)PicoPlugins_HostState(app, "composer");
    PicoComposer *c = &app->composer;
    c->sel_anchor = 0;
    c->cursor = 0;
    ComposerDeleteRange(c, 0, c->length);
    if (text && text[0])
    {
        ComposerInsert(c, text, (int)strlen(text));
    }
}

static void DeleteSelection(PicoComposer *c)
{
    if (c->sel_anchor != c->cursor)
    {
        ComposerDeleteRange(c, SelFrom(c), SelTo(c));
    }
}

static void ComposerInsert(PicoComposer *c, const char *bytes, int nbytes)
{
    if (nbytes <= 0)
    {
        return;
    }
    if (!ComposerReserve(c, nbytes))
    {
        return;
    }
    DeleteSelection(c);
    memmove(c->text + c->cursor + nbytes, c->text + c->cursor, (size_t)(c->length - c->cursor));
    memcpy(c->text + c->cursor, bytes, (size_t)nbytes);
    c->length += nbytes;
    c->cursor += nbytes;
    c->sel_anchor = c->cursor;
    c->text[c->length] = '\0';
    c->revision++;
    ComposerState *s = ActiveComposerState();
    if (s)
    {
        s->goal_x = -1;
        s->wrap_cache.valid = false;
    }
    NoteCaretActivity();
}

static void MoveCursor(PicoComposer *c, int pos, bool extend)
{
    if (pos < 0)
    {
        pos = 0;
    }
    if (pos > c->length)
    {
        pos = c->length;
    }
    c->cursor = pos;
    if (!extend)
    {
        c->sel_anchor = pos;
    }
    ComposerState *s = ActiveComposerState();
    if (s)
    {
        s->goal_x = -1;
    }
    NoteCaretActivity();
}


void PicoComposer_Copy(PicoHost *app)
{
    s_active_composer_state = (ComposerState *)PicoPlugins_HostState(app, "composer");
    PicoComposer *c = &app->composer;
    if (c->sel_anchor == c->cursor || !c->text)
    {
        return;
    }
    int from = SelFrom(c);
    int to = SelTo(c);
    int n = to - from;
    char *copy = (char *)malloc((size_t)n + 1);
    if (!copy)
    {
        return;
    }
    memcpy(copy, c->text + from, (size_t)n);
    copy[n] = '\0';
    SetClipboardText(copy);
    free(copy);
}

static void OpenPreview(int index)
{
    if (index < 0 || index >= g_attach_n)
    {
        return;
    }
    if (!ClosePreview() || !g_app || !pico_ui_modal_push(g_app, "preview"))
    {
        return;
    }
    g_preview = index;
    if (!IsWindowReady())
    {
        return;
    }
    g_preview_src = LoadImage(g_attach[index].path);
    UpdatePreviewDisplay();
}

static void PasteClipboardText(PicoComposer *c, const char *clip)
{
    if (!clip || clip[0] == '\0')
    {
        return;
    }
    int len = (int)strlen(clip);
    if (len >= PASTE_TEMP_THRESHOLD)
    {
        char tmpl[] = "/tmp/pico-paste-XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0)
        {
            ssize_t written = write(fd, clip, (size_t)len);
            close(fd);
            if (written == (ssize_t)len)
            {
                ComposerInsert(c, tmpl, (int)strlen(tmpl));
                return;
            }
            unlink(tmpl); /* partial/failed write: don't leave an orphan temp file */
        }
    }
    ComposerInsert(c, clip, len);
}

static void PasteClipboard(PicoComposer *c)
{
    PasteClipboardText(c, GetClipboardText());
}

void PicoComposer_HandleInput(PicoHost *app)
{
    s_active_composer_state = (ComposerState *)PicoPlugins_HostState(app, "composer");
    if (!s_active_composer_state || PicoUi_QuestionnaireOpen(app) || PicoUi_ModalOpen(app) || PicoChatFind_BlocksInput(app))
    {
        return;
    }
    PicoComposer *c = &app->composer;
    bool ctrl = IsCtrlDown();
    bool shift = IsShiftDown();
    const char *text = c->text ? c->text : "";
    bool repeat_left = IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT);
    bool repeat_right = IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT);
    bool repeat_back = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE);
    bool repeat_del = IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE);

    if (!PicoClarification_View(app) && PicoComplete_HandleKeys(app))
    {
        return;
    }

    if (ctrl && Pico_ShortcutPressed('c'))
    {
        if (PicoChatSel_HasSelection(app))
        {
            PicoChatSel_Copy(app);
        }
        else if (PicoComposer_HasSelection(app))
        {
            PicoComposer_Copy(app);
        }
        return;
    }
    if (ctrl && Pico_ShortcutPressed('x'))
    {
        PicoComposer_Copy(app);
        DeleteSelection(c);
        return;
    }

    if (ctrl && Pico_ShortcutPressed('a'))
    {
        MoveCursor(c, 0, false);
        MoveCursor(c, c->length, true);
    }

    if (IsKeyPressed(KEY_HOME))
    {
        MoveCursor(c, LineStart(text, c->cursor), shift);
    }

    if (IsKeyPressed(KEY_END))
    {
        MoveCursor(c, LineEnd(text, c->length, c->cursor), shift);
    }

    if (repeat_left)
    {
        int pos = ctrl ? PicoText_PrevWord(text, c->cursor) : PicoText_Utf8Prev(text, c->cursor);
        MoveCursor(c, pos, shift);
    }
    if (repeat_right)
    {
        int pos = ctrl ? PicoText_NextWord(text, c->length, c->cursor) : PicoText_Utf8Next(text, c->length, c->cursor);
        MoveCursor(c, pos, shift);
    }

    bool repeat_up = IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP);
    bool repeat_down = IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN);
    if (repeat_up)
    {
        MoveVertical(app, -1, shift);
    }
    if (repeat_down)
    {
        MoveVertical(app, 1, shift);
    }

    if (ctrl && Pico_ShortcutRepeat('w'))
    {
        if (PicoComposer_HasSelection(app))
        {
            DeleteSelection(c);
        }
        else
        {
            ComposerDeleteRange(c, PicoText_PrevWord(text, c->cursor), c->cursor);
        }
    }
    else if (repeat_back)
    {
        if (PicoComposer_HasSelection(app))
        {
            DeleteSelection(c);
        }
        else if (ctrl)
        {
            ComposerDeleteRange(c, PicoText_PrevWord(text, c->cursor), c->cursor);
        }
        else
        {
            ComposerDeleteRange(c, PicoText_Utf8Prev(text, c->cursor), c->cursor);
        }
    }

    if (repeat_del)
    {
        if (PicoComposer_HasSelection(app))
        {
            DeleteSelection(c);
        }
        else
        {
            ComposerDeleteRange(c, c->cursor, PicoText_Utf8Next(text, c->length, c->cursor));
        }
    }

    if (ctrl && Pico_ShortcutPressed('v'))
    {
        if (PicoClarification_View(app))
        {
            const char *clip = GetClipboardText();
            if (clip) ComposerInsert(c, clip, (int)strlen(clip));
        }
        else if (!PasteClipboardImage(app))
        {
#if defined(__linux__)
            PicoComposer_BeginClipboardPaste(app);
#else
            PasteClipboard(c);
#endif
        }
    }

    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && !shift)
    {
        PicoHost_Submit(app);
        return;
    }

    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressedRepeat(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) ||
         IsKeyPressedRepeat(KEY_KP_ENTER)) &&
        shift)
    {
        ComposerInsert(c, "\n", 1);
    }

    if (IsKeyPressed(KEY_TAB))
    {
        ComposerInsert(c, "  ", 2);
    }

    if (!ctrl)
    {
        int cp;
        while ((cp = GetCharPressed()) != 0)
        {
            if (cp < 32)
            {
                continue;
            }
            char bytes[4];
            int n = PicoText_Utf8Encode(cp, bytes);
            ComposerInsert(c, bytes, n);
            pico_host_request_redraw(app);
        }
    }
    if (!PicoClarification_View(app)) PicoComplete_Refresh(app);
}

static void ComposerUnitRange(const PicoComposer *c, int pos, int granularity, int *from, int *to)
{
    const char *text = c->text ? c->text : "";
    if (granularity >= 3)
    {
        PicoText_ParaRange(text, c->length, pos, from, to);
    }
    else
    {
        PicoText_WordRange(text, c->length, pos, from, to);
    }
}

static void ComposerSelectUnit(PicoComposer *c, int pos, int granularity)
{
    c->granularity = granularity;
    if (granularity <= 1)
    {
        c->unit_from = pos;
        c->unit_to = pos;
        MoveCursor(c, pos, IsShiftDown());
        return;
    }
    int from = pos;
    int to = pos;
    ComposerUnitRange(c, pos, granularity, &from, &to);
    c->unit_from = from;
    c->unit_to = to;
    c->sel_anchor = from;
    c->cursor = to;
    ComposerState *s = ActiveComposerState();
    if (s)
    {
        s->goal_x = -1;
    }
    NoteCaretActivity();
}

static void ComposerExtendUnit(PicoComposer *c, int pos)
{
    if (c->granularity <= 1)
    {
        MoveCursor(c, pos, true);
        return;
    }
    int from = pos;
    int to = pos;
    ComposerUnitRange(c, pos, c->granularity, &from, &to);
    int span_from = 0;
    int span_to = 0;
    PicoText_UnionRange(c->unit_from, c->unit_to, from, to, &span_from, &span_to);
    if (pos >= c->unit_from)
    {
        c->sel_anchor = c->unit_from;
        c->cursor = span_to;
    }
    else
    {
        c->sel_anchor = c->unit_to;
        c->cursor = span_from;
    }
    ComposerState *s = ActiveComposerState();
    if (s)
    {
        s->goal_x = -1;
    }
    NoteCaretActivity();
}

bool PicoComposer_PointerOverAttachments(void)
{
    return g_attach_n > 0 &&
           Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ComposerAttachStrip")));
}

bool PicoComposer_PointerOverAttachmentRemove(void)
{
    for (int i = 0; i < g_attach_n; i++)
    {
        if (Clay_PointerOver(CLAY_IDI("CompAttachRemove", i)))
        {
            return true;
        }
    }
    return false;
}

static bool ComposerHandleAttachPointer(void)
{
    if (g_attach_n <= 0 || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return PicoComposer_PointerOverAttachments();
    }
    for (int i = 0; i < g_attach_n; i++)
    {
        if (Clay_PointerOver(CLAY_IDI("CompAttachRemove", i)))
        {
            pico_composer_remove_at(i);
            return true;
        }
    }
    for (int i = 0; i < g_attach_n; i++)
    {
        if (Clay_PointerOver(CLAY_IDI("CompAttach", i)))
        {
            OpenPreview(i);
            return true;
        }
    }
    return PicoComposer_PointerOverAttachments();
}

void PicoComposer_HandlePointer(PicoHost *app)
{
    if (PicoChatFind_PointerOver(app)) return;
    s_active_composer_state = (ComposerState *)PicoPlugins_HostState(app, "composer");
    if (!s_active_composer_state)
    {
        return;
    }
    PicoComposer *c = &app->composer;
    Vector2 mouse = GetMousePosition();
    bool over_bar = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("CompScrollBarHandle"))) ||
                    Clay_PointerOver(Clay_GetElementId(CLAY_STRING("CompScrollTrack")));
    bool over = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ComposerScroll")));

    if (over_bar)
    {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            c->mouse_selecting = false;
            PicoClickSeq_Reset(&c->click_seq);
        }
        return;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && over)
    {
        int pos = OffsetAtPoint(app, mouse.x, mouse.y);
        int count = PicoClickSeq_Press(&c->click_seq, GetTime(), mouse.x, mouse.y);
        ComposerSelectUnit(c, pos, count);
        c->mouse_selecting = true;
        PicoChatSel_Clear(app);
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        c->mouse_selecting = false;
    }
    else if (c->mouse_selecting)
    {
        ComposerExtendUnit(c, OffsetAtPoint(app, mouse.x, mouse.y));
    }
}

static bool ComposerVision(PicoHost *app)
{
    bool vision = true;
    PicoAgent *agent = PicoHost_SelectedAgent(app);
    PicoModel *model = PicoAgent_IsBusy(agent) ? PicoSettings_ActiveModel(agent)
                                               : PicoSettings_SelectedModel(agent);
    if (model)
    {
        vision = model->vision;
    }
    return vision;
}

static void ComposerAttachRender(PicoHost *app, void *state)
{
    s_active_composer_state = state ? (ComposerState *)state : (ComposerState *)PicoPlugins_HostState(app, "composer");
    if (!s_active_composer_state || PicoClarification_View(app) || PicoUi_QuestionnaireOpen(app) || g_attach_n <= 0)
    {
        return;
    }

    bool vision = ComposerVision(app);
    float screen_w = (float)GetScreenWidth();
    float max_w = s_composer_width > 0 ? s_composer_width : screen_w - 24.0f;
    if (max_w > screen_w - 24.0f)
    {
        max_w = screen_w - 24.0f;
    }
    if (max_w < 72.0f)
    {
        max_w = 72.0f;
    }

    CLAY(CLAY_ID("ComposerAttachStrip"),
         {.floating = {.offset = {.y = -ATTACH_GAP},
                       .parentId = CLAY_ID("Composer").id,
                       .zIndex = 10,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_BOTTOM,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP},
                       .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_CAPTURE,
                       .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .padding = {8, 8, 8, 8},
                     .childGap = 4,
                     .sizing = {.width = CLAY_SIZING_FIT(0, max_w), .height = CLAY_SIZING_FIT(0)}},
          .backgroundColor = COLOR_COMPOSER_BG,
          .cornerRadius = CLAY_CORNER_RADIUS(8)})
    {
        CLAY(CLAY_ID("ComposerAttachRow"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childGap = 8,
                         .sizing = {.width = CLAY_SIZING_FIT(0),
                                    .height = CLAY_SIZING_FIXED((float)ATTACH_THUMB)}}})
        {
            for (int i = 0; i < g_attach_n; i++)
            {
                ComposerAttach *a = &g_attach[i];
                float thumb_w = (float)ATTACH_THUMB;
                float thumb_h = (float)ATTACH_THUMB;
                if (a->loaded && a->thumb.width > 0 && a->thumb.height > 0)
                {
                    if (a->thumb.width >= a->thumb.height)
                    {
                        thumb_h = (float)ATTACH_THUMB * ((float)a->thumb.height / (float)a->thumb.width);
                    }
                    else
                    {
                        thumb_w = (float)ATTACH_THUMB * ((float)a->thumb.width / (float)a->thumb.height);
                    }
                }
                CLAY(CLAY_IDI("CompAttach", i),
                     {.layout = {.sizing = {.width = CLAY_SIZING_FIXED((float)ATTACH_THUMB),
                                            .height = CLAY_SIZING_FIXED((float)ATTACH_THUMB)},
                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                      .backgroundColor = COLOR_CODE_BG,
                      .cornerRadius = CLAY_CORNER_RADIUS(6)})
                {
                    if (a->loaded)
                    {
                        CLAY(CLAY_IDI("CompAttachImg", i),
                             {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(thumb_w),
                                                    .height = CLAY_SIZING_FIXED(thumb_h)}},
                              .image = {.imageData = &a->thumb}})
                        {
                        }
                    }
                    if (Clay_Hovered() || Clay_PointerOver(CLAY_IDI("CompAttachRemove", i)))
                    {
                        CLAY(CLAY_IDI("CompAttachRemove", i),
                             {.floating = {.attachTo = CLAY_ATTACH_TO_PARENT,
                                           .zIndex = 12,
                                           .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                                           .attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_TOP,
                                                            .parent = CLAY_ATTACH_POINT_RIGHT_TOP},
                                           .offset = {-3, 3}},
                              .layout = {.sizing = {.width = CLAY_SIZING_FIXED((float)ATTACH_REMOVE),
                                                    .height = CLAY_SIZING_FIXED((float)ATTACH_REMOVE)},
                                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER,
                                                            .y = CLAY_ALIGN_Y_CENTER}},
                              .backgroundColor = Clay_Hovered() ? (Clay_Color){50, 28, 32, 240}
                                                                : (Clay_Color){20, 20, 24, 220},
                              .cornerRadius = CLAY_CORNER_RADIUS(9)})
                        {
                            CLAY_TEXT(CLAY_STRING("×"),
                                      CLAY_TEXT_CONFIG({.fontId = FONT_BOLD,
                                                        .fontSize = PICO_FONT_CAPTION,
                                                        .textColor = COLOR_TEXT}));
                        }
                    }
                }
            }
        }
        if (!vision)
        {
            CLAY_TEXT(CLAY_STRING("This model doesn't accept images"),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                        .fontSize = PICO_FONT_CAPTION,
                                        .textColor = COLOR_MUTED,
                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
        }
    }
}

void PicoComposer_Render(PicoHost *app, void *state)
{
    s_active_composer_state = state ? (ComposerState *)state : (ComposerState *)PicoPlugins_HostState(app, "composer");
    if (!s_active_composer_state || !PicoHost_SelectedAgent(app) || PicoUi_QuestionnaireOpen(app))
    {
        return;
    }
    PicoComposer *c = &app->composer;
    const char *placeholder = PicoClarification_View(app) ? "Ask about the question…  (Enter to send)" :
                              PicoAgent_IsBusy(PicoHost_SelectedAgent(app)) ?
                              "Steer Pico…  (Enter to queue, Shift+Enter for newline)" :
                              "Message Pico…  (Enter to send, Shift+Enter for newline)";
    bool empty = c->length == 0;
    float wrap_width = ComposerWrapWidth(app);
    CompLine lines[COMPOSER_MAX_LINES];
    float line_height = ComposerPx();
    int line_count = empty ? 1 : WrapComposerCached(ActiveComposerState(), c, ComposerFont(), wrap_width, lines, COMPOSER_MAX_LINES, &line_height);
    if (line_height < 1)
    {
        line_height = ComposerPx();
    }

    float content_h = (float)line_count * line_height;
    float box_h = content_h + (float)COMPOSER_PAD_Y * 2;
    if (box_h < (float)COMPOSER_MIN_HEIGHT)
    {
        box_h = (float)COMPOSER_MIN_HEIGHT;
    }
    float max_h = (float)COMPOSER_MAX_GROW_LINES * line_height + (float)COMPOSER_PAD_Y * 2;
    if (box_h > max_h)
    {
        box_h = max_h;
    }

    Clay_SizingAxis composer_width = CLAY_SIZING_GROW(0);
    float column_max = Pico_ChatColumnMaxPx(app);
    if (column_max > 0.0f)
    {
        composer_width = CLAY_SIZING_GROW(0, column_max);
    }

    /* Match the fixed child height. FIT participates in Clay's vertical
     * compression and can accumulate residue while chat follows the bottom. */
    CLAY(CLAY_ID("ComposerAlign"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER},
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(box_h)}}})
    {
        CLAY(CLAY_ID("Composer"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {COMPOSER_PAD_X, COMPOSER_PAD_X, COMPOSER_PAD_Y, COMPOSER_PAD_Y},
                         .sizing = {.width = composer_width, .height = CLAY_SIZING_FIXED(box_h)}},
              .backgroundColor = COLOR_COMPOSER_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
        CLAY(CLAY_ID("ComposerRow"),
             {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childGap = SCROLLBAR_GAP,
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}})
        {
            CLAY(CLAY_ID("ComposerScroll"),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
                  .clip = {.vertical = true, .horizontal = false, .childOffset = Clay_GetScrollOffset()}})
            {
                CLAY(CLAY_ID("ComposerContent"),
                     {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                 .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}}})
                {
                    if (empty)
                    {
                        Clay_String text = {.length = (int32_t)strlen(placeholder), .chars = placeholder};
                        CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                          .fontSize = COMPOSER_FONT_SIZE,
                                                          .textColor = COLOR_MUTED,
                                                          .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                    }
                    else
                    {
                        for (int i = 0; i < line_count; i++)
                        {
                            CLAY(CLAY_IDI("CompLine", i),
                                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                                        .height = CLAY_SIZING_FIXED(line_height)}}})
                            {
                                if (lines[i].length > 0)
                                {
                                    Clay_String text = {.length = (int32_t)lines[i].length,
                                                        .chars = c->text + lines[i].start};
                                    CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                                      .fontSize = COMPOSER_FONT_SIZE,
                                                                      .textColor = COLOR_TEXT,
                                                                      .wrapMode = CLAY_TEXT_WRAP_NONE}));
                                }
                            }
                        }
                    }
                }
            }
            if (app->composer_overflow)
            {
                PicoScrollbar_Render(CLAY_STRING("ComposerScroll"), CLAY_STRING("CompScrollTrack"),
                                     CLAY_STRING("CompScrollBarHandle"));
            }
        }
        if (!PicoClarification_View(app)) PicoComplete_Render(app);
        }
    }
}

static void ComposerPreviewRender(PicoHost *app, void *state)
{
    s_active_composer_state = state ? (ComposerState *)state : (ComposerState *)PicoPlugins_HostState(app, "composer");
    if (!s_active_composer_state || g_preview < 0)
    {
        return;
    }
    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
    float img_w = 0;
    float img_h = 0;
    if (g_preview_loaded && g_preview_tex.width > 0 && g_preview_tex.height > 0)
    {
        img_w = (float)g_preview_tex.width;
        img_h = (float)g_preview_tex.height;
    }

    CLAY(CLAY_ID("ComposerPreviewDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT,
                       .zIndex = 50,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
          .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(sw), .height = CLAY_SIZING_FIXED(sh)}},
          .backgroundColor = {0, 0, 0, 180}})
    {
        if (g_preview_loaded && img_w > 1 && img_h > 1)
        {
            CLAY(CLAY_ID("ComposerPreviewImage"),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(img_w),
                                        .height = CLAY_SIZING_FIXED(img_h)}},
                  .image = {.imageData = &g_preview_tex}})
            {
            }
        }
    }
}

void PicoComposer_DrawOverlay(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)event;
    s_active_composer_state = state ? (ComposerState *)state : (ComposerState *)PicoPlugins_HostState(app, "composer");
    if (!s_active_composer_state || !PicoHost_SelectedAgent(app) || PicoUi_QuestionnaireOpen(app) || PicoUi_ModalOpen(app))
    {
        return;
    }
    PicoComposer *c = &app->composer;
    ComposerView v = GetComposerView(app);
    if (!v.found)
    {
        return;
    }

    Pico_Scissor((int)v.clip.x, (int)v.clip.y, (int)v.clip.width, (int)v.clip.height);

    if (PicoComposer_HasSelection(app) && c->text)
    {
        int sel_from = SelFrom(c);
        int sel_to = SelTo(c);
        Color fill = {(unsigned char)COLOR_SELECTION.r, (unsigned char)COLOR_SELECTION.g, (unsigned char)COLOR_SELECTION.b,
                      (unsigned char)COLOR_SELECTION.a};
        for (int i = 0; i < v.line_count; i++)
        {
            int start = v.lines[i].start;
            int end = start + v.lines[i].length;
            int range_lo = start;
            int range_hi = end;
            if (v.lines[i].length == 0 && start > 0)
            {
                range_lo = start - 1;
            }
            if (sel_from >= range_hi || sel_to <= range_lo)
            {
                continue;
            }
            float y = v.origin_y + (float)i * v.line_height + v.scroll_y;
            if (v.lines[i].length == 0)
            {
                DrawRectangle((int)v.origin_x, (int)y, 6, (int)v.line_height, fill);
                continue;
            }
            int a = sel_from > start ? sel_from : start;
            int b = sel_to < end ? sel_to : end;
            if (a > b)
            {
                a = b;
            }
            float x0 = MeasureSlice(ComposerFont(), c->text, start, a - start, ComposerPx());
            float x1 = MeasureSlice(ComposerFont(), c->text, start, b - start, ComposerPx());
            DrawRectangle((int)(v.origin_x + x0), (int)y, (int)(x1 - x0 < 2 ? 2 : x1 - x0), (int)v.line_height, fill);
        }
    }

    {
        PicoSpellLine spell_lines[COMPOSER_MAX_LINES];
        int spell_line_count = 0;
        if (c->text && c->length > 0)
        {
            spell_line_count = v.line_count;
            for (int i = 0; i < v.line_count; i++)
            {
                spell_lines[i].start = v.lines[i].start;
                spell_lines[i].length = v.lines[i].length;
            }
        }
        /* Empty text is still reported so clearing the field commits the
         * word that was being typed. */
        PicoSpellView view = {
            .text = c->text ? c->text : "",
            .length = c->text ? c->length : 0,
            .lines = spell_lines,
            .line_count = spell_line_count,
            .origin_x = v.origin_x,
            .origin_y = v.origin_y,
            .line_height = v.line_height,
            .scroll_y = v.scroll_y,
            .clip = v.clip,
            .font = ComposerFont(),
            .font_px = ComposerPx(),
            .cursor = c->cursor,
        };
        PicoSpell_DrawSquiggles(app, PICO_SPELL_FIELD_COMPOSER, &view);
    }

    double elapsed = GetTime() - s_caret_blink_at;
    if (elapsed < 0)
    {
        elapsed = 0;
    }
    pico_host_request_redraw_after(app, (floor(elapsed * CARET_BLINK_HZ) + 1.0) /
                                               CARET_BLINK_HZ - elapsed);
    if (((int)(elapsed * CARET_BLINK_HZ) & 1) == 0)
    {
        float x, y, h;
        CaretPos(app, &x, &y, &h);
        Color caret = {(unsigned char)COLOR_CURSOR.r, (unsigned char)COLOR_CURSOR.g, (unsigned char)COLOR_CURSOR.b, 255};
        DrawRectangle((int)x, (int)y, 2, (int)h, caret);
    }

    Pico_ScissorEnd();
}

static void ComposerAfterLayout(PicoHost *app, const PicoHookEvent *event, void *state)
{
    (void)event;
    s_active_composer_state = state ? (ComposerState *)state : (ComposerState *)PicoPlugins_HostState(app, "composer");
    if (!s_active_composer_state || !PicoHost_SelectedAgent(app)) return;
    if (PicoUi_QuestionnaireOpen(app))
    {
        if (g_preview >= 0 && pico_ui_modal_is_top(app, "preview") &&
            (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))) ClosePreview();
        return;
    }
    ComposerView v = GetComposerView(app);
    if (v.wrap_width > 10)
    {
        s_wrap_width = v.wrap_width;
    }
    Clay_ElementData composer = Clay_GetElementData(CLAY_ID("Composer"));
    if (composer.found)
    {
        s_composer_width = composer.boundingBox.width;
    }
    app->composer_overflow = PicoScrollbar_Overflows(CLAY_STRING("ComposerScroll"));
    if (g_preview >= 0)
    {
        if (!pico_ui_modal_is_top(app, "preview"))
        {
            EnsureCaretVisible(app);
            return;
        }
        if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            ClosePreview();
        }
        EnsureCaretVisible(app);
        return;
    }
    if (!PicoUi_ModalOpen(app))
    {
        if (!PicoClarification_View(app) && ComposerHandleAttachPointer())
        {
            /* strip consumed the click */
        }
        else if (PicoClarification_View(app) || !PicoComplete_HandlePointer(app))
        {
            PicoComposer_HandlePointer(app);
        }
    }
    EnsureCaretVisible(app);
}

static void UpdateComposerScrollbarDrag(PicoHost *app)
{
    PicoScrollbar_UpdateDrag(&app->composer_scrollbar, CLAY_STRING("ComposerScroll"),
                             CLAY_STRING("CompScrollBarHandle"));
}

/* Drain OS file drops every frame so a later drop cannot pile onto an ignored
 * one. Image paths attach like clipboard paste; other files are ignored. */
static void ConsumeDroppedFiles(PicoHost *app)
{
    if (!IsFileDropped())
    {
        return;
    }
    FilePathList files = LoadDroppedFiles();
    bool attach = app && !PicoClarification_View(app) && PicoHost_SelectedAgent(app) && !PicoUi_QuestionnaireOpen(app) && !PicoUi_ModalOpen(app);
    if (attach && files.paths)
    {
        for (unsigned int i = 0; i < files.count; i++)
        {
            pico_composer_attach_path(files.paths[i], false);
            pico_host_request_redraw(app);
        }
    }
    UnloadDroppedFiles(files);
}

static void ComposerFrame(PicoHost *app, void *state, float dt)
{
    (void)dt;
    s_active_composer_state = state ? (ComposerState *)state : (ComposerState *)PicoPlugins_HostState(app, "composer");
    if (!s_active_composer_state)
    {
        return;
    }
    ConsumeDroppedFiles(app);
    if (!PicoHost_SelectedAgent(app) || PicoUi_QuestionnaireOpen(app))
    {
        return;
    }
    PicoComposer_HandleInput(app);
    UpdatePreviewDisplay();
    if (!PicoUi_ModalOpen(app))
    {
        UpdateComposerScrollbarDrag(app);
    }
}

void PicoComposer_ResetPresentation(PicoHost *app)
{
    ComposerState *s = app ? PicoPlugins_HostState(app, "composer") : NULL;
    if (!s) return;
    s_active_composer_state = s;
#if defined(__linux__)
    PicoComposer_CancelClipboardPaste();
#endif
    s->wrap_cache.valid = false;
    s->seen_cursor = s->seen_length = -1;
    s->goal_x = -1;
    app->composer.mouse_selecting = false;
    PicoClickSeq_Reset(&app->composer.click_seq);
}

static int ComposerInit(PicoHost *app, void **state_out)
{
    ComposerState *s = (ComposerState *)calloc(1, sizeof(ComposerState));
    if (!s)
    {
        return 1;
    }
    s->app = app;
    s->seen_cursor = -1;
    s->seen_length = -1;
    s->goal_x = -1;
    s->preview = -1;
    if (state_out)
    {
        *state_out = s;
    }
    s_active_composer_state = s;
    pico_host_add_view(app, PICO_SLOT_COMPOSER, 0, PicoComposer_Render);
    pico_host_add_view(app, PICO_SLOT_OVERLAY, 6, ComposerAttachRender);
    pico_host_add_view(app, PICO_SLOT_OVERLAY, 25, ComposerPreviewRender);
    pico_host_add_hook(app, PICO_HOOK_AFTER_LAYOUT, ComposerAfterLayout);
    pico_host_add_hook(app, PICO_HOOK_AFTER_RENDER, PicoComposer_DrawOverlay);
    return 0;
}

static void ComposerShutdown(PicoHost *app, void *state)
{
    (void)app;
    ComposerState *s = (ComposerState *)state;
    if (!s)
    {
        return;
    }
    s_active_composer_state = s;
#if defined(__linux__)
    PicoComposer_CancelClipboardPaste();
#endif
    if (s->preview >= 0 && s->app)
    {
        (void)pico_ui_modal_pop(s->app, "preview");
    }
    ResetPreview();
    PicoComposer_DiscardAttachments();
    free(s);
    s_active_composer_state = NULL;
}

PicoExt pico_ext_composer(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "composer",
        .description = "Prompt input",
        .host_init = ComposerInit,
        .host_shutdown = ComposerShutdown,
        .host_on_frame = ComposerFrame,
    };
}
