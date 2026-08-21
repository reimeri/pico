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

Full file: `examples/echo_tool.c`. Builtin reference: `app/builtins/shell.c` (`sh`). Asking the user: `examples/ask_tool.c` (`pico_tool_ask` + builtin confirm overlay).

## Asking the user

Call `pico_tool_ask` from inside `PicoToolFn` only. Pico validates and copies `request_json`, blocks the worker, and shows UI on the main thread. The tool continues after `pico_tool_answer` or Esc. Malformed JSON and builtin confirmations without a non-empty `message` return immediately with `PICO_ASK_OK` and `{"error":"invalid ask payload; fix it and try again"}` so the tool can pass that result back to the agent.

```c
char *answer = NULL;
int rc = pico_tool_ask(app, "{\"type\":\"confirm\",\"message\":\"Proceed?\"}", &answer);
if (rc != PICO_ASK_OK)
{
    *out = JsonDup("cancelled");
    free(answer); /* always NULL on CANCEL/FAIL */
    return;
}
*out = answer; /* malloc'd; Pico frees *out */
```

- `PICO_ASK_OK` — `*answer_json` is malloc'd; the tool frees it (or hands it to `*out`). This includes the immediate error answer for an invalid payload.
- `PICO_ASK_CANCEL` / `PICO_ASK_FAIL` — `*answer_json` is always `NULL`. Return promptly; do not ask again after cancel.
- Request/answer copies are capped at `PICO_TOOL_ASK_MAX_REQUEST` / `PICO_TOOL_ASK_MAX_ANSWER` (64 KiB). Oversized values fail / are rejected.
- Nested asks (ask while already waiting) fail. Sequential asks in one tool are allowed; each gets a new `id`.

Main thread:

```c
PicoToolAsk ask;
if (pico_tool_pending_ask(app, &ask))
{
    /* ask.request_json is valid until the next PicoAgent_Pump. Do not store it. */
    pico_tool_answer(app, ask.id, "{\"ok\":true}"); /* false if id is stale */
}
```

`pico_tool_answer` returns false if the id is stale, cancelled, or already answered. Ask ids are not reused during the process lifetime. Overlay **Deny/Approve** must answer; **Esc** cancels the turn (`PICO_ASK_CANCEL`), not the same as Deny.

Builtin overlay handles `{"type":"confirm","message":"…"}` (Approve/Deny → `{"ok":true}` / `{"ok":false}`) and scrolls long messages without truncating them. Set `"ui":"custom"` or use another `type` to render your own overlay. Custom UIs may read characters from `on_frame` while a pending ask is open (`PicoUi_ModalOpen` skips the composer). Invalid JSON and invalid builtin confirmations return the immediate error answer instead of opening UI.

Pico does not model questionnaire steps. Preferred: one `pico_tool_ask` with the full schema; the overlay keeps Next/Back and answers once. Sequential `pico_tool_ask` calls work too — drop widgets bound to a previous `id`.

## Contract

- `name`, `description`, `params_json` must outlive the extension — use string literals.
- `params_json` is a JSON Schema object (OpenAI function parameters).
- `*out` must be malloc'd. Pico frees it. Always set a string, even on error (`JsonDup("…")`).
- Parse arguments with `#include "json.h"` (`JsonParse`, `JsonObjStr`, …).
- Runs on the **worker thread**. Do not call Clay, add views, or mutate chat UI. Returning output is enough; Pico shows it in the trace.
- No cancellation callback on the tool itself. Esc asks the in-flight LLM request to abort, and wakes `pico_tool_ask` with `PICO_ASK_CANCEL`. A tool that does not ask still runs until it returns.
- A second Esc while that cancel is still outstanding **force-cancels**: the UI goes idle immediately and the worker is abandoned. The tool function may keep running in the background until it returns. Reload/F5 still wait until that abandoned worker finishes so your code is not `dlclose`d underneath it. Do not use your own condition variable to wait for UI; Pico cannot wake it.
- If the tool forks a child, call `pico_tool_set_child(app, pid)` after spawn (and `pico_tool_set_child(app, 0)` when it exits) so force-cancel can kill the process group. Put the child in its own group (`setpgid`) first. Builtin `sh` does this.
- Max 64 tools (`PICO_MAX_TOOLS`). Names should be unique; the provider exposes all registered tools.
