#include "host_internal.h"
/* Included by agent_behavior_test.c to reuse its deterministic app host. */

static int TestSubagentProfileResolution(void)
{
    const char *name = "subagent profile model and effort resolution";
    PicoHost app;
    InitApp(&app);
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&app);
    PicoModel *models = (PicoModel *)realloc(ws->models, 2 * sizeof(PicoModel));
    if (!models)
    {
        PicoHost_Shutdown(&app);
        return Fail(name, "could not extend the model catalog");
    }
    ws->models = models;
    memset(&ws->models[1], 0, sizeof(ws->models[1]));
    ws->model_count = 2;
    snprintf(ws->models[1].id, sizeof(ws->models[1].id), "review-model");
    snprintf(ws->models[1].provider, sizeof(ws->models[1].provider), "test");
    snprintf(ws->models[1].effort[0], sizeof(ws->models[1].effort[0]), "low");
    snprintf(ws->models[1].effort[1], sizeof(ws->models[1].effort[1]), "high");
    ws->models[1].effort_count = 2;
    snprintf(ws->models[1].default_effort, sizeof(ws->models[1].default_effort), "low");

    PicoAgent *parent = TestAgent(&app);
    PicoSubagentProfileInfo profile;
    memset(&profile, 0, sizeof(profile));
    snprintf(profile.name, sizeof(profile.name), "review");
    snprintf(profile.purpose, sizeof(profile.purpose), "Review only");
    char model[128];
    char effort[PICO_EFFORT_LEN];
    bool inherited = PicoSubagentConfig_Resolve(&app, parent, &profile,
                                                model, sizeof(model), effort, sizeof(effort)) &&
                     strcmp(model, "test-model") == 0 && strcmp(effort, "none") == 0;
    profile.has_model = true;
    snprintf(profile.model, sizeof(profile.model), "review-model");
    bool target_default = PicoSubagentConfig_Resolve(&app, parent, &profile,
                                                     model, sizeof(model), effort, sizeof(effort)) &&
                          strcmp(model, "review-model") == 0 && strcmp(effort, "low") == 0;
    profile.has_effort = true;
    snprintf(profile.effort, sizeof(profile.effort), "high");
    bool explicit_effort = PicoSubagentConfig_Resolve(&app, parent, &profile,
                                                      model, sizeof(model), effort, sizeof(effort)) &&
                           strcmp(effort, "high") == 0;
    snprintf(profile.effort, sizeof(profile.effort), "unsupported");
    bool invalid = !PicoSubagentConfig_Resolve(&app, parent, &profile,
                                               model, sizeof(model), effort, sizeof(effort));
    bool parent_unchanged = strcmp(parent->model, "test-model") == 0 &&
                            strcmp(parent->effort, "none") == 0;
    PicoHost_Shutdown(&app);
    return inherited && target_default && explicit_effort && invalid && parent_unchanged
               ? 0 : Fail(name, "resolution did not follow inheritance/default/override rules");
}

static bool WriteConfigProfile(const char *dir, const char *filename, const char *source)
{
    char path[4096];
    if (!PicoPath_Format(path, sizeof(path), "%s/%s", dir, filename))
    {
        return false;
    }
    FILE *file = fopen(path, "wb");
    if (!file)
    {
        return false;
    }
    bool ok = fputs(source, file) >= 0 && fclose(file) == 0;
    return ok;
}

static bool FindLoadedProfile(const PicoHost *app, const char *name,
                              PicoSubagentProfileInfo *out)
{
    for (int i = 0; i < pico_subagent_profile_count(app); i++)
    {
        PicoSubagentProfileInfo info;
        if (pico_subagent_profile_info(app, i, &info) &&
            strcmp(info.name, name) == 0)
        {
            if (out)
            {
                *out = info;
            }
            return true;
        }
    }
    return false;
}

