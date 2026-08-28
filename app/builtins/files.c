#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "canonical.h"
#include "complete_internal.h"
#include "json.h"
#include "path.h"
#include "settings.h"
#include "host_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILES_MAX 8000
#define FILES_PATH 512
#define FILES_MAX_BYTES (1024 * 1024)
#define FILES_WALK_DEPTH 12

static char **g_files;
static int g_file_count;
static bool g_scanned;
static uint64_t g_token_id;
static char g_root[4096];

static bool SkipDirName(const char *name)
{
    if (!name || !name[0] || name[0] == '.')
    {
        return true;
    }
    return strcmp(name, "node_modules") == 0 || strcmp(name, "dist") == 0 || strcmp(name, "build") == 0 ||
           strcmp(name, "target") == 0 || strcmp(name, "__pycache__") == 0;
}

static void FilesClear(void)
{
    for (int i = 0; i < g_file_count; i++)
    {
        free(g_files[i]);
    }
    free(g_files);
    g_files = NULL;
    g_file_count = 0;
    g_scanned = false;
}

static void FilesAdd(const char *rel)
{
    if (!rel || !rel[0] || g_file_count >= FILES_MAX)
    {
        return;
    }
    char **next = (char **)realloc(g_files, (size_t)(g_file_count + 1) * sizeof(char *));
    if (!next)
    {
        return;
    }
    g_files = next;
    g_files[g_file_count] = JsonDup(rel);
    if (g_files[g_file_count])
    {
        g_file_count++;
    }
}

