#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "background_model.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/socket.h>
#endif

#define PICO_BG_MAX_JOBS (PICO_MAX_AGENTS * PICO_BG_MAX_RECORDS)
#define PICO_BG_READ_CHUNK 4096
#define PICO_BG_READ_BUDGET (16 * PICO_BG_READ_CHUNK)
/* Admission includes detached cleanup, not just visible history. */
#define PICO_BG_MAX_PENDING PICO_BG_MAX_JOBS

typedef struct PicoBgChild {
    struct PicoBgChild *next;
    pid_t pid;
    int control;
    int status;
    bool done;
    bool attached;
} PicoBgChild;

/* Compiled-in ownership, independent of host/workspace/plugin lifetimes. Only
 * this service waits for its exact child pids; it never adopts unrelated work. */
static pthread_mutex_t children_mu = PTHREAD_MUTEX_INITIALIZER;
static PicoBgChild *children;
static int children_count;
static bool children_reaper_running;

static void *ReapChildren(void *unused)
{
    (void)unused;
    for (;;)
    {
        pthread_mutex_lock(&children_mu);
        PicoBgChild **link = &children;
        while (*link)
        {
            PicoBgChild *child = *link;
            int status = 0;
            pid_t result = child->pid > 0 ? waitpid(child->pid, &status, WNOHANG) : 0;
            if (result > 0 || (result < 0 && errno == ECHILD))
            {
                /* ECHILD means an external reaper took our child. Never retain
                 * or signal its now-unowned numeric identity. */
                child->status = result > 0 ? status : 127 << 8;
                child->done = true;
                if (child->control >= 0) close(child->control);
                child->control = -1;
                *link = child->next;
                children_count--;
                if (!child->attached) free(child);
            }
            else link = &child->next;
        }
        if (!children)
        {
            children_reaper_running = false;
            pthread_mutex_unlock(&children_mu);
            return NULL;
        }
        pthread_mutex_unlock(&children_mu);
        struct timespec pause = {.tv_nsec = 10000000L};
        nanosleep(&pause, NULL);
    }
}

/* Reserve the owner and start the service before fork. Resource exhaustion must
 * reject a spawn, never leave an already-created child with no reaping owner. */
static PicoBgChild *ReserveChild(void)
{
    PicoBgChild *child = calloc(1, sizeof(*child));
    if (!child) return NULL;
    child->control = -1;
    child->attached = true;
    pthread_mutex_lock(&children_mu);
    if (children_count >= PICO_BG_MAX_PENDING) goto fail;
    if (!children_reaper_running)
    {
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) goto fail;
        pthread_t thread;
        bool ok = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0 &&
                  pthread_create(&thread, &attr, ReapChildren, NULL) == 0;
        pthread_attr_destroy(&attr);
        if (!ok) goto fail;
        children_reaper_running = true;
    }
    child->next = children;
    children = child;
    children_count++;
    pthread_mutex_unlock(&children_mu);
    return child;
fail:
    pthread_mutex_unlock(&children_mu);
    free(child);
    return NULL;
}

static void CancelChildReservation(PicoBgChild *child)
{
    pthread_mutex_lock(&children_mu);
    PicoBgChild **link = &children;
    while (*link != child) link = &(*link)->next;
    *link = child->next;
    children_count--;
    pthread_mutex_unlock(&children_mu);
    free(child);
}

static void StopChildLocked(PicoBgChild *child)
{
    if (!child || child->done) return;
#if defined(__linux__)
    /* shutdown affects the endpoint even if a concurrent fork inherited a
     * duplicate. Plain close/pipe EOF could wait on that unrelated child. */
    if (child->control >= 0)
    {
        (void)shutdown(child->control, SHUT_WR);
        close(child->control);
    }
    child->control = -1;
#else
    if (child->pid > 0) (void)kill(-child->pid, SIGKILL);
#endif
}

static void StopChild(PicoBgChild *child)
{
    pthread_mutex_lock(&children_mu);
    StopChildLocked(child);
    pthread_mutex_unlock(&children_mu);
}

