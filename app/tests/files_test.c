#include "canonical.h"
#include "json.h"
#include "pico/app.h"
#include "host_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>

char *pico_files_expand_mentions(const char *workspace, const char *text, bool vision,
                                 char **parts_json_out);
int pico_files_complete(PicoWorkspace *workspace, const char *prefix, PicoCompleteItem *out, int max, void *state);
PicoExt pico_ext_files(void);
PicoExt pico_ext_composer(void);

static void *g_files_state = NULL;
static void *g_composer_state = NULL;

void *PicoPlugins_HostState(const PicoHost *host, const char *name)
{
    (void)host;
    if (name && strcmp(name, "composer") == 0)
    {
        return g_composer_state;
    }
    return NULL;
}

void *PicoPlugins_WorkspaceState(const PicoWorkspace *workspace, const char *name)
{
    (void)workspace;
    if (name && strcmp(name, "files") == 0)
    {
        return g_files_state;
    }
    return NULL;
}

void PicoPlugins_InitWorkspace(PicoHost *host, PicoWorkspace *workspace)
{
    (void)host;
    (void)workspace;
}

void PicoPlugins_CancelCompiles(PicoHost *host) { (void)host; }

void PicoPlugins_Poll(PicoHost *host) { (void)host; }

bool PicoPlugins_LoadWorkspaceSources(PicoHost *host, PicoWorkspace *workspace)
{
    (void)host;
    (void)workspace;
    return true;
}

static int g_failed;

static void Check(bool ok, const char *message)
{
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        g_failed = 1;
    }
}

static bool WriteFile(const char *path, const char *body)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        return false;
    }
    size_t n = strlen(body);
    bool ok = fwrite(body, 1, n, f) == n;
    fclose(f);
    return ok;
}

static bool QueryHas(PicoWorkspace *workspace, const char *prefix, const char *label)
{
    PicoCompleteItem items[PICO_MAX_COMPLETE_ITEMS];
    memset(items, 0, sizeof(items));
    int n = pico_files_complete(workspace, prefix, items, PICO_MAX_COMPLETE_ITEMS, NULL);
    for (int i = 0; i < n; i++)
    {
        if (strcmp(items[i].label, label) == 0)
        {
            return true;
        }
    }
    return false;
}

static void SetComposer(PicoHost *app, char *buf, size_t cap, const char *text)
{
    snprintf(buf, cap, "%s", text ? text : "");
    app->composer.text = buf;
    app->composer.length = (int)strlen(buf);
    app->composer.capacity = (int)cap;
    app->composer.cursor = app->composer.length;
}

static void TestMentionImagePart(void)
{
    char temp[] = "/tmp/pico-files-XXXXXX";
    if (!mkdtemp(temp))
    {
        fprintf(stderr, "FAIL: could not create temp dir\n");
        g_failed = 1;
        return;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s/pic.png", temp);
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        fprintf(stderr, "FAIL: could not write image\n");
        g_failed = 1;
        rmdir(temp);
        return;
    }
    fwrite("PNG\0bin", 1, 7, f);
    fclose(f);

    char *parts = NULL;
    char *expanded = pico_files_expand_mentions(temp, "see @pic.png", true, &parts);
    Check(parts && strstr(parts, "\"type\":\"image\"") && strstr(parts, "pic.png") &&
              expanded && !strstr(expanded, "(binary file omitted)"),
          "@ of a local image on a vision model produces a user image part, not (binary file omitted)");
    free(expanded);
    free(parts);

    char *normalized = NULL;
    bool invalid_parts = !pico_canonical_normalize_user_parts(
        "[{\"type\":\"video\",\"path\":\"clip.mp4\"}]", &normalized) && !normalized;
    Check(invalid_parts, "structured user parts reject unsupported media types");
    free(normalized);
    normalized = NULL;
    bool embedded_bytes = !pico_canonical_normalize_user_parts(
        "[{\"type\":\"image\",\"url\":\"data:image/png;base64,UE5H\"}]", &normalized) &&
                          !normalized;
    Check(embedded_bytes, "structured user parts reject embedded file bytes");
    free(normalized);
    unlink(path);
    rmdir(temp);
}