static int TestSubagentProfileDiscovery(void)
{
    const char *name = "subagent profile discovery and snapshot reload";
    char temp[] = "/tmp/pico-subagent-config-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail(name, "could not create config directory");
    }
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoWorkspace_LoadProfiles(PicoHost_PrimaryWorkspace(&app));

    char dir[4096];
    bool directory_path_ok = PicoPath_Format(dir, sizeof(dir), "%s/subagents", temp);
    struct stat st;
    bool created_empty = directory_path_ok && stat(dir, &st) == 0 && S_ISDIR(st.st_mode) &&
                         pico_subagent_profile_count(&app) == 0;

    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&app);
    PicoModel *models = (PicoModel *)realloc(ws->models, 2 * sizeof(PicoModel));
    if (!models)
    {
        PicoHost_Shutdown(&app);
        rmdir(dir);
        rmdir(temp);
        snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
        return Fail(name, "could not extend model catalog");
    }
    ws->models = models;
    memset(&ws->models[1], 0, sizeof(ws->models[1]));
    ws->model_count = 2;
    snprintf(ws->models[1].id, sizeof(ws->models[1].id), "gpt-5.6-sol");
    snprintf(ws->models[1].provider, sizeof(ws->models[1].provider), "test");
    snprintf(ws->models[1].effort[0], sizeof(ws->models[1].effort[0]), "low");
    snprintf(ws->models[1].effort[1], sizeof(ws->models[1].effort[1]), "high");
    ws->models[1].effort_count = 2;
    snprintf(ws->models[1].default_effort, sizeof(ws->models[1].default_effort), "low");
    pico_add_tool(PicoHost_PrimaryWorkspace(&app), "sh", "shell fixture", "{}", EchoTool, NULL);

    size_t exploration_len = 0;
    size_t review_len = 0;
    char *exploration = Pico_ReadFile(PICO_SOURCE_ROOT "/examples/subagents/exploration.json",
                                      &exploration_len);
    char *review = Pico_ReadFile(PICO_SOURCE_ROOT "/examples/subagents/review.json",
                                 &review_len);
    bool wrote = exploration && review &&
                 WriteConfigProfile(dir, "exploration.json", exploration) &&
                 WriteConfigProfile(dir, "review.json", review) &&
                 WriteConfigProfile(dir, "missing-purpose.json", "{\"description\":\"bad\"}") &&
                 WriteConfigProfile(dir, "duplicate-tools.json",
                                    "{\"purpose\":\"bad\",\"tools\":[\"sh\",\"sh\"]}") &&
                 WriteConfigProfile(dir, "unknown-tool.json",
                                    "{\"purpose\":\"bad\",\"tools\":[\"missing\"]}") &&
                 WriteConfigProfile(dir, "unknown-model.json",
                                    "{\"purpose\":\"bad\",\"model\":\"missing\"}") &&
                 WriteConfigProfile(dir, "bad-effort.json",
                                    "{\"purpose\":\"bad\",\"model\":\"gpt-5.6-sol\","
                                    "\"effort\":\"medium\"}") &&
                 WriteConfigProfile(dir, "bad name.json", "{\"purpose\":\"bad\"}") &&
                 WriteConfigProfile(dir, ".hidden.json", "{\"purpose\":\"hidden\"}");
    free(exploration);
    free(review);

    char nested[4096];
    bool nested_path_ok = PicoPath_Format(nested, sizeof(nested), "%s/nested", dir);
    if (nested_path_ok)
    {
        Pico_MkdirP(nested);
    }
    wrote = wrote && nested_path_ok &&
            WriteConfigProfile(nested, "ignored.json", "{\"purpose\":\"nested\"}");
    PicoWorkspace_LoadProfiles(PicoHost_PrimaryWorkspace(&app));
    PicoSubagentProfileInfo exploration_info;
    PicoSubagentProfileInfo review_info;
    bool examples = wrote && pico_subagent_profile_count(&app) == 2 &&
                    FindLoadedProfile(&app, "exploration", &exploration_info) &&
                    FindLoadedProfile(&app, "review", &review_info) &&
                    exploration_info.restricted_tools && exploration_info.tool_count == 1 &&
                    strcmp(exploration_info.tools[0], "sh") == 0 &&
                    review_info.restricted_tools && review_info.tool_count == 1 &&
                    strcmp(review_info.tools[0], "sh") == 0;

    char exploration_path[4096];
    char review_path[4096];
    if (PicoPath_Format(exploration_path, sizeof(exploration_path), "%s/exploration.json", dir))
    {
        unlink(exploration_path);
    }
    if (PicoPath_Format(review_path, sizeof(review_path), "%s/review.json", dir))
    {
        unlink(review_path);
    }
    bool replacement_written = WriteConfigProfile(
        dir, "replacement.json",
        "{/* jsonc */\"description\":\"Replacement\",\"purpose\":\"New snapshot\","
        "\"future_key\":true}");
    PicoWorkspace_LoadProfiles(PicoHost_PrimaryWorkspace(&app));
    PicoSubagentProfileInfo replacement;
    bool swapped = replacement_written && pico_subagent_profile_count(&app) == 1 &&
                   FindLoadedProfile(&app, "replacement", &replacement) &&
                   strcmp(replacement.purpose, "New snapshot") == 0 &&
                   !FindLoadedProfile(&app, "exploration", NULL) &&
                   !FindLoadedProfile(&app, "review", NULL);

    const char *files[] = {
        "missing-purpose.json", "duplicate-tools.json", "unknown-tool.json",
        "unknown-model.json", "bad-effort.json", "bad name.json", ".hidden.json",
        "replacement.json",
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
    {
        char cleanup[4096];
        if (PicoPath_Format(cleanup, sizeof(cleanup), "%s/%s", dir, files[i]))
        {
            unlink(cleanup);
        }
    }
    char nested_file[4096];
    if (PicoPath_Format(nested_file, sizeof(nested_file), "%s/ignored.json", nested))
    {
        unlink(nested_file);
    }
    rmdir(nested);
    PicoHost_Shutdown(&app);
    rmdir(dir);
    rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return created_empty && examples && swapped
               ? 0 : Fail(name, "discovery, invalid-file isolation, examples, or snapshot reload failed");
}
