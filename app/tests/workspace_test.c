#define _POSIX_C_SOURCE 200809L

#include "json.h"
#include "path.h"
#include "workspace.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static char g_config[4096];

bool Pico_ConfigDir(char *out, size_t cap)
{
    return PicoPath_Format(out, cap, "%s", g_config);
}

void Pico_MkdirP(const char *path)
{
    char buffer[4096];
    snprintf(buffer, sizeof(buffer), "%s", path ? path : "");
    for (char *p = buffer + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            (void)mkdir(buffer, 0700);
            *p = '/';
        }
    }
    (void)mkdir(buffer, 0700);
}

static int Fail(const char *message)
{
    fprintf(stderr, "workspace: %s\n", message);
    return 1;
}

static bool WriteText(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file)
    {
        return false;
    }
    bool ok = fputs(text, file) >= 0 && fclose(file) == 0;
    return ok;
}

static bool MakeDir(const char *path)
{
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static bool GrowPastMetadataLimit(const char *path)
{
    FILE *file = fopen(path, "ab");
    if (!file)
    {
        return false;
    }
    char chunk[4096];
    memset(chunk, 'x', sizeof(chunk));
    bool ok = true;
    for (int i = 0; i < 1100 && ok; i++)
    {
        ok = fwrite(chunk, 1, sizeof(chunk), file) == sizeof(chunk);
    }
    return fclose(file) == 0 && ok;
}

static void RemoveTree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
    {
        return;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
    {
        (void)unlink(path);
        return;
    }
    DIR *directory = opendir(path);
    if (directory)
    {
        struct dirent *entry;
        while ((entry = readdir(directory)))
        {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            {
                continue;
            }
            char child[4096];
            if (PicoPath_Format(child, sizeof(child), "%s/%s", path, entry->d_name))
            {
                RemoveTree(child);
            }
        }
        closedir(directory);
    }
    (void)rmdir(path);
}

static bool HasWorkspaceTemporary(const char *directory_path)
{
    DIR *directory = opendir(directory_path);
    if (!directory)
    {
        return false;
    }
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(directory)))
    {
        if (strncmp(entry->d_name, ".workspaces.json.tmp.", 21) == 0)
        {
            found = true;
            break;
        }
    }
    closedir(directory);
    return found;
}

static const PicoWorkspace *FindPath(const PicoWorkspaceRegistry *registry, const char *path)
{
    char canonical[4096];
    return realpath(path, canonical) ? PicoWorkspaceRegistry_FindPath(registry, canonical) : NULL;
}

static int TestRegistrationAndRecovery(const char *root)
{
    char project[4096];
    snprintf(project, sizeof(project), "%s/project", root);
    if (!MakeDir(project))
    {
        return Fail("could not create project");
    }
    PicoWorkspaceRegistry registry;
    if (PicoWorkspaceRegistry_Init(&registry) != PICO_WORKSPACE_OK)
    {
        return Fail("registry init failed");
    }
    char key[4096];
    if (PicoWorkspaceRegistry_Register(&registry, project, "Project name", key, sizeof(key)) !=
            PICO_WORKSPACE_OK ||
        PicoWorkspaceRegistry_Count(&registry) != 1)
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("registration failed");
    }
    const PicoWorkspace *workspace = PicoWorkspaceRegistry_FindKey(&registry, key);
    if (!workspace || !workspace->available || strcmp(workspace->name, "Project name") != 0)
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("registered presentation metadata was wrong");
    }
    if (PicoWorkspaceRegistry_SetCollapsed(&registry, key, true) != PICO_WORKSPACE_OK)
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("collapse mutation failed");
    }
    char metadata[4096];
    snprintf(metadata, sizeof(metadata), "%s/sessions/workspaces.json", root);
    if (!WriteText(metadata, "not json"))
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("could not corrupt root metadata");
    }
    if (PicoWorkspaceRegistry_Refresh(&registry) != PICO_WORKSPACE_OK ||
        PicoWorkspaceRegistry_Count(&registry) != 1 ||
        !(workspace = PicoWorkspaceRegistry_FindKey(&registry, key)) || !workspace->available)
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("manifest did not recover an empty workspace");
    }
    if (rmdir(project) != 0 || PicoWorkspaceRegistry_Refresh(&registry) != PICO_WORKSPACE_OK ||
        !(workspace = PicoWorkspaceRegistry_FindKey(&registry, key)) || workspace->available)
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("missing project was not retained as unavailable");
    }
    PicoWorkspaceRegistry_Free(&registry);
    return 0;
}

