// Example workspace-scoped Pico tool. Copy to ~/.config/pico/extensions/ or
// <workspace>/.pico/extensions/ (a subfolder is fine) then press F5.
//
//   mkdir -p ~/.config/pico/extensions/echo
//   cp examples/echo_tool.c ~/.config/pico/extensions/echo/

#include "pico/plugin.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

static const char *kParams =
    "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Text to "
    "echo back\"}},\"required\":[\"text\"]}";

static void EchoRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)state;
    (void)ctx;
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
            out->output = JsonDup("echo: bad json");
            out->is_error = true;
        }
        return;
    }
    char *text = JsonObjStr(&doc, 0, "text");
    JsonFree(&doc);
    if (out)
    {
        out->output = text ? text : JsonDup("echo: missing text");
        out->is_error = text == NULL;
    }
    else
    {
        free(text);
    }
}

static int EchoInit(PicoWorkspace *workspace, void **state_out)
{
    (void)state_out;
    pico_add_tool(workspace, "echo", "Echo text back", kParams, EchoRun, NULL);
    return 0;
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "echo",
        .description = "Echo tool example",
        .workspace_init = EchoInit,
    };
}
