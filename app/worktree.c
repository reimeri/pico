#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "worktree.h"

#include "host_internal.h"
#include "agent.h"
#include "path.h"
#include "settings.h"
#include "workspace_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct PicoWorktreeJob {
    pthread_mutex_t mu;
    pid_t pid;
    bool cancelled;
    bool done;
    bool joined;
    bool success;
    bool source_selected;
    PicoAgentId source_agent_id;
    char source[4096];
    char project[4096];
    char name[256];
    char path[4096];
    char error[1024];
} PicoWorktreeJob;

static bool CopyCanonical(const char *path, char *out, size_t cap)
{
    char real[4096];
    if (!path || !realpath(path, real) || strlen(real) >= cap)
    {
        return false;
    }
    snprintf(out, cap, "%s", real);
    return true;
}

static int RunArgv(char *const argv[], char *output, size_t cap, PicoWorktreeJob *job)
{
    int pipefd[2];
    pid_t pid;
    int status = 0;
    size_t used = 0;
    bool reaped = false;
    bool terminate_sent = false;
    struct timespec terminate_at = {0};
    struct timespec started;
    if (output && cap > 0) output[0] = '\0';
    if (pipe(pipefd) != 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    bool actions_ready = posix_spawn_file_actions_init(&actions) == 0;
    bool attr_ready = actions_ready && posix_spawnattr_init(&attr) == 0;
    short spawn_flags = POSIX_SPAWN_SETPGROUP;
    extern char **environ;
    int spawn_rc = !attr_ready ? ENOMEM :
        (posix_spawn_file_actions_addclose(&actions, pipefd[0]) != 0 ||
         posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO) != 0 ||
         posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO) != 0 ||
         posix_spawn_file_actions_addclose(&actions, pipefd[1]) != 0 ||
         posix_spawnattr_setflags(&attr, spawn_flags) != 0 ||
         posix_spawnattr_setpgroup(&attr, 0) != 0)
            ? EINVAL
            : posix_spawnp(&pid, argv[0], &actions, &attr, argv, environ);
    if (attr_ready) posix_spawnattr_destroy(&attr);
    if (actions_ready) posix_spawn_file_actions_destroy(&actions);
    if (spawn_rc != 0)
    {
        close(pipefd[0]); close(pipefd[1]); return -1;
    }
    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) (void)fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    if (job)
    {
        pthread_mutex_lock(&job->mu);
        job->pid = pid;
        bool cancel = job->cancelled;
        pthread_mutex_unlock(&job->mu);
        if (cancel) (void)kill(-pid, SIGTERM);
    }
    clock_gettime(CLOCK_MONOTONIC, &started);
    while (!reaped)
    {
        for (;;)
        {
            char buf[1024];
            ssize_t got = read(pipefd[0], buf, sizeof(buf));
            if (got > 0)
            {
                if (output && cap > 1 && used < cap - 1)
                {
                    size_t copy = (size_t)got;
                    if (copy > cap - 1 - used) copy = cap - 1 - used;
                    memcpy(output + used, buf, copy);
                    used += copy;
                }
                continue;
            }
            if (got < 0 && errno == EINTR) continue;
            break;
        }
        if (job)
        {
            pthread_mutex_lock(&job->mu);
            bool cancelled = job->cancelled;
            pthread_mutex_unlock(&job->mu);
            if (cancelled && !terminate_sent)
            {
                (void)kill(-pid, SIGTERM);
                clock_gettime(CLOCK_MONOTONIC, &terminate_at);
                terminate_sent = true;
            }
            else if (cancelled && terminate_sent)
            {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ms = (long)(now.tv_sec - terminate_at.tv_sec) * 1000L +
                                  (long)(now.tv_nsec - terminate_at.tv_nsec) / 1000000L;
                if (elapsed_ms >= 250) (void)kill(-pid, SIGKILL);
            }
        }
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD))
        {
            reaped = true;
            break;
        }
        if (!job)
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - started.tv_sec >= 2)
            {
                (void)kill(-pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                reaped = true;
                status = -1;
                break;
            }
        }
        struct timespec pause = {.tv_nsec = 10000000L};
        nanosleep(&pause, NULL);
    }
    for (;;)
    {
        char buf[1024];
        ssize_t got = read(pipefd[0], buf, sizeof(buf));
        if (got <= 0) break;
        if (output && cap > 1 && used < cap - 1)
        {
            size_t copy = (size_t)got;
            if (copy > cap - 1 - used) copy = cap - 1 - used;
            memcpy(output + used, buf, copy);
            used += copy;
        }
    }
    close(pipefd[0]);
    if (job)
    {
        pthread_mutex_lock(&job->mu); job->pid = 0; pthread_mutex_unlock(&job->mu);
    }
    if (output && cap > 0) output[used] = '\0';
    return status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}


