#define _POSIX_C_SOURCE 200809L

#include "http_capture.h"

#ifdef PICO_DEBUG_SSE_CAPTURE

#include "config.h"
#include "json.h"
#include "path.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PICO_SSE_CAPTURE_KEEP 100

static pthread_mutex_t g_capture_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_capture_sequence;

static bool Timestamp(char *out, size_t cap, long *nanoseconds)
{
    struct timespec now;
    struct tm utc;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || !gmtime_r(&now.tv_sec, &utc))
    {
        return false;
    }
    if (nanoseconds)
    {
        *nanoseconds = now.tv_nsec;
    }
    return strftime(out, cap, "%Y-%m-%dT%H:%M:%S", &utc) > 0;
}

static bool MkdirParents(const char *path)
{
    char copy[4096];
    if (!PicoPath_Format(copy, sizeof(copy), "%s", path))
    {
        return false;
    }
    for (char *p = copy + 1; *p; p++)
    {
        if (*p != '/')
        {
            continue;
        }
        *p = '\0';
        if (mkdir(copy, 0700) != 0 && errno != EEXIST)
        {
            return false;
        }
        *p = '/';
    }
    return mkdir(copy, 0700) == 0 || errno == EEXIST;
}

static bool ValidateDirectory(const char *path, bool make_private)
{
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    if (fd < 0)
    {
        return false;
    }
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid();
    if (ok && make_private && fchmod(fd, 0700) != 0)
    {
        ok = false;
    }
    close(fd);
    return ok;
}

static bool EnsurePrivateDirectory(const char *path)
{
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
    {
        return false;
    }
    return ValidateDirectory(path, true);
}

static FILE *OpenPrivate(const char *path)
{
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags, 0600);
    if (fd < 0)
    {
        return NULL;
    }
    FILE *file = fdopen(fd, "wb");
    if (!file)
    {
        close(fd);
        unlink(path);
    }
    return file;
}

static int NameCompare(const void *a, const void *b)
{
    const char *const *left = (const char *const *)a;
    const char *const *right = (const char *const *)b;
    return strcmp(*left, *right);
}

static void Prune(const char *directory)
{
    DIR *dir = opendir(directory);
    if (!dir)
    {
        return;
    }
    char **names = NULL;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        size_t length = strlen(entry->d_name);
        if (strncmp(entry->d_name, "pico-sse-", strlen("pico-sse-")) != 0 ||
            length <= strlen(".json") ||
            strcmp(entry->d_name + length - strlen(".json"), ".json") != 0)
        {
            continue;
        }
        char **next = (char **)realloc(names, (count + 1) * sizeof(char *));
        if (!next)
        {
            break;
        }
        names = next;
        names[count] = JsonDup(entry->d_name);
        if (names[count])
        {
            count++;
        }
    }
    closedir(dir);

    qsort(names, count, sizeof(char *), NameCompare);
    size_t remove_count = count > PICO_SSE_CAPTURE_KEEP ? count - PICO_SSE_CAPTURE_KEEP : 0;
    for (size_t i = 0; i < remove_count; i++)
    {
        char path[4096];
        if (PicoPath_Format(path, sizeof(path), "%s/%s", directory, names[i]))
        {
            unlink(path);
        }
        size_t length = strlen(names[i]);
        names[i][length - strlen(".json")] = '\0';
        if (PicoPath_Format(path, sizeof(path), "%s/%s.sse", directory, names[i]))
        {
            unlink(path);
        }
    }
    for (size_t i = 0; i < count; i++)
    {
        free(names[i]);
    }
    free(names);
}

static void PutSize(JsonBuf *json, size_t value)
{
    char number[32];
    snprintf(number, sizeof(number), "%zu", value);
    JsonBuf_Puts(json, number);
}

void PicoHttpCapture_Begin(PicoHttpCapture *capture)
{
    if (!capture)
    {
        return;
    }
    memset(capture, 0, sizeof(*capture));

    char config[4096];
    char debug[4096];
    char timestamp[80];
    long nanoseconds = 0;
    if (!Pico_ConfigDir(config, sizeof(config)) ||
        !MkdirParents(config) || !ValidateDirectory(config, false) ||
        !PicoPath_Format(debug, sizeof(debug), "%s/debug", config) ||
        !EnsurePrivateDirectory(debug) ||
        !PicoPath_Format(capture->directory, sizeof(capture->directory),
                         "%s/sse", debug) ||
        !EnsurePrivateDirectory(capture->directory) ||
        !Timestamp(timestamp, sizeof(timestamp), &nanoseconds))
    {
        return;
    }
    char fraction[16];
    snprintf(fraction, sizeof(fraction), "%09dZ", (int)nanoseconds);
    if (!PicoPath_Format(capture->started_at, sizeof(capture->started_at),
                         "%s.%s", timestamp, fraction))
    {
        return;
    }

    char file_timestamp[80];
    if (!PicoPath_Format(file_timestamp, sizeof(file_timestamp), "%s", timestamp))
    {
        return;
    }
    for (char *p = file_timestamp; *p; p++)
    {
        if (*p == ':')
        {
            *p = '-';
        }
    }

    pthread_mutex_lock(&g_capture_mu);
    unsigned long sequence = ++g_capture_sequence;
    pthread_mutex_unlock(&g_capture_mu);
    if (!PicoPath_Format(capture->raw_name, sizeof(capture->raw_name),
                         "pico-sse-%s.%09ldZ-%ld-%lu.sse", file_timestamp, nanoseconds,
                         (long)getpid(), sequence) ||
        !PicoPath_Format(capture->raw_path, sizeof(capture->raw_path),
                         "%s/%s", capture->directory, capture->raw_name) ||
        !PicoPath_Format(capture->metadata_path, sizeof(capture->metadata_path),
                         "%s/%.*s.json", capture->directory,
                         (int)(strlen(capture->raw_name) - strlen(".sse")),
                         capture->raw_name))
    {
        capture->raw_name[0] = '\0';
        return;
    }
    capture->file = OpenPrivate(capture->raw_path);
    if (!capture->file)
    {
        capture->raw_name[0] = '\0';
    }
}

