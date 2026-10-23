#include "browser.h"
#include "host_internal.h"
#include "agent_internal.h"
#include "clarification.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
static void Open(PicoAgentContext *ctx, const char *args, PicoToolResult *out, void *state)
{
    (void)state;
    JsonDoc doc;
    if (JsonParse(&doc, args, strlen(args)) != 0) {
        out->is_error = true; out->output = JsonDup("browser_open: invalid arguments."); return;
    }
    char *url = JsonObjStr(&doc, 0, "url"); JsonFree(&doc);
    out->output = PicoBrowser_ToolOpen(ctx, url, &out->is_error); free(url);
}
static void Guidance(PicoWorkspace *ws, PicoAgentId id, PicoLlmEvent *event, void *state)
{
    (void)state;
    PicoAgent *a = PicoHost_FindAgent(ws->host, id);
    bool offered = false;
    for (int i = 0; event && event->include_tools && i < event->tool_count; ++i) {
        if (!strcmp(event->tools[i].name, "browser_open")) {
            if (!PicoAgent_IsUserMain(a)) { if (event->exclude) event->exclude[i] = true; }
            else if (!event->exclude || !event->exclude[i]) offered = true;
        }
    }
    if (offered) event->extra_instructions = JsonDup("Browser access is main-session-only; do not delegate browser tasks to subagents.");
}
static int Init(PicoWorkspace *ws, void **state)
{
    (void)state;
    PicoBrowser_Init(ws->host);
    pico_add_tool(ws, "browser_open", "Open a URL in this main session's embedded browser. Navigation only; no screenshot or agent input tools yet.",
        "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"HTTP(S) URL, including http://localhost:PORT\"}},\"required\":[\"url\"]}",
        Open, NULL, PICO_TOOL_SEQUENTIAL);
    pico_add_llm_hook(ws, Guidance);
    return 0;
}
PicoExt pico_ext_browser(void)
{
    return (PicoExt){.abi=PICO_EXT_ABI, .name="browser", .description="Per-main-session embedded Chromium browser", .workspace_init=Init};
}
