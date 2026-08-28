#include "pico/plugin.h"
#include "host_internal.h"

#include <stdio.h>
#include <string.h>

static int g_failed;
static int g_hook_a;
static int g_hook_b;

static void Fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    g_failed = 1;
}

static void Check(bool ok, const char *msg)
{
    if (!ok)
    {
        Fail(msg);
    }
}

static void HookA(PicoWorkspace *workspace, PicoToolRowEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    g_hook_a++;
    if (event && event->name && strcmp(event->name, "plain") == 0)
    {
        Check(event->args_json && strcmp(event->args_json, "{\"query\":\"x\"}") == 0,
              "hook receives raw args JSON");
        Check(event->child_id == 42, "hook receives child id");
        Check(event->child_session_id && strcmp(event->child_session_id, "session-1") == 0,
              "hook receives child session id");
    }
    if (event && event->name && strcmp(event->name, "handled") == 0)
    {
        event->handled = true;
    }
}

static void HookB(PicoWorkspace *workspace, PicoToolRowEvent *event, void *state)
{
    (void)workspace;
    (void)state;
    (void)event;
    g_hook_b++;
}

static int TestModalStack(void)
{
    PicoHost app;
    char long_name[PICO_UI_MODAL_NAME + 8];
    int i;

    memset(&app, 0, sizeof(app));
    Check(!pico_ui_modal_push(NULL, "a"), "push null app");
    Check(!pico_ui_modal_push(&app, NULL), "push null name");
    Check(!pico_ui_modal_push(&app, ""), "push empty name");
    Check(!pico_ui_modal_pop(&app, "a"), "pop empty stack");
    Check(pico_ui_modal_count(&app) == 0, "empty count");
    Check(!pico_ui_modal_claimed(&app), "empty claimed");
    Check(pico_ui_modal_top(&app) == NULL, "empty top");

    Check(pico_ui_modal_push(&app, "a"), "push a");
    Check(pico_ui_modal_push(&app, "b"), "push b");
    Check(pico_ui_modal_count(&app) == 2, "count 2");
    Check(pico_ui_modal_top(&app) && strcmp(pico_ui_modal_top(&app), "b") == 0, "top b");
    Check(pico_ui_modal_is_top(&app, "b"), "is top b");
    Check(!pico_ui_modal_is_top(&app, "a"), "a is not top");
    Check(pico_ui_modal_has(&app, "a") && pico_ui_modal_has(&app, "b"), "has a and b");
    Check(!pico_ui_modal_pop(&app, "a"), "pop non-top fails");
    Check(pico_ui_modal_top(&app) && strcmp(pico_ui_modal_top(&app), "b") == 0, "top still b");
    Check(pico_ui_modal_pop(&app, "b"), "pop top b");
    Check(pico_ui_modal_top(&app) && strcmp(pico_ui_modal_top(&app), "a") == 0, "top a");
    Check(pico_ui_modal_pop(&app, "a"), "pop a");
    Check(pico_ui_modal_count(&app) == 0, "stack empty again");

    Check(pico_ui_modal_push(&app, "same"), "push same");
    Check(!pico_ui_modal_push(&app, "same"), "duplicate name rejected");
    Check(pico_ui_modal_count(&app) == 1, "duplicate did not grow stack");
    Check(pico_ui_modal_pop(&app, "same"), "pop unique name");

    Check(pico_ui_modal_push(&app, "reload-a"), "push reload a");
    Check(pico_ui_modal_push(&app, "reload-b"), "push reload b");
    pico_ui_modal_reset(&app);
    Check(pico_ui_modal_count(&app) == 0 && !pico_ui_modal_claimed(&app),
          "lifecycle reset clears every claim");

    memset(&app, 0, sizeof(app));
    for (i = 0; i < PICO_MAX_UI_MODALS; i++)
    {
        char name[8];
        snprintf(name, sizeof(name), "m%d", i);
        if (!pico_ui_modal_push(&app, name))
        {
            Fail("stack fill");
            break;
        }
    }
    Check(!pico_ui_modal_push(&app, "overflow"), "full stack rejects");
    Check(pico_ui_modal_count(&app) == PICO_MAX_UI_MODALS, "full count");

    memset(&app, 0, sizeof(app));
    memset(long_name, 'x', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = '\0';
    Check(!pico_ui_modal_push(&app, long_name), "oversized name rejected");
    return g_failed;
}

static int TestToolRowHooks(void)
{
    PicoHost app;
    PicoWorkspace workspace;
    PicoTraceLine line;

    memset(&app, 0, sizeof(app));
    memset(&workspace, 0, sizeof(workspace));
    workspace.host = &app;
    workspace.id = 1;
    workspace.state = PICO_WORKSPACE_OPEN;
    app.workspaces[0] = &workspace;
    app.workspace_count = 1;
    memset(&line, 0, sizeof(line));
    g_hook_a = 0;
    g_hook_b = 0;
    Check(!pico_tool_row_activate(&app, 1, NULL), "null line");
    line.is_tool = false;
    line.tool_name = "handled";
    Check(!pico_tool_row_activate(&app, 1, &line), "non-tool line");

    line.is_tool = true;
    line.tool_name = "plain";
    line.tool_call_id = "c1";
    line.tool_args = "query: x";
    line.tool_args_json = "{\"query\":\"x\"}";
    line.child_id = 42;
    snprintf(line.child_session_id, sizeof(line.child_session_id), "session-1");
    pico_add_tool_row_hook(PicoHost_PrimaryWorkspace(&app), HookA);
    pico_add_tool_row_hook(PicoHost_PrimaryWorkspace(&app), HookB);
    Check(!pico_tool_row_activate(&app, 7, &line), "unhandled returns false");
    Check(g_hook_a == 1 && g_hook_b == 1, "both hooks run when unhandled");

    line.tool_name = "handled";
    g_hook_a = 0;
    g_hook_b = 0;
    Check(pico_tool_row_activate(&app, 7, &line), "handled returns true");
    Check(g_hook_a == 1 && g_hook_b == 0, "later hook skipped");
    return g_failed;
}

int main(void)
{
    TestModalStack();
    TestToolRowHooks();
    return g_failed ? 1 : 0;
}