static void BaseName(const char *path, char *out, size_t cap)
{
    const char *end = path ? path + strlen(path) : NULL;
    while (end && end > path && end[-1] == '/') end--;
    const char *start = end;
    while (start && start > path && start[-1] != '/') start--;
    if (!start || start == end) snprintf(out, cap, "worktree");
    else snprintf(out, cap, "%.*s", (int)(end - start), start);
}

static bool ResolveMetadataPath(const char *base, const char *value, char *out, size_t cap)
{
    char candidate[4096];
    if (!value || !value[0]) return false;
    if (value[0] == '/') return CopyCanonical(value, out, cap);
    return PicoPath_Format(candidate, sizeof(candidate), "%s/%s", base, value) &&
           CopyCanonical(candidate, out, cap);
}

static bool ReadMetadataLine(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    if (!ok) return false;
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
    return out[0] != '\0';
}

static bool CommonDirHasCommittedHead(const char *common)
{
    char head_path[4096], head[4096];
    if (!PicoPath_Format(head_path, sizeof(head_path), "%s/HEAD", common) ||
        !ReadMetadataLine(head_path, head, sizeof(head))) return false;
    const char ref_prefix[] = "ref: ";
    if (strncmp(head, ref_prefix, sizeof(ref_prefix) - 1) != 0)
        return strlen(head) >= 40;
    const char *ref = head + sizeof(ref_prefix) - 1;
    char ref_path[4096], value[4096];
    if (PicoPath_Format(ref_path, sizeof(ref_path), "%s/%s", common, ref) &&
        ReadMetadataLine(ref_path, value, sizeof(value)) && strlen(value) >= 40)
        return true;
    char packed_path[4096];
    if (!PicoPath_Format(packed_path, sizeof(packed_path), "%s/packed-refs", common)) return false;
    FILE *f = fopen(packed_path, "rb");
    if (!f) return false;
    bool found = false;
    char line[4096];
    while (fgets(line, sizeof(line), f))
    {
        char *space = strchr(line, ' ');
        if (space && strncmp(line, "#", 1) != 0 && strcmp(space + 1, ref) == 0)
        {
            found = true;
            break;
        }
        if (space)
        {
            size_t len = strlen(space + 1);
            while (len > 0 && (space[1 + len - 1] == '\n' || space[1 + len - 1] == '\r'))
                space[1 + --len] = '\0';
            if (strcmp(space + 1, ref) == 0) { found = true; break; }
        }
    }
    fclose(f);
    return found;
}

static bool CommonDirIsBare(const char *common)
{
    char config[4096];
    if (!PicoPath_Format(config, sizeof(config), "%s/config", common)) return true;
    FILE *f = fopen(config, "rb");
    if (!f) return true;
    bool bare = false;
    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        char compact[1024];
        size_t n = 0;
        for (size_t i = 0; line[i] && n + 1 < sizeof(compact); i++)
            if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n')
                compact[n++] = line[i];
        compact[n] = '\0';
        if (strcmp(compact, "bare=true") == 0) { bare = true; break; }
    }
    fclose(f);
    return bare;
}

