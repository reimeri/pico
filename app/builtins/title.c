#include "pico/plugin.h"
#include "host_internal.h"
#include "workspace_internal.h"
#include "session.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

static const char *kParams =
    "{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":72,"
    "\"description\":\"Succinct session title (a few words, not a paragraph). Keep it stable unless the goal changes.\"}},"
    "\"required\":[\"title\"],\"additionalProperties\":false}";

static char *ParseTitle(const char *json)
{
    JsonDoc doc;
    if (!json || !JsonValidSyntax(json, strlen(json)) || JsonParse(&doc, json, strlen(json)) != 0)
    {
        return NULL;
    }
    int token = JsonObjGet(&doc, 0, "title");
    char *title = JsonIsObject(&doc, 0) && JsonObjLen(&doc, 0) == 1 && JsonIsString(&doc, token) ? JsonStrDup(&doc, token) : NULL;
    JsonFree(&doc);
    if (!title)
    {
        return NULL;
    }

    /* Syntax validation guarantees UTF-8; count code points, not encoded bytes.
     * Reject controls before trimming so newlines/tabs cannot become a title. */
    int count = 0;
    for (const unsigned char *p = (const unsigned char *)title; *p; p++)
    {
        if (*p < 0x20 || *p == 0x7F || (*p == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F) ||
            (*p == 0xE2 && p[1] == 0x80 && (p[2] == 0xA8 || p[2] == 0xA9)))
        {
            free(title);
            return NULL;
        }
        if ((*p & 0xC0) != 0x80) count++;
    }
    char *start = title;
    while (*start == ' ') start++;
    char *end = title + strlen(title);
    while (end > start && end[-1] == ' ') end--;
    count -= (int)((start - title) + (title + strlen(title) - end));
    if (count < 1 || count > 72)
    {
        free(title);
        return NULL;
    }
    size_t length = (size_t)(end - start);
    memmove(title, start, length);
    title[length] = '\0';
    return title;
}

static void SetTitle(PicoAgentContext *ctx, const char *args, PicoToolResult *out, void *state)
{
    (void)ctx;
    (void)state;
    char *title = ParseTitle(args);
    if (!title)
    {
        out->is_error = true;
        out->output = JsonDup("set_title: title must be a non-empty, single-line string of at most 72 characters, without control characters. Surrounding spaces are trimmed.");
        return;
    }
    JsonBuf details;
    JsonBuf_Init(&details);
    JsonBuf_Puts(&details, "{\"title\":");
    JsonBuf_String(&details, title);
    JsonBuf_Putc(&details, '}');
    out->details_json = JsonBuf_Steal(&details);
    JsonBuf output;
    JsonBuf_Init(&output);
    JsonBuf_Puts(&output, "Session title requested: ");
    JsonBuf_String(&output, title);
    out->output = JsonBuf_Steal(&output);
    free(title);
    if (!out->output || !out->details_json)
    {
        free(out->output);
        free(out->details_json);
        out->details_json = NULL;
        out->output = JsonDup("set_title: could not build title update.");
        out->is_error = true;
    }
}

static bool ApplyTitle(PicoWorkspace *workspace, PicoAgentId agent_id, const char *details, bool replay, void *state)
{
    (void)state;
    char *title = ParseTitle(details);
    if (!title) return false;
    bool ok = true;
    if (!replay)
    {
        PicoHost *host = pico_workspace_host(workspace);
        PicoAgent *agent = PicoHost_FindAgent(host, agent_id);
        ok = agent && agent->workspace == workspace &&
             PicoSession_LogTitle(host, agent, title) == PICO_SESSION_WRITE_OK;
    }
    /* Live apply queues the title before its tool result. Replay/reload must
     * not write historical titles back over the authoritative session header. */
    free(title);
    return ok;
}

static int Init(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    return pico_add_tool(workspace, "set_title",
        "Change the title of your own session, including a subagent session. Use a succinct, non-empty, single-line title of at most 72 characters. Surrounding spaces are trimmed. Keep it stable unless the goal changes.",
        kParams, SetTitle, ApplyTitle, PICO_TOOL_SEQUENTIAL) ? 0 : 1;
}

PicoExt pico_ext_title(void)
{
    return (PicoExt){.abi = PICO_EXT_ABI, .name = "session-title", .description = "Agent session titles", .workspace_init = Init};
}
