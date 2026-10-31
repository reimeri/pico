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
    TestAddTool(&app, "sh", "shell fixture", "{}", EchoTool, NULL);

    size_t exploration_len = 0;
    size_t review_len = 0;
    char *exploration = Pico_ReadFile(PICO_SOURCE_ROOT "/examples/subagents/explore.json",
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
                    exploration_info.parallel_safe && review_info.parallel_safe &&
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

static int TestSubagentParallelSettingValidation(void)
{
    const char *name = "profile parallel limit validation";
    char temp[] = "/tmp/pico-profile-parallel-XXXXXX";
    if (!mkdtemp(temp)) return Fail(name, "temporary directory failed");
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoWorkspace_LoadProfiles(TestWs(&app));
    char dir[4096], path[4096];
    snprintf(dir, sizeof(dir), "%s/subagents", temp);
    snprintf(path, sizeof(path), "%s/subagents/probe.json", temp);
    const char *values[] = {"1", "0", "2.5", "\"2\"", "9999999999999999999999999999"};
    bool ok = true;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
    {
        char json[128];
        snprintf(json, sizeof(json), "{\"purpose\":\"probe\",\"max_parallel_tools\":%s}", values[i]);
        bool wrote = WriteConfigProfile(dir, "probe.json", json);
        PicoWorkspace_LoadProfiles(TestWs(&app));
        PicoSubagentProfileInfo profile;
        bool found = FindLoadedProfile(&app, "probe", &profile);
        ok = wrote && (i == 0 ? found && profile.max_parallel_tools == 1 : !found) && ok;
    }
    PicoHost_Shutdown(&app);
    unlink(path); rmdir(dir); rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return ok ? 0 : Fail(name, "invalid limit made a profile available");
}

static int TestProfileParallelSafeValidation(void)
{
    char temp[] = "/tmp/pico-parallel-safe-XXXXXX";
    if (!mkdtemp(temp)) return Fail("parallel_safe validation", "temporary directory failed");
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoWorkspace_LoadProfiles(TestWs(&app));
    char dir[4096], path[4096];
    snprintf(dir, sizeof(dir), "%s/subagents", temp);
    snprintf(path, sizeof(path), "%s/subagents/probe.json", temp);
    const char *fields[] = {"", ",\"parallel_safe\":false", ",\"parallel_safe\":true",
                           ",\"parallel_safe\":\"true\"", ",\"parallel_safe\":1", ",\"parallel_safe\":null"};
    bool ok = true;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
    {
        char json[128];
        snprintf(json, sizeof(json), "{\"purpose\":\"probe\"%s}", fields[i]);
        bool wrote = WriteConfigProfile(dir, "probe.json", json);
        PicoWorkspace_LoadProfiles(TestWs(&app));
        PicoSubagentProfileInfo profile;
        bool found = FindLoadedProfile(&app, "probe", &profile);
        ok = wrote && (i < 3 ? found && profile.parallel_safe == (i == 2) : !found) && ok;
    }
    PicoHost_Shutdown(&app);
    unlink(path); rmdir(dir); rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return ok ? 0 : Fail("parallel_safe validation", "default, opt-in, or boolean validation failed");
}

static int TestProfileFastValidation(void)
{
    const char *name = "profile Fast opt-in validation";
    char temp[] = "/tmp/pico-profile-fast-XXXXXX";
    if (!mkdtemp(temp)) return Fail(name, "temporary directory failed");
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    ResetTest(TEST_SINGLE, 0);
    PicoHost app;
    InitApp(&app);
    PicoWorkspace *ws = TestWs(&app);
    PicoWorkspace_LoadProfiles(ws);
    char dir[4096], path[4096];
    snprintf(dir, sizeof(dir), "%s/subagents", temp);
    snprintf(path, sizeof(path), "%s/subagents/probe.json", temp);
    const char *fields[] = {"", ",\"fast\":false", ",\"fast\":true", ",\"fast\":\"true\""};
    bool ok = true;
    for (int i = 0; i < 4; i++)
    {
        char json[128];
        snprintf(json, sizeof(json), "{\"purpose\":\"probe\"%s}", fields[i]);
        bool wrote = WriteConfigProfile(dir, "probe.json", json);
        PicoWorkspace_LoadProfiles(ws);
        PicoSubagentProfileInfo profile;
        bool found = FindLoadedProfile(&app, "probe", &profile);
        ok = wrote && (i < 3 ? found && profile.fast == (i == 2) : !found) && ok;
        if (i == 2 && found)
        {
            char model[128], effort[PICO_EFFORT_LEN];
            ok = !PicoSubagentConfig_Resolve(&app, TestAgent(&app), &profile,
                       model, sizeof(model), effort, sizeof(effort)) && ok;
            ws->models[0].supports_fast = true;
            ok = PicoSubagentConfig_Resolve(&app, TestAgent(&app), &profile,
                      model, sizeof(model), effort, sizeof(effort)) && ok;
        }
    }
    PicoHost_Shutdown(&app);
    unlink(path); rmdir(dir); rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return ok ? 0 : Fail(name, "profile accepted a malformed or unsupported Fast selection");
}

static int TestSubagentProfileEditing(void)
{
    const char *name = "subagent profile editing preserves policy and JSONC with safe file operations";
    char temp[] = "/tmp/pico-subagent-editor-XXXXXX";
    if (!mkdtemp(temp)) return Fail(name, "temporary config");
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    PicoHost app;
    InitApp(&app);
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&app);
    TestAddTool(&app, "sh", "shell", "{}", EchoTool, NULL);
    char dir[4096], path[4096], renamed[4096];
    PicoSubagentConfig_Directory(dir, sizeof(dir));
    Pico_MkdirP(dir);
    PicoPath_Format(path, sizeof(path), "%s/review.json", dir);
    PicoPath_Format(renamed, sizeof(renamed), "%s/renamed.json", dir);
    const char *fixture = "{\n  // keep this role comment\n  \"purpose\": \"\",\n"
        "  \"model\": \"missing-model\",\n  \"tools\": [\"sh\"],\n  \"fast\": true,\n"
        "  \"parallel_safe\": true,\n  \"max_parallel_tools\": 1,\n"
        "  \"custom\": {\"tag\": 7} // keep this, including the comma\n}\n";
    bool ok = WriteConfigProfile(dir, "review.json", fixture);
    PicoSubagentDocument *docs = NULL;
    int count = 0;
    char error[512] = "";
    ok = ok && PicoSubagentConfig_List(&docs, &count, error, sizeof(error)) && count == 1 &&
        !docs[0].valid && docs[0].parseable;
    char *source = NULL;
    if (ok) {
        PicoSubagentProfileInfo repaired = docs[0].profile;
        /* Repair required text and remove an optional field without changing policy. */
        snprintf(repaired.purpose, sizeof(repaired.purpose), "Review only");
        repaired.has_model = false;
        source = PicoSubagentConfig_Build(&docs[0], &repaired, error, sizeof(error));
        ok = source && strstr(source, "// keep this role comment") &&
            strstr(source, "// keep this, including the comma") && strstr(source, "\"custom\": {\"tag\": 7}") &&
            PicoSubagentConfig_Save(&docs[0], "review", source, error, sizeof(error));
        if (ok) {
            PicoWorkspace_LoadProfiles(ws);
            const PicoSubagentProfileInfo *loaded = PicoSubagentConfig_Find(ws, "review");
            ok = loaded && loaded->restricted_tools && loaded->tool_count == 1 && !strcmp(loaded->tools[0], "sh") &&
                loaded->fast && loaded->parallel_safe && loaded->max_parallel_tools == 1 && !loaded->has_model;
        }
        /* Stale drafts must not overwrite or delete a newer file. */
        ok = ok && !PicoSubagentConfig_Save(&docs[0], "review", source, error, sizeof(error)) &&
                   !PicoSubagentConfig_Delete(&docs[0], error, sizeof(error));
    }
    free(source);
    PicoSubagentConfig_FreeDocuments(docs, count);
    docs = NULL; count = 0;
    if (ok) {
        ok = PicoSubagentConfig_List(&docs, &count, error, sizeof(error)) && count == 1;
        source = ok ? PicoSubagentConfig_Build(&docs[0], &docs[0].profile, error, sizeof(error)) : NULL;
        ok = source && !PicoSubagentConfig_Save(NULL, "review", source, error, sizeof(error)) &&
            PicoSubagentConfig_Save(NULL, "copy", source, error, sizeof(error)) &&
            PicoSubagentConfig_Save(&docs[0], "renamed", source, error, sizeof(error)) && access(path, F_OK) != 0;
        free(source);
    }
    PicoSubagentConfig_FreeDocuments(docs, count);
    /* Full original bytes, including NUL, remain available for confirmed repair/delete. */
    FILE *f = fopen(path, "wb");
    static const char binary[] = "{\"purpose\":\"prefix\"}\0trailing bytes";
    if (f) { ok = fwrite(binary, 1, sizeof(binary) - 1, f) == sizeof(binary) - 1 && ok; fclose(f); }
    else ok = false;
    docs = NULL; count = 0;
    if (ok) {
        ok = PicoSubagentConfig_List(&docs, &count, error, sizeof(error));
        bool deleted = false;
        for (int i = 0; i < count; i++) if (!strcmp(docs[i].filename, "review.json")) {
            deleted = !docs[i].valid && !docs[i].parseable && PicoSubagentConfig_Delete(&docs[i], error, sizeof(error));
        }
        ok = ok && deleted;
    }
    PicoSubagentConfig_FreeDocuments(docs, count);
    char copy[4096];
    PicoPath_Format(copy, sizeof(copy), "%s/copy.json", dir);
    unlink(copy); unlink(renamed); unlink(path); rmdir(dir); rmdir(temp);
    PicoHost_Shutdown(&app);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return ok ? 0 : Fail(name, error[0] ? error : "edit/collision/rename/binary-delete contract");
}