static bool IsStandardCommonDir(const char *common, char *project, size_t project_cap)
{
    char head[4096], objects[4096];
    struct stat st;
    size_t len = common ? strlen(common) : 0;
    if (len < 5 || strcmp(common + len - 5, "/.git") != 0 ||
        !PicoPath_Format(head, sizeof(head), "%s/HEAD", common) ||
        !PicoPath_Format(objects, sizeof(objects), "%s/objects", common) ||
        stat(head, &st) != 0 || !S_ISREG(st.st_mode) ||
        stat(objects, &st) != 0 || !S_ISDIR(st.st_mode) || len - 5 >= project_cap)
        return false;
    memcpy(project, common, len - 5);
    project[len - 5] = '\0';
    char canonical[4096];
    if (!CopyCanonical(project, canonical, sizeof(canonical))) return false;
    snprintf(project, project_cap, "%s", canonical);
    return true;
}

bool PicoWorktree_Discover(const char *path, PicoWorktreeInfo *out)
{
    char canonical[4096];
    char dot_git[4096];
    struct stat git_stat;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!CopyCanonical(path, canonical, sizeof(canonical))) return false;
    snprintf(out->checkout_path, sizeof(out->checkout_path), "%s", canonical);
    snprintf(out->project_path, sizeof(out->project_path), "%s", canonical);
    BaseName(canonical, out->checkout_name, sizeof(out->checkout_name));
    if (!PicoPath_Format(dot_git, sizeof(dot_git), "%s/.git", canonical) ||
        stat(dot_git, &git_stat) != 0)
        return true;
    if (S_ISDIR(git_stat.st_mode))
    {
        char project[4096];
        if (!IsStandardCommonDir(dot_git, project, sizeof(project)) || strcmp(project, canonical) != 0)
            return true;
        out->checkout_root = true;
        out->can_create = !CommonDirIsBare(dot_git) && CommonDirHasCommittedHead(dot_git);
        return true;
    }
    if (!S_ISREG(git_stat.st_mode)) return true;
    char line[4096] = {0};
    const char prefix[] = "gitdir: ";
    if (!ReadMetadataLine(dot_git, line, sizeof(line)) ||
        strncmp(line, prefix, sizeof(prefix) - 1) != 0)
        return true;
    char gitdir_real[4096];
    if (!ResolveMetadataPath(canonical, line + sizeof(prefix) - 1,
                             gitdir_real, sizeof(gitdir_real)))
        return true;
    char commondir_path[4096], commondir_value[4096], common_real[4096];
    if (!PicoPath_Format(commondir_path, sizeof(commondir_path), "%s/commondir", gitdir_real) ||
        !ReadMetadataLine(commondir_path, commondir_value, sizeof(commondir_value)) ||
        !ResolveMetadataPath(gitdir_real, commondir_value, common_real, sizeof(common_real)))
        return true; /* separate-git-dir repositories are outside v1 */
    char project[4096];
    if (!IsStandardCommonDir(common_real, project, sizeof(project))) return true;
    out->checkout_root = true;
    out->can_create = !CommonDirIsBare(common_real) && CommonDirHasCommittedHead(common_real);
    snprintf(out->project_path, sizeof(out->project_path), "%s", project);
    out->linked = strcmp(out->checkout_path, out->project_path) != 0;
    return true;
}