static void DetachChild(PicoBgChild *child)
{
    if (!child) return;
    pthread_mutex_lock(&children_mu);
    StopChildLocked(child);
    child->attached = false;
    if (child->done) free(child);
    pthread_mutex_unlock(&children_mu);
}

typedef struct PicoBgJob {
    PicoAgentId agent_id;
    char id[PICO_BG_ID_MAX];
    char *description;
    char *command;
    PicoBgChild *child;
    int fd;
    PicoBgStatus status;
    bool reaped;
    int exit_code;
    int term_signal;
    char *log;
    size_t log_len;
    size_t log_cap;
} PicoBgJob;

typedef struct PicoBgSeq {
    PicoAgentId agent_id;
    uint32_t next_serial;
} PicoBgSeq;

struct PicoBgTable {
    pthread_mutex_t mu;
    PicoBgJob jobs[PICO_BG_MAX_JOBS];
    int job_count;
    PicoBgSeq seqs[PICO_MAX_TOTAL_AGENTS];
    int seq_count;
};

const char *PicoBgStatus_Name(PicoBgStatus status)
{
    if (status == PICO_BG_KILLED)
    {
        return "killed";
    }
    if (status == PICO_BG_EXITED)
    {
        return "exited";
    }
    return "running";
}

/* Close-on-exec pipe: children forked by concurrent spawns must not inherit this
 * job's pipe ends past their exec, or the parent's read end never sees EOF while
 * such a child lives. dup2 in the child keeps the descriptors it needs. */
