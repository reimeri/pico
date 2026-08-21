#define _POSIX_C_SOURCE 200809L

#include "agent.h"
#include "json.h"
#include "session.h"
#include "settings.h"
#include "usage.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_config_dir[4096];

static int Fail(const char *message)
{
    fprintf(stderr, "session usage: %s\n", message);
    return 1;
}

void Pico_ConfigDir(char *out, size_t cap)
{
    snprintf(out, cap, "%s", g_config_dir);
}

void Pico_MkdirP(const char *path)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path ? path : "");
    for (char *p = buf + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(buf, 0700);
            *p = '/';
        }
    }
    mkdir(buf, 0700);
}

void Pico_RandomHex(char *out, size_t cap)
{
    static unsigned value;
    snprintf(out, cap, "%08x", ++value);
}

void Pico_IsoTime(char *out, size_t cap, bool filename)
{
    snprintf(out, cap, "%s", filename ? "20260101_000000" : "2026-01-01T00:00:00Z");
}

const char *PicoAgent_CacheKey(const PicoApp *app)
{
    (void)app;
    return "cache-key";
}

void PicoAgent_SetCacheKey(PicoApp *app, const char *key)
{
    (void)app;
    (void)key;
}

void PicoAgent_RotateCacheKey(PicoApp *app)
{
    (void)app;
}

bool PicoAgent_IsBusy(const PicoApp *app)
{
    (void)app;
    return false;
}

void PicoAgent_DismissError(PicoApp *app)
{
    if (app)
    {
        free(app->agent_error);
        app->agent_error = NULL;
    }
}

void PicoAgent_ClearInput(PicoApp *app)
{
    (void)app;
}

void PicoAgent_PushHistoryUser(PicoApp *app, const char *text)
{
    (void)app;
    (void)text;
}

void PicoAgent_PushHistoryAssistant(PicoApp *app, const char *text)
{
    (void)app;
    (void)text;
}

void PicoAgent_PushHistoryFunctionCall(PicoApp *app, const char *call_id, const char *name, const char *args)
{
    (void)app;
    (void)call_id;
    (void)name;
    (void)args;
}

void PicoAgent_PushHistoryFunctionOutput(PicoApp *app, const char *call_id, const char *output)
{
    (void)app;
    (void)call_id;
    (void)output;
}

void PicoApp_AddMessage(PicoApp *app, PicoRole role, const char *text)
{
    (void)app;
    (void)role;
    (void)text;
}

void PicoApp_AppendAssistant(PicoApp *app, const char *text)
{
    (void)app;
    (void)text;
}

void PicoApp_AddToolCall(PicoApp *app, const char *name, const char *args_json)
{
    (void)app;
    (void)name;
    (void)args_json;
}

void PicoApp_SetLastToolOutput(PicoApp *app, const char *output, bool is_error)
{
    (void)app;
    (void)output;
    (void)is_error;
}

void PicoApp_ClearMessages(PicoApp *app)
{
    (void)app;
}

PicoModel *PicoSettings_ActiveModel(PicoApp *app)
{
    (void)app;
    return NULL;
}

void PicoSettings_SyncActive(PicoApp *app)
{
    (void)app;
}

static bool AppendRaw(const char *path, const char *line)
{
    FILE *f = fopen(path, "ab");
    if (!f)
    {
        return false;
    }
    fprintf(f, "%s\n", line);
    fclose(f);
    return true;
}

int main(void)
{
    char temp[] = "/tmp/pico-session-usage-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create temporary directory");
    }
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);

    PicoApp writer;
    memset(&writer, 0, sizeof(writer));
    snprintf(writer.workspace, sizeof(writer.workspace), "/workspace");
    snprintf(writer.settings.model, sizeof(writer.settings.model), "test-model");
    PicoSession_LogUsage(&writer, 100, 20);
    PicoSession_LogUsage(&writer, 200, 150);
    PicoSession_LogAssistant(&writer, "assistant response");
    PicoSession_LogCompaction(&writer, "brief", 200);
    if (!writer.session_path[0])
    {
        return Fail("usage did not create a session file");
    }

    size_t file_len = 0;
    char *file = Pico_ReadFile(writer.session_path, &file_len);
    if (!file || !strstr(file, "\"version\":2") || !strstr(file, "\"type\":\"usage\"") ||
        strstr(file, "\"usage\":{"))
    {
        free(file);
        return Fail("session schema did not use dedicated version 2 usage events");
    }
    free(file);

    PicoApp compacted;
    memset(&compacted, 0, sizeof(compacted));
    PicoSession_Start(&compacted, PICO_SESSION_NEW, writer.session_path);
    if (compacted.session_input_tokens != 300 || compacted.session_cached_tokens != 170 ||
        compacted.tokens_used != 0 || compacted.tokens_cached != 0)
    {
        return Fail("replay did not retain totals and clear latest usage at compaction");
    }

    if (!AppendRaw(writer.session_path, "{\"type\":\"usage\",\"input_tokens\":50,\"cached_tokens\":-3}") ||
        !AppendRaw(writer.session_path, "{\"type\":\"usage\",\"input_tokens\":-4,\"cached_tokens\":2}") ||
        !AppendRaw(writer.session_path, "{\"type\":\"usage\",\"input_tokens\":10,\"cached_tokens\":20}"))
    {
        return Fail("could not append replay boundary cases");
    }

    PicoApp replayed;
    memset(&replayed, 0, sizeof(replayed));
    PicoSession_Start(&replayed, PICO_SESSION_NEW, writer.session_path);
    if (replayed.session_input_tokens != 360 || replayed.session_cached_tokens != 180 ||
        replayed.tokens_used != 10 || replayed.tokens_cached != 10)
    {
        return Fail("replay did not normalize and aggregate usage events");
    }

    PicoApp opened;
    memset(&opened, 0, sizeof(opened));
    snprintf(opened.workspace, sizeof(opened.workspace), "/workspace");
    opened.session_input_tokens = 999;
    opened.session_cached_tokens = 999;
    if (PicoSession_Open(&opened, writer.session_id) != 0 || opened.session_input_tokens != 360 ||
        opened.session_cached_tokens != 180 || opened.tokens_used != 10 || opened.tokens_cached != 10)
    {
        return Fail("session open did not reset and rebuild usage totals");
    }

    PicoSession_Reset(&replayed);
    if (replayed.session_input_tokens != 0 || replayed.session_cached_tokens != 0 ||
        replayed.tokens_used != 0 || replayed.tokens_cached != 0)
    {
        return Fail("session reset did not clear usage state");
    }

    unlink(writer.session_path);
    return 0;
}
