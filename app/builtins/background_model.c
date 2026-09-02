#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "background_model.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#define PICO_BG_MAX_JOBS (PICO_MAX_AGENTS * PICO_BG_MAX_RECORDS)
#define PICO_BG_READ_CHUNK 4096
#define PICO_BG_READ_BUDGET (16 * PICO_BG_READ_CHUNK)
/* Bound on waiting for SIGKILLed children to become reapable before their records
 * are removed; a record removed while unreaped leaves a zombie nobody can waitpid. */
#define PICO_BG_REAP_TRIES 100
#define PICO_BG_REAP_WAIT_NS 1000000L

typedef struct PicoBgJob {
    PicoAgentId agent_id;
    char id[PICO_BG_ID_MAX];
    char *description;
    char *command;
    pid_t pid;
    pid_t pgid;
    int fd;
    PicoBgStatus status;
    bool kill_requested;
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

static void KillProcessGroup(pid_t pgid)
{
    if (pgid <= 0)
    {
        return;
    }
    if (kill(-pgid, SIGKILL) != 0)
    {
        kill(pgid, SIGKILL);
    }
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
    if (!job->reaped && job->pid > 0)
    {
        KillProcessGroup(job->pgid > 0 ? job->pgid : job->pid);
        (void)waitpid(job->pid, NULL, WNOHANG);
    }
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

static void DrainFd(PicoBgJob *job, bool until_eof)
{
    char buf[PICO_BG_READ_CHUNK];
    size_t budget = until_eof ? (size_t)-1 : (size_t)PICO_BG_READ_BUDGET;
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
            if (budget != (size_t)-1)
            {
                budget = budget > (size_t)n ? budget - (size_t)n : 0;
            }
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
    if (job->status != PICO_BG_KILLED)
    {
        job->status = PICO_BG_EXITED;
    }
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
        if (job->term_signal > 0)
        {
            JsonBuf_Puts(&b, ",\"signal\":");
            JsonBuf_Int(&b, job->term_signal);
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
    int i;
    if (!table)
    {
        return;
    }
    pthread_mutex_lock(&table->mu);
    for (i = 0; i < table->job_count; i++)
    {
        PicoBgJob *job = &table->jobs[i];
        if (!job->reaped && job->pid > 0)
        {
            KillProcessGroup(job->pgid > 0 ? job->pgid : job->pid);
        }
    }
    pthread_mutex_unlock(&table->mu);
    for (i = 0; i < 40; i++)
    {
        PicoBgTable_Pump(table);
        pthread_mutex_lock(&table->mu);
        {
            int running = 0;
            int j;
            for (j = 0; j < table->job_count; j++)
            {
                if (table->jobs[j].status == PICO_BG_RUNNING)
                {
                    running++;
                }
            }
            pthread_mutex_unlock(&table->mu);
            if (running == 0)
            {
                break;
            }
        }
        {
            struct timespec req = {.tv_sec = 0, .tv_nsec = 10000000L};
            nanosleep(&req, NULL);
        }
    }
    pthread_mutex_lock(&table->mu);
    while (table->job_count > 0)
    {
        RemoveJobAt(table, table->job_count - 1);
    }
    pthread_mutex_unlock(&table->mu);
    pthread_mutex_destroy(&table->mu);
    free(table);
}

void PicoBgTable_Pump(PicoBgTable *table)
{
    int i;
    if (!table)
    {
        return;
    }
    pthread_mutex_lock(&table->mu);
    for (i = 0; i < table->job_count; i++)
    {
        PicoBgJob *job = &table->jobs[i];
        int status = 0;
        pid_t waited;
        DrainFd(job, false);
        if (job->reaped || job->pid <= 0)
        {
            continue;
        }
        waited = waitpid(job->pid, &status, WNOHANG);
        if (waited == job->pid)
        {
            DrainFd(job, true);
            ReapJob(job, status);
        }
    }
    pthread_mutex_unlock(&table->mu);
}

char *PicoBgTable_Spawn(PicoBgTable *table, PicoAgentId agent_id, const char *workspace,
                        const char *description, const char *command, char **error)
{
    int pipefd[2];
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
    pthread_mutex_unlock(&table->mu);

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
        return NULL;
    }

    if (pipe(pipefd) != 0)
    {
        free(desc_copy);
        free(cmd_copy);
        if (error)
        {
            *error = DupError("run_background: pipe failed");
        }
        return NULL;
    }

    pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        free(desc_copy);
        free(cmd_copy);
        if (error)
        {
            *error = DupError("run_background: fork failed");
        }
        return NULL;
    }
    if (pid == 0)
    {
        setpgid(0, 0);
#if defined(__linux__)
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() == 1)
        {
            _exit(127);
        }
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
        if (workspace && workspace[0] && chdir(workspace) != 0)
        {
            _exit(127);
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    setpgid(pid, pid);
    close(pipefd[1]);
    flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    }

    pthread_mutex_lock(&table->mu);
    if (CountAgent(table, agent_id, true) >= PICO_BG_MAX_RUNNING ||
        table->job_count >= PICO_BG_MAX_JOBS)
    {
        pthread_mutex_unlock(&table->mu);
        KillProcessGroup(pid);
        (void)waitpid(pid, NULL, 0);
        close(pipefd[0]);
        free(desc_copy);
        free(cmd_copy);
        if (error)
        {
            *error = DupError("run_background: too many running jobs");
        }
        return NULL;
    }
    job = &table->jobs[table->job_count++];
    memset(job, 0, sizeof(*job));
    job->agent_id = agent_id;
    snprintf(job->id, sizeof(job->id), "%s", id);
    job->description = desc_copy;
    job->command = cmd_copy;
    job->pid = pid;
    job->pgid = pid;
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
    pid_t pgid = 0;
    pid_t pid = 0;
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
    if (job->status == PICO_BG_RUNNING && job->pid > 0)
    {
        job->kill_requested = true;
        job->status = PICO_BG_KILLED;
        pgid = job->pgid > 0 ? job->pgid : job->pid;
        pid = job->pid;
    }
    result = FormatJobJson(job, true);
    pthread_mutex_unlock(&table->mu);
    if (pid > 0)
    {
        KillProcessGroup(pgid);
    }
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

static bool AgentHasUnreapedJob(PicoBgTable *table, PicoAgentId agent_id)
{
    int i;
    for (i = 0; i < table->job_count; i++)
    {
        if (table->jobs[i].agent_id == agent_id && !table->jobs[i].reaped &&
            table->jobs[i].pid > 0)
        {
            return true;
        }
    }
    return false;
}

void PicoBgTable_ResetAgent(PicoBgTable *table, PicoAgentId agent_id)
{
    int i;
    if (!table || agent_id == 0)
    {
        return;
    }
    pthread_mutex_lock(&table->mu);
    for (i = 0; i < table->job_count; i++)
    {
        PicoBgJob *job = &table->jobs[i];
        if (job->agent_id == agent_id && !job->reaped && job->pid > 0)
        {
            KillProcessGroup(job->pgid > 0 ? job->pgid : job->pid);
            job->kill_requested = true;
            job->status = PICO_BG_KILLED;
        }
    }
    pthread_mutex_unlock(&table->mu);
    /* Wait (briefly, bounded) until the killed children are reaped; removing a
     * record while its child is still a zombie leaks it for the host's lifetime. */
    for (i = 0; i < PICO_BG_REAP_TRIES; i++)
    {
        bool pending;
        PicoBgTable_Pump(table);
        pthread_mutex_lock(&table->mu);
        pending = AgentHasUnreapedJob(table, agent_id);
        pthread_mutex_unlock(&table->mu);
        if (!pending)
        {
            break;
        }
        {
            struct timespec req = {.tv_sec = 0, .tv_nsec = PICO_BG_REAP_WAIT_NS};
            nanosleep(&req, NULL);
        }
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