static int OpenJobPipe(int pipefd[2])
{
#if defined(__linux__)
    return pipe2(pipefd, O_CLOEXEC);
#else
    if (pipe(pipefd) != 0)
    {
        return -1;
    }
    (void)fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

static void FreeJobContents(PicoBgJob *job)
{
    if (!job)
    {
        return;
    }
    if (job->fd >= 0)
    {
        close(job->fd);
        job->fd = -1;
    }
    free(job->description);
    free(job->command);
    free(job->log);
    job->description = NULL;
    job->command = NULL;
    job->log = NULL;
    job->log_len = 0;
    job->log_cap = 0;
}

static void RemoveJobAt(PicoBgTable *table, int index)
{
    PicoBgJob *job;
    if (!table || index < 0 || index >= table->job_count)
    {
        return;
    }
    job = &table->jobs[index];
    DetachChild(job->child);
    FreeJobContents(job);
    if (index < table->job_count - 1)
    {
        memmove(&table->jobs[index], &table->jobs[index + 1],
                (size_t)(table->job_count - index - 1) * sizeof(table->jobs[0]));
    }
    table->job_count--;
    memset(&table->jobs[table->job_count], 0, sizeof(table->jobs[0]));
}

static PicoBgJob *FindJob(PicoBgTable *table, PicoAgentId agent_id, const char *id)
{
    int i;
    if (!table || !id || !id[0] || agent_id == 0)
    {
        return NULL;
    }
    for (i = 0; i < table->job_count; i++)
    {
        if (table->jobs[i].agent_id == agent_id && strcmp(table->jobs[i].id, id) == 0)
        {
            return &table->jobs[i];
        }
    }
    return NULL;
}

static int CountAgent(PicoBgTable *table, PicoAgentId agent_id, bool running_only)
{
    int i;
    int n = 0;
    for (i = 0; i < table->job_count; i++)
    {
        if (table->jobs[i].agent_id != agent_id)
        {
            continue;
        }
        if (running_only && table->jobs[i].status != PICO_BG_RUNNING)
        {
            continue;
        }
        n++;
    }
    return n;
}

static int OldestFinishedIndex(PicoBgTable *table, PicoAgentId agent_id)
{
    int i;
    for (i = 0; i < table->job_count; i++)
    {
        if (table->jobs[i].agent_id == agent_id && table->jobs[i].status != PICO_BG_RUNNING)
        {
            return i;
        }
    }
    return -1;
}

static uint32_t NextSerial(PicoBgTable *table, PicoAgentId agent_id)
{
    int i;
    for (i = 0; i < table->seq_count; i++)
    {
        if (table->seqs[i].agent_id == agent_id)
        {
            table->seqs[i].next_serial++;
            if (table->seqs[i].next_serial == 0)
            {
                table->seqs[i].next_serial = 1;
            }
            return table->seqs[i].next_serial;
        }
    }
    if (table->seq_count >= PICO_MAX_TOTAL_AGENTS)
    {
        return 0;
    }
    table->seqs[table->seq_count].agent_id = agent_id;
    table->seqs[table->seq_count].next_serial = 1;
    table->seq_count++;
    return 1;
}

static void LogAppend(PicoBgJob *job, const char *data, size_t n)
{
    size_t i;
    char *chunk;
    if (!job || !data || n == 0)
    {
        return;
    }
    chunk = (char *)malloc(n);
    if (!chunk)
    {
        return;
    }
    memcpy(chunk, data, n);
    for (i = 0; i < n; i++)
    {
        if (chunk[i] == '\0')
        {
            chunk[i] = '?';
        }
    }
    if (n > (size_t)PICO_BG_LOG_MAX)
    {
        data = chunk + (n - (size_t)PICO_BG_LOG_MAX);
        n = (size_t)PICO_BG_LOG_MAX;
        job->log_len = 0;
    }
    else
    {
        data = chunk;
        while (job->log_len + n > (size_t)PICO_BG_LOG_MAX && job->log_len > 0)
        {
            char *nl = (char *)memchr(job->log, '\n', job->log_len);
            if (nl)
            {
                size_t drop = (size_t)(nl - job->log) + 1;
                memmove(job->log, job->log + drop, job->log_len - drop);
                job->log_len -= drop;
            }
            else
            {
                size_t keep = (size_t)PICO_BG_LOG_MAX - n;
                if (keep < job->log_len)
                {
                    memmove(job->log, job->log + (job->log_len - keep), keep);
                    job->log_len = keep;
                }
                break;
            }
        }
    }
    if (job->log_cap < job->log_len + n + 1)
    {
        size_t cap = job->log_len + n + 1;
        char *grown = (char *)realloc(job->log, cap);
        if (!grown)
        {
            free(chunk);
            return;
        }
        job->log = grown;
        job->log_cap = cap;
    }
    memcpy(job->log + job->log_len, data, n);
    job->log_len += n;
    job->log[job->log_len] = '\0';
    free(chunk);
}

static void DrainFd(PicoBgJob *job)
{
    char buf[PICO_BG_READ_CHUNK];
    size_t budget = PICO_BG_READ_BUDGET;
    if (!job || job->fd < 0)
    {
        return;
    }
    while (budget > 0)
    {
        ssize_t n = read(job->fd, buf, sizeof(buf));
        if (n > 0)
        {
            LogAppend(job, buf, (size_t)n);
            budget = budget > (size_t)n ? budget - (size_t)n : 0;
            continue;
        }
        if (n == 0)
        {
            close(job->fd);
            job->fd = -1;
            return;
        }
        if (errno == EINTR)
        {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return;
        }
        close(job->fd);
        job->fd = -1;
        return;
    }
}

static void ReapJob(PicoBgJob *job, int status)
{
    if (!job)
    {
        return;
    }
    job->reaped = true;
    if (WIFEXITED(status))
    {
        job->exit_code = WEXITSTATUS(status);
        job->term_signal = 0;
    }
    else if (WIFSIGNALED(status))
    {
        job->term_signal = WTERMSIG(status);
        job->exit_code = 0;
    }
    if (job->status == PICO_BG_KILLED) job->term_signal = SIGKILL;
    else job->status = PICO_BG_EXITED;
}

static char *DupError(const char *message)
{
    return JsonDup(message ? message : "background: error");
}

static char *FormatJobJson(const PicoBgJob *job, bool include_command)
{
    JsonBuf b;
    if (!job)
    {
        return NULL;
    }
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"id\":");
    JsonBuf_String(&b, job->id);
    JsonBuf_Puts(&b, ",\"status\":");
    JsonBuf_String(&b, PicoBgStatus_Name(job->status));
    JsonBuf_Puts(&b, ",\"description\":");
    JsonBuf_String(&b, job->description ? job->description : "");
    if (include_command)
    {
        JsonBuf_Puts(&b, ",\"command\":");
        JsonBuf_String(&b, job->command ? job->command : "");
    }
    if (job->status != PICO_BG_RUNNING)
    {
        int sig = job->term_signal;
        if (sig <= 0 && job->status == PICO_BG_KILLED)
        {
            sig = SIGKILL; /* requested payload signal; cleanup may still be pending */
        }
        if (sig > 0)
        {
            JsonBuf_Puts(&b, ",\"signal\":");
            JsonBuf_Int(&b, sig);
        }
        else
        {
            JsonBuf_Puts(&b, ",\"exit_code\":");
            JsonBuf_Int(&b, job->exit_code);
        }
    }
    JsonBuf_Putc(&b, '}');
    return JsonBuf_Steal(&b);
}