void PicoHttpCapture_Write(PicoHttpCapture *capture, const char *data, size_t length)
{
    if (!capture || !data || length == 0)
    {
        return;
    }
    capture->response_bytes += length;
    if (!capture->file || capture->write_failed)
    {
        return;
    }
    size_t wrote = fwrite(data, 1, length, (FILE *)capture->file);
    capture->captured_bytes += wrote;
    if (wrote != length)
    {
        capture->write_failed = 1;
    }
}

void PicoHttpCapture_Finish(PicoHttpCapture *capture, const char *url, long http_status,
                            const char *outcome, int transport_code,
                            const char *transport_error)
{
    if (!capture || !capture->file)
    {
        return;
    }
    FILE *raw = (FILE *)capture->file;
    bool complete = !capture->write_failed && fflush(raw) == 0;
    if (fclose(raw) != 0)
    {
        complete = false;
    }
    capture->file = NULL;
    complete = complete && capture->captured_bytes == capture->response_bytes;

    char finished_base[80] = {0};
    char finished_at[80] = {0};
    long finished_nanoseconds = 0;
    if (Timestamp(finished_base, sizeof(finished_base), &finished_nanoseconds))
    {
        char fraction[16];
        snprintf(fraction, sizeof(fraction), "%09dZ", (int)finished_nanoseconds);
        (void)PicoPath_Format(finished_at, sizeof(finished_at),
                              "%s.%s", finished_base, fraction);
    }

    JsonBuf json;
    JsonBuf_Init(&json);
    JsonBuf_Puts(&json, "{\n  \"started_at\": ");
    JsonBuf_String(&json, capture->started_at);
    JsonBuf_Puts(&json, ",\n  \"finished_at\": ");
    JsonBuf_String(&json, finished_at);
    JsonBuf_Puts(&json, ",\n  \"url\": ");
    JsonBuf_String(&json, url ? url : "");
    JsonBuf_Puts(&json, ",\n  \"http_status\": ");
    char number[64];
    snprintf(number, sizeof(number), "%ld", http_status);
    JsonBuf_Puts(&json, number);
    JsonBuf_Puts(&json, ",\n  \"outcome\": ");
    JsonBuf_String(&json, outcome ? outcome : "unknown");
    JsonBuf_Puts(&json, ",\n  \"transport_code\": ");
    snprintf(number, sizeof(number), "%d", transport_code);
    JsonBuf_Puts(&json, number);
    JsonBuf_Puts(&json, ",\n  \"transport_error\": ");
    JsonBuf_String(&json, transport_error ? transport_error : "");
    JsonBuf_Puts(&json, ",\n  \"response_bytes\": ");
    PutSize(&json, capture->response_bytes);
    JsonBuf_Puts(&json, ",\n  \"captured_bytes\": ");
    PutSize(&json, capture->captured_bytes);
    JsonBuf_Puts(&json, ",\n  \"capture_complete\": ");
    JsonBuf_Bool(&json, complete);
    JsonBuf_Puts(&json, ",\n  \"raw_file\": ");
    JsonBuf_String(&json, capture->raw_name);
    JsonBuf_Puts(&json, "\n}\n");

    char metadata_temp[4096];
    bool temp_path_ok = PicoPath_Format(metadata_temp, sizeof(metadata_temp),
                                        "%s.tmp", capture->metadata_path);
    FILE *metadata = temp_path_ok ? OpenPrivate(metadata_temp) : NULL;
    bool metadata_ok = metadata && json.data &&
                       fwrite(json.data, 1, json.len, metadata) == json.len &&
                       fflush(metadata) == 0;
    if (metadata && fclose(metadata) != 0)
    {
        metadata_ok = false;
    }
    if (metadata_ok && rename(metadata_temp, capture->metadata_path) != 0)
    {
        metadata_ok = false;
    }
    if (!metadata_ok)
    {
        if (temp_path_ok)
        {
            unlink(metadata_temp);
        }
        unlink(capture->metadata_path);
        unlink(capture->raw_path);
    }
    JsonBuf_Free(&json);

    if (metadata_ok)
    {
        pthread_mutex_lock(&g_capture_mu);
        Prune(capture->directory);
        pthread_mutex_unlock(&g_capture_mu);
    }
}

#else

void PicoHttpCapture_Begin(PicoHttpCapture *capture)
{
    (void)capture;
}

void PicoHttpCapture_Write(PicoHttpCapture *capture, const char *data, size_t length)
{
    (void)capture;
    (void)data;
    (void)length;
}

void PicoHttpCapture_Finish(PicoHttpCapture *capture, const char *url, long http_status,
                            const char *outcome, int transport_code,
                            const char *transport_error)
{
    (void)capture;
    (void)url;
    (void)http_status;
    (void)outcome;
    (void)transport_code;
    (void)transport_error;
}

#endif
