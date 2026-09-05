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
        "options");
    char error[256] = {0};
    char *empty = PicoAskUser_BuildRequest("{\"questions\":[]}", error, sizeof(error));
    int maximum = 0;
    if (empty || sscanf(error, "questions must contain between 1 and %d items", &maximum) != 1 ||
        maximum < 1)
    {
        fprintf(stderr, "empty questionnaire must report its accepted count range\n");
        free(empty);
        return 1;
    }

    char *at_limit = BuildQuestionList(maximum);
    char *too_many = BuildQuestionList(maximum + 1);
    if (!at_limit || !too_many)
    {
        fprintf(stderr, "question limit: allocation failed\n");
        failed = 1;
    }
    else
    {
        char *accepted = PicoAskUser_BuildRequest(at_limit, error, sizeof(error));
        if (!accepted)
        {
            fprintf(stderr, "question count at advertised limit must be accepted: %s\n", error);
            failed = 1;
        }
        free(accepted);
        failed |= ExpectError("question limit", too_many, "questions must contain between");
    }
    free(at_limit);
    free(too_many);
    return failed;
}