PicoBgTable *PicoBgTable_Create(void)
{
    PicoBgTable *table = (PicoBgTable *)calloc(1, sizeof(PicoBgTable));
    if (!table)
    {
        return NULL;
    }
    if (pthread_mutex_init(&table->mu, NULL) != 0)
    {
        free(table);
        return NULL;
    }
    return table;
}

void PicoBgTable_Destroy(PicoBgTable *table)
{
    if (!table) return;
    pthread_mutex_lock(&table->mu);
    while (table->job_count > 0) RemoveJobAt(table, table->job_count - 1);
    pthread_mutex_unlock(&table->mu);
    pthread_mutex_destroy(&table->mu);
    free(table);
}

void PicoBgTable_Pump(PicoBgTable *table)
{
    if (!table) return;
    pthread_mutex_lock(&table->mu);
    for (int i = 0; i < table->job_count; i++)
    {
        PicoBgJob *job = &table->jobs[i];
        /* Even after completion, an escaped process may retain stdout. Never
         * turn that out-of-scope writer into an unbounded main-thread drain. */
        DrainFd(job);
        if (job->reaped) continue;
        pthread_mutex_lock(&children_mu);
        if (job->child->done) ReapJob(job, job->child->status);
        pthread_mutex_unlock(&children_mu);
    }
    pthread_mutex_unlock(&table->mu);
}

#if defined(__linux__)
static pid_t supervisor_parent;
static volatile sig_atomic_t supervisor_stop;
static sigset_t supervisor_original_mask;
static struct sigaction supervisor_original_term;

static void SupervisorSignal(int signal)
{
    (void)signal;
    /* Linux delivers PDEATHSIG for the creating thread too. Its completion is
     * not host death. Also check this in the loop: delivery can be exhausted
     * by worker exit, and unrelated fork copies must not mask host death. */
    if (getppid() != supervisor_parent) supervisor_stop = 1;
}

static void ConfigureSupervisor(pid_t parent)
{
    supervisor_parent = parent;
    supervisor_stop = 0;
    sigset_t blocked;
    sigfillset(&blocked);
    if (sigprocmask(SIG_SETMASK, &blocked, &supervisor_original_mask) != 0) _exit(127);
    struct sigaction action = {.sa_handler = SupervisorSignal};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, &supervisor_original_term) != 0 ||
        prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) _exit(127);
    SupervisorSignal(0);
}

/* The supervisor first creates the payload group using its own pid, forks the
 * command into it, then rejoins its inherited host group. Its live pid pins the
 * payload PGID even after the command is reaped or the last member escapes.
 * Never move it back into the payload group from the parent. */
