# Tools

Tools are functions the model can call. Register in `init`:

```c
#include "pico/plugin.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

static const char *kParams =
    "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}";

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
```

Full file: `examples/echo_tool.c`. Builtin reference: `app/builtins/shell.c` (`sh`).

## Contract

- `name`, `description`, `params_json` must outlive the extension — use string literals.
- `params_json` is a JSON Schema object (OpenAI function parameters).
- `*out` must be malloc'd. Pico frees it. Always set a string, even on error (`JsonDup("…")`).
- Parse arguments with `#include "json.h"` (`JsonParse`, `JsonObjStr`, …).
- Runs on the **worker thread**. Do not call Clay, add views, or mutate chat UI. Returning output is enough; Pico shows it in the trace.
- No cancellation callback. Esc asks the in-flight LLM request to abort; a blocking tool still runs until it returns.
- A second Esc while that cancel is still outstanding **force-cancels**: the UI goes idle immediately and the worker is abandoned. The tool function may keep running in the background until it returns. Reload/F5 still wait until that abandoned worker finishes so your code is not `dlclose`d underneath it.
- If the tool forks a child, call `pico_tool_set_child(app, pid)` after spawn (and `pico_tool_set_child(app, 0)` when it exits) so force-cancel can kill the process group. Put the child in its own group (`setpgid`) first. Builtin `sh` does this.
- Max 64 tools (`PICO_MAX_TOOLS`). Names should be unique; the provider exposes all registered tools.
