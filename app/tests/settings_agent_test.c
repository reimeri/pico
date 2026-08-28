#define _POSIX_C_SOURCE 200809L

#include "docs_path.h"
#include "settings.h"
#include "host_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int Fail(const char *message)
{
    fprintf(stderr, "settings agent: %s\n", message);
    return 1;
}

static int TestModelContextBeatsFallback(void)
{
    PicoHost app;
    PicoAgent agent;
    PicoModel model;
    memset(&app, 0, sizeof(app));
    memset(&agent, 0, sizeof(agent));
    memset(&model, 0, sizeof(model));

    snprintf(model.id, sizeof(model.id), "grok-4.6");
    snprintf(model.name, sizeof(model.name), "Grok 4.6");
    model.context_limit = 500000;
    app.models = &model;
    app.model_count = 1;
    snprintf(app.settings.model, sizeof(app.settings.model), "grok-4.6");
    app.settings.context_limit = 1000000;

    PicoSettings_InitAgent(&app, &agent);
    if (agent.context_limit != 500000)
    {
        return Fail("selected model context_limit lost to root/env fallback");
    }
    return 0;
}

static int TestFallbackWhenModelHasNoLimit(void)
{
    PicoHost app;
    PicoAgent agent;
    PicoModel model;
    memset(&app, 0, sizeof(app));
    memset(&agent, 0, sizeof(agent));
    memset(&model, 0, sizeof(model));

    snprintf(model.id, sizeof(model.id), "custom");
    snprintf(model.name, sizeof(model.name), "Custom");
    app.models = &model;
    app.model_count = 1;
    snprintf(app.settings.model, sizeof(app.settings.model), "custom");
    app.settings.context_limit = 128000;

    PicoSettings_InitAgent(&app, &agent);
    if (agent.context_limit != 128000)
    {
        return Fail("missing model context_limit did not use root/env fallback");
    }
    return 0;
}

static int TestPerAgentSelection(void)
{
    PicoHost app;
    PicoAgent first;
    PicoAgent second;
    PicoModel models[2];
    memset(&app, 0, sizeof(app));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    memset(models, 0, sizeof(models));

    snprintf(models[0].id, sizeof(models[0].id), "model-a");
    snprintf(models[0].name, sizeof(models[0].name), "Model A");
    models[0].context_limit = 100;
    snprintf(models[0].effort[0], sizeof(models[0].effort[0]), "high");
    models[0].effort_count = 1;
    snprintf(models[0].default_effort, sizeof(models[0].default_effort), "high");

    snprintf(models[1].id, sizeof(models[1].id), "model-b");
    snprintf(models[1].name, sizeof(models[1].name), "Model B");
    models[1].context_limit = 200;
    snprintf(models[1].effort[0], sizeof(models[1].effort[0]), "low");
    models[1].effort_count = 1;
    snprintf(models[1].default_effort, sizeof(models[1].default_effort), "low");

    app.models = models;
    app.model_count = 2;
    snprintf(app.settings.model, sizeof(app.settings.model), "model-a");
    app.settings.compact_enabled = true;
    app.settings.compact_ratio = 0.75;

    PicoSettings_InitAgent(&app, &first);
    PicoSettings_InitAgent(&app, &second);
    snprintf(first.model, sizeof(first.model), "model-b");
    first.effort[0] = '\0';
    PicoSettings_SyncAgent(&app, &first);

    if (strcmp(first.model, "model-b") != 0 || strcmp(first.effort, "low") != 0 ||
        first.context_limit != 200)
    {
        return Fail("target model selection did not resolve its own effort/context");
    }
    if (strcmp(second.model, "model-a") != 0 || strcmp(second.effort, "high") != 0 ||
        second.context_limit != 100 || strcmp(models[0].default_effort, "high") != 0 ||
        strcmp(models[1].default_effort, "low") != 0)
    {
        return Fail("one agent mutated another agent or the immutable catalog");
    }
    return 0;
}

static int TestDisabledExtensionsFromUserSettings(void)
{
    char temp[] = "/tmp/pico-disabled-ext-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create isolated settings directory");
    }

    char dir[sizeof(temp) + 8];
    snprintf(dir, sizeof(dir), "%s/pico", temp);
    Pico_MkdirP(dir);
    char path[sizeof(temp) + 32];
    snprintf(path, sizeof(path), "%s/pico/settings.json", temp);
    FILE *f = fopen(path, "w");
    if (!f)
    {
        rmdir(dir);
        rmdir(temp);
        return Fail("could not write isolated settings.json");
    }
    fputs("{\n  \"disabled_extensions\": [\"composer\"]\n}\n", f);
    fclose(f);

    PicoHost app;
    memset(&app, 0, sizeof(app));
    PicoHost_SetPath(&app, temp);
    setenv("XDG_CONFIG_HOME", temp, 1);
    PicoSettings_Load(&app);
    int failed = app.settings.disabled_extension_count != 1 ||
                 strcmp(app.settings.disabled_extensions[0], "composer") != 0;

    free(app.models);
    unsetenv("XDG_CONFIG_HOME");
    unlink(path);
    rmdir(dir);
    rmdir(temp);
    return failed ? Fail("user disabled_extensions did not populate the disabled set") : 0;
}

