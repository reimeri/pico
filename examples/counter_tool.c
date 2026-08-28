// Example Pico extension demonstrating a stateful workspace-scoped tool.
// Copy to ~/.config/pico/extensions/ or <workspace>/.pico/extensions/ then press F5.
//
//   mkdir -p ~/.config/pico/extensions/counter_tool
//   cp examples/counter_tool.c ~/.config/pico/extensions/counter_tool/

#include "pico/plugin.h"
#include "json.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CounterState {
    pthread_mutex_t mu;
    int count;
} CounterState;

static const char *kParams = "{\"type\":\"object\",\"properties\":{}}";

static void CounterToolRun(PicoAgentContext *ctx, const char *args_json, PicoToolResult *out, void *state)
{
    (void)ctx;
    (void)args_json;
    CounterState *s = (CounterState *)state;
    int val = 0;
    if (s)
    {
        pthread_mutex_lock(&s->mu);
        s->count++;
        val = s->count;
        pthread_mutex_unlock(&s->mu);
    }
    if (out)
    {
        memset(out, 0, sizeof(*out));
        char buf[64];
        snprintf(buf, sizeof(buf), "counter: %d", val);
        out->output = JsonDup(buf);
    }
}

static int CounterWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    CounterState *s = (CounterState *)calloc(1, sizeof(CounterState));
    if (!s)
    {
        return 1;
    }
    pthread_mutex_init(&s->mu, NULL);
    if (state_out)
    {
        *state_out = s;
    }
    pico_add_tool(workspace, "counter_increment", "Increment the per-workspace counter and return its new value",
                  kParams, CounterToolRun, NULL);
    return 0;
}

static void CounterWorkspaceShutdown(PicoWorkspace *workspace, void *state)
{
    (void)workspace;
    CounterState *s = (CounterState *)state;
    if (s)
    {
        pthread_mutex_destroy(&s->mu);
        free(s);
    }
}

PicoExt pico_ext(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "counter_tool",
        .description = "Per-workspace counter tool example",
        .workspace_init = CounterWorkspaceInit,
        .workspace_shutdown = CounterWorkspaceShutdown,
    };
}
