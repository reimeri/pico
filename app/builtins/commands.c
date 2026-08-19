#include "pico/plugin.h"
#include "agent.h"
#include "session.h"
#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "raylib.h"

static void Note(PicoApp *app, const char *text)
{
    PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, text);
}

static void ClearComposer(PicoApp *app)
{
    PicoComposer_SetText(app, "");
}

static int Fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static bool FoldEq(const char *a, const char *b)
{
    if (!a || !b)
    {
        return false;
    }
    while (*a && *b)
    {
        if (Fold((unsigned char)*a) != Fold((unsigned char)*b))
        {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static bool FoldPrefix(const char *s, const char *prefix)
{
    if (!prefix || !prefix[0])
    {
        return true;
    }
    while (*prefix)
    {
        if (Fold((unsigned char)*s) != Fold((unsigned char)*prefix))
        {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

static bool FoldContains(const char *s, const char *needle)
{
    if (!needle || !needle[0])
    {
        return true;
    }
    for (; *s; s++)
    {
        if (FoldPrefix(s, needle))
        {
            return true;
        }
    }
    return false;
}

static PicoModel *FindCatalog(PicoApp *app, const char *q)
{
    if (!q || !q[0])
    {
        return NULL;
    }
    for (int i = 0; i < app->model_count; i++)
    {
        if (FoldEq(app->models[i].id, q) || FoldEq(app->models[i].name, q))
        {
            return &app->models[i];
        }
    }
    return NULL;
}

static void LogSelection(PicoApp *app)
{
    PicoSession_LogModelChange(app, app->settings.model, PicoSettings_ActiveEffort(app));
}

static void CmdModel(PicoApp *app, const char *args)
{
    while (args && *args && isspace((unsigned char)*args))
    {
        args++;
    }
    if (!args || !args[0])
    {
        PicoComposer_SetText(app, "/model ");
        app->submit_cancel = true;
        PicoComplete_Refresh(app);
        return;
    }
    PicoModel *m = FindCatalog(app, args);
    if (!m)
    {
        char line[256];
        snprintf(line, sizeof(line), "Unknown model `%s`. Try `/model` for the catalog.", args);
        Note(app, line);
        ClearComposer(app);
        app->submit_cancel = true;
        return;
    }
    snprintf(app->settings.model, sizeof(app->settings.model), "%s", m->id);
    PicoSettings_SyncActive(app);
    PicoSettings_SaveSelection(app, true, false);
    LogSelection(app);
    char line[256];
    snprintf(line, sizeof(line), "Model `%s` · effort `%s`", m->name[0] ? m->name : m->id,
             PicoSettings_ActiveEffort(app));
    Note(app, line);
    ClearComposer(app);
    app->submit_cancel = true;
}

static void CmdEffort(PicoApp *app, const char *args)
{
    while (args && *args && isspace((unsigned char)*args))
    {
        args++;
    }
    PicoModel *m = PicoSettings_ActiveModel(app);
    if (!args || !args[0])
    {
        PicoComposer_SetText(app, "/effort ");
        app->submit_cancel = true;
        PicoComplete_Refresh(app);
        return;
    }
    if (!m)
    {
        Note(app, "No model in the catalog. Add one in settings.json.");
        ClearComposer(app);
        app->submit_cancel = true;
        return;
    }
    const char *level = args;
    if (m->effort_count > 0 && !PicoSettings_EffortAllowed(m, level))
    {
        char line[256];
        snprintf(line, sizeof(line), "`%s` is not in this model's effort list.", level);
        Note(app, line);
        ClearComposer(app);
        app->submit_cancel = true;
        return;
    }
    snprintf(m->selected_effort, sizeof(m->selected_effort), "%s", level);
    PicoSettings_SaveSelection(app, false, true);
    LogSelection(app);
    char line[256];
    snprintf(line, sizeof(line), "Effort `%s` for `%s`", m->selected_effort, m->name[0] ? m->name : m->id);
    Note(app, line);
    ClearComposer(app);
    app->submit_cancel = true;
}

static void CmdCompact(PicoApp *app, const char *args)
{
    (void)args;
    PicoAgent_Compact(app);
    ClearComposer(app);
    app->submit_cancel = true;
}

static void CmdQuit(PicoApp *app, const char *args)
{
    (void)args;
    ClearComposer(app);
    app->submit_cancel = true;
    CloseWindow();
}

static void CmdHelp(PicoApp *app, const char *args)
{
    (void)args;
    char buf[1024];
    size_t n = 0;
    n += (size_t)snprintf(buf + n, sizeof(buf) - n, "Commands:\n");
    for (int i = 0; i < app->command_count && n + 8 < sizeof(buf); i++)
    {
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "`/%s` — %s\n", app->commands[i].name,
                              app->commands[i].help ? app->commands[i].help : "");
    }
    Note(app, buf);
    ClearComposer(app);
    app->submit_cancel = true;
}

static void CmdReload(PicoApp *app, const char *args)
{
    (void)args;
    PicoApp_RequestReload(app);
    Note(app, "Reloading extensions…");
    ClearComposer(app);
    app->submit_cancel = true;
}

static const PicoCommand *FindCommand(PicoApp *app, const char *name)
{
    for (int i = 0; i < app->command_count; i++)
    {
        if (FoldEq(app->commands[i].name, name))
        {
            return &app->commands[i];
        }
    }
    return NULL;
}

static void SplitPrefix(const char *prefix, char *cmd, size_t cmd_cap, const char **rest)
{
    cmd[0] = '\0';
    *rest = "";
    if (!prefix)
    {
        return;
    }
    while (*prefix && isspace((unsigned char)*prefix))
    {
        prefix++;
    }
    size_t n = 0;
    while (prefix[n] && !isspace((unsigned char)prefix[n]) && n + 1 < cmd_cap)
    {
        cmd[n] = prefix[n];
        n++;
    }
    cmd[n] = '\0';
    const char *r = prefix + n;
    while (*r && isspace((unsigned char)*r))
    {
        r++;
    }
    *rest = r;
}

static bool NeedsArgs(const char *name)
{
    return FoldEq(name, "model") || FoldEq(name, "effort");
}

static int CommandQuery(PicoApp *app, const char *prefix, PicoCompleteItem *out, int max)
{
    char cmd[64];
    const char *rest = "";
    SplitPrefix(prefix, cmd, sizeof(cmd), &rest);
    bool has_space = false;
    if (prefix)
    {
        for (const char *p = prefix; *p; p++)
        {
            if (isspace((unsigned char)*p))
            {
                has_space = true;
                break;
            }
        }
    }
    int n = 0;
    if (!has_space)
    {
        for (int i = 0; i < app->command_count && n < max; i++)
        {
            if (!FoldPrefix(app->commands[i].name, cmd) && !FoldContains(app->commands[i].name, cmd))
            {
                continue;
            }
            snprintf(out[n].label, sizeof(out[n].label), "/%s", app->commands[i].name);
            snprintf(out[n].detail, sizeof(out[n].detail), "%s",
                     app->commands[i].help ? app->commands[i].help : "");
            if (NeedsArgs(app->commands[i].name))
            {
                snprintf(out[n].insert, sizeof(out[n].insert), "/%s ", app->commands[i].name);
            }
            else
            {
                snprintf(out[n].insert, sizeof(out[n].insert), "/%s", app->commands[i].name);
            }
            n++;
        }
        return n;
    }
    if (FoldEq(cmd, "model"))
    {
        for (int i = 0; i < app->model_count && n < max; i++)
        {
            PicoModel *m = &app->models[i];
            if (!FoldPrefix(m->id, rest) && !FoldContains(m->id, rest) && !FoldContains(m->name, rest))
            {
                continue;
            }
            snprintf(out[n].label, sizeof(out[n].label), "%s", m->id);
            snprintf(out[n].detail, sizeof(out[n].detail), "%s", m->name);
            snprintf(out[n].insert, sizeof(out[n].insert), "/model %s", m->id);
            n++;
        }
        return n;
    }
    if (FoldEq(cmd, "effort"))
    {
        PicoModel *m = PicoSettings_ActiveModel(app);
        if (!m)
        {
            return 0;
        }
        for (int i = 0; i < m->effort_count && n < max; i++)
        {
            if (!FoldPrefix(m->effort[i], rest) && !FoldContains(m->effort[i], rest))
            {
                continue;
            }
            snprintf(out[n].label, sizeof(out[n].label), "%s", m->effort[i]);
            out[n].detail[0] = '\0';
            snprintf(out[n].insert, sizeof(out[n].insert), "/effort %s", m->effort[i]);
            n++;
        }
        if (n == 0 && m->effort_count == 0 && FoldPrefix("none", rest))
        {
            snprintf(out[n].label, sizeof(out[n].label), "none");
            snprintf(out[n].insert, sizeof(out[n].insert), "/effort none");
            n++;
        }
        return n;
    }
    return 0;
}

static void CommandsBeforeSubmit(PicoApp *app)
{
    if (app->submit_cancel || !app->composer.text)
    {
        return;
    }
    const char *s = app->composer.text;
    if (s[0] != '/')
    {
        return;
    }
    char cmd[64];
    const char *rest = "";
    SplitPrefix(s + 1, cmd, sizeof(cmd), &rest);
    const PicoCommand *found = FindCommand(app, cmd);
    if (!found)
    {
        return;
    }
    found->run(app, rest);
}

static void CommandsInit(PicoApp *app)
{
    pico_add_command(app, "model", "Switch model", CmdModel);
    pico_add_command(app, "effort", "Set reasoning effort for this model", CmdEffort);
    pico_add_command(app, "compact", "Compact the current session", CmdCompact);
    pico_add_command(app, "quit", "Quit Pico", CmdQuit);
    pico_add_command(app, "help", "List commands", CmdHelp);
    pico_add_command(app, "reload", "Reload extensions", CmdReload);
    pico_add_completer(app, '/', true, CommandQuery, NULL);
    pico_add_hook(app, PICO_HOOK_BEFORE_SUBMIT, CommandsBeforeSubmit);
}

PicoExt pico_ext_commands(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "commands",
        .init = CommandsInit,
    };
}
