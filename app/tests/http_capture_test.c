#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "http_capture.h"
#include "json.h"
#include "path.h"

#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_config_dir[4096];

bool Pico_ConfigDir(char *out, size_t cap)
{
    return PicoPath_Format(out, cap, "%s", g_config_dir);
}

static int Fail(const char *message)
{
    fprintf(stderr, "http capture: %s\n", message);
    return 1;
}

static bool WritePrivate(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file)
    {
        return false;
    }
    bool ok = fwrite(text, 1, strlen(text), file) == strlen(text);
    if (fclose(file) != 0)
    {
        ok = false;
    }
    return ok;
}

static int CountSuffix(const char *directory, const char *suffix)
{
    DIR *dir = opendir(directory);
    if (!dir)
    {
        return -1;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        size_t name_len = strlen(entry->d_name);
        size_t suffix_len = strlen(suffix);
        if (name_len >= suffix_len && strcmp(entry->d_name + name_len - suffix_len, suffix) == 0)
        {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static bool PrivateMode(const char *path, mode_t expected)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & 0777) == expected;
}

typedef struct ConcurrentCapture {
    PicoHttpCapture capture;
    char payload[32];
} ConcurrentCapture;

static void *RunConcurrentCapture(void *user)
{
    ConcurrentCapture *item = (ConcurrentCapture *)user;
    PicoHttpCapture_Begin(&item->capture);
    PicoHttpCapture_Write(&item->capture, item->payload, strlen(item->payload));
    PicoHttpCapture_Finish(&item->capture, "https://concurrent.example.test", 200,
                           "completed", 0, "");
    return NULL;
}

static void Cleanup(const char *directory)
{
    DIR *dir = opendir(directory);
    if (dir)
    {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            {
                continue;
            }
            char path[4096];
            if (PicoPath_Format(path, sizeof(path), "%s/%s", directory, entry->d_name))
            {
                unlink(path);
            }
        }
        closedir(dir);
    }
    rmdir(directory);
    char debug[4096];
    if (PicoPath_Format(debug, sizeof(debug), "%s/debug", g_config_dir))
    {
        rmdir(debug);
    }
    rmdir(g_config_dir);
}

