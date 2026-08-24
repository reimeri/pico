#include "settings.h"

#include <stdio.h>
#include <string.h>

static int Fail(const char *message)
{
    fprintf(stderr, "settings agent: %s\n", message);
    return 1;
}

static int TestModelContextBeatsFallback(void)
{
    PicoApp app;
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
    PicoApp app;
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
    return 0;
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
    return TestFallbackWhenModelHasNoLimit();
}
