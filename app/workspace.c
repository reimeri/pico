#define _POSIX_C_SOURCE 200809L

#include "workspace.h"

#include "json.h"
#include "path.h"

bool Pico_ConfigDir(char *out, size_t cap);

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#define WORKSPACE_METADATA_VERSION 1

typedef struct SavedWorkspace {
    char key[PICO_WORKSPACE_KEY_MAX];
    char path[PICO_WORKSPACE_PATH_MAX];
    char name[PICO_WORKSPACE_NAME_MAX];
    int order;
    bool collapsed;
} SavedWorkspace;

static void WorkspaceFree(PicoWorkspace *workspace)
{
    if (!workspace)
    {
        return;
    }
    free(workspace->sessions);
    memset(workspace, 0, sizeof(*workspace));
}

void PicoWorkspaceRegistry_Free(PicoWorkspaceRegistry *registry)
{
    if (!registry)
    {
        return;
    }
    for (int i = 0; i < registry->count; i++)
    {
        WorkspaceFree(&registry->items[i]);
    }
    free(registry->items);
    memset(registry, 0, sizeof(*registry));
}

static bool CopyString(char *out, size_t cap, const char *value)
{
    if (!out || cap == 0 || !value)
    {
        return false;
    }
    size_t length = strlen(value);
    if (length >= cap)
    {
        out[0] = '\0';
        return false;
    }
    memcpy(out, value, length + 1);
    return true;
}

