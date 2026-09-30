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
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdatomic.h>

#define FILES_MAX 8000
#define FILES_PATH 512
#define FILES_MAX_BYTES (1024 * 1024)
#define FILES_WALK_DEPTH 12

typedef struct FilesList {
    char **files;
    int file_count;
    int file_cap;
} FilesList;

typedef struct FilesRebuildWorker {
    atomic_bool cancelled;
    PicoWorkspaceId workspace_id;
    uint64_t token_id;
    uint64_t serial;
    char root[4096];
    char **files;
    int file_count;
    int file_cap;
} FilesRebuildWorker;

typedef struct FilesState {
    PicoWorkspace *workspace;
    char **files;
    int file_count;
    int file_cap;
    bool scanned;
    uint64_t token_id;
    char root[4096];
    uint64_t rebuild_serial;
    FilesRebuildWorker *rebuild_worker;
    char cache_prefix[512];
    PicoCompleteItem cache_items[PICO_MAX_COMPLETE_ITEMS];
    int cache_n;
    int cache_max;
    bool cache_valid;
} FilesState;

static bool SkipDirName(const char *name)
{
    if (!name || !name[0] || name[0] == '.')
    {
        return true;
    }
    return strcmp(name, "node_modules") == 0 || strcmp(name, "dist") == 0 || strcmp(name, "build") == 0 ||
           strcmp(name, "target") == 0 || strcmp(name, "__pycache__") == 0;
}

static void FilesListClear(FilesList *s)
{
    if (!s)
    {
        return;
    }
    for (int i = 0; i < s->file_count; i++)
    {
        free(s->files[i]);
    }
    free(s->files);
    s->files = NULL;
    s->file_count = 0;
    s->file_cap = 0;
}

static void FilesClear(FilesState *s)
{
    FilesList list;
    if (!s)
    {
        return;
    }
    list.files = s->files;
    list.file_count = s->file_count;
    list.file_cap = s->file_cap;
    FilesListClear(&list);
    s->files = NULL;
    s->file_count = 0;
    s->file_cap = 0;
    s->scanned = false;
    s->cache_valid = false;
}

static void FilesAdd(FilesList *s, const char *rel)
{
    if (!s || !rel || !rel[0] || s->file_count >= FILES_MAX)
    {
        return;
    }
    if (s->file_count >= s->file_cap)
    {
        int cap = s->file_cap == 0 ? 32 : s->file_cap * 2;
        char **next;
        if (cap > FILES_MAX)
        {
            cap = FILES_MAX;
        }
        next = (char **)realloc(s->files, (size_t)cap * sizeof(char *));
        if (!next)
        {
            return;
        }
        s->files = next;
        s->file_cap = cap;
    }
    s->files[s->file_count] = JsonDup(rel);
    if (s->files[s->file_count])
    {
        s->file_count++;
    }
}

static bool FilesCancelled(const atomic_bool *cancelled)
{
    return cancelled && atomic_load(cancelled);
}

static void Walk(FilesList *s, const char *root, const char *rel, int depth, const atomic_bool *cancelled)
{
    if (depth > FILES_WALK_DEPTH || !s || s->file_count >= FILES_MAX || FilesCancelled(cancelled))
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
    while ((ent = readdir(d)) != NULL && s->file_count < FILES_MAX)
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
            Walk(s, root, child, depth + 1, cancelled);
        }
        else if (S_ISREG(st.st_mode))
        {
            FilesAdd(s, child);
        }
    }
    closedir(d);
}

static void FilesAdoptList(FilesState *s, FilesList *list)
{
    if (!s)
    {
        return;
    }
    FilesClear(s);
    if (!list)
    {
        s->scanned = true;
        return;
    }
    s->files = list->files;
    s->file_count = list->file_count;
    s->file_cap = list->file_cap;
    list->files = NULL;
    list->file_count = 0;
    list->file_cap = 0;
    s->scanned = true;
    s->cache_valid = false;
}

static void FilesRebuild(FilesState *s, const char *root)
{
    FilesList list;
    memset(&list, 0, sizeof(list));
    if (!s)
    {
        return;
    }
    snprintf(s->root, sizeof(s->root), "%s", root ? root : "");
    if (root && root[0])
    {
        Walk(&list, root, "", 0, NULL);
    }
    FilesAdoptList(s, &list);
}