static void SuperviseJob(int control, pid_t inherited_group)
{
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) _exit(127);
    pid_t group = getpid();
    if (setpgid(0, 0) != 0) _exit(127);
    int ready[2];
    if (OpenJobPipe(ready) != 0) _exit(127);
    pid_t command = fork();
    if (command < 0) _exit(127);
    if (command == 0)
    {
        close(ready[1]);
        close(control);
        char byte;
        ssize_t n;
        do { n = read(ready[0], &byte, 1); } while (n < 0 && errno == EINTR);
        close(ready[0]);
        if (n != 1) _exit(127);
        sigaction(SIGTERM, &supervisor_original_term, NULL);
        sigprocmask(SIG_SETMASK, &supervisor_original_mask, NULL);
        return;
    }
    close(ready[0]);
    if (setpgid(0, inherited_group) != 0)
    {
        /* The inherited group may disappear with the host. The payload is
         * still gated, so no descendants can escape cleanup during startup. */
        kill(command, SIGKILL);
        while (waitpid(command, NULL, 0) < 0 && errno == EINTR) {}
        _exit(127);
    }
    char byte = 'x';
    if (write(ready[1], &byte, 1) != 1) supervisor_stop = 1;
    close(ready[1]);
    sigset_t parent_death;
    sigemptyset(&parent_death);
    sigaddset(&parent_death, SIGTERM);
    sigprocmask(SIG_UNBLOCK, &parent_death, NULL);

    /* Keep only the request socket, not capture pipes or host descriptors. */
    bool closed = (control == 0 || close_range(0, (unsigned)control - 1, 0) == 0) &&
                  close_range((unsigned)control + 1, ~0U, 0) == 0;
    if (!closed) supervisor_stop = 1;
    bool stopping = false, have_status = false;
    int command_status = 0;
    for (;;)
    {
        SupervisorSignal(0);
        if (supervisor_stop && !stopping)
        {
            kill(-group, SIGKILL);
            stopping = true;
        }
        int status;
        pid_t child = waitpid(-group, &status, WNOHANG);
        if (child > 0)
        {
            if (child == command) { command_status = status; have_status = true; }
            continue;
        }
        if (child < 0)
        {
            if (errno == EINTR) continue;
            if (errno != ECHILD) _exit(127);
            /* An escaped parent can still own children in our group. ECHILD
             * alone does not mean the group is empty. Our live pid pins its
             * identity while we wait for those members too. */
            if (kill(-group, 0) != 0 && errno == ESRCH) break;
        }
        /* Polling also observes adopted descendants even when SIGCHLD is
         * blocked. No host/UI thread waits for this cleanup. */
        struct pollfd request = {.fd = stopping ? -1 : control, .events = POLLIN};
        if (poll(&request, 1, 10) > 0 && request.revents) supervisor_stop = 1;
    }
    if (!have_status) _exit(127);
    if (WIFSIGNALED(command_status))
    {
        int sig = WTERMSIG(command_status);
        struct sigaction action = {.sa_handler = SIG_DFL};
        sigemptyset(&action.sa_mask);
        sigaction(sig, &action, NULL);
        sigset_t unblocked;
        sigemptyset(&unblocked);
        sigaddset(&unblocked, sig);
        sigprocmask(SIG_UNBLOCK, &unblocked, NULL);
        kill(getpid(), sig);
        _exit(128 + sig);
    }
    _exit(WEXITSTATUS(command_status));
}
#endif

