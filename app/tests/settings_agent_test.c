#include "settings.h"
#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int Fail(const char *message)
{
    fprintf(stderr, "settings agent: %s\n", message);
    return 1;
}

static int WriteText(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file)
    {
        return -1;
    }
    int result = fputs(text, file) >= 0 && fclose(file) == 0 ? 0 : -1;
    return result;
}

int main(void)
{
    PicoApp app;
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

    char root[] = "/tmp/pico-settings-global-XXXXXX";
    if (!mkdtemp(root) || setenv("XDG_CONFIG_HOME", root, 1) != 0)
    {
        return Fail("could not create isolated settings root");
    }
    unsetenv("PICO_MODEL");
    unsetenv("OPENAI_MODEL");
    unsetenv("PICO_CONTEXT_LIMIT");
    unsetenv("PICO_RESUME_LAST");
    unsetenv("PICO_COMPACT_AT");
    char config[4096];
    char global_settings[4096];
    char global_system[4096];
    char workspace_a[4096];
    char workspace_b[4096];
    char local_dir[4096];
    char local_settings[4096];
    char local_system[4096];
    char agents_a[4096];
    char agents_b[4096];
    if (!PicoPath_Format(config, sizeof(config), "%s/pico", root) ||
        !PicoPath_Format(global_settings, sizeof(global_settings), "%s/settings.json", config) ||
        !PicoPath_Format(global_system, sizeof(global_system), "%s/SYSTEM.md", config) ||
        !PicoPath_Format(workspace_a, sizeof(workspace_a), "%s/workspace-a", root) ||
        !PicoPath_Format(workspace_b, sizeof(workspace_b), "%s/workspace-b", root) ||
        !PicoPath_Format(local_dir, sizeof(local_dir), "%s/.pico", workspace_a) ||
        !PicoPath_Format(local_settings, sizeof(local_settings), "%s/settings.json", local_dir) ||
        !PicoPath_Format(local_system, sizeof(local_system), "%s/SYSTEM.md", local_dir) ||
        !PicoPath_Format(agents_a, sizeof(agents_a), "%s/AGENTS.md", workspace_a) ||
        !PicoPath_Format(agents_b, sizeof(agents_b), "%s/AGENTS.md", workspace_b))
    {
        return Fail("settings fixture paths were too long");
    }
    Pico_MkdirP(config);
    Pico_MkdirP(local_dir);
    Pico_MkdirP(workspace_b);
    if (WriteText(global_settings,
                  "{\"model\":\"global-model\",\"models\":[{\"id\":\"global-model\","
                  "\"name\":\"Global\",\"provider\":\"openai\",\"context_limit\":321}]}" ) != 0 ||
        WriteText(local_settings,
                  "{\"model\":\"local-model\",\"models\":[{\"id\":\"local-model\","
                  "\"name\":\"Local\",\"provider\":\"openai\",\"context_limit\":999}]}" ) != 0 ||
        WriteText(global_system, "global system") != 0 ||
        WriteText(local_system, "workspace A system") != 0 ||
        WriteText(agents_a, "workspace A agents") != 0 ||
        WriteText(agents_b, "workspace B agents") != 0)
    {
        return Fail("could not write settings/context fixtures");
    }
    PicoApp loaded;
    memset(&loaded, 0, sizeof(loaded));
    snprintf(loaded.workspace, sizeof(loaded.workspace), "%s", workspace_a);
    PicoSettings_Load(&loaded);
    if (strcmp(loaded.settings.model, "global-model") != 0 || loaded.model_count != 1 ||
        strcmp(loaded.models[0].id, "global-model") != 0)
    {
        free(loaded.models);
        return Fail("workspace-local settings or models overrode global configuration");
    }
    PicoAgent context_a;
    PicoAgent context_b;
    memset(&context_a, 0, sizeof(context_a));
    memset(&context_b, 0, sizeof(context_b));
    snprintf(context_a.workspace_path, sizeof(context_a.workspace_path), "%s", workspace_a);
    snprintf(context_b.workspace_path, sizeof(context_b.workspace_path), "%s", workspace_b);
    char *prompt_a = PicoSettings_LoadSystemPrompt(&loaded, &context_a);
    char *prompt_b = PicoSettings_LoadSystemPrompt(&loaded, &context_b);
    bool correct_context = prompt_a && prompt_b && strstr(prompt_a, "global system") &&
                           strstr(prompt_a, "workspace A system") &&
                           strstr(prompt_a, "workspace A agents") &&
                           !strstr(prompt_b, "workspace A") && strstr(prompt_b, "workspace B agents");
    free(prompt_a);
    free(prompt_b);
    free(loaded.models);
    if (!correct_context)
    {
        return Fail("explicit agent workspace prompt context crossed workspaces");
    }
    return 0;
}
