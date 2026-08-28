#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "canonical.h"
#include "complete_internal.h"
#include "composer_internal.h"
#include "json.h"
#include "path.h"
#include "settings.h"
#include "text_range.h"
#include "scrollbar.h"

#include "clay/clay.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__linux__)
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif
#include <unistd.h>

#define PASTE_TEMP_THRESHOLD 4096
#define COMPOSER_PAD_X 14
#define COMPOSER_PAD_Y 10
#define COMPOSER_FONT_SIZE 16
#define COMPOSER_MAX_LINES 256
#define COMPOSER_MIN_HEIGHT 56
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

static int Utf8Next(const char *s, int length, int pos)
{
    if (pos >= length)
    {
        return length;
    }
    unsigned char c = (unsigned char)s[pos];
    int step = 1;
    if ((c & 0xE0) == 0xC0)
    {
        step = 2;
    }
    else if ((c & 0xF0) == 0xE0)
    {
        step = 3;
    }
    else if ((c & 0xF8) == 0xF0)
    {
        step = 4;
    }
    pos += step;
    return pos > length ? length : pos;
}

static int Utf8Prev(const char *s, int pos)
{
    if (pos <= 0)
    {
        return 0;
    }
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
    {
        pos--;
    }
    return pos;
}

static int PrevWord(const char *s, int pos)
{
    while (pos > 0 && isspace((unsigned char)s[pos - 1]))
    {
        pos--;
    }
    while (pos > 0 && PicoText_IsWordByte((unsigned char)s[pos - 1]))
    {
        pos--;
    }
    if (pos > 0 && !PicoText_IsWordByte((unsigned char)s[pos - 1]) && !isspace((unsigned char)s[pos - 1]))
    {
        pos--;
        while (pos > 0 && !PicoText_IsWordByte((unsigned char)s[pos - 1]) && !isspace((unsigned char)s[pos - 1]))
        {
            pos--;
        }
    }
    return pos;
}