char *PicoBgTable_Spawn(PicoBgTable *table, PicoAgentId agent_id, const char *workspace,
                        const char *description, const char *command, char **error)
{
    int pipefd[2];
#if defined(__linux__)
    int control[2];
#endif
    PicoBgChild *child;
    pid_t pid;
    PicoBgJob *job;
    char id[PICO_BG_ID_MAX];
    char *desc_copy;
    char *cmd_copy;
    char *result;
    uint32_t serial;
    int flags;

    if (error)
    {
        *error = NULL;
    }
    if (!table || agent_id == 0)
    {
        if (error)
        {
            *error = DupError("background: missing table");
        }
        return NULL;
    }
    if (!description || !description[0] || !command || !command[0])
    {
        if (error)
        {
            *error = DupError("run_background: description and command are required");
        }
        return NULL;
    }
    if (strlen(description) > (size_t)PICO_BG_DESC_MAX)
    {
        if (error)
        {
            *error = DupError("run_background: description is too long");
        }
        return NULL;
    }
    if (strlen(command) > (size_t)PICO_BG_CMD_MAX)
    {
        if (error)
        {
            *error = DupError("run_background: command is too long");
        }
        return NULL;
    }

    pthread_mutex_lock(&table->mu);
    if (CountAgent(table, agent_id, true) >= PICO_BG_MAX_RUNNING)
    {
        pthread_mutex_unlock(&table->mu);
        if (error)
        {
            *error = DupError("run_background: too many running jobs");
        }
        return NULL;
    }
    while (CountAgent(table, agent_id, false) >= PICO_BG_MAX_RECORDS)
    {
        int oldest = OldestFinishedIndex(table, agent_id);
        if (oldest < 0)
        {
            pthread_mutex_unlock(&table->mu);
            if (error)
            {
                *error = DupError("run_background: too many jobs");
            }
            return NULL;
        }
        RemoveJobAt(table, oldest);
    }
    if (table->job_count >= PICO_BG_MAX_JOBS)
    {
        pthread_mutex_unlock(&table->mu);
        if (error)
        {
            *error = DupError("run_background: too many jobs");
        }
        return NULL;
    }
    serial = NextSerial(table, agent_id);
    if (serial == 0)
    {
        pthread_mutex_unlock(&table->mu);
        if (error)
        {
            *error = DupError("run_background: too many jobs");
        }
        return NULL;
    }
    snprintf(id, sizeof(id), "bg_%u", serial);

    desc_copy = JsonDup(description);
    cmd_copy = JsonDup(command);
    if (!desc_copy || !cmd_copy)
    {
        free(desc_copy);
        free(cmd_copy);
        if (error)
        {
            *error = DupError("run_background: out of memory");
        }
        pthread_mutex_unlock(&table->mu);
        return NULL;
    }

    if (OpenJobPipe(pipefd) != 0)
    {
        free(desc_copy);
        free(cmd_copy);
        if (error)
        {
            *error = DupError("run_background: pipe failed");
        }
        pthread_mutex_unlock(&table->mu);
        return NULL;
    }

#if defined(__linux__)
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, control) != 0)
    {
        close(pipefd[0]); close(pipefd[1]);
        free(desc_copy); free(cmd_copy);
        if (error) *error = DupError("run_background: control socket failed");
        pthread_mutex_unlock(&table->mu);
        return NULL;
    }
    pid_t parent = getpid();
    pid_t inherited_group = getpgrp();
#endif
    child = ReserveChild();
    if (!child)
    {
        close(pipefd[0]); close(pipefd[1]);
#if defined(__linux__)
        close(control[0]); close(control[1]);
#endif
        free(desc_copy); free(cmd_copy);
        if (error) *error = DupError("run_background: background cleanup capacity unavailable");
        pthread_mutex_unlock(&table->mu);
        return NULL;
    }
    pid = fork();
    if (pid < 0)
    {
        CancelChildReservation(child);
#if defined(__linux__)
        close(control[0]); close(control[1]);
#endif
        close(pipefd[0]);
        close(pipefd[1]);
        free(desc_copy);
        free(cmd_copy);
        if (error)
        {
            *error = DupError("run_background: fork failed");
        }
        pthread_mutex_unlock(&table->mu);
        return NULL;
    }
    if (pid == 0)
    {
#if defined(__linux__)
        close(control[1]);
        ConfigureSupervisor(parent);
#else
        setpgid(0, 0);
#endif
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0)
        {
            _exit(127);
        }
        close(pipefd[1]);
        {
            int nullfd = open("/dev/null", O_RDONLY);
            if (nullfd >= 0)
            {
                dup2(nullfd, STDIN_FILENO);
                close(nullfd);
            }
        }
