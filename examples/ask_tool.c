// Example workspace-scoped tool that asks the user before returning.
// Copy to ~/.config/pico/extensions/ or <workspace>/.pico/extensions/ then F5.
//
//   mkdir -p ~/.config/pico/extensions/ask
//   cp examples/ask_tool.c ~/.config/pico/extensions/ask/

#include "pico/plugin.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

static const char *kParams =
    "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\",\"description\":"
    "\"Confirmation prompt\"}},\"required\":[\"message\"]}";

static void AskRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    JsonDoc doc;
    const char *src = args_json ? args_json : "";
    if (JsonParse(&doc, src, strlen(src)) != 0)
    {
        if (out)
        {
            out->output = JsonDup("ask: bad json");
            out->is_error = true;
        }
        return;
    }
    char *message = JsonObjStr(&doc, 0, "message");
    JsonFree(&doc);
    if (!message || !message[0])
    {
        free(message);
        if (out)
        {
            out->output = JsonDup("ask: missing message");
            out->is_error = true;
        }
        return;
    }

    JsonBuf req;
    JsonBuf_Init(&req);
    JsonBuf_Puts(&req, "{\"type\":\"confirm\",\"message\":");
    JsonBuf_String(&req, message);
    JsonBuf_Putc(&req, '}');
    free(message);
    char *request = JsonBuf_Steal(&req);

    char *answer = NULL;
    int rc = pico_tool_ask(ctx, request, &answer);
    free(request);
    if (rc != PICO_ASK_OK)
    {
        free(answer);
        if (out)
        {
            out->output = JsonDup(rc == PICO_ASK_CANCEL ? "ask: cancelled" : "ask: failed");
            out->is_error = true;
        }
        return;
    }
    if (out)
    {
        out->output = answer ? answer : JsonDup("{}");
    }
    else
    {
        free(answer);
    }
}

static int AskInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_add_tool(workspace, "confirm", "Ask the user to confirm something", kParams, AskRun, NULL);
    return 0;
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "confirm",
        .description = "Builtin confirm overlay via pico_tool_ask",
        .workspace_init = AskInit,
    };
}
