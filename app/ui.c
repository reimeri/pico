#include "pico/plugin.h"
#include "host_internal.h"

#include <string.h>

static bool CopyModalName(char *dst, size_t cap, const char *name)
{
    size_t n;
    if (!dst || cap == 0 || !name || !name[0])
    {
        return false;
    }
    n = strlen(name);
    if (n >= cap)
    {
        return false;
    }
    memcpy(dst, name, n + 1);
    return true;
}

bool pico_ui_modal_push(PicoHost *app, const char *name)
{
    int i;
    if (!app || app->ui_modal_count >= PICO_MAX_UI_MODALS)
    {
        return false;
    }
    for (i = 0; name && name[0] && i < app->ui_modal_count; i++)
    {
        if (strcmp(app->ui_modals[i], name) == 0)
        {
            return false;
        }
    }
    if (!CopyModalName(app->ui_modals[app->ui_modal_count], PICO_UI_MODAL_NAME, name))
    {
        return false;
    }
    app->ui_modal_count++;
    return true;
}

bool pico_ui_modal_pop(PicoHost *app, const char *name)
{
    if (!app || app->ui_modal_count <= 0 || !name || !name[0])
    {
        return false;
    }
    if (strcmp(app->ui_modals[app->ui_modal_count - 1], name) != 0)
    {
        return false;
    }
    app->ui_modal_count--;
    app->ui_modals[app->ui_modal_count][0] = '\0';
    return true;
}

const char *pico_ui_modal_top(const PicoHost *app)
{
    if (!app || app->ui_modal_count <= 0)
    {
        return NULL;
    }
    return app->ui_modals[app->ui_modal_count - 1];
}

int pico_ui_modal_count(const PicoHost *app)
{
    return app ? app->ui_modal_count : 0;
}

bool pico_ui_modal_claimed(const PicoHost *app)
{
    return pico_ui_modal_count(app) > 0;
}

bool pico_ui_modal_has(const PicoHost *app, const char *name)
{
    int i;
    if (!app || !name || !name[0])
    {
        return false;
    }
    for (i = 0; i < app->ui_modal_count; i++)
    {
        if (strcmp(app->ui_modals[i], name) == 0)
        {
            return true;
        }
    }
    return false;
}

bool pico_ui_modal_is_top(const PicoHost *app, const char *name)
{
    const char *top = pico_ui_modal_top(app);
    return top && name && name[0] && strcmp(top, name) == 0;
}

void pico_ui_modal_reset(PicoHost *app)
{
    if (!app)
    {
        return;
    }
    memset(app->ui_modals, 0, sizeof(app->ui_modals));
    app->ui_modal_count = 0;
}

void pico_add_tool_row_hook(PicoWorkspace *workspace, PicoToolRowFn fn)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    if (!host || !fn || host->tool_row_hook_count >= PICO_MAX_TOOL_ROW_HOOKS)
    {
        return;
    }
    host->tool_row_hooks[host->tool_row_hook_count].fn = fn;
    host->tool_row_hook_count++;
}

bool pico_tool_row_activate(PicoHost *host, PicoAgentId agent_id, const PicoTraceLine *line)
{
    PicoToolRowEvent ev;
    int i;
    PicoWorkspace *workspace;

    if (!host || !line || !line->is_tool)
    {
        return false;
    }
    memset(&ev, 0, sizeof(ev));
    ev.agent_id = agent_id;
    ev.name = line->tool_name;
    ev.call_id = line->tool_call_id;
    ev.args_json = line->tool_args_json;
    ev.output = line->tool_output;
    ev.child_id = line->child_id;
    ev.child_session_id = line->child_session_id[0] ? line->child_session_id : NULL;
    ev.is_error = line->tool_error;
    workspace = PicoHost_PrimaryWorkspace(host);
    for (i = 0; i < host->tool_row_hook_count; i++)
    {
        if (!host->tool_row_hooks[i].fn)
        {
            continue;
        }
        host->tool_row_hooks[i].fn(workspace, &ev, host->tool_row_hooks[i].state);
        if (ev.handled)
        {
            return true;
        }
    }
    return false;
}