static void Walk(const char *root, const char *rel, int depth)
{
    if (depth > FILES_WALK_DEPTH || g_file_count >= FILES_MAX)
    {
        return;
    }
    char dir[4096];
    if (rel[0])
    {
        snprintf(dir, sizeof(dir), "%s/%s", root, rel);
    }
    else
    {
        snprintf(dir, sizeof(dir), "%s", root);
    }
    DIR *d = opendir(dir);
    if (!d)
    {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_file_count < FILES_MAX)
    {
        if (SkipDirName(ent->d_name) || strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        {
            continue;
        }
        char child[FILES_PATH];
        if (rel[0])
        {
            snprintf(child, sizeof(child), "%s/%s", rel, ent->d_name);
        }
        else
        {
            snprintf(child, sizeof(child), "%s", ent->d_name);
        }
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", root, child);
        struct stat st;
        if (stat(full, &st) != 0)
        {
            continue;
        }
        if (S_ISDIR(st.st_mode))
        {
            Walk(root, child, depth + 1);
        }
        else if (S_ISREG(st.st_mode))
        {
            FilesAdd(child);
        }
    }
    closedir(d);
}

static const char *FilesRoot(const PicoHost *app)
{
    return PicoWorkspace_Path(PicoHost_SelectedWorkspaceConst(app));
}

static void FilesRebuild(PicoHost *app)
{
    const char *root;
    FilesClear();
    root = FilesRoot(app);
    snprintf(g_root, sizeof(g_root), "%s", root);
    if (root[0])
    {
        Walk(root, "", 0);
    }
    g_scanned = true;
}

static int Fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static bool ContainsFold(const char *s, const char *needle)
{
    if (!needle || !needle[0])
    {
        return true;
    }
    for (; *s; s++)
    {
        const char *a = s;
        const char *b = needle;
        while (*a && *b && Fold((unsigned char)*a) == Fold((unsigned char)*b))
        {
            a++;
            b++;
        }
        if (!*b)
        {
            return true;
        }
    }
    return false;
}

static const char *BaseName(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int pico_files_complete(PicoHost *app, const char *prefix, PicoCompleteItem *out, int max, void *state)
{
    (void)state;
    uint64_t token_id = PicoComplete_TokenId();
    const char *root = FilesRoot(app);
    if (!g_scanned || token_id != g_token_id || strcmp(g_root, root) != 0)
    {
        FilesRebuild(app);
        g_token_id = token_id;
    }
    int n = 0;
    for (int pass = 0; pass < 2 && n < max; pass++)
    {
        for (int i = 0; i < g_file_count && n < max; i++)
        {
            const char *path = g_files[i];
            bool prefix_hit = ContainsFold(path, prefix) || ContainsFold(BaseName(path), prefix);
            if (!prefix_hit)
            {
                continue;
            }
            bool starts = true;
            const char *p = prefix;
            const char *s = path;
            while (*p)
            {
                if (Fold((unsigned char)*s) != Fold((unsigned char)*p))
                {
                    starts = false;
                    break;
                }
                s++;
                p++;
            }
            if ((pass == 0 && !starts) || (pass == 1 && starts))
            {
                continue;
            }
            bool dup = false;
            for (int j = 0; j < n; j++)
            {
                if (strcmp(out[j].label, path) == 0)
                {
                    dup = true;
                    break;
                }
            }
            if (dup)
            {
                continue;
            }
            snprintf(out[n].label, sizeof(out[n].label), "%s", path);
            out[n].detail[0] = '\0';
            snprintf(out[n].insert, sizeof(out[n].insert), "@%s", path);
            n++;
        }
    }
    return n;
}

static bool IsMentionChar(unsigned char c)
{
    return isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/';
}

static bool AlreadyAdded(char **paths, int n, const char *path)
{
    for (int i = 0; i < n; i++)
    {
        if (strcmp(paths[i], path) == 0)
        {
            return true;
        }
    }
    return false;
}

static void AppendFileBlock(JsonBuf *b, const char *abs, const char *body)
{
    JsonBuf_Puts(b, "\n\n<file name=\"");
    JsonBuf_Puts(b, abs);
    JsonBuf_Puts(b, "\">\n");
    JsonBuf_Puts(b, body);
    if (body[0] && body[strlen(body) - 1] != '\n')
    {
        JsonBuf_Putc(b, '\n');
    }
    JsonBuf_Puts(b, "</file>");
}

char *pico_files_expand_mentions(const char *workspace, const char *text, bool vision,
                                 char **parts_json_out)
{
    if (parts_json_out)
    {
        *parts_json_out = NULL;
    }
    if (!text || !text[0])
    {
        return NULL;
    }
    char *paths[32];
    int path_n = 0;
    for (int i = 0; text[i] && path_n < 32; i++)
    {
        if (text[i] != '@' || (i > 0 && !isspace((unsigned char)text[i - 1]) && text[i - 1] != '\n'))
        {
            continue;
        }
        int j = i + 1;
        while (text[j] && IsMentionChar((unsigned char)text[j]))
        {
            j++;
        }
        if (j <= i + 1)
        {
            continue;
        }
        char rel[FILES_PATH];
        int n = j - (i + 1);
        if (n >= (int)sizeof(rel))
        {
            n = (int)sizeof(rel) - 1;
        }
        memcpy(rel, text + i + 1, (size_t)n);
        rel[n] = '\0';
        char joined[4096];
        if (rel[0] == '/')
        {
            snprintf(joined, sizeof(joined), "%s", rel);
        }
        else if (!PicoPath_Format(joined, sizeof(joined), "%s/%s", workspace ? workspace : ".", rel))
        {
            continue;
        }
        char resolved[4096];
        if (!realpath(joined, resolved))
        {
            continue;
        }
        if (AlreadyAdded(paths, path_n, resolved))
        {
            continue;
        }
        paths[path_n++] = JsonDup(resolved);
        i = j - 1;
    }
    if (path_n == 0)
    {
        return NULL;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, text);
    PicoLlmPart media[32];
    memset(media, 0, sizeof(media));
    int media_n = 0;
    for (int i = 0; i < path_n; i++)
    {
        bool image = pico_canonical_is_image_path(paths[i]);
        bool audio = pico_canonical_is_audio_path(paths[i]);
        if (vision && (image || audio) && media_n < 32)
        {
            media[media_n].kind = image ? PICO_LLM_PART_IMAGE : PICO_LLM_PART_AUDIO;
            media[media_n].path = paths[i];
            media[media_n].mime = (char *)pico_canonical_mime_for_path(paths[i]);
            media_n++;
            continue;
        }
        size_t len = 0;
        char *src = Pico_ReadFile(paths[i], &len);
        if (!src)
        {
            free(paths[i]);
            paths[i] = NULL;
            continue;
        }
        if (memchr(src, '\0', len > 4096 ? 4096 : len))
        {
            AppendFileBlock(&b, paths[i], "(binary file omitted)");
        }
        else if (len > FILES_MAX_BYTES)
        {
            AppendFileBlock(&b, paths[i], "(file too large, omitted)");
        }
        else
        {
            AppendFileBlock(&b, paths[i], src);
        }
        free(src);
        free(paths[i]);
        paths[i] = NULL;
    }
    char *inline_text = JsonBuf_Steal(&b);
    if (media_n > 0 && parts_json_out)
    {
        PicoLlmPart parts[33];
        memset(parts, 0, sizeof(parts));
        int n = 0;
        parts[n].kind = PICO_LLM_PART_TEXT;
        parts[n].text = inline_text ? inline_text : (char *)"";
        n++;
        for (int i = 0; i < media_n; i++)
        {
            parts[n++] = media[i];
        }
        *parts_json_out = pico_canonical_parts_json(parts, n);
    }
    for (int i = 0; i < media_n; i++)
    {
        free(media[i].path);
    }
    return inline_text;
}

static void FilesBeforeSubmit(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    PicoHost *app = workspace ? workspace->host : NULL;
    PicoAgent *agent = PicoHost_FindAgent(app, event ? event->agent_id : 0);
    const char *root;
    (void)state;
    if (!app || app->submit_cancel || !app->composer.text)
    {
        return;
    }
    bool vision = false;
    PicoModel *model = PicoSettings_ActiveModel(app, agent);
    if (model)
    {
        vision = model->vision;
    }
    root = PicoAgent_WorkspacePath(agent);
    if (!root[0])
    {
        root = PicoWorkspace_Path(workspace);
    }
    char *parts = NULL;
    char *expanded = pico_files_expand_mentions(root[0] ? root : ".", app->composer.text, vision, &parts);
    if (!expanded)
    {
        return;
    }
    pico_host_set_agent_input(app, expanded);
    pico_host_set_agent_parts(app, parts);
}

static int FilesInit(PicoHost *app, void **state_out)
{
    (void)state_out;
    pico_host_add_completer(app, '@', false, pico_files_complete, NULL);
    pico_workspace_add_hook(PicoHost_PrimaryWorkspace(app), PICO_HOOK_BEFORE_SUBMIT, FilesBeforeSubmit);
    return 0;
}

void pico_files_reset(void)
{
    FilesClear();
    g_token_id = 0;
    g_root[0] = '\0';
}

static void FilesShutdown(PicoHost *app, void *state)
{
    (void)state;
    (void)app;
    pico_files_reset();
}

PicoExt pico_ext_files(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "files",
        .description = "Workspace file completion",
        .host_init = FilesInit,
        .host_shutdown = FilesShutdown,
    };
}