int main(void)
{
    char temp[] = "/tmp/pico-http-capture-XXXXXX";
    if (!mkdtemp(temp) || !PicoPath_Format(g_config_dir, sizeof(g_config_dir), "%s", temp))
    {
        return Fail("could not create temporary config directory");
    }
    char debug_dir[4096];
    char sse_dir[4096];
    if (!PicoPath_Format(debug_dir, sizeof(debug_dir), "%s/debug", g_config_dir) ||
        !PicoPath_Format(sse_dir, sizeof(sse_dir), "%s/sse", debug_dir) ||
        mkdir(debug_dir, 0755) != 0 || mkdir(sse_dir, 0755) != 0 ||
        chmod(debug_dir, 0755) != 0 || chmod(sse_dir, 0755) != 0)
    {
        Cleanup(sse_dir);
        return Fail("could not seed public capture directories");
    }

    PicoHttpCapture capture;
    PicoHttpCapture_Begin(&capture);
    if (!capture.file || !capture.directory[0] || !capture.raw_path[0] || !capture.metadata_path[0])
    {
        Cleanup(capture.directory);
        return Fail("capture did not open its raw response file");
    }

    char oldest_json[4096];
    char oldest_sse[4096];
    for (int i = 0; i < 100; i++)
    {
        char json_path[4096];
        char raw_path[4096];
        if (!PicoPath_Format(json_path, sizeof(json_path),
                             "%s/pico-sse-0000-%03d.json", capture.directory, i) ||
            !PicoPath_Format(raw_path, sizeof(raw_path),
                             "%s/pico-sse-0000-%03d.sse", capture.directory, i) ||
            !WritePrivate(json_path, "{}\n") || !WritePrivate(raw_path, "old"))
        {
            Cleanup(capture.directory);
            return Fail("could not seed retention fixtures");
        }
        if (i == 0)
        {
            (void)PicoPath_Format(oldest_json, sizeof(oldest_json), "%s", json_path);
            (void)PicoPath_Format(oldest_sse, sizeof(oldest_sse), "%s", raw_path);
        }
    }

    static const char first[] = "event: response.output_item.done\r\ndata: {\"type\":\"reasoning\"}\r\n\r\n";
    static const char second[] = "data: [DONE]\n\n";
    PicoHttpCapture_Write(&capture, first, sizeof(first) - 1);
    PicoHttpCapture_Write(&capture, second, sizeof(second) - 1);
    PicoHttpCapture_Finish(&capture, "https://api.example.test/v1/responses", 200,
                           "completed", 0, "");

    JsonBuf expected;
    JsonBuf_Init(&expected);
    JsonBuf_Append(&expected, first, sizeof(first) - 1);
    JsonBuf_Append(&expected, second, sizeof(second) - 1);
    size_t raw_len = 0;
    char *raw = Pico_ReadFile(capture.raw_path, &raw_len);
    bool raw_ok = raw && raw_len == expected.len && memcmp(raw, expected.data, expected.len) == 0;
    free(raw);
    JsonBuf_Free(&expected);
    if (!raw_ok)
    {
        Cleanup(capture.directory);
        return Fail("raw capture did not preserve the exact response bytes");
    }

    size_t metadata_len = 0;
    char *metadata = Pico_ReadFile(capture.metadata_path, &metadata_len);
    JsonDoc doc;
    int parsed = metadata ? JsonParse(&doc, metadata, metadata_len) : -1;
    if (parsed != 0)
    {
        free(metadata);
        Cleanup(capture.directory);
        return Fail("metadata is missing or invalid JSON");
    }
    char *url = JsonObjStr(&doc, 0, "url");
    char *outcome = JsonObjStr(&doc, 0, "outcome");
    char *raw_file = JsonObjStr(&doc, 0, "raw_file");
    bool metadata_ok = url && strcmp(url, "https://api.example.test/v1/responses") == 0 &&
                       outcome && strcmp(outcome, "completed") == 0 &&
                       raw_file && strcmp(raw_file, capture.raw_name) == 0 &&
                       JsonObjInt(&doc, 0, "http_status", 0) == 200 &&
                       JsonObjInt(&doc, 0, "response_bytes", -1) ==
                           (int)((sizeof(first) - 1) + (sizeof(second) - 1)) &&
                       JsonObjInt(&doc, 0, "captured_bytes", -1) ==
                           (int)((sizeof(first) - 1) + (sizeof(second) - 1)) &&
                       JsonEq(&doc, JsonObjGet(&doc, 0, "capture_complete"), "true");
    free(url);
    free(outcome);
    free(raw_file);
    JsonFree(&doc);
    free(metadata);
    if (!metadata_ok)
    {
        Cleanup(capture.directory);
        return Fail("metadata did not describe the completed capture");
    }

    bool retention_ok = CountSuffix(capture.directory, ".json") == 100 &&
                        CountSuffix(capture.directory, ".sse") == 100 &&
                        CountSuffix(capture.directory, ".tmp") == 0 &&
                        access(oldest_json, F_OK) != 0 && access(oldest_sse, F_OK) != 0;
    if (!retention_ok)
    {
        Cleanup(capture.directory);
        return Fail("retention did not keep exactly the newest 100 capture pairs");
    }
    if (!PrivateMode(capture.directory, 0700) ||
        !PrivateMode(capture.raw_path, 0600) ||
        !PrivateMode(capture.metadata_path, 0600))
    {
        Cleanup(capture.directory);
        return Fail("capture directory or files are not private");
    }

    Cleanup(capture.directory);

    char unsafe_config[] = "/tmp/pico-http-capture-unsafe-XXXXXX";
    char outside[] = "/tmp/pico-http-capture-outside-XXXXXX";
    if (!mkdtemp(unsafe_config) || !mkdtemp(outside) ||
        !PicoPath_Format(g_config_dir, sizeof(g_config_dir), "%s", unsafe_config) ||
        !PicoPath_Format(debug_dir, sizeof(debug_dir), "%s/debug", g_config_dir) ||
        symlink(outside, debug_dir) != 0)
    {
        return Fail("could not create symlink safety fixture");
    }
    memset(&capture, 0, sizeof(capture));
    PicoHttpCapture_Begin(&capture);
    bool symlink_rejected = !capture.file;
    PicoHttpCapture_Write(&capture, "secret", strlen("secret"));
    PicoHttpCapture_Finish(&capture, "https://example.test", 200, "completed", 0, "");
    unlink(debug_dir);
    rmdir(unsafe_config);
    rmdir(outside);
    if (!symlink_rejected)
    {
        return Fail("capture followed an unsafe debug-directory symlink");
    }

    char failure_config[] = "/tmp/pico-http-capture-failure-XXXXXX";
    if (!mkdtemp(failure_config) ||
        !PicoPath_Format(g_config_dir, sizeof(g_config_dir), "%s", failure_config))
    {
        return Fail("could not create metadata-failure fixture");
    }
    memset(&capture, 0, sizeof(capture));
    PicoHttpCapture_Begin(&capture);
    if (!capture.file || mkdir(capture.metadata_path, 0700) != 0)
    {
        Cleanup(capture.directory);
        return Fail("could not block metadata publication");
    }
    PicoHttpCapture_Write(&capture, "sensitive", strlen("sensitive"));
    PicoHttpCapture_Finish(&capture, "https://failure.example.test", 200,
                           "completed", 0, "");
    bool failed_pair_removed = access(capture.raw_path, F_OK) != 0 &&
                               access(capture.metadata_path, F_OK) == 0;
    rmdir(capture.metadata_path);
    Cleanup(capture.directory);
    if (!failed_pair_removed)
    {
        return Fail("metadata publication failure left a raw capture behind");
    }

    char concurrent_config[] = "/tmp/pico-http-capture-concurrent-XXXXXX";
    if (!mkdtemp(concurrent_config) ||
        !PicoPath_Format(g_config_dir, sizeof(g_config_dir), "%s", concurrent_config))
    {
        return Fail("could not create concurrent capture fixture");
    }
    enum { CONCURRENT_COUNT = 16 };
    ConcurrentCapture concurrent[CONCURRENT_COUNT];
    pthread_t threads[CONCURRENT_COUNT];
    memset(concurrent, 0, sizeof(concurrent));
    int started = 0;
    for (int i = 0; i < CONCURRENT_COUNT; i++)
    {
        snprintf(concurrent[i].payload, sizeof(concurrent[i].payload), "payload-%d", i);
        if (pthread_create(&threads[i], NULL, RunConcurrentCapture, &concurrent[i]) != 0)
        {
            break;
        }
        started++;
    }
    for (int i = 0; i < started; i++)
    {
        pthread_join(threads[i], NULL);
    }
    bool concurrent_ok = started == CONCURRENT_COUNT;
    for (int i = 0; concurrent_ok && i < CONCURRENT_COUNT; i++)
    {
        size_t length = 0;
        char *content = Pico_ReadFile(concurrent[i].capture.raw_path, &length);
        concurrent_ok = concurrent[i].capture.raw_path[0] &&
                        concurrent[i].capture.metadata_path[0] && content &&
                        length == strlen(concurrent[i].payload) &&
                        memcmp(content, concurrent[i].payload, length) == 0 &&
                        access(concurrent[i].capture.metadata_path, F_OK) == 0;
        free(content);
    }
    const char *concurrent_dir = concurrent[0].capture.directory;
    concurrent_ok = concurrent_ok && CountSuffix(concurrent_dir, ".sse") == CONCURRENT_COUNT &&
                    CountSuffix(concurrent_dir, ".json") == CONCURRENT_COUNT &&
                    CountSuffix(concurrent_dir, ".tmp") == 0;
    Cleanup(concurrent_dir);
    if (!concurrent_ok)
    {
        return Fail("concurrent captures did not publish complete unique pairs");
    }
    return 0;
}
