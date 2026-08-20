// Example Pico tool extension. Copy to ~/.config/pico/extensions/ or
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

static void EchoRun(PicoApp *app, const char *args_json, char **out)
{
    (void)app;
    if (out)
    {
        *out = NULL;
    }
    JsonDoc doc;
    const char *src = args_json ? args_json : "";
    if (JsonParse(&doc, src, strlen(src)) != 0)
    {
        if (out)
        {
            *out = JsonDup("echo: bad json");
        }
        return;
    }
    char *text = JsonObjStr(&doc, 0, "text");
    JsonFree(&doc);
    if (out)
    {
        *out = text ? text : JsonDup("echo: missing text");
    }
    else
    {
        free(text);
    }
}

static void EchoInit(PicoApp *app)
{
    pico_add_tool(app, "echo", "Echo text back", kParams, EchoRun);
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "echo",
        .init = EchoInit,
    };
}