static int TestCollision(const char *root)
{
    char left[4096];
    char parent[4096];
    char right[4096];
    if (!PicoPath_Format(left, sizeof(left), "%s/collision-a-b", root) ||
        !PicoPath_Format(parent, sizeof(parent), "%s/collision-a", root) ||
        !PicoPath_Format(right, sizeof(right), "%s/b", parent))
    {
        return Fail("collision paths were too long");
    }
    if (!MakeDir(left) || !MakeDir(parent) || !MakeDir(right))
    {
        return Fail("could not create collision projects");
    }
    PicoWorkspaceRegistry registry;
    if (PicoWorkspaceRegistry_Init(&registry) != PICO_WORKSPACE_OK)
    {
        return Fail("collision registry init failed");
    }
    char left_key[4096];
    char right_key[4096];
    if (PicoWorkspaceRegistry_Register(&registry, left, NULL, left_key, sizeof(left_key)) !=
            PICO_WORKSPACE_OK ||
        PicoWorkspaceRegistry_Register(&registry, right, NULL, right_key, sizeof(right_key)) !=
            PICO_WORKSPACE_OK ||
        strcmp(left_key, right_key) == 0 || !strstr(right_key, "-"))
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("encoded collision did not receive a unique stable suffix");
    }
    char repeated[4096];
    if (PicoWorkspaceRegistry_Register(&registry, right, NULL, repeated, sizeof(repeated)) !=
            PICO_WORKSPACE_OK ||
        strcmp(right_key, repeated) != 0 || PicoWorkspaceRegistry_Count(&registry) < 2)
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("collision key was not stable");
    }
    PicoWorkspaceRegistry_Free(&registry);
    return 0;
}

static int TestInferenceAndMixedDirectory(const char *root)
{
    char project_a[4096];
    char project_b[4096];
    char sessions[4096];
    char inferred_dir[4096];
    if (!PicoPath_Format(project_a, sizeof(project_a), "%s/inferred-a", root) ||
        !PicoPath_Format(project_b, sizeof(project_b), "%s/inferred-b", root) ||
        !PicoPath_Format(sessions, sizeof(sessions), "%s/sessions", root) ||
        !PicoPath_Format(inferred_dir, sizeof(inferred_dir), "%s/legacy", sessions))
    {
        return Fail("inference paths were too long");
    }
    if (!MakeDir(project_a) || !MakeDir(project_b) || !MakeDir(inferred_dir))
    {
        return Fail("could not create inferred workspace");
    }
    char canonical_a[4096];
    char canonical_b[4096];
    if (!realpath(project_a, canonical_a) || !realpath(project_b, canonical_b))
    {
        return Fail("could not canonicalize inferred projects");
    }
    char first[4096];
    char second[4096];
    if (!PicoPath_Format(first, sizeof(first), "%s/one.jsonl", inferred_dir) ||
        !PicoPath_Format(second, sizeof(second), "%s/two.jsonl", inferred_dir))
    {
        return Fail("session paths were too long");
    }
    char line[10000];
    snprintf(line, sizeof(line),
             "{\"type\":\"session\",\"version\":3,\"id\":\"one\",\"cwd\":\"%s\",\"model\":\"m\",\"kind\":\"normal\"}\n"
             "{\"type\":\"message\",\"role\":\"user\",\"content\":\"First title\"}\n",
             canonical_a);
    if (!WriteText(first, line))
    {
        return Fail("could not write inferred session");
    }
    PicoWorkspaceRegistry registry;
    if (PicoWorkspaceRegistry_Init(&registry) != PICO_WORKSPACE_OK)
    {
        return Fail("inference registry init failed");
    }
    const PicoWorkspace *workspace = PicoWorkspaceRegistry_FindKey(&registry, "legacy");
    if (!workspace || !workspace->available || strcmp(workspace->path, canonical_a) != 0 ||
        workspace->session_count != 1 || strcmp(workspace->sessions[0].title, "First title") != 0)
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("consistent session headers did not recover workspace/cache");
    }
    if (!GrowPastMetadataLimit(first) ||
        PicoWorkspaceRegistry_Refresh(&registry) != PICO_WORKSPACE_OK ||
        !(workspace = PicoWorkspaceRegistry_FindKey(&registry, "legacy")) ||
        !workspace->available || workspace->session_count != 1)
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("large durable session disappeared from workspace recovery");
    }
    snprintf(line, sizeof(line),
             "{\"type\":\"session\",\"version\":3,\"id\":\"two\",\"cwd\":\"%s\",\"model\":\"m\",\"kind\":\"normal\"}\n",
             canonical_b);
    if (!WriteText(second, line) || PicoWorkspaceRegistry_Refresh(&registry) != PICO_WORKSPACE_OK)
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("mixed refresh failed");
    }
    workspace = PicoWorkspaceRegistry_FindKey(&registry, "legacy");
    if (!workspace || workspace->available || workspace->path[0])
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("mixed-cwd directory was guessed instead of marked unavailable");
    }
    PicoWorkspaceRegistry_Free(&registry);
    return 0;
}

