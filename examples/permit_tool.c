// Workspace-scoped permission prompt in front of every tool, including builtin sh.
// Copy to ~/.config/pico/extensions/ or <workspace>/.pico/extensions/ then F5.
//
//   mkdir -p ~/.config/pico/extensions/permit
//   cp examples/permit_tool.c ~/.config/pico/extensions/permit/

#include "pico/plugin.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

static void PermitBefore(PicoAgentContext *ctx, PicoToolEvent *ev, void *state)
{
    (void)state;
    JsonBuf msg;
    JsonBuf_Init(&msg);
    JsonBuf_Puts(&msg, "Allow `");
    JsonBuf_Puts(&msg, ev->name && ev->name[0] ? ev->name : "tool");
    JsonBuf_Puts(&msg, "`");
    if (ev->args_json && ev->args_json[0] && strcmp(ev->args_json, "{}") != 0)
    {
        JsonBuf_Puts(&msg, "\n");
        JsonBuf_Puts(&msg, ev->args_json);
    }
    JsonBuf_Puts(&msg, "?");
    char *message = JsonBuf_Steal(&msg);

    JsonBuf req;
    JsonBuf_Init(&req);
    JsonBuf_Puts(&req, "{\"type\":\"confirm\",\"message\":");
    JsonBuf_String(&req, message ? message : "Allow this tool?");
    JsonBuf_Putc(&req, '}');
    free(message);
    char *request = JsonBuf_Steal(&req);

    char *answer = NULL;
    int rc = pico_tool_ask(ctx, request, &answer);
    free(request);
    if (rc != PICO_ASK_OK)
    {
        free(answer);
        return;
    }

    JsonDoc doc;
    const char *src = answer ? answer : "";
    bool allow = false;
    if (JsonParse(&doc, src, strlen(src)) == 0)
    {
        allow = JsonEq(&doc, JsonObjGet(&doc, 0, "ok"), "true");
        JsonFree(&doc);
    }
    free(answer);
    if (!allow)
    {
        ev->deny = true;
        ev->result = JsonDup("User denied this tool.");
    }
}

static int PermitInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_add_tool_before_hook(workspace, PermitBefore);
    return 0;
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "permit",
        .description = "Confirm each tool call before it runs",
        .workspace_init = PermitInit,
    };
}