static int NextWord(const char *s, int length, int pos)
{
    while (pos < length && isspace((unsigned char)s[pos]))
    {
        pos++;
    }
    while (pos < length && PicoText_IsWordByte((unsigned char)s[pos]))
    {
        pos++;
    }
    if (pos < length && !PicoText_IsWordByte((unsigned char)s[pos]) && !isspace((unsigned char)s[pos]))
    {
        while (pos < length && !PicoText_IsWordByte((unsigned char)s[pos]) && !isspace((unsigned char)s[pos]))
        {
            pos++;
        }
    }
    return pos;
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
            int next = Utf8Next(c->text, c->length, i);
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

static float s_wrap_width = 0;

static float ComposerFallbackWrap(PicoApp *app)
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

static float ComposerWrapWidth(PicoApp *app)
{
    return s_wrap_width > 10 ? s_wrap_width : ComposerFallbackWrap(app);
}

static float s_composer_width = 0;
static int s_seen_cursor = -1;
static int s_seen_length = -1;
static float s_goal_x = -1;
static double s_caret_blink_at;

typedef struct ComposerAttach {
    char path[4096];
    Texture2D thumb;
    bool loaded;
    bool owned;
} ComposerAttach;

static PicoApp *g_app;
static ComposerAttach g_attach[COMPOSER_MAX_ATTACH];
static int g_attach_n;
static int g_preview = -1;
static Texture2D g_preview_tex;
static Image g_preview_src;
static bool g_preview_loaded;

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

bool PicoComposer_HasAttachments(const PicoApp *app)
{
    (void)app;
    return g_attach_n > 0;
}

bool PicoComposer_PreviewOpen(void)
{
    return g_preview >= 0;
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

bool PicoComposer_ApplyAttachments(PicoApp *app)
{
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

static bool ComposerMediaDir(const PicoApp *app, char *out, size_t cap)
{
    const char *ws = (app && app->workspace[0]) ? app->workspace : "/tmp";
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

typedef struct ClipboardProcess {
    pid_t pid;
    int fd;
    int command;
    unsigned char *bytes;
    size_t length;
    size_t capacity;
    double deadline;
    double terminate_deadline;
    bool active;
    bool terminating;
} ClipboardProcess;

static ClipboardProcess g_clip_process = {.fd = -1};
static pid_t g_clip_reap[8];
static int g_clip_reap_count;

static const char *const kClipboardCommands[][8] = {
    {"wl-paste", "--type", "image/png", NULL},
    {"wl-paste", "--type", "image/jpeg", NULL},
    {"wl-paste", "--type", "image/webp", NULL},
    {"xclip", "-selection", "clipboard", "-t", "image/png", "-o", NULL},
    {"xclip", "-selection", "clipboard", "-t", "image/jpeg", "-o", NULL},
    {"xclip", "-selection", "clipboard", "-t", "image/webp", "-o", NULL},
};

static double MonotonicSeconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void ClipboardProcessQueueReap(pid_t pid)
{
    if (pid <= 0)
    {
        return;
    }
    if (g_clip_reap_count < (int)(sizeof(g_clip_reap) / sizeof(g_clip_reap[0])))
    {
        g_clip_reap[g_clip_reap_count++] = pid;
    }
}

static void ClipboardProcessPumpReapers(void)
{
    for (int i = 0; i < g_clip_reap_count;)
    {
        pid_t waited = waitpid(g_clip_reap[i], NULL, WNOHANG);
        if (waited == g_clip_reap[i] || (waited < 0 && errno == ECHILD))
        {
            g_clip_reap[i] = g_clip_reap[--g_clip_reap_count];
            continue;
        }
        i++;
    }
}

static void ClipboardProcessKillAndReap(pid_t pid)
{
    if (pid <= 0)
    {
        return;
    }
    kill(pid, SIGKILL);
    double deadline = MonotonicSeconds() + 0.05;
    for (;;)
    {
        pid_t waited = waitpid(pid, NULL, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD))
        {
            return;
        }
        if (waited < 0 && errno != EINTR)
        {
            return;
        }
        if (MonotonicSeconds() >= deadline)
        {
            ClipboardProcessQueueReap(pid);
            return;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
}

static void ClipboardProcessResetAttempt(void)
{
    if (g_clip_process.fd >= 0)
    {
        close(g_clip_process.fd);
    }
    g_clip_process.fd = -1;
    g_clip_process.pid = 0;
    free(g_clip_process.bytes);
    g_clip_process.bytes = NULL;
    g_clip_process.length = 0;
    g_clip_process.capacity = 0;
    g_clip_process.terminate_deadline = 0;
    g_clip_process.terminating = false;
}

static void ClipboardProcessClear(void)
{
    ClipboardProcessResetAttempt();
    memset(&g_clip_process, 0, sizeof(g_clip_process));
    g_clip_process.fd = -1;
}

void PicoComposer_CancelClipboardPaste(void)
{
    if (g_clip_process.pid > 0)
    {
        if (g_clip_process.fd >= 0)
        {
            close(g_clip_process.fd);
            g_clip_process.fd = -1;
        }
        ClipboardProcessKillAndReap(g_clip_process.pid);
    }
    ClipboardProcessClear();
}

static bool ClipboardProcessSpawn(void)
{
    int pipefd[2];
    if (pipe(pipefd) != 0)
    {
        return false;
    }
    pid_t pid = fork();
    if (pid == 0)
    {
        int null_fd = open("/dev/null", O_WRONLY);
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
            (null_fd >= 0 && dup2(null_fd, STDERR_FILENO) < 0))
        {
            _exit(126);
        }
        close(pipefd[1]);
        if (null_fd >= 0)
        {
            close(null_fd);
        }
        execvp(kClipboardCommands[g_clip_process.command][0],
               (char *const *)kClipboardCommands[g_clip_process.command]);
        _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0)
    {
        close(pipefd[0]);
        return false;
    }
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) != 0)
    {
        close(pipefd[0]);
        ClipboardProcessKillAndReap(pid);
        return false;
    }
    g_clip_process.pid = pid;
    g_clip_process.fd = pipefd[0];
    return true;
}

static bool ClipboardProcessAppend(const unsigned char *bytes, size_t n)
{
    if (n > CLIP_IMAGE_MAX - g_clip_process.length)
    {
        return false;
    }
    size_t needed = g_clip_process.length + n;
    if (needed > g_clip_process.capacity)
    {
        size_t capacity = g_clip_process.capacity ? g_clip_process.capacity : 4096;
        while (capacity < needed)
        {
            size_t next = capacity * 2;
            capacity = next > CLIP_IMAGE_MAX ? CLIP_IMAGE_MAX : next;
        }
        unsigned char *grown = (unsigned char *)realloc(g_clip_process.bytes, capacity);
        if (!grown)
        {
            return false;
        }
        g_clip_process.bytes = grown;
        g_clip_process.capacity = capacity;
    }
    memcpy(g_clip_process.bytes + g_clip_process.length, bytes, n);
    g_clip_process.length += n;
    return true;
}

static bool ClipboardProcessDrain(void)
{
    unsigned char chunk[8192];
    for (;;)
    {
        ssize_t got = read(g_clip_process.fd, chunk, sizeof(chunk));
        if (got > 0)
        {
            if (!ClipboardProcessAppend(chunk, (size_t)got))
            {
                return false;
            }
            continue;
        }
        if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        {
            return got == 0;
        }
        if (errno == EINTR)
        {
            continue;
        }
        return true;
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

static bool PersistClipboardImage(PicoApp *app, const unsigned char *bytes, size_t n, const char *ext)
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

static bool PasteClipboardImage(PicoApp *app)
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
static bool ClipboardProcessStartNext(void)
{
    ClipboardProcessResetAttempt();
    int count = (int)(sizeof(kClipboardCommands) / sizeof(kClipboardCommands[0]));
    while (g_clip_process.command < count)
    {
        if (ClipboardProcessSpawn())
        {
            return true;
        }
        g_clip_process.command++;
    }
    return false;
}

static void ClipboardProcessFallback(PicoApp *app)
{
    ClipboardProcessClear();
    PasteClipboard(&app->composer);
}

bool PicoComposer_ClipboardPasteBusy(void)
{
    return g_clip_process.active;
}

void PicoComposer_BeginClipboardPaste(PicoApp *app)
{
    if (g_clip_process.active)
    {
        return;
    }
    if (g_attach_n >= COMPOSER_MAX_ATTACH)
    {
        pico_status_warn(app, "Too many attached images.");
        return;
    }
    g_clip_process.active = true;
    g_clip_process.command = 0;
    g_clip_process.deadline = MonotonicSeconds() + CLIP_PROCESS_TIMEOUT_SECONDS;
    if (!ClipboardProcessStartNext())
    {
        ClipboardProcessFallback(app);
    }
}

static void ClipboardProcessTerminate(void)
{
    if (g_clip_process.terminating)
    {
        return;
    }
    if (g_clip_process.pid > 0)
    {
        kill(g_clip_process.pid, SIGKILL);
    }
    g_clip_process.terminate_deadline = MonotonicSeconds() + 0.05;
    if (g_clip_process.fd >= 0)
    {
        close(g_clip_process.fd);
        g_clip_process.fd = -1;
    }
    g_clip_process.terminating = true;
}

void PicoComposer_PumpClipboardPaste(PicoApp *app)
{
    ClipboardProcessPumpReapers();
    if (!g_clip_process.active || g_clip_process.pid <= 0)
    {
        return;
    }
    if (!g_clip_process.terminating && !ClipboardProcessDrain())
    {
        ClipboardProcessTerminate();
    }
    if (!g_clip_process.terminating && MonotonicSeconds() >= g_clip_process.deadline)
    {
        ClipboardProcessTerminate();
    }

    int status = 0;
    pid_t waited = waitpid(g_clip_process.pid, &status, WNOHANG);
    if (waited == 0 || (waited < 0 && errno == EINTR))
    {
        if (g_clip_process.terminating &&
            MonotonicSeconds() >= g_clip_process.terminate_deadline)
        {
            ClipboardProcessQueueReap(g_clip_process.pid);
            g_clip_process.pid = 0;
            g_clip_process.command++;
            if (MonotonicSeconds() >= g_clip_process.deadline || !ClipboardProcessStartNext())
            {
                ClipboardProcessFallback(app);
            }
        }
        return;
    }
    bool exited_ok = waited == g_clip_process.pid && !g_clip_process.terminating &&
                     WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (exited_ok && g_clip_process.fd >= 0 && !ClipboardProcessDrain())
    {
        exited_ok = false;
    }
    const char *ext = exited_ok ? ImageExtFromBytes(g_clip_process.bytes, g_clip_process.length) : NULL;
    if (ext)
    {
        bool attached = PersistClipboardImage(app, g_clip_process.bytes, g_clip_process.length, ext);
        ClipboardProcessClear();
        if (!attached)
        {
            pico_status_warn(app, "Could not attach the pasted image.");
        }
        return;
    }

    g_clip_process.command++;
    if (MonotonicSeconds() >= g_clip_process.deadline || !ClipboardProcessStartNext())
    {
        ClipboardProcessFallback(app);
    }
}
#endif

static void MoveCursor(PicoComposer *c, int pos, bool extend);

static void NoteCaretActivity(void)
{
    s_caret_blink_at = GetTime();
}

static ComposerView GetComposerView(PicoApp *app)
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
    v.line_count = WrapComposer(c, ComposerFont(), v.wrap_width, v.lines, COMPOSER_MAX_LINES, &v.line_height);
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
        int next = Utf8Next(c->text, c->length, pos);
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

static void MoveVertical(PicoApp *app, int dir, bool extend)
{
    PicoComposer *c = &app->composer;
    CompLine lines[COMPOSER_MAX_LINES];
    float line_height = ComposerPx();
    float wrap = ComposerWrapWidth(app);
    int line_count = WrapComposer(c, ComposerFont(), wrap, lines, COMPOSER_MAX_LINES, &line_height);
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
    float goal = s_goal_x >= 0 ? s_goal_x : x;
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
    s_goal_x = goal;
}

static int OffsetAtPoint(PicoApp *app, float x, float y)
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
        int next = Utf8Next(c->text, c->length, pos);
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

static void CaretPos(PicoApp *app, float *out_x, float *out_y, float *out_h)
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

static void EnsureCaretVisible(PicoApp *app)
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

static void ComposerReserve(PicoComposer *c, int extra)
{
    int needed = c->length + extra + 1;
    if (needed <= c->capacity)
    {
        return;
    }
    int capacity = c->capacity == 0 ? 256 : c->capacity;
    while (capacity < needed)
    {
        capacity *= 2;
    }
    c->text = (char *)realloc(c->text, (size_t)capacity);
    c->capacity = capacity;
    if (!c->text)
    {
        c->capacity = 0;
        c->length = 0;
        c->cursor = 0;
        c->sel_anchor = 0;
    }
}

static int SelFrom(const PicoComposer *c)
{
    return c->sel_anchor < c->cursor ? c->sel_anchor : c->cursor;
}

static int SelTo(const PicoComposer *c)
{
    return c->sel_anchor > c->cursor ? c->sel_anchor : c->cursor;
}

bool PicoComposer_HasSelection(const PicoApp *app)
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
    s_goal_x = -1;
    NoteCaretActivity();
}

void PicoComposer_ReplaceRange(PicoApp *app, int from, int to, const char *text)
{
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

void PicoComposer_SetText(PicoApp *app, const char *text)
{
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
    DeleteSelection(c);
    ComposerReserve(c, nbytes);
    if (!c->text)
    {
        return;
    }
    memmove(c->text + c->cursor + nbytes, c->text + c->cursor, (size_t)(c->length - c->cursor));
    memcpy(c->text + c->cursor, bytes, (size_t)nbytes);
    c->length += nbytes;
    c->cursor += nbytes;
    c->sel_anchor = c->cursor;
    c->text[c->length] = '\0';
    s_goal_x = -1;
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
    s_goal_x = -1;
    NoteCaretActivity();
}

static int Utf8Encode(int cp, char out[4])
{
    if (cp < 0x80)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

void PicoComposer_Copy(PicoApp *app)
{
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

static void PasteClipboard(PicoComposer *c)
{
    const char *clip = GetClipboardText();
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
        }
    }
    ComposerInsert(c, clip, len);
}

void PicoComposer_HandleInput(PicoApp *app)
{
    if (PicoUi_ModalOpen(app))
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

    if (PicoComplete_HandleKeys(app))
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

    if (ctrl && (Pico_ShortcutPressed('a') || IsKeyPressed(KEY_HOME)))
    {
        MoveCursor(c, LineStart(text, c->cursor), shift);
    }
    else if (IsKeyPressed(KEY_HOME))
    {
        MoveCursor(c, LineStart(text, c->cursor), shift);
    }

    if (ctrl && (Pico_ShortcutPressed('e') || IsKeyPressed(KEY_END)))
    {
        MoveCursor(c, LineEnd(text, c->length, c->cursor), shift);
    }
    else if (IsKeyPressed(KEY_END))
    {
        MoveCursor(c, LineEnd(text, c->length, c->cursor), shift);
    }

    if (repeat_left)
    {
        int pos = ctrl ? PrevWord(text, c->cursor) : Utf8Prev(text, c->cursor);
        MoveCursor(c, pos, shift);
    }
    if (repeat_right)
    {
        int pos = ctrl ? NextWord(text, c->length, c->cursor) : Utf8Next(text, c->length, c->cursor);
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
            ComposerDeleteRange(c, PrevWord(text, c->cursor), c->cursor);
        }
    }
    else if (repeat_back)
    {
        if (PicoComposer_HasSelection(app))
        {
            DeleteSelection(c);
        }
        else
        {
            ComposerDeleteRange(c, Utf8Prev(text, c->cursor), c->cursor);
        }
    }

    if (ctrl && Pico_ShortcutPressed('k'))
    {
        int to = LineEnd(text, c->length, c->cursor);
        if (to == c->cursor && to < c->length && text[to] == '\n')
        {
            to++;
        }
        ComposerDeleteRange(c, c->cursor, to);
    }
    else if (repeat_del)
    {
        if (PicoComposer_HasSelection(app))
        {
            DeleteSelection(c);
        }
        else
        {
            ComposerDeleteRange(c, c->cursor, Utf8Next(text, c->length, c->cursor));
        }
    }

    if (ctrl && Pico_ShortcutPressed('v'))
    {
        if (!PasteClipboardImage(app))
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
        PicoApp_Submit(app);
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
            int n = Utf8Encode(cp, bytes);
            ComposerInsert(c, bytes, n);
        }
    }
    PicoComplete_Refresh(app);
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
    s_goal_x = -1;
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
    s_goal_x = -1;
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

void PicoComposer_HandlePointer(PicoApp *app)
{
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

static bool ComposerVision(PicoApp *app)
{
    bool vision = true;
    PicoModel *model = PicoSettings_ActiveModel(app, PicoApp_ActiveAgent(app));
    if (model)
    {
        vision = model->vision;
    }
    return vision;
}

static void ComposerAttachRender(PicoApp *app)
{
    if (g_attach_n <= 0)
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
                                                        .fontSize = 12,
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
                                        .fontSize = 12,
                                        .textColor = COLOR_MUTED,
                                        .wrapMode = CLAY_TEXT_WRAP_WORDS}));
        }
    }
}