static int TestValidationAndNoFollow(const char *root)
{
    char project[4096];
    if (!PicoPath_Format(project, sizeof(project), "%s/validation-project", root) ||
        !MakeDir(project))
    {
        return Fail("could not create validation project");
    }
    PicoWorkspaceRegistry registry;
    if (PicoWorkspaceRegistry_Init(&registry) != PICO_WORKSPACE_OK)
    {
        return Fail("validation registry init failed");
    }
    int original_count = PicoWorkspaceRegistry_Count(&registry);
    char long_name[PICO_WORKSPACE_NAME_MAX + 1];
    memset(long_name, 'n', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    char tiny_key[2];
    if (PicoWorkspaceRegistry_Register(&registry, project, long_name, NULL, 0) !=
            PICO_WORKSPACE_INVALID ||
        PicoWorkspaceRegistry_Register(&registry, project, "Valid", tiny_key, sizeof(tiny_key)) !=
            PICO_WORKSPACE_INVALID ||
        PicoWorkspaceRegistry_Count(&registry) != original_count)
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("invalid registration left authoritative state behind");
    }
    char key[4096];
    if (PicoWorkspaceRegistry_Register(&registry, project, "Valid", key, sizeof(key)) !=
        PICO_WORKSPACE_OK)
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("valid registration failed after rollback checks");
    }
    char sessions[4096];
    char metadata[4096];
    if (!PicoPath_Format(sessions, sizeof(sessions), "%s/sessions", root) ||
        !PicoPath_Format(metadata, sizeof(metadata), "%s/workspaces.json", sessions))
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("atomic test paths were too long");
    }
    size_t before_length = 0;
    char *before = Pico_ReadFile(metadata, &before_length);
    if (!before || chmod(sessions, 0500) != 0)
    {
        free(before);
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("could not prepare atomic write failure");
    }
    PicoWorkspaceResult failed_write = PicoWorkspaceRegistry_SetName(&registry, key, "Not saved");
    (void)chmod(sessions, 0700);
    size_t after_length = 0;
    char *after = Pico_ReadFile(metadata, &after_length);
    bool preserved = failed_write == PICO_WORKSPACE_IO_ERROR && after &&
                     before_length == after_length && memcmp(before, after, before_length) == 0 &&
                     !HasWorkspaceTemporary(sessions);
    free(before);
    free(after);
    if (!preserved)
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("failed atomic mutation damaged metadata or left a temporary file");
    }
    PicoWorkspaceRegistry_Free(&registry);

    char redirect[4096];
    char linked_config[4096];
    char target_sessions[4096];
    if (!PicoPath_Format(redirect, sizeof(redirect), "%s/redirect", root) ||
        !PicoPath_Format(linked_config, sizeof(linked_config), "%s/config-link", root) ||
        !PicoPath_Format(target_sessions, sizeof(target_sessions), "%s/sessions", redirect) ||
        !MakeDir(redirect) || symlink(redirect, linked_config) != 0)
    {
        return Fail("could not prepare no-follow test");
    }
    char saved_config[4096];
    snprintf(saved_config, sizeof(saved_config), "%s", g_config);
    snprintf(g_config, sizeof(g_config), "%s", linked_config);
    PicoWorkspaceResult result = PicoWorkspaceRegistry_Init(&registry);
    PicoWorkspaceRegistry_Free(&registry);
    snprintf(g_config, sizeof(g_config), "%s", saved_config);
    struct stat st;
    if (result != PICO_WORKSPACE_IO_ERROR || stat(target_sessions, &st) == 0)
    {
        return Fail("symlinked config component redirected metadata writes");
    }
    return 0;
}