#if defined(__linux__)
        /* Isolate descriptors before any filesystem work or the gated second
         * fork. The payload must not retain another job's control endpoint. */
        if ((control[0] > 3 && close_range(3, (unsigned)control[0] - 1, 0) != 0) ||
            close_range((unsigned)control[0] + 1, ~0U, 0) != 0) _exit(127);
#endif
        if (workspace && workspace[0] && chdir(workspace) != 0)
        {
            _exit(127);
        }
#if defined(__linux__)
        SuperviseJob(control[0], inherited_group);
#endif
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

#if defined(__linux__)
    close(control[0]);
#else
    setpgid(pid, pid);
#endif
    pthread_mutex_lock(&children_mu);
    child->pid = pid;
#if defined(__linux__)
    child->control = control[1];
#endif
    pthread_mutex_unlock(&children_mu);
    close(pipefd[1]);
    flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    }

    job = &table->jobs[table->job_count++];
    memset(job, 0, sizeof(*job));
    job->agent_id = agent_id;
    snprintf(job->id, sizeof(job->id), "%s", id);
    job->description = desc_copy;
    job->command = cmd_copy;
    job->child = child;
    job->fd = pipefd[0];
    job->status = PICO_BG_RUNNING;
    job->exit_code = 0;
    job->term_signal = 0;
    result = FormatJobJson(job, true);
    pthread_mutex_unlock(&table->mu);
    if (!result && error)
    {
        *error = DupError("run_background: out of memory");
    }
    return result;
}

char *PicoBgTable_Kill(PicoBgTable *table, PicoAgentId agent_id, const char *id, char **error)
{
    PicoBgJob *job;
    char *result;
    if (error)
    {
        *error = NULL;
    }
    if (!table || agent_id == 0 || !id || !id[0])
    {
        if (error)
        {
            *error = DupError("kill_background: missing id");
        }
        return NULL;
    }
    pthread_mutex_lock(&table->mu);
    job = FindJob(table, agent_id, id);
    if (!job)
    {
        pthread_mutex_unlock(&table->mu);
        if (error)
        {
            *error = DupError("kill_background: unknown id");
        }
        return NULL;
    }
    if (job->status == PICO_BG_RUNNING)
    {
        job->status = PICO_BG_KILLED;
        StopChild(job->child);
        job->term_signal = SIGKILL;
    }
    result = FormatJobJson(job, true);
    pthread_mutex_unlock(&table->mu);
    if (!result && error)
    {
        *error = DupError("kill_background: out of memory");
    }
    return result;
}

char *PicoBgTable_ListJson(PicoBgTable *table, PicoAgentId agent_id)
{
    JsonBuf b;
    int i;
    bool first = true;
    JsonBuf_Init(&b);
    JsonBuf_Putc(&b, '[');
    if (!table || agent_id == 0)
    {
        JsonBuf_Putc(&b, ']');
        return JsonBuf_Steal(&b);
    }
    pthread_mutex_lock(&table->mu);
    for (i = 0; i < table->job_count; i++)
    {
        char *item;
        if (table->jobs[i].agent_id != agent_id)
        {
            continue;
        }
        item = FormatJobJson(&table->jobs[i], true);
        if (!item)
        {
            continue;
        }
        if (!first)
        {
            JsonBuf_Putc(&b, ',');
        }
        first = false;
        JsonBuf_Puts(&b, item);
        free(item);
    }
    pthread_mutex_unlock(&table->mu);
    JsonBuf_Putc(&b, ']');
    return JsonBuf_Steal(&b);
}