static int WriteFile(const char *path, const char *contents)
{
    FILE *f = fopen(path, "w");
    if (!f)
    {
        return 1;
    }
    fputs(contents, f);
    fclose(f);
    return 0;
}

static int ExpectSpan(const PicoPromptSpan *span, PicoPromptSource source, const char *text,
                      const char *want)
{
    if (!span || span->source != source || !text)
    {
        return 1;
    }
    size_t n = strlen(want);
    return span->length == n && strncmp(text + span->start, want, n) == 0 ? 0 : 1;
}

static int TestPromptSourceSpans(void)
{
    char temp[] = "/tmp/pico-prompt-spans-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create isolated prompt directory");
    }

    char config[sizeof(temp) + 8];
    snprintf(config, sizeof(config), "%s/pico", temp);
    Pico_MkdirP(config);
    char workspace[sizeof(temp) + 16];
    snprintf(workspace, sizeof(workspace), "%s/work", temp);
    Pico_MkdirP(workspace);
    char pico_dir[sizeof(workspace) + 8];
    snprintf(pico_dir, sizeof(pico_dir), "%s/.pico", workspace);
    Pico_MkdirP(pico_dir);

    char user_system[sizeof(config) + 16];
    char workspace_system[sizeof(pico_dir) + 16];
    char agents[sizeof(workspace) + 16];
    snprintf(user_system, sizeof(user_system), "%s/SYSTEM.md", config);
    snprintf(workspace_system, sizeof(workspace_system), "%s/SYSTEM.md", pico_dir);
    snprintf(agents, sizeof(agents), "%s/AGENTS.md", workspace);
    if (WriteFile(user_system, "user-system") || WriteFile(workspace_system, "workspace-system") ||
        WriteFile(agents, "agents-md"))
    {
        return Fail("could not write prompt source files");
    }

    PicoHost app;
    memset(&app, 0, sizeof(app));
    PicoHost_SetPath(&app, workspace);
    setenv("XDG_CONFIG_HOME", temp, 1);
    Pico_DocsSetAppDir(NULL);

    PicoPromptSpan spans[PICO_PROMPT_SPAN_MAX];
    int n = 0;
    char *text = PicoSettings_LoadSystemPromptSpans(PicoHost_PrimaryWorkspace(&app), spans, &n);
    int failed = !text || n != 3 || ExpectSpan(&spans[0], PICO_PROMPT_SOURCE_BASE, text, "user-system") ||
                 ExpectSpan(&spans[1], PICO_PROMPT_SOURCE_WORKSPACE_SYSTEM, text, "workspace-system") ||
                 ExpectSpan(&spans[2], PICO_PROMPT_SOURCE_AGENTS, text, "agents-md") ||
                 strcmp(text, "user-system\n\nworkspace-system\n\nagents-md") != 0;

    free(text);
    unsetenv("XDG_CONFIG_HOME");
    unlink(user_system);
    unlink(workspace_system);
    unlink(agents);
    rmdir(pico_dir);
    rmdir(workspace);
    rmdir(config);
    rmdir(temp);
    return failed ? Fail("prompt source spans did not match assembled sections") : 0;
}

static int TestDocsHintIsBaseSpan(void)
{
    char temp[] = "/tmp/pico-prompt-docs-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create isolated docs directory");
    }

    char config[sizeof(temp) + 8];
    snprintf(config, sizeof(config), "%s/pico", temp);
    Pico_MkdirP(config);

    PicoHost app;
    memset(&app, 0, sizeof(app));
    setenv("XDG_CONFIG_HOME", temp, 1);
    Pico_DocsSetAppDir(temp);

    PicoPromptSpan spans[PICO_PROMPT_SPAN_MAX];
    int n = 0;
    char *text = PicoSettings_LoadSystemPromptSpans(PicoHost_PrimaryWorkspace(&app), spans, &n);
    const char *hint = text ? strstr(text, "If the user asks about Pico") : NULL;
    bool covered = false;
    if (hint && text)
    {
        size_t offset = (size_t)(hint - text);
        for (int i = 0; i < n; i++)
        {
            size_t start = spans[i].start;
            size_t end = start + spans[i].length;
            if (offset >= start && offset < end && spans[i].source == PICO_PROMPT_SOURCE_BASE)
            {
                covered = true;
                break;
            }
        }
    }
    int failed = !covered;

    free(text);
    Pico_DocsSetAppDir(NULL);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(config);
    rmdir(temp);
    return failed ? Fail("docs hint was not classified as the base system prompt") : 0;
}

int main(void)
{
    int rc = TestPerAgentSelection();
    if (rc)
    {
        return rc;
    }
    rc = TestModelContextBeatsFallback();
    if (rc)
    {
        return rc;
    }
    rc = TestFallbackWhenModelHasNoLimit();
    if (rc)
    {
        return rc;
    }
    rc = TestDisabledExtensionsFromUserSettings();
    if (rc)
    {
        return rc;
    }
    rc = TestPromptSourceSpans();
    if (rc)
    {
        return rc;
    }
    return TestDocsHintIsBaseSpan();
}