static bool SuggestPrefix(const char *project, char *out, size_t cap)
{
    char raw[256];
    size_t n = 0;
    if (!out || cap == 0) return false;
    BaseName(project, raw, sizeof(raw));
    for (size_t i = 0; raw[i] && n + 1 < cap; i++)
    {
        unsigned char c = (unsigned char)raw[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            c = '-';
        if ((c == '-' || c == '.') && n == 0) continue;
        if (c == '-' && n > 0 && out[n - 1] == '-') continue;
        if (c == '.' && n > 0 && out[n - 1] == '.') continue;
        out[n++] = (char)c;
    }
    while (n > 0 && (out[n - 1] == '-' || out[n - 1] == '.')) n--;
    if (n == 0)
    {
        if (cap < 5) return false;
        memcpy(out, "pico", 5);
        return true;
    }
    out[n] = '\0';
    return true;
}

bool PicoWorktree_SuggestName(const char *project_path, char *out, size_t cap)
{
    char prefix[113];
    char stamp[17];
    time_t now = time(NULL);
    struct tm tmv;
    if (!out || cap == 0 || !localtime_r(&now, &tmv)) return false;
    if (!SuggestPrefix(project_path, prefix, sizeof(prefix))) return false;
    if (strftime(stamp, sizeof(stamp), "-%Y%m%d-%H%M%S", &tmv) == 0) return false;
    int n = snprintf(out, cap, "%s%s", prefix, stamp);
    return n > 0 && (size_t)n < cap;
}

bool PicoWorktree_ValidateName(const char *name, char *error, size_t error_cap)
{
    size_t n = name ? strlen(name) : 0;
    if (!name || n == 0 || n > 128)
    {
        snprintf(error, error_cap, "Use a name between 1 and 128 characters.");
        return false;
    }
    if (name[0] == '-' || name[0] == '.' || name[n - 1] == '.' || strstr(name, ".."))
    {
        snprintf(error, error_cap, "Use a simple branch name without leading or repeated dots.");
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
        {
            snprintf(error, error_cap, "Use letters, numbers, dashes, underscores, or dots.");
            return false;
        }
    }
    return true;
}

static bool ProjectKey(const char *project, char out[33])
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(project, strlen(project), digest, &len, EVP_sha256(), NULL) != 1 || len < 16)
        return false;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++)
    {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[32] = '\0';
    return true;
}

static bool WorktreePath(const char *project, const char *name, char *out, size_t cap)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    char root[4096];
    char key[33];
    if (!ProjectKey(project, key)) return false;
    if (xdg && xdg[0])
    {
        if (!PicoPath_Format(root, sizeof(root), "%s/pico/worktrees/%s", xdg, key)) return false;
    }
    else
    {
        if (!home || !home[0] || !PicoPath_Format(root, sizeof(root), "%s/.local/share/pico/worktrees/%s", home, key))
            return false;
    }
    Pico_MkdirP(root);
    return PicoPath_Format(out, cap, "%s/%s", root, name);
}

static void *CreateRun(void *user)
{
    PicoWorktreeJob *job = user;
    char oid[128] = {0};
    char output[1024] = {0};
    char *rev[] = {"git", "-C", job->project, "rev-parse", "--verify", "HEAD^{commit}", NULL};
    int rc = RunArgv(rev, oid, sizeof(oid), job);
    while (oid[0] && (oid[strlen(oid) - 1] == '\n' || oid[strlen(oid) - 1] == '\r'))
        oid[strlen(oid) - 1] = '\0';
    if (rc == 0)
    {
        char *add[] = {"git", "-C", job->project, "worktree", "add", "-b", job->name,
                       job->path, oid, NULL};
        rc = RunArgv(add, output, sizeof(output), job);
    }
    pthread_mutex_lock(&job->mu);
    if (job->cancelled)
        snprintf(job->error, sizeof(job->error), "Worktree creation was cancelled.");
    else if (rc == 0)
        job->success = true;
    else
        snprintf(job->error, sizeof(job->error), "%s", output[0] ? output : "Git could not create the worktree.");
    job->done = true;
    pthread_mutex_unlock(&job->mu);
    return NULL;
}

static void CreateCancel(void *user)
{
    PicoWorktreeJob *job = user;
    pthread_mutex_lock(&job->mu);
    job->cancelled = true;
    pid_t pid = job->pid;
    pthread_mutex_unlock(&job->mu);
    if (pid > 0) (void)kill(-pid, SIGTERM);
}