static void *FilesRebuildRun(void *arg)
{
    FilesRebuildWorker *worker = (FilesRebuildWorker *)arg;
    FilesList list;
    memset(&list, 0, sizeof(list));
    if (worker->root[0] && !atomic_load(&worker->cancelled))
    {
        Walk(&list, worker->root, "", 0, &worker->cancelled);
    }
    if (atomic_load(&worker->cancelled))
    {
        FilesListClear(&list);
        return NULL;
    }
    worker->files = list.files;
    worker->file_count = list.file_count;
    worker->file_cap = list.file_cap;
    return NULL;
}

static void FilesRebuildCancel(void *arg)
{
    atomic_store(&((FilesRebuildWorker *)arg)->cancelled, true);
}

static void FilesRebuildDestroy(void *arg)
{
    FilesRebuildWorker *worker = (FilesRebuildWorker *)arg;
    FilesList list;
    memset(&list, 0, sizeof(list));
    list.files = worker->files;
    list.file_count = worker->file_count;
    list.file_cap = worker->file_cap;
    FilesListClear(&list);
    free(worker);
}

static void FilesRebuildCompleted(PicoHost *host, void *arg)
{
    FilesRebuildWorker *worker = (FilesRebuildWorker *)arg;
    PicoWorkspace *workspace = PicoHost_FindWorkspace(host, worker->workspace_id);
    FilesState *s = workspace ? (FilesState *)PicoPlugins_WorkspaceState(workspace, "files") : NULL;
    FilesList list;
    if (s && s->rebuild_worker == worker)
    {
        s->rebuild_worker = NULL;
    }
    if (!s || s->rebuild_serial != worker->serial || atomic_load(&worker->cancelled))
    {
        return;
    }
    memset(&list, 0, sizeof(list));
    list.files = worker->files;
    list.file_count = worker->file_count;
    list.file_cap = worker->file_cap;
    worker->files = NULL;
    worker->file_count = 0;
    worker->file_cap = 0;
    snprintf(s->root, sizeof(s->root), "%s", worker->root);
    s->token_id = worker->token_id;
    FilesAdoptList(s, &list);
}

static bool FilesHostCanTask(PicoHost *host)
{
    return host && host->ask_id_mu_ready && !host->terminal_shutdown;
}