void PicoComposer_Render(PicoApp *app)
{
    PicoComposer *c = &app->composer;
    const char *placeholder = "Message Pico…  (Enter to send, Shift+Enter for newline)";
    bool empty = c->length == 0;
    float wrap_width = ComposerWrapWidth(app);
    CompLine lines[COMPOSER_MAX_LINES];
    float line_height = ComposerPx();
    int line_count = empty ? 1 : WrapComposer(c, ComposerFont(), wrap_width, lines, COMPOSER_MAX_LINES, &line_height);
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
        PicoComplete_Render(app);
        }
    }
}

static void ComposerPreviewRender(PicoApp *app)
{
    (void)app;
    if (g_preview < 0)
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

void PicoComposer_DrawOverlay(PicoApp *app, const PicoHookEvent *event)
{
    (void)event;
    if (PicoUi_ModalOpen(app))
    {
        return;
    }
    PicoComposer *c = &app->composer;
    ComposerView v = GetComposerView(app);
    if (!v.found)
    {
        return;
    }

    BeginScissorMode((int)v.clip.x, (int)v.clip.y, (int)v.clip.width, (int)v.clip.height);

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

    double elapsed = GetTime() - s_caret_blink_at;
    if (elapsed < 0)
    {
        elapsed = 0;
    }
    if (((int)(elapsed * CARET_BLINK_HZ) & 1) == 0)
    {
        float x, y, h;
        CaretPos(app, &x, &y, &h);
        Color caret = {(unsigned char)COLOR_CURSOR.r, (unsigned char)COLOR_CURSOR.g, (unsigned char)COLOR_CURSOR.b, 255};
        DrawRectangle((int)x, (int)y, 2, (int)h, caret);
    }

    EndScissorMode();
}

static void ComposerAfterLayout(PicoApp *app, const PicoHookEvent *event)
{
    (void)event;
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
        if (ComposerHandleAttachPointer())
        {
            /* strip consumed the click */
        }
        else if (!PicoComplete_HandlePointer(app))
        {
            PicoComposer_HandlePointer(app);
        }
    }
    EnsureCaretVisible(app);
}