static bool IsSafeKey(const char *key)
{
    if (!key || !key[0] || strcmp(key, ".") == 0 || strcmp(key, "..") == 0 ||
        strchr(key, '/'))
    {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
    {
        if (*p < 0x20 || *p == 0x7f)
        {
            return false;
        }
    }
    return true;
}

static bool ExistingDirectory(const char *path, char *canonical, size_t cap)
{
    char resolved[PICO_WORKSPACE_PATH_MAX];
    struct stat st;
    return path && realpath(path, resolved) && stat(resolved, &st) == 0 &&
           S_ISDIR(st.st_mode) && CopyString(canonical, cap, resolved);
}

static const char *PathBasename(const char *path)
{
    if (!path || !path[0])
    {
        return "Workspace";
    }
    const char *end = path + strlen(path);
    while (end > path + 1 && end[-1] == '/')
    {
        end--;
    }
    const char *base = end;
    while (base > path && base[-1] != '/')
    {
        base--;
    }
    return *base ? base : "/";
}

bool PicoWorkspace_EncodePath(const char *canonical_path, char *out, size_t cap)
{
    if (!canonical_path || canonical_path[0] != '/' || !out || cap == 0)
    {
        return false;
    }
    size_t needed = 5;
    for (const char *p = canonical_path + 1; *p; p++)
    {
        needed++;
    }
    if (needed > cap)
    {
        out[0] = '\0';
        return false;
    }
    char *dst = out;
    *dst++ = '-';
    *dst++ = '-';
    for (const char *p = canonical_path + 1; *p; p++)
    {
        *dst++ = *p == '/' ? '-' : *p;
    }
    *dst++ = '-';
    *dst++ = '-';
    *dst = '\0';
    return true;
}

static uint32_t PathHash(const char *path)
{
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char *p = (const unsigned char *)path; p && *p; p++)
    {
        hash ^= *p;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static bool RegistryReserve(PicoWorkspaceRegistry *registry, int count)
{
    if (count <= registry->capacity)
    {
        return true;
    }
    int capacity = registry->capacity ? registry->capacity * 2 : 8;
    while (capacity < count)
    {
        capacity *= 2;
    }
    PicoWorkspace *next = realloc(registry->items, (size_t)capacity * sizeof(*next));
    if (!next)
    {
        return false;
    }
    registry->items = next;
    registry->capacity = capacity;
    return true;
}

static PicoWorkspace *RegistryAppend(PicoWorkspaceRegistry *registry)
{
    if (!RegistryReserve(registry, registry->count + 1))
    {
        return NULL;
    }
    PicoWorkspace *workspace = &registry->items[registry->count++];
    memset(workspace, 0, sizeof(*workspace));
    return workspace;
}

static PicoWorkspace *FindKeyMutable(PicoWorkspaceRegistry *registry, const char *key)
{
    if (!registry || !key)
    {
        return NULL;
    }
    for (int i = 0; i < registry->count; i++)
    {
        if (strcmp(registry->items[i].key, key) == 0)
        {
            return &registry->items[i];
        }
    }
    return NULL;
}

static PicoWorkspace *FindPathMutable(PicoWorkspaceRegistry *registry, const char *path)
{
    if (!registry || !path || !path[0])
    {
        return NULL;
    }
    for (int i = 0; i < registry->count; i++)
    {
        if (strcmp(registry->items[i].path, path) == 0)
        {
            return &registry->items[i];
        }
    }
    return NULL;
}

const PicoWorkspace *PicoWorkspaceRegistry_FindKey(const PicoWorkspaceRegistry *registry,
                                                   const char *key)
{
    return FindKeyMutable((PicoWorkspaceRegistry *)registry, key);
}

const PicoWorkspace *PicoWorkspaceRegistry_FindPath(const PicoWorkspaceRegistry *registry,
                                                    const char *path)
{
    if (!registry || !path)
    {
        return NULL;
    }
    char canonical[PICO_WORKSPACE_PATH_MAX];
    const char *query = ExistingDirectory(path, canonical, sizeof(canonical)) ? canonical : path;
    return FindPathMutable((PicoWorkspaceRegistry *)registry, query);
}

int PicoWorkspaceRegistry_Count(const PicoWorkspaceRegistry *registry)
{
    return registry ? registry->count : 0;
}

const PicoWorkspace *PicoWorkspaceRegistry_Get(const PicoWorkspaceRegistry *registry, int index)
{
    return registry && index >= 0 && index < registry->count ? &registry->items[index] : NULL;
}

bool PicoWorkspace_SessionDir(const PicoWorkspaceRegistry *registry, const char *key,
                              char *out, size_t cap)
{
    return registry && IsSafeKey(key) &&
           PicoPath_Format(out, cap, "%s/%s", registry->sessions_dir, key);
}

static bool IsJsonl(const char *name)
{
    size_t length = name ? strlen(name) : 0;
    return length > 6 && name[0] != '.' && strcmp(name + length - 6, ".jsonl") == 0;
}

static bool ReadRegularFileAt(int directory_fd, const char *name, char **out, size_t *out_length)
{
    *out = NULL;
    if (out_length)
    {
        *out_length = 0;
    }
    int fd = openat(directory_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
    {
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 4 * 1024 * 1024)
    {
        close(fd);
        return false;
    }
    size_t length = (size_t)st.st_size;
    char *data = malloc(length + 1);
    if (!data)
    {
        close(fd);
        return false;
    }
    size_t offset = 0;
    while (offset < length)
    {
        ssize_t got = read(fd, data + offset, length - offset);
        if (got > 0)
        {
            offset += (size_t)got;
        }
        else if (got < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            free(data);
            close(fd);
            return false;
        }
    }
    data[length] = '\0';
    close(fd);
    *out = data;
    if (out_length)
    {
        *out_length = length;
    }
    return true;
}

static bool ParseSessionHeader(const char *data, size_t length, char *cwd, size_t cwd_cap,
                               PicoWorkspaceSessionCache *cache)
{
    const char *newline = memchr(data, '\n', length);
    size_t line_length = newline ? (size_t)(newline - data) : length;
    while (line_length > 0 && data[line_length - 1] == '\r')
    {
        line_length--;
    }
    JsonDoc doc;
    if (line_length == 0 || JsonParse(&doc, data, line_length) != 0)
    {
        return false;
    }
    char *type = JsonObjStr(&doc, 0, "type");
    char *header_cwd = JsonObjStr(&doc, 0, "cwd");
    char *id = JsonObjStr(&doc, 0, "id");
    char *kind = JsonObjStr(&doc, 0, "kind");
    char *model = JsonObjStr(&doc, 0, "model");
    int version = JsonObjInt(&doc, 0, "version", -1);
    bool ok = type && strcmp(type, "session") == 0 && version == 3 && header_cwd &&
              header_cwd[0] == '/' && id && id[0] && kind && strcmp(kind, "normal") == 0 &&
              CopyString(cwd, cwd_cap, header_cwd);
    if (ok && cache)
    {
        CopyString(cache->id, sizeof(cache->id), id);
        if (model)
        {
            CopyString(cache->model, sizeof(cache->model), model);
        }
    }
    free(type);
    free(header_cwd);
    free(id);
    free(kind);
    free(model);
    JsonFree(&doc);
    return ok;
}

static void MakeTitle(char *out, size_t cap, const char *source)
{
    if (!source)
    {
        CopyString(out, cap, "Untitled");
        return;
    }
    while (*source && isspace((unsigned char)*source))
    {
        source++;
    }
    size_t used = 0;
    bool pending_space = false;
    for (const char *p = source; *p && used < 72 && used + 1 < cap; p++)
    {
        if (isspace((unsigned char)*p))
        {
            pending_space = used > 0;
            continue;
        }
        if (pending_space && used + 2 < cap && used < 72)
        {
            out[used++] = ' ';
        }
        pending_space = false;
        out[used++] = *p;
    }
    out[used] = '\0';
    if (!used)
    {
        CopyString(out, cap, "Untitled");
    }
}

static void ScanPresentationLine(const char *line, size_t length,
                                 PicoWorkspaceSessionCache *cache, bool *title_set)
{
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
    {
        length--;
    }
    JsonDoc doc;
    if (length == 0 || JsonParse(&doc, line, length) != 0)
    {
        return;
    }
    char *type = JsonObjStr(&doc, 0, "type");
    if (!*title_set && type && strcmp(type, "message") == 0)
    {
        char *role = JsonObjStr(&doc, 0, "role");
        if (role && strcmp(role, "user") == 0)
        {
            char *display = JsonObjStr(&doc, 0, "display");
            char *content = JsonObjStr(&doc, 0, "content");
            MakeTitle(cache->title, sizeof(cache->title),
                      display && display[0] ? display : content);
            *title_set = true;
            free(display);
            free(content);
        }
        free(role);
    }
    else if (type && strcmp(type, "model_change") == 0)
    {
        char *model = JsonObjStr(&doc, 0, "model");
        char *effort = JsonObjStr(&doc, 0, "effort");
        if (model)
        {
            CopyString(cache->model, sizeof(cache->model), model);
        }
        if (effort)
        {
            CopyString(cache->effort, sizeof(cache->effort), effort);
        }
        free(model);
        free(effort);
    }
    free(type);
    JsonFree(&doc);
}

static bool ScanSessionAt(int directory_fd, const char *name, struct stat *st,
                          char *cwd, size_t cwd_cap, PicoWorkspaceSessionCache *cache)
{
    int fd = openat(directory_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, st) != 0 || !S_ISREG(st->st_mode))
    {
        if (fd >= 0)
        {
            close(fd);
        }
        return false;
    }
    FILE *file = fdopen(fd, "rb");
    if (!file)
    {
        close(fd);
        return false;
    }
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, file);
    bool ok = length >= 0 && ParseSessionHeader(line, (size_t)length, cwd, cwd_cap, cache);
    bool title_set = false;
    while (ok && (length = getline(&line, &capacity, file)) >= 0)
    {
        ScanPresentationLine(line, (size_t)length, cache, &title_set);
    }
    if (ok && !title_set)
    {
        CopyString(cache->title, sizeof(cache->title), "Untitled");
    }
    free(line);
    fclose(file);
    return ok;
}

static bool SessionAppend(PicoWorkspace *workspace, const PicoWorkspaceSessionCache *session)
{
    PicoWorkspaceSessionCache *next = realloc(
        workspace->sessions, (size_t)(workspace->session_count + 1) * sizeof(*next));
    if (!next)
    {
        return false;
    }
    workspace->sessions = next;
    workspace->sessions[workspace->session_count++] = *session;
    return true;
}

static int CompareSessionMtime(const void *left, const void *right)
{
    const PicoWorkspaceSessionCache *a = left;
    const PicoWorkspaceSessionCache *b = right;
    if (a->mtime != b->mtime)
    {
        return a->mtime > b->mtime ? -1 : 1;
    }
    return strcmp(a->id, b->id);
}

static bool InspectWorkspaceDirectory(int root_fd, const char *key, PicoWorkspace *workspace,
                                      bool *mixed)
{
    *mixed = false;
    int directory_fd = openat(root_fd, key, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd < 0)
    {
        return false;
    }
    CopyString(workspace->key, sizeof(workspace->key), key);

    char *manifest = NULL;
    size_t manifest_length = 0;
    if (ReadRegularFileAt(directory_fd, ".workspace.json", &manifest, &manifest_length))
    {
        JsonDoc doc;
        if (JsonParse(&doc, manifest, manifest_length) == 0)
        {
            if (JsonObjInt(&doc, 0, "version", -1) == WORKSPACE_METADATA_VERSION)
            {
                char *manifest_key = JsonObjStr(&doc, 0, "key");
                char *manifest_path = JsonObjStr(&doc, 0, "path");
                if (manifest_key && manifest_path && strcmp(manifest_key, key) == 0 &&
                    manifest_path[0] == '/')
                {
                    CopyString(workspace->path, sizeof(workspace->path), manifest_path);
                }
                free(manifest_key);
                free(manifest_path);
            }
            JsonFree(&doc);
        }
        free(manifest);
    }

    int scan_fd = dup(directory_fd);
    DIR *directory = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!directory)
    {
        if (scan_fd >= 0)
        {
            close(scan_fd);
        }
        close(directory_fd);
        return false;
    }
    char inferred[PICO_WORKSPACE_PATH_MAX] = "";
    struct dirent *entry;
    while ((entry = readdir(directory)))
    {
        if (!IsJsonl(entry->d_name))
        {
            continue;
        }
        struct stat st;
        if (fstatat(directory_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(st.st_mode))
        {
            continue;
        }
        PicoWorkspaceSessionCache cache;
        char cwd[PICO_WORKSPACE_PATH_MAX];
        memset(&cache, 0, sizeof(cache));
        if (!ScanSessionAt(directory_fd, entry->d_name, &st, cwd, sizeof(cwd), &cache))
        {
            continue;
        }
        if (!inferred[0])
        {
            CopyString(inferred, sizeof(inferred), cwd);
        }
        else if (strcmp(inferred, cwd) != 0)
        {
            *mixed = true;
        }
        if (workspace->path[0] && strcmp(workspace->path, cwd) != 0)
        {
            *mixed = true;
        }
        cache.mtime = st.st_mtime;
        if (!SessionAppend(workspace, &cache))
        {
            closedir(directory);
            close(directory_fd);
            return false;
        }
    }
    closedir(directory);
    close(directory_fd);
    if (!workspace->path[0] && inferred[0] && !*mixed)
    {
        CopyString(workspace->path, sizeof(workspace->path), inferred);
    }
    workspace->available = false;
    if (workspace->path[0] && !*mixed)
    {
        char canonical[PICO_WORKSPACE_PATH_MAX];
        if (ExistingDirectory(workspace->path, canonical, sizeof(canonical)) &&
            strcmp(canonical, workspace->path) == 0)
        {
            workspace->available = true;
        }
    }
    if (*mixed)
    {
        workspace->available = false;
        workspace->path[0] = '\0';
    }
    if (workspace->session_count > 1)
    {
        qsort(workspace->sessions, (size_t)workspace->session_count,
              sizeof(*workspace->sessions), CompareSessionMtime);
    }
    return true;
}

static void FreeSaved(SavedWorkspace *saved)
{
    free(saved);
}

static int ReadSavedMetadata(int root_fd, SavedWorkspace **out)
{
    *out = NULL;
    char *data = NULL;
    size_t length = 0;
    if (!ReadRegularFileAt(root_fd, "workspaces.json", &data, &length))
    {
        return 0;
    }
    JsonDoc doc;
    if (JsonParse(&doc, data, length) != 0)
    {
        free(data);
        return 0;
    }
    if (JsonObjInt(&doc, 0, "version", -1) != WORKSPACE_METADATA_VERSION)
    {
        JsonFree(&doc);
        free(data);
        return 0;
    }
    int array = JsonObjGet(&doc, 0, "workspaces");
    int count = JsonIsArray(&doc, array) ? JsonArrayLen(&doc, array) : 0;
    SavedWorkspace *saved = count > 0 ? calloc((size_t)count, sizeof(*saved)) : NULL;
    if (count > 0 && !saved)
    {
        JsonFree(&doc);
        free(data);
        return -1;
    }
    int used = 0;
    for (int i = 0; i < count; i++)
    {
        int object = JsonArrayAt(&doc, array, i);
        char *key = JsonObjStr(&doc, object, "key");
        char *path = JsonObjStr(&doc, object, "path");
        char *name = JsonObjStr(&doc, object, "name");
        int collapsed_token = JsonObjGet(&doc, object, "collapsed");
        if (IsSafeKey(key) && path && path[0] == '/' && name && name[0] &&
            CopyString(saved[used].key, sizeof(saved[used].key), key) &&
            CopyString(saved[used].path, sizeof(saved[used].path), path) &&
            CopyString(saved[used].name, sizeof(saved[used].name), name))
        {
            saved[used].order = JsonObjInt(&doc, object, "order", i);
            saved[used].collapsed = JsonEq(&doc, collapsed_token, "true");
            used++;
        }
        free(key);
        free(path);
        free(name);
    }
    JsonFree(&doc);
    free(data);
    *out = saved;
    return used;
}

static const SavedWorkspace *FindSaved(const SavedWorkspace *saved, int count, const char *key)
{
    for (int i = 0; i < count; i++)
    {
        if (strcmp(saved[i].key, key) == 0)
        {
            return &saved[i];
        }
    }
    return NULL;
}

static int CompareWorkspaceOrder(const void *left, const void *right)
{
    const PicoWorkspace *a = left;
    const PicoWorkspace *b = right;
    if (a->order != b->order)
    {
        return a->order < b->order ? -1 : 1;
    }
    return strcmp(a->key, b->key);
}

static PicoWorkspaceResult Reconcile(PicoWorkspaceRegistry *registry, int root_fd)
{
    SavedWorkspace *saved = NULL;
    int saved_count = ReadSavedMetadata(root_fd, &saved);
    if (saved_count < 0)
    {
        return PICO_WORKSPACE_NO_MEMORY;
    }
    for (int i = 0; i < registry->count; i++)
    {
        WorkspaceFree(&registry->items[i]);
    }
    registry->count = 0;

    int scan_fd = dup(root_fd);
    DIR *directory = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!directory)
    {
        if (scan_fd >= 0)
        {
            close(scan_fd);
        }
        FreeSaved(saved);
        return PICO_WORKSPACE_IO_ERROR;
    }
    int next_order = saved_count;
    struct dirent *entry;
    while ((entry = readdir(directory)))
    {
        if (!IsSafeKey(entry->d_name))
        {
            continue;
        }
        struct stat st;
        if (fstatat(root_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISDIR(st.st_mode))
        {
            continue;
        }
        PicoWorkspace *workspace = RegistryAppend(registry);
        if (!workspace)
        {
            closedir(directory);
            FreeSaved(saved);
            return PICO_WORKSPACE_NO_MEMORY;
        }
        bool mixed = false;
        if (!InspectWorkspaceDirectory(root_fd, entry->d_name, workspace, &mixed))
        {
            registry->count--;
            WorkspaceFree(workspace);
            continue;
        }
        const SavedWorkspace *metadata = FindSaved(saved, saved_count, workspace->key);
        if (metadata)
        {
            if (!workspace->path[0] && !mixed)
            {
                CopyString(workspace->path, sizeof(workspace->path), metadata->path);
            }
            CopyString(workspace->name, sizeof(workspace->name), metadata->name);
            workspace->order = metadata->order;
            workspace->collapsed = metadata->collapsed;
            if (workspace->path[0] && !mixed)
            {
                char canonical[PICO_WORKSPACE_PATH_MAX];
                workspace->available = ExistingDirectory(workspace->path, canonical,
                                                         sizeof(canonical)) &&
                                       strcmp(canonical, workspace->path) == 0;
            }
        }
        else
        {
            workspace->order = next_order++;
        }
        if (!workspace->name[0])
        {
            CopyString(workspace->name, sizeof(workspace->name),
                       workspace->path[0] ? PathBasename(workspace->path) : workspace->key);
        }
    }
    closedir(directory);
    FreeSaved(saved);
    if (registry->count > 1)
    {
        qsort(registry->items, (size_t)registry->count, sizeof(*registry->items),
              CompareWorkspaceOrder);
    }
    for (int i = 0; i < registry->count; i++)
    {
        registry->items[i].order = i;
    }
    return PICO_WORKSPACE_OK;
}

static bool LockRoot(int root_fd, int *lock_fd)
{
    *lock_fd = openat(root_fd, ".workspaces.lock",
                      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (*lock_fd < 0)
    {
        return false;
    }
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    while (fcntl(*lock_fd, F_SETLKW, &lock) != 0)
    {
        if (errno != EINTR)
        {
            close(*lock_fd);
            *lock_fd = -1;
            return false;
        }
    }
    return true;
}

static void UnlockRoot(int lock_fd)
{
    if (lock_fd < 0)
    {
        return;
    }
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    (void)fcntl(lock_fd, F_SETLK, &lock);
    close(lock_fd);
}

static bool WriteAll(int fd, const char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length)
    {
        ssize_t written = write(fd, data + offset, length - offset);
        if (written > 0)
        {
            offset += (size_t)written;
        }
        else if (written < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            return false;
        }
    }
    return true;
}

static bool AtomicWriteAt(int directory_fd, const char *name, const char *data, size_t length)
{
    char temporary[128];
    int fd = -1;
    for (unsigned attempt = 0; attempt < 100; attempt++)
    {
        snprintf(temporary, sizeof(temporary), ".%s.tmp.%ld.%u", name, (long)getpid(), attempt);
        fd = openat(directory_fd, temporary,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST)
        {
            break;
        }
    }
    if (fd < 0)
    {
        return false;
    }
    bool ok = WriteAll(fd, data, length) && fsync(fd) == 0;
    if (close(fd) != 0)
    {
        ok = false;
    }
    if (ok)
    {
        struct stat st;
        if (fstatat(directory_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            !S_ISREG(st.st_mode))
        {
            ok = false;
        }
    }
    if (ok && renameat(directory_fd, temporary, directory_fd, name) != 0)
    {
        ok = false;
    }
    if (ok && fsync(directory_fd) != 0)
    {
        ok = false;
    }
    if (!ok)
    {
        unlinkat(directory_fd, temporary, 0);
    }
    return ok;
}

static char *RegistryJson(const PicoWorkspaceRegistry *registry)
{
    JsonBuf json;
    JsonBuf_Init(&json);
    JsonBuf_Puts(&json, "{\"version\":1,\"workspaces\":[");
    for (int i = 0; i < registry->count; i++)
    {
        const PicoWorkspace *workspace = &registry->items[i];
        if (i)
        {
            JsonBuf_Putc(&json, ',');
        }
        JsonBuf_Puts(&json, "{\"key\":");
        JsonBuf_String(&json, workspace->key);
        JsonBuf_Puts(&json, ",\"path\":");
        JsonBuf_String(&json, workspace->path);
        JsonBuf_Puts(&json, ",\"name\":");
        JsonBuf_String(&json, workspace->name);
        JsonBuf_Puts(&json, ",\"order\":");
        JsonBuf_Int(&json, workspace->order);
        JsonBuf_Puts(&json, ",\"collapsed\":");
        JsonBuf_Bool(&json, workspace->collapsed);
        JsonBuf_Puts(&json, ",\"sessions\":[");
        for (int j = 0; j < workspace->session_count; j++)
        {
            const PicoWorkspaceSessionCache *session = &workspace->sessions[j];
            if (j)
            {
                JsonBuf_Putc(&json, ',');
            }
            JsonBuf_Puts(&json, "{\"id\":");
            JsonBuf_String(&json, session->id);
            JsonBuf_Puts(&json, ",\"title\":");
            JsonBuf_String(&json, session->title);
            JsonBuf_Puts(&json, ",\"model\":");
            JsonBuf_String(&json, session->model);
            JsonBuf_Puts(&json, ",\"effort\":");
            JsonBuf_String(&json, session->effort);
            JsonBuf_Puts(&json, ",\"mtime\":");
            char number[64];
            snprintf(number, sizeof(number), "%lld", (long long)session->mtime);
            JsonBuf_Puts(&json, number);
            JsonBuf_Putc(&json, '}');
        }
        JsonBuf_Puts(&json, "]}");
    }
    JsonBuf_Puts(&json, "]}\n");
    return JsonBuf_Steal(&json);
}

static bool WriteManifest(int root_fd, const PicoWorkspace *workspace)
{
    int directory_fd = openat(root_fd, workspace->key,
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd < 0)
    {
        return false;
    }
    JsonBuf json;
    JsonBuf_Init(&json);
    JsonBuf_Puts(&json, "{\"version\":1,\"key\":");
    JsonBuf_String(&json, workspace->key);
    JsonBuf_Puts(&json, ",\"path\":");
    JsonBuf_String(&json, workspace->path);
    JsonBuf_Puts(&json, "}\n");
    bool ok = json.data && AtomicWriteAt(directory_fd, ".workspace.json", json.data, json.len);
    JsonBuf_Free(&json);
    close(directory_fd);
    return ok;
}

static PicoWorkspaceResult Save(PicoWorkspaceRegistry *registry, int root_fd)
{
    char *json = RegistryJson(registry);
    if (!json)
    {
        return PICO_WORKSPACE_NO_MEMORY;
    }
    bool ok = AtomicWriteAt(root_fd, "workspaces.json", json, strlen(json));
    free(json);
    if (!ok)
    {
        return PICO_WORKSPACE_IO_ERROR;
    }
    return PICO_WORKSPACE_OK;
}

static int OpenDirectoryHierarchy(const char *path)
{
    if (!path || !path[0])
    {
        errno = EINVAL;
        return -1;
    }
    char copy[PICO_WORKSPACE_PATH_MAX];
    if (!CopyString(copy, sizeof(copy), path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    int current = open(path[0] == '/' ? "/" : ".",
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (current < 0)
    {
        return -1;
    }
    char *cursor = copy + (path[0] == '/');
    char *save = NULL;
    for (char *component = strtok_r(cursor, "/", &save); component;
         component = strtok_r(NULL, "/", &save))
    {
        if (strcmp(component, ".") == 0)
        {
            continue;
        }
        if (strcmp(component, "..") == 0)
        {
            close(current);
            errno = EINVAL;
            return -1;
        }
        if (mkdirat(current, component, 0700) != 0 && errno != EEXIST)
        {
            close(current);
            return -1;
        }
        int next = openat(current, component,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0)
        {
            close(current);
            return -1;
        }
        close(current);
        current = next;
    }
    return current;
}

static int OpenRoot(PicoWorkspaceRegistry *registry)
{
    char config[PICO_WORKSPACE_PATH_MAX];
    if (!Pico_ConfigDir(config, sizeof(config)) ||
        !PicoPath_Format(registry->sessions_dir, sizeof(registry->sessions_dir),
                         "%s/sessions", config))
    {
        return -1;
    }
    int config_fd = OpenDirectoryHierarchy(config);
    if (config_fd < 0)
    {
        return -1;
    }
    if (mkdirat(config_fd, "sessions", 0700) != 0 && errno != EEXIST)
    {
        close(config_fd);
        return -1;
    }
    int root_fd = openat(config_fd, "sessions",
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(config_fd);
    return root_fd;
}

PicoWorkspaceResult PicoWorkspaceRegistry_Refresh(PicoWorkspaceRegistry *registry)
{
    if (!registry)
    {
        return PICO_WORKSPACE_INVALID;
    }
    int root_fd = OpenRoot(registry);
    if (root_fd < 0)
    {
        return PICO_WORKSPACE_IO_ERROR;
    }
    int lock_fd = -1;
    if (!LockRoot(root_fd, &lock_fd))
    {
        close(root_fd);
        return PICO_WORKSPACE_IO_ERROR;
    }
    PicoWorkspaceResult result = Reconcile(registry, root_fd);
    if (result == PICO_WORKSPACE_OK)
    {
        result = Save(registry, root_fd);
    }
    UnlockRoot(lock_fd);
    close(root_fd);
    return result;
}

PicoWorkspaceResult PicoWorkspaceRegistry_Init(PicoWorkspaceRegistry *registry)
{
    if (!registry)
    {
        return PICO_WORKSPACE_INVALID;
    }
    memset(registry, 0, sizeof(*registry));
    return PicoWorkspaceRegistry_Refresh(registry);
}

static bool KeyForPath(PicoWorkspaceRegistry *registry, const char *canonical,
                       char *key, size_t cap)
{
    char base[PICO_WORKSPACE_KEY_MAX];
    if (!PicoWorkspace_EncodePath(canonical, base, sizeof(base)))
    {
        return false;
    }
    for (unsigned probe = 0; probe < 10000; probe++)
    {
        char suffix[32] = "";
        if (probe == 0)
        {
            /* Preserve the existing encoded directory key whenever available. */
        }
        else if (probe == 1)
        {
            snprintf(suffix, sizeof(suffix), "-%08x", PathHash(canonical));
        }
        else
        {
            snprintf(suffix, sizeof(suffix), "-%08x-%u", PathHash(canonical), probe - 1);
        }
        size_t base_length = strlen(base);
        size_t suffix_length = strlen(suffix);
        if (base_length + suffix_length >= cap)
        {
            return false;
        }
        memcpy(key, base, base_length);
        memcpy(key + base_length, suffix, suffix_length + 1);
        const PicoWorkspace *existing = FindKeyMutable(registry, key);
        if (!existing || strcmp(existing->path, canonical) == 0)
        {
            return true;
        }
    }
    return false;
}

PicoWorkspaceResult PicoWorkspaceRegistry_Register(PicoWorkspaceRegistry *registry,
                                                   const char *path, const char *name,
                                                   char *key_out, size_t key_cap)
{
    if (!registry || !path || !path[0] || (key_out && key_cap == 0))
    {
        return PICO_WORKSPACE_INVALID;
    }
    char canonical[PICO_WORKSPACE_PATH_MAX];
    if (!ExistingDirectory(path, canonical, sizeof(canonical)))
    {
        return PICO_WORKSPACE_INVALID;
    }
    const char *display = name && name[0] ? name : PathBasename(canonical);
    if (!display[0] || strlen(display) >= PICO_WORKSPACE_NAME_MAX)
    {
        return PICO_WORKSPACE_INVALID;
    }
    int root_fd = OpenRoot(registry);
    int lock_fd = -1;
    if (root_fd < 0 || !LockRoot(root_fd, &lock_fd))
    {
        if (root_fd >= 0)
        {
            close(root_fd);
        }
        return PICO_WORKSPACE_IO_ERROR;
    }
    PicoWorkspaceResult result = Reconcile(registry, root_fd);
    PicoWorkspace *workspace = result == PICO_WORKSPACE_OK
                                   ? FindPathMutable(registry, canonical)
                                   : NULL;
    char key[PICO_WORKSPACE_KEY_MAX] = "";
    bool created = false;
    if (result == PICO_WORKSPACE_OK)
    {
        if (workspace)
        {
            CopyString(key, sizeof(key), workspace->key);
        }
        else if (!KeyForPath(registry, canonical, key, sizeof(key)))
        {
            result = PICO_WORKSPACE_INVALID;
        }
    }
    if (result == PICO_WORKSPACE_OK && key_out && strlen(key) >= key_cap)
    {
        result = PICO_WORKSPACE_INVALID;
    }
    if (result == PICO_WORKSPACE_OK && !workspace)
    {
        if (mkdirat(root_fd, key, 0700) != 0)
        {
            result = PICO_WORKSPACE_IO_ERROR;
        }
        else
        {
            workspace = RegistryAppend(registry);
            if (!workspace)
            {
                unlinkat(root_fd, key, AT_REMOVEDIR);
                result = PICO_WORKSPACE_NO_MEMORY;
            }
            else
            {
                created = true;
                CopyString(workspace->key, sizeof(workspace->key), key);
                CopyString(workspace->path, sizeof(workspace->path), canonical);
                workspace->available = true;
                workspace->order = registry->count - 1;
            }
        }
    }
    if (result == PICO_WORKSPACE_OK && workspace)
    {
        CopyString(workspace->name, sizeof(workspace->name), display);
        if (!WriteManifest(root_fd, workspace))
        {
            result = PICO_WORKSPACE_IO_ERROR;
        }
        else
        {
            result = Save(registry, root_fd);
        }
    }
    if (result != PICO_WORKSPACE_OK && created)
    {
        int directory_fd = openat(root_fd, key,
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory_fd >= 0)
        {
            (void)unlinkat(directory_fd, ".workspace.json", 0);
            close(directory_fd);
        }
        (void)unlinkat(root_fd, key, AT_REMOVEDIR);
        WorkspaceFree(&registry->items[registry->count - 1]);
        registry->count--;
    }
    if (result == PICO_WORKSPACE_OK && key_out)
    {
        CopyString(key_out, key_cap, key);
    }
    UnlockRoot(lock_fd);
    close(root_fd);
    return result;
}

static PicoWorkspaceResult MutateMetadata(PicoWorkspaceRegistry *registry, const char *key,
                                          const char *name, bool set_collapsed, bool collapsed)
{
    if (!registry || !IsSafeKey(key))
    {
        return PICO_WORKSPACE_INVALID;
    }
    int root_fd = OpenRoot(registry);
    int lock_fd = -1;
    if (root_fd < 0 || !LockRoot(root_fd, &lock_fd))
    {
        if (root_fd >= 0)
        {
            close(root_fd);
        }
        return PICO_WORKSPACE_IO_ERROR;
    }
    PicoWorkspaceResult result = Reconcile(registry, root_fd);
    PicoWorkspace *workspace = result == PICO_WORKSPACE_OK ? FindKeyMutable(registry, key) : NULL;
    if (result == PICO_WORKSPACE_OK && !workspace)
    {
        result = PICO_WORKSPACE_NOT_FOUND;
    }
    if (result == PICO_WORKSPACE_OK && name &&
        (!name[0] || !CopyString(workspace->name, sizeof(workspace->name), name)))
    {
        result = PICO_WORKSPACE_INVALID;
    }
    if (result == PICO_WORKSPACE_OK && set_collapsed)
    {
        workspace->collapsed = collapsed;
    }
    if (result == PICO_WORKSPACE_OK)
    {
        result = Save(registry, root_fd);
    }
    UnlockRoot(lock_fd);
    close(root_fd);
    return result;
}

PicoWorkspaceResult PicoWorkspaceRegistry_SetCollapsed(PicoWorkspaceRegistry *registry,
                                                       const char *key, bool collapsed)
{
    return MutateMetadata(registry, key, NULL, true, collapsed);
}

PicoWorkspaceResult PicoWorkspaceRegistry_SetName(PicoWorkspaceRegistry *registry,
                                                  const char *key, const char *name)
{
    return MutateMetadata(registry, key, name, false, false);
}