static bool FilesStartRebuild(FilesState *s, PicoHost *host, const char *root, uint64_t token_id)
{
    FilesRebuildWorker *worker;
    if (!s)
    {
        return false;
    }
    if (s->rebuild_worker)
    {
        atomic_store(&s->rebuild_worker->cancelled, true);
        s->rebuild_worker = NULL;
    }
    s->rebuild_serial++;
    if (s->rebuild_serial == 0)
    {
        s->rebuild_serial = 1;
    }
    FilesClear(s);
    snprintf(s->root, sizeof(s->root), "%s", root ? root : "");
    s->token_id = token_id;
    if (!FilesHostCanTask(host) || !s->workspace)
    {
        FilesRebuild(s, root);
        return true;
    }
    worker = (FilesRebuildWorker *)calloc(1, sizeof(*worker));
    if (!worker)
    {
        return false;
    }
    worker->workspace_id = s->workspace->id;
    worker->token_id = token_id;
    worker->serial = s->rebuild_serial;
    snprintf(worker->root, sizeof(worker->root), "%s", root ? root : "");
    if (!PicoHost_StartTaskCompleted(host, FilesRebuildRun, worker, FilesRebuildCancel,
                                     FilesRebuildCompleted, FilesRebuildDestroy))
    {
        free(worker);
        return false;
    }
    s->rebuild_worker = worker;
    return false;
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

int pico_files_complete(PicoWorkspace *workspace, const char *prefix, PicoCompleteItem *out, int max, void *state)
{
    FilesState *s = (FilesState *)state;
    if (!s)
    {
        s = (FilesState *)PicoPlugins_WorkspaceState(workspace, "files");
    }
    if (!s)
    {
        return 0;
    }
    uint64_t token_id = PicoComplete_TokenId();
    const char *root = (s->workspace && s->workspace->path[0]) ? s->workspace->path : s->root;
    if (!root[0] && workspace)
    {
        root = PicoWorkspace_Path(workspace);
    }
    if (s->rebuild_worker && s->token_id == token_id && strcmp(s->root, root) == 0)
    {
        return -1;
    }
    if (!s->scanned || token_id != s->token_id || strcmp(s->root, root) != 0)
    {
        PicoHost *host = pico_workspace_host(s->workspace ? s->workspace : workspace);
        FilesStartRebuild(s, host, root, token_id);
        if (!s->scanned)
        {
            return -1;
        }
    }
    if (s->cache_valid && s->cache_max == max &&
        strcmp(s->cache_prefix, prefix ? prefix : "") == 0)
    {
        int n = s->cache_n;
        if (n > max)
        {
            n = max;
        }
        if (n > 0)
        {
            memcpy(out, s->cache_items, (size_t)n * sizeof(*out));
        }
        return n;
    }
    int n = 0;
    for (int pass = 0; pass < 2 && n < max; pass++)
    {
        for (int i = 0; i < s->file_count && n < max; i++)
        {
            const char *path = s->files[i];
            bool prefix_hit = ContainsFold(path, prefix) || ContainsFold(BaseName(path), prefix);
            if (!prefix_hit)
            {
                continue;
            }
            bool starts = true;
            const char *p = prefix;
            const char *s_str = path;
            while (*p)
            {
                if (Fold((unsigned char)*s_str) != Fold((unsigned char)*p))
                {
                    starts = false;
                    break;
                }
                s_str++;
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
    s->cache_n = n;
    s->cache_max = max;
    if (n > 0)
    {
        memcpy(s->cache_items, out, (size_t)n * sizeof(*out));
    }
    snprintf(s->cache_prefix, sizeof(s->cache_prefix), "%s", prefix ? prefix : "");
    s->cache_valid = true;
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
        errno = 0;
        char *src = Pico_ReadFileLimited(paths[i], FILES_MAX_BYTES, &len);
        if (!src)
        {
            if (errno == EFBIG)
            {
                char note[80];
                if (len > 0)
                {
                    snprintf(note, sizeof(note), "(file too large, omitted, %.1f MB)",
                             (double)len / (1024.0 * 1024.0));
                }
                else
                {
                    snprintf(note, sizeof(note), "(file too large, omitted)");
                }
                AppendFileBlock(&b, paths[i], note);
            }
            free(paths[i]);
            paths[i] = NULL;
            continue;
        }
        if (memchr(src, '\0', len > 4096 ? 4096 : len))
        {
            AppendFileBlock(&b, paths[i], "(binary file omitted)");
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
    PicoModel *model = PicoSettings_SelectedModel(agent);
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
    const char *text = app->agent_input && app->agent_input[0] ? app->agent_input : app->composer.text;
    char *expanded = pico_files_expand_mentions(root[0] ? root : ".", text, vision, &parts);
    if (!expanded)
    {
        return;
    }
    pico_host_set_agent_input(app, expanded);
    pico_host_set_agent_parts(app, parts);
}

static int FilesWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    FilesState *s = (FilesState *)calloc(1, sizeof(FilesState));
    if (!s)
    {
        return 1;
    }
    if (!state_out)
    {
        free(s);
        return 1;
    }
    s->workspace = workspace;
    snprintf(s->root, sizeof(s->root), "%s", workspace ? workspace->path : "");
    *state_out = s;
    pico_workspace_add_completer(workspace, '@', false, pico_files_complete, NULL);
    pico_workspace_add_hook(workspace, PICO_HOOK_BEFORE_SUBMIT, FilesBeforeSubmit);
    return 0;
}

static void FilesWorkspaceShutdown(PicoWorkspace *workspace, void *state)
{
    (void)workspace;
    FilesState *s = (FilesState *)state;
    if (s)
    {
        if (s->rebuild_worker)
        {
            atomic_store(&s->rebuild_worker->cancelled, true);
            s->rebuild_worker = NULL;
        }
        s->rebuild_serial++;
        FilesClear(s);
        free(s);
    }
}

PicoExt pico_ext_files(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "files",
        .description = "Workspace file completion",
        .workspace_init = FilesWorkspaceInit,
        .workspace_shutdown = FilesWorkspaceShutdown,
    };
}
