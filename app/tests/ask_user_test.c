#include "builtins/ask_user.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ExpectRequest(const char *name, const char *args, const char *expected)
{
    char error[256] = {0};
    char *request = PicoAskUser_BuildRequest(args, error, sizeof(error));
    if (!request || strcmp(request, expected) != 0)
    {
        fprintf(stderr, "%s: expected %s, got %s (%s)\n", name, expected,
                request ? request : "NULL", error);
        free(request);
        return 1;
    }
    free(request);
    return 0;
}

static int ExpectError(const char *name, const char *args, const char *message)
{
    char error[256] = {0};
    char *request = PicoAskUser_BuildRequest(args, error, sizeof(error));
    if (request || !strstr(error, message))
    {
        fprintf(stderr, "%s: expected error containing '%s', got %s (%s)\n", name, message,
                request ? request : "NULL", error);
        free(request);
        return 1;
    }
    return 0;
}

static char *BuildQuestionList(int count)
{
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\"questions\":[");
    for (int i = 0; i < count; i++)
    {
        if (i > 0)
        {
            JsonBuf_Putc(&b, ',');
        }
        JsonBuf_Puts(&b, "{\"id\":");
        char id[24];
        snprintf(id, sizeof(id), "q%d", i);
        JsonBuf_String(&b, id);
        JsonBuf_Puts(&b, ",\"question\":\"Question?\",\"kind\":\"text\"}");
    }
    JsonBuf_Puts(&b, "]}");
    return JsonBuf_Steal(&b);
}

int main(void)
{
    int failed = 0;
    failed |= ExpectRequest(
        "mixed questionnaire",
        "{\"questions\":[{\"id\":\"target\",\"question\":\"Which?\",\"kind\":\"select\","
        "\"options\":[\"CLI\",\"GUI\"]},{\"id\":\"notes\","
        "\"question\":\"Notes?\",\"kind\":\"text\",\"options\":\"ignored\"}]}",
        "{\"type\":\"questionnaire\",\"ui\":\"custom\",\"questions\":[{\"id\":\"target\","
        "\"question\":\"Which?\",\"kind\":\"select\",\"options\":[\"CLI\",\"GUI\"]},"
        "{\"id\":\"notes\",\"question\":\"Notes?\",\"kind\":\"text\"}]}");

    failed |= ExpectError(
        "duplicate ids",
        "{\"questions\":[{\"id\":\"same\",\"question\":\"One?\",\"kind\":\"text\"},"
        "{\"id\":\"same\",\"question\":\"Two?\",\"kind\":\"text\"}]}",
        "duplicated");
    failed |= ExpectError(
        "select requires options",
        "{\"questions\":[{\"id\":\"choice\",\"question\":\"Choose?\",\"kind\":\"select\"}]}",
        "needs between 1 and 20 options");
    failed |= ExpectError("empty questionnaire", "{\"questions\":[]}", "between 1 and 24 items");

    char *too_many = BuildQuestionList(25);
    if (!too_many)
    {
        fprintf(stderr, "question limit: allocation failed\n");
        failed = 1;
    }
    else
    {
        failed |= ExpectError("question limit", too_many, "between 1 and 24 items");
    }
    free(too_many);
    return failed;
}