static void TestCompleteRebuildsAtTokenStart(void)
{
    char temp[] = "/tmp/pico-files-XXXXXX";
    if (!mkdtemp(temp))
    {
        fprintf(stderr, "FAIL: could not create complete temp dir\n");
        g_failed = 1;
        return;
    }
    char old_path[4096];
    char new_path[4096];
    snprintf(old_path, sizeof(old_path), "%s/old.txt", temp);
    snprintf(new_path, sizeof(new_path), "%s/new.txt", temp);
    if (!WriteFile(old_path, "old\n"))
    {
        fprintf(stderr, "FAIL: could not write old.txt\n");
        g_failed = 1;
        rmdir(temp);
        return;
    }

    PicoHost *app = (PicoHost *)calloc(1, sizeof(PicoHost));
    PicoWorkspace *workspace;
    if (!app)
    {
        fprintf(stderr, "FAIL: could not allocate PicoHost\n");
        g_failed = 1;
        unlink(old_path);
        rmdir(temp);
        return;
    }
    workspace = (PicoWorkspace *)calloc(1, sizeof(PicoWorkspace));
    workspace->host = app;
    workspace->id = 1;
    snprintf(workspace->path, sizeof(workspace->path), "%s", temp);
    workspace->state = PICO_WORKSPACE_OPEN;
    app->workspaces[0] = workspace;
    app->workspace_count = 1;
    PicoExt ext = pico_ext_files();
    ext.workspace_init(workspace, &g_files_state);
    PicoExt comp_ext = pico_ext_composer();
    comp_ext.host_init(app, &g_composer_state);
    app->completers[0] = (PicoCompleter){
        .trigger = '@',
        .bol_only = false,
        .workspace_query = pico_files_complete,
    };
    app->completer_count = 1;
    char composer[64];
    SetComposer(app, composer, sizeof(composer), "@");
    PicoComplete_Refresh(app);
    bool saw_old = QueryHas(workspace, "", "old.txt");

    if (!WriteFile(new_path, "new\n"))
    {
        fprintf(stderr, "FAIL: could not write new.txt\n");
        g_failed = 1;
        ext.workspace_shutdown(workspace, g_files_state);
        g_files_state = NULL;
        comp_ext.host_shutdown(app, g_composer_state);
        g_composer_state = NULL;
        free(workspace);
        free(app);
        unlink(old_path);
        rmdir(temp);
        return;
    }
    SetComposer(app, composer, sizeof(composer), "@n");
    PicoComplete_Refresh(app);
    bool same_token_misses_new = saw_old && !QueryHas(workspace, "n", "new.txt");
    Check(same_token_misses_new,
          "a file created after @ is typed stays out of the active token snapshot");

    PicoComposer_ReplaceRange(app, 0, app->composer.length, "@");
    PicoComplete_Refresh(app);
    Check(QueryHas(workspace, "", "new.txt"),
          "replacing a mention at the same offset starts a fresh file snapshot");

    ext.workspace_shutdown(workspace, g_files_state);
    g_files_state = NULL;
    comp_ext.host_shutdown(app, g_composer_state);
    g_composer_state = NULL;
    free(workspace);
    free(app);
    unlink(old_path);
    unlink(new_path);
    rmdir(temp);
}

static void TestBoundedMentions(void)
{
    char root[] = "/tmp/pico-bounded-files-XXXXXX";
    Check(mkdtemp(root) != NULL, "create bounded file fixture");
    char large[4096], fifo[4096];
    snprintf(large, sizeof(large), "%s/large.txt", root);
    snprintf(fifo, sizeof(fifo), "%s/pipe", root);
    FILE *file = fopen(large, "wb");
    Check(file && ftruncate(fileno(file), 256u * 1024u * 1024u) == 0, "create sparse oversized file");
    if (file) fclose(file);
    Check(mkfifo(fifo, 0600) == 0, "create FIFO fixture");
    pid_t pid = fork();
    if (pid == 0)
    {
        alarm(3);
        struct rlimit limit = {128u * 1024u * 1024u, 128u * 1024u * 1024u};
        if (setrlimit(RLIMIT_AS, &limit) != 0) _exit(2);
        char *result = pico_files_expand_mentions(root, "@large.txt @pipe", false, NULL);
        bool ok = result && strstr(result, "(file too large, omitted, 256.0 MB)");
        free(result);
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    Check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "oversized files are omitted within memory budget and FIFOs never block submission");
    unlink(large);
    unlink(fifo);
    rmdir(root);
}

int main(void)
{
    TestBoundedMentions();
    TestMentionImagePart();
    TestCompleteRebuildsAtTokenStart();
    return g_failed;
}