static void UpdateComposerScrollbarDrag(PicoApp *app)
{
    PicoScrollbar_UpdateDrag(&app->composer_scrollbar, CLAY_STRING("ComposerScroll"),
                             CLAY_STRING("CompScrollBarHandle"));
}

static void ComposerFrame(PicoApp *app, float dt)
{
    (void)dt;
#if defined(__linux__)
    PicoComposer_PumpClipboardPaste(app);
#endif
    PicoComposer_HandleInput(app);
    UpdatePreviewDisplay();
    if (!PicoUi_ModalOpen(app))
    {
        UpdateComposerScrollbarDrag(app);
    }
}

static void ComposerInit(PicoApp *app)
{
    g_app = app;
    if (g_preview >= 0 && !pico_ui_modal_has(app, "preview"))
    {
        ResetPreview();
    }
    pico_add_view(app, PICO_SLOT_COMPOSER, 0, PicoComposer_Render);
    pico_add_view(app, PICO_SLOT_OVERLAY, 6, ComposerAttachRender);
    pico_add_view(app, PICO_SLOT_OVERLAY, 25, ComposerPreviewRender);
    pico_add_hook(app, PICO_HOOK_AFTER_LAYOUT, ComposerAfterLayout);
    pico_add_hook(app, PICO_HOOK_AFTER_RENDER, PicoComposer_DrawOverlay);
}

static void ComposerShutdown(PicoApp *app)
{
    (void)app;
#if defined(__linux__)
    PicoComposer_CancelClipboardPaste();
#endif
    if (g_preview >= 0 && g_app)
    {
        (void)pico_ui_modal_pop(g_app, "preview");
    }
    ResetPreview();
    PicoComposer_DiscardAttachments();
    g_app = NULL;
}

PicoExt pico_ext_composer(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "composer",
        .description = "Prompt input",
        .init = ComposerInit,
        .shutdown = ComposerShutdown,
        .on_frame = ComposerFrame,
    };
}