char *PicoBgTable_Log(PicoBgTable *table, PicoAgentId agent_id, const char *id, char **error)
{
    PicoBgJob *job;
    char *out;
    if (error)
    {
        *error = NULL;
    }
    if (!table || agent_id == 0 || !id || !id[0])
    {
        if (error)
        {
            *error = DupError("log_background: missing id");
        }
        return NULL;
    }
    pthread_mutex_lock(&table->mu);
    job = FindJob(table, agent_id, id);
    if (!job)
    {
        pthread_mutex_unlock(&table->mu);
        if (error)
        {
            *error = DupError("log_background: unknown id");
        }
        return NULL;
    }
    if (!job->log || job->log_len == 0)
    {
        pthread_mutex_unlock(&table->mu);
        return JsonDup("(no output)");
    }
    out = (char *)malloc(job->log_len + 1);
    if (!out)
    {
        pthread_mutex_unlock(&table->mu);
        if (error)
        {
            *error = DupError("log_background: out of memory");
        }
        return NULL;
    }
    memcpy(out, job->log, job->log_len);
    out[job->log_len] = '\0';
    pthread_mutex_unlock(&table->mu);
    return out;
}

void PicoBgTable_ResetAgent(PicoBgTable *table, PicoAgentId agent_id)
{
    int i;
    if (!table || agent_id == 0)
    {
        return;
    }
    pthread_mutex_lock(&table->mu);
    for (i = table->job_count - 1; i >= 0; i--)
    {
        if (table->jobs[i].agent_id == agent_id)
        {
            RemoveJobAt(table, i);
        }
    }
    /* No records remain for this agent, so its id serial can be recycled: fresh ids
     * cannot collide with live records, and long sessions do not exhaust seqs. */
    for (i = 0; i < table->seq_count; i++)
    {
        if (table->seqs[i].agent_id == agent_id)
        {
            if (i < table->seq_count - 1)
            {
                memmove(&table->seqs[i], &table->seqs[i + 1],
                        (size_t)(table->seq_count - i - 1) * sizeof(table->seqs[0]));
            }
            table->seq_count--;
            break;
        }
    }
    pthread_mutex_unlock(&table->mu);
}

int PicoBgTable_RunningCount(PicoBgTable *table, PicoAgentId agent_id)
{
    int n;
    if (!table || agent_id == 0)
    {
        return 0;
    }
    pthread_mutex_lock(&table->mu);
    n = CountAgent(table, agent_id, true);
    pthread_mutex_unlock(&table->mu);
    return n;
}

int PicoBgTable_CopyJobs(PicoBgTable *table, PicoAgentId agent_id, PicoBgJobInfo *out, int cap)
{
    int i;
    int n = 0;
    if (!table || !out || cap <= 0 || agent_id == 0)
    {
        return 0;
    }
    pthread_mutex_lock(&table->mu);
    for (i = 0; i < table->job_count && n < cap; i++)
    {
        PicoBgJob *job = &table->jobs[i];
        PicoBgJobInfo *info;
        if (job->agent_id != agent_id)
        {
            continue;
        }
        info = &out[n++];
        memset(info, 0, sizeof(*info));
        snprintf(info->id, sizeof(info->id), "%s", job->id);
        snprintf(info->description, sizeof(info->description), "%s",
                 job->description ? job->description : "");
        snprintf(info->command, sizeof(info->command), "%s", job->command ? job->command : "");
        info->status = job->status;
        info->exit_code = job->exit_code;
        info->term_signal = job->term_signal;
    }
    pthread_mutex_unlock(&table->mu);
    return n;
}

bool PicoBgTable_CopyLog(PicoBgTable *table, PicoAgentId agent_id, const char *id, char *out,
                         size_t cap, size_t *len_out)
{
    PicoBgJob *job;
    size_t n;
    if (len_out)
    {
        *len_out = 0;
    }
    if (!table || !out || cap == 0 || agent_id == 0 || !id || !id[0])
    {
        return false;
    }
    pthread_mutex_lock(&table->mu);
    job = FindJob(table, agent_id, id);
    if (!job)
    {
        pthread_mutex_unlock(&table->mu);
        return false;
    }
    n = job->log_len;
    if (n >= cap)
    {
        n = cap - 1;
    }
    if (job->log && n > 0)
    {
        memcpy(out, job->log, n);
    }
    out[n] = '\0';
    if (len_out)
    {
        *len_out = n;
    }
    pthread_mutex_unlock(&table->mu);
    return true;
}
