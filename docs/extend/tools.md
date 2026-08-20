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
- No cancellation callback. A blocking tool holds the worker (and defers extension reload) until it returns.
- Max 64 tools (`PICO_MAX_TOOLS`). Names should be unique; the provider exposes all registered tools.