static int TestSubagentGlobalProfileValidation(void)
{
    const char *name = "global profile editing validates schema without workspace registries";
    char temp[] = "/tmp/pico-global-profile-validation-XXXXXX";
    if (!mkdtemp(temp)) return Fail(name, "temporary config");
    snprintf(g_config_dir, sizeof(g_config_dir), "%s", temp);
    char directory[4096];
    PicoSubagentConfig_Directory(directory, sizeof(directory));
    Pico_MkdirP(directory);
    const struct {
        const char *filename;
        const char *source;
        bool valid;
    } cases[] = {
        {"future-model.json", "{\"purpose\":\"Future model\",\"model\":\"future-model\",\"effort\":\"research\"}", true},
        {"future-tool.json", "{\"purpose\":\"Future tool\",\"tools\":[\"future-tool\"]}", true},
        {"future-effort.json", "{\"purpose\":\"Future effort\",\"model\":\"test-model\",\"effort\":\"research\"}", true},
        {"duplicate-tools.json", "{\"purpose\":\"Invalid schema\",\"tools\":[\"future-tool\",\"future-tool\"]}", false},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        ok = WriteConfigProfile(directory, cases[i].filename, cases[i].source) && ok;
    /* Without a catalog lookup, oversized IDs must still be rejected instead
     * of silently truncated into a different stored profile definition. */
    char long_model[1024];
    memset(long_model, 'x', sizeof(long_model) - 1);
    long_model[sizeof(long_model) - 1] = '\0';
    char oversized[1200];
    snprintf(oversized, sizeof(oversized), "{\"purpose\":\"Oversized ID\",\"model\":\"%s\"}", long_model);
    ok = WriteConfigProfile(directory, "oversized-model.json", oversized) && ok;
    PicoSubagentDocument *documents = NULL;
    int count = 0;
    char error[512] = "";
    ok = PicoSubagentConfig_List(&documents, &count, error, sizeof(error)) && count == 5 && ok;
    for (int i = 0; i < count; i++) {
        bool expected = false;
        for (size_t j = 0; j < sizeof(cases) / sizeof(cases[0]); j++)
            if (!strcmp(documents[i].filename, cases[j].filename)) expected = cases[j].valid;
        ok = documents[i].valid == expected && ok;
        if (expected) {
            char *source = PicoSubagentConfig_Build(&documents[i], &documents[i].profile, error, sizeof(error));
            ok = source && PicoSubagentConfig_Save(&documents[i], documents[i].profile.name,
                                                    source, error, sizeof(error)) && ok;
            free(source);
        }
    }
    PicoSubagentConfig_FreeDocuments(documents, count);

    PicoHost app;
    InitApp(&app);
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&app);
    PicoWorkspace_LoadProfiles(ws);
    ok = ws->profile_count == 0 && ok;
    PicoModel *models = realloc(ws->models, 2 * sizeof(*models));
    if (models) {
        ws->models = models;
        memset(&models[1], 0, sizeof(models[1]));
        ws->model_count = 2;
        snprintf(models[1].id, sizeof(models[1].id), "future-model");
        for (int i = 0; i < 2; i++) {
            snprintf(models[i].effort[0], sizeof(models[i].effort[0]), "research");
            models[i].effort_count = 1;
        }
        TestAddTool(&app, "future-tool", "future fixture", "{}", EchoTool, NULL);
        PicoWorkspace_LoadProfiles(ws);
        ok = PicoSubagentConfig_Find(ws, "future-model") && PicoSubagentConfig_Find(ws, "future-tool") &&
            PicoSubagentConfig_Find(ws, "future-effort") && ws->profile_count == 3 && ok;
    } else ok = false;
    PicoHost_Shutdown(&app);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char path[4096];
        PicoPath_Format(path, sizeof(path), "%s/%s", directory, cases[i].filename);
        unlink(path);
    }
    char path[4096];
    PicoPath_Format(path, sizeof(path), "%s/oversized-model.json", directory);
    unlink(path); rmdir(directory); rmdir(temp);
    snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/pico-agent-behavior");
    return ok ? 0 : Fail(name, error[0] ? error : "schema and workspace compatibility were not kept separate");
}