static int TestConcurrentMutation(const char *root)
{
    char one[4096];
    char two[4096];
    snprintf(one, sizeof(one), "%s/concurrent-one", root);
    snprintf(two, sizeof(two), "%s/concurrent-two", root);
    if (!MakeDir(one) || !MakeDir(two))
    {
        return Fail("could not create concurrent projects");
    }
    pid_t first = fork();
    if (first == 0)
    {
        PicoWorkspaceRegistry registry;
        int result = PicoWorkspaceRegistry_Init(&registry) == PICO_WORKSPACE_OK &&
                     PicoWorkspaceRegistry_Register(&registry, one, "One", NULL, 0) ==
                         PICO_WORKSPACE_OK
                         ? 0
                         : 1;
        PicoWorkspaceRegistry_Free(&registry);
        _exit(result);
    }
    pid_t second = fork();
    if (second == 0)
    {
        PicoWorkspaceRegistry registry;
        int result = PicoWorkspaceRegistry_Init(&registry) == PICO_WORKSPACE_OK &&
                     PicoWorkspaceRegistry_Register(&registry, two, "Two", NULL, 0) ==
                         PICO_WORKSPACE_OK
                         ? 0
                         : 1;
        PicoWorkspaceRegistry_Free(&registry);
        _exit(result);
    }
    int first_status = 0;
    int second_status = 0;
    waitpid(first, &first_status, 0);
    waitpid(second, &second_status, 0);
    PicoWorkspaceRegistry registry;
    if (!WIFEXITED(first_status) || WEXITSTATUS(first_status) != 0 ||
        !WIFEXITED(second_status) || WEXITSTATUS(second_status) != 0 ||
        PicoWorkspaceRegistry_Init(&registry) != PICO_WORKSPACE_OK ||
        !FindPath(&registry, one) || !FindPath(&registry, two))
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("concurrent metadata mutation lost a workspace");
    }
    char key[4096];
    const PicoWorkspace *target = FindPath(&registry, one);
    snprintf(key, sizeof(key), "%s", target->key);
    PicoWorkspaceRegistry_Free(&registry);
    first = fork();
    if (first == 0)
    {
        PicoWorkspaceRegistry child;
        int result = PicoWorkspaceRegistry_Init(&child) == PICO_WORKSPACE_OK &&
                     PicoWorkspaceRegistry_SetName(&child, key, "Renamed") == PICO_WORKSPACE_OK
                         ? 0
                         : 1;
        PicoWorkspaceRegistry_Free(&child);
        _exit(result);
    }
    second = fork();
    if (second == 0)
    {
        PicoWorkspaceRegistry child;
        int result = PicoWorkspaceRegistry_Init(&child) == PICO_WORKSPACE_OK &&
                     PicoWorkspaceRegistry_SetCollapsed(&child, key, true) == PICO_WORKSPACE_OK
                         ? 0
                         : 1;
        PicoWorkspaceRegistry_Free(&child);
        _exit(result);
    }
    waitpid(first, &first_status, 0);
    waitpid(second, &second_status, 0);
    if (!WIFEXITED(first_status) || WEXITSTATUS(first_status) != 0 ||
        !WIFEXITED(second_status) || WEXITSTATUS(second_status) != 0 ||
        PicoWorkspaceRegistry_Init(&registry) != PICO_WORKSPACE_OK ||
        !(target = PicoWorkspaceRegistry_FindKey(&registry, key)) ||
        strcmp(target->name, "Renamed") != 0 || !target->collapsed)
    {
        PicoWorkspaceRegistry_Free(&registry);
        return Fail("concurrent presentation mutations overwrote each other");
    }
    PicoWorkspaceRegistry_Free(&registry);
    return 0;
}

int main(void)
{
    char root[] = "/tmp/pico-workspace-test-XXXXXX";
    if (!mkdtemp(root))
    {
        return Fail("could not create test root");
    }
    snprintf(g_config, sizeof(g_config), "%s", root);
    int result = TestRegistrationAndRecovery(root) != 0 || TestCollision(root) != 0 ||
                 TestInferenceAndMixedDirectory(root) != 0 ||
                 TestValidationAndNoFollow(root) != 0 || TestConcurrentMutation(root) != 0;
    RemoveTree(root);
    if (result)
    {
        return 1;
    }
    printf("workspace: ok\n");
    return 0;
}