static void CreateJoined(void *user)
{
    PicoWorktreeJob *job = user;
    pthread_mutex_lock(&job->mu);
    job->joined = true;
    pthread_mutex_unlock(&job->mu);
}

PicoResult PicoWorktree_Request(PicoHost *host, PicoAgentId source_agent_id,
                                const char *name, char *error, size_t error_cap)
{
    PicoAgent *agent = PicoHost_FindAgent(host, source_agent_id);
    PicoWorkspace *workspace = agent ? agent->workspace : NULL;
    PicoWorktreeInfo info;
    if (!host || !agent || !workspace) return PICO_NOT_FOUND;
    if (host->worktree_job) return PICO_BUSY;
    if (agent->accepted_submit || agent->message_count > 0 || PicoAgent_IsBusy(agent)) return PICO_BUSY;
    if (!PicoWorktree_ValidateName(name, error, error_cap)) return PICO_INVALID;
    if (!PicoWorktree_Discover(workspace->path, &info) || !info.checkout_root || !info.can_create)
    {
        snprintf(error, error_cap, "Worktrees require a Git checkout root with at least one commit.");
        return PICO_INVALID;
    }
    PicoWorktreeJob *job = calloc(1, sizeof(*job));
    if (!job) return PICO_NO_MEMORY;
    pthread_mutex_init(&job->mu, NULL);
    job->source_agent_id = source_agent_id;
    job->source_selected = pico_agent_active(host) == source_agent_id;
    snprintf(job->source, sizeof(job->source), "%s", workspace->path);
    snprintf(job->project, sizeof(job->project), "%s", info.project_path);
    snprintf(job->name, sizeof(job->name), "%s", name);
    if (!WorktreePath(info.project_path, name, job->path, sizeof(job->path)))
    {
        pthread_mutex_destroy(&job->mu);
        free(job);
        return PICO_INVALID;
    }
    struct stat st;
    if (lstat(job->path, &st) == 0 || errno != ENOENT)
    {
        snprintf(error, error_cap, "That worktree destination already exists.");
        pthread_mutex_destroy(&job->mu);
        free(job);
        return PICO_ALREADY_OPEN;
    }
    host->worktree_job = job;
    if (!PicoHost_StartTask(host, CreateRun, job, CreateCancel, CreateJoined))
    {
        host->worktree_job = NULL;
        pthread_mutex_destroy(&job->mu);
        free(job);
        return PICO_NO_MEMORY;
    }
    return PICO_OK;
}

bool PicoWorktree_TakeResult(PicoHost *host, PicoWorktreeResult *out)
{
    PicoWorktreeJob *job = host ? host->worktree_job : NULL;
    if (!job || !out) return false;
    pthread_mutex_lock(&job->mu);
    bool ready = job->done && job->joined;
    if (ready)
    {
        memset(out, 0, sizeof(*out));
        out->success = job->success;
        out->source_selected = job->source_selected;
        out->source_agent_id = job->source_agent_id;
        snprintf(out->name, sizeof(out->name), "%s", job->name);
        snprintf(out->path, sizeof(out->path), "%s", job->path);
        snprintf(out->error, sizeof(out->error), "%s", job->error);
    }
    pthread_mutex_unlock(&job->mu);
    if (!ready) return false;
    pthread_mutex_destroy(&job->mu);
    free(job);
    host->worktree_job = NULL;
    return true;
}

bool PicoWorktree_Pending(const PicoHost *host)
{
    return host && host->worktree_job;
}

bool PicoWorktree_PendingFor(const PicoHost *host, PicoAgentId source_agent_id)
{
    const PicoWorktreeJob *job = host ? host->worktree_job : NULL;
    return job && job->source_agent_id == source_agent_id;
}

void PicoWorktree_Cleanup(PicoHost *host)
{
    if (!host || !host->worktree_job) return;
    PicoWorktreeJob *job = host->worktree_job;
    pthread_mutex_destroy(&job->mu);
    free(job);
    host->worktree_job = NULL;
}
