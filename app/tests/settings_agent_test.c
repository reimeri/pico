#define _POSIX_C_SOURCE 200809L

#include "docs_path.h"
#include "json.h"
#include "settings.h"
#include "host_internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int Fail(const char *message)
{
    fprintf(stderr, "settings agent: %s\n", message);
    return 1;
}

static int TestModelContextBeatsFallback(void)
{
    PicoWorkspace ws;
    PicoAgent agent;
    PicoModel model;
    memset(&ws, 0, sizeof(ws));
    memset(&agent, 0, sizeof(agent));
    memset(&model, 0, sizeof(model));

    snprintf(model.id, sizeof(model.id), "grok-4.6");
    snprintf(model.name, sizeof(model.name), "Grok 4.6");
    model.context_limit = 500000;
    ws.models = &model;
    ws.model_count = 1;
    snprintf(ws.settings.default_model, sizeof(ws.settings.default_model), "grok-4.6");
    ws.settings.context_limit_fallback = 1000000;
    agent.workspace = &ws;

    PicoSettings_InitAgent(&agent);
    if (agent.context_limit != 500000)
    {
        return Fail("selected model context_limit lost to root/env fallback");
    }
    return 0;
}

static int TestFallbackWhenModelHasNoLimit(void)
{
    PicoWorkspace ws;
    PicoAgent agent;
    PicoModel model;
    memset(&ws, 0, sizeof(ws));
    memset(&agent, 0, sizeof(agent));
    memset(&model, 0, sizeof(model));

    snprintf(model.id, sizeof(model.id), "custom");
    snprintf(model.name, sizeof(model.name), "Custom");
    ws.models = &model;
    ws.model_count = 1;
    snprintf(ws.settings.default_model, sizeof(ws.settings.default_model), "custom");
    ws.settings.context_limit_fallback = 128000;
    agent.workspace = &ws;

    PicoSettings_InitAgent(&agent);
    if (agent.context_limit != 128000)
    {
        return Fail("missing model context_limit did not use root/env fallback");
    }
    return 0;
}

static int TestPerAgentSelection(void)
{
    PicoWorkspace ws;
    PicoAgent first;
    PicoAgent second;
    PicoModel models[2];
    memset(&ws, 0, sizeof(ws));
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

    ws.models = models;
    ws.model_count = 2;
    snprintf(ws.settings.default_model, sizeof(ws.settings.default_model), "model-a");
    ws.settings.compact_enabled = true;
    ws.settings.compact_ratio = 0.75;
    first.workspace = &ws;
    second.workspace = &ws;

    PicoSettings_InitAgent(&first);
    PicoSettings_InitAgent(&second);
    snprintf(first.model, sizeof(first.model), "model-b");
    first.effort[0] = '\0';
    PicoSettings_SyncAgent(&first);

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
    fputs("{\n  \"disabled_host_extensions\": [\"composer\"]\n}\n", f);
    fclose(f);

    PicoHost app;
    memset(&app, 0, sizeof(app));
    PicoHost_SetPath(&app, temp);
    setenv("XDG_CONFIG_HOME", temp, 1);
    PicoHostPreferences_Load(&app);
    int failed = app.preferences.disabled_host_extension_count != 1 ||
                 strcmp(app.preferences.disabled_host_extensions[0], "composer") != 0;

    unsetenv("XDG_CONFIG_HOME");
    unlink(path);
    rmdir(dir);
    rmdir(temp);
    return failed ? Fail("user disabled_host_extensions did not populate the disabled set") : 0;
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

static int TestConcurrentHostSettingsWritePreservesUpdate(void)
{
    char temp[] = "/tmp/pico-settings-lock-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create settings lock directory");
    }
    char dir[sizeof(temp) + 8];
    char settings_path[sizeof(temp) + 32];
    char lock_path[sizeof(temp) + 40];
    snprintf(dir, sizeof(dir), "%s/pico", temp);
    snprintf(settings_path, sizeof(settings_path), "%s/pico/settings.json", temp);
    snprintf(lock_path, sizeof(lock_path), "%s/pico/settings.json.lock", temp);
    Pico_MkdirP(dir);
    if (WriteFile(settings_path, "{\n  \"model\": \"before\"\n}\n"))
    {
        return Fail("could not write settings lock fixture");
    }

    int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (lock_fd < 0 || fcntl(lock_fd, F_SETLK, &lock) != 0)
    {
        if (lock_fd >= 0)
        {
            close(lock_fd);
        }
        return Fail("could not hold settings lock fixture");
    }

    setenv("XDG_CONFIG_HOME", temp, 1);
    int ready[2];
    if (pipe(ready) != 0)
    {
        close(lock_fd);
        return Fail("could not create settings lock gate");
    }
    pid_t child = fork();
    if (child == 0)
    {
        close(ready[0]);
        close(lock_fd);
        PicoHost host;
        memset(&host, 0, sizeof(host));
        pthread_mutex_init(&host.settings_mu, NULL);
        if (write(ready[1], "x", 1) != 1)
        {
            _exit(2);
        }
        close(ready[1]);
        bool ok = PicoHost_SetExtensionDisabled(&host, "footer", true);
        pthread_mutex_destroy(&host.settings_mu);
        _exit(ok ? 0 : 3);
    }
    close(ready[1]);
    char token = '\0';
    int failed = child < 0 || read(ready[0], &token, 1) != 1;
    close(ready[0]);
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 200000000L};
    nanosleep(&delay, NULL);
    int status = 0;
    pid_t early = child > 0 ? waitpid(child, &status, WNOHANG) : -1;
    if (early != 0)
    {
        failed = 1;
    }
    if (WriteFile(settings_path, "{\n  \"model\": \"after\"\n}\n"))
    {
        failed = 1;
    }
    close(lock_fd);
    if (child > 0 && early == 0 &&
        (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0))
    {
        failed = 1;
    }

    size_t len = 0;
    char *saved = Pico_ReadFile(settings_path, &len);
    if (!saved || !strstr(saved, "after") || !strstr(saved, "disabled_host_extensions") ||
        !strstr(saved, "footer"))
    {
        failed = 1;
    }
    free(saved);

    unsetenv("XDG_CONFIG_HOME");
    unlink(lock_path);
    unlink(settings_path);
    rmdir(dir);
    rmdir(temp);
    return failed ? Fail("concurrent settings write lost an unrelated update") : 0;
}

static int TestCreatesUserSettingsFromBundledExample(void)
{
    static const char example[] = "{\n  // generated example\n  \"models\": []\n}\n";
    char temp[] = "/tmp/pico-settings-create-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create settings bootstrap directory");
    }

    char data[sizeof(temp) + 8];
    char examples[sizeof(temp) + 24];
    char source[sizeof(temp) + 48];
    char destination[sizeof(temp) + 48];
    snprintf(data, sizeof(data), "%s/data", temp);
    snprintf(examples, sizeof(examples), "%s/data/examples", temp);
    snprintf(source, sizeof(source), "%s/data/examples/settings.json", temp);
    snprintf(destination, sizeof(destination), "%s/config/pico/settings.json", temp);
    Pico_MkdirP(examples);
    if (WriteFile(source, example))
    {
        return Fail("could not write bundled settings fixture");
    }

    char config[sizeof(temp) + 16];
    snprintf(config, sizeof(config), "%s/config", temp);
    PicoHost app;
    memset(&app, 0, sizeof(app));
    setenv("XDG_CONFIG_HOME", config, 1);
    Pico_DocsSetAppDir(data);
    PicoHostPreferences_Load(&app);

    size_t len = 0;
    char *created = Pico_ReadFile(destination, &len);
    int failed = !created || len != strlen(example) || memcmp(created, example, strlen(example)) != 0;
    free(created);

    Pico_DocsSetAppDir(NULL);
    unsetenv("XDG_CONFIG_HOME");
    unlink(destination);
    unlink(source);
    char config_pico[sizeof(temp) + 24];
    snprintf(config_pico, sizeof(config_pico), "%s/config/pico", temp);
    rmdir(config_pico);
    rmdir(config);
    rmdir(examples);
    rmdir(data);
    rmdir(temp);
    return failed ? Fail("first startup did not copy the bundled settings example") : 0;
}

static int TestPreservesExistingUserSettings(void)
{
    static const char custom[] = "{\n  \"model\": \"keep-me\"\n}\n";
    char temp[] = "/tmp/pico-settings-preserve-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create settings preservation directory");
    }

    char data[sizeof(temp) + 8];
    char examples[sizeof(temp) + 24];
    char source[sizeof(temp) + 48];
    char config[sizeof(temp) + 16];
    char config_pico[sizeof(temp) + 24];
    char destination[sizeof(temp) + 48];
    snprintf(data, sizeof(data), "%s/data", temp);
    snprintf(examples, sizeof(examples), "%s/data/examples", temp);
    snprintf(source, sizeof(source), "%s/data/examples/settings.json", temp);
    snprintf(config, sizeof(config), "%s/config", temp);
    snprintf(config_pico, sizeof(config_pico), "%s/config/pico", temp);
    snprintf(destination, sizeof(destination), "%s/config/pico/settings.json", temp);
    Pico_MkdirP(examples);
    Pico_MkdirP(config_pico);
    if (WriteFile(source, "{\n  \"models\": []\n}\n") || WriteFile(destination, custom))
    {
        return Fail("could not write settings preservation fixtures");
    }

    PicoHost app;
    memset(&app, 0, sizeof(app));
    setenv("XDG_CONFIG_HOME", config, 1);
    Pico_DocsSetAppDir(data);
    PicoHostPreferences_Load(&app);

    size_t len = 0;
    char *preserved = Pico_ReadFile(destination, &len);
    int failed = !preserved || len != strlen(custom) || memcmp(preserved, custom, strlen(custom)) != 0;
    free(preserved);

    unlink(destination);
    if (mkdir(destination, 0700) != 0)
    {
        return Fail("could not create existing settings directory fixture");
    }
    PicoHostPreferences_Load(&app);
    struct stat st;
    if (lstat(destination, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        failed = 1;
    }
    rmdir(destination);

    char target[sizeof(temp) + 48];
    snprintf(target, sizeof(target), "%s/config/pico/custom.json", temp);
    if (WriteFile(target, custom) || symlink(target, destination) != 0)
    {
        return Fail("could not create existing settings symlink fixture");
    }
    PicoHostPreferences_Load(&app);
    preserved = Pico_ReadFile(target, &len);
    if (lstat(destination, &st) != 0 || !S_ISLNK(st.st_mode) || !preserved ||
        len != strlen(custom) || memcmp(preserved, custom, strlen(custom)) != 0)
    {
        failed = 1;
    }
    free(preserved);

    Pico_DocsSetAppDir(NULL);
    unsetenv("XDG_CONFIG_HOME");
    unlink(destination);
    unlink(target);
    unlink(source);
    rmdir(config_pico);
    rmdir(config);
    rmdir(examples);
    rmdir(data);
    rmdir(temp);
    return failed ? Fail("startup replaced an existing user settings file") : 0;
}

static int TestMissingBundledSettingsIsNonFatal(void)
{
    char temp[] = "/tmp/pico-settings-missing-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create missing settings template directory");
    }
    char config[sizeof(temp) + 16];
    char destination[sizeof(temp) + 48];
    snprintf(config, sizeof(config), "%s/config", temp);
    snprintf(destination, sizeof(destination), "%s/config/pico/settings.json", temp);

    PicoHost app;
    memset(&app, 0, sizeof(app));
    setenv("XDG_CONFIG_HOME", config, 1);
    Pico_DocsSetAppDir(temp);
    PicoHostPreferences_Load(&app);

    struct stat st;
    int failed = lstat(destination, &st) == 0;
    Pico_DocsSetAppDir(NULL);
    unsetenv("XDG_CONFIG_HOME");
    char config_pico[sizeof(temp) + 24];
    snprintf(config_pico, sizeof(config_pico), "%s/config/pico", temp);
    rmdir(config_pico);
    rmdir(config);
    rmdir(temp);
    return failed ? Fail("missing bundled template created an invalid user settings file") : 0;
}

static void CreateSettingsInChild(int gate, const char *data)
{
    char token;
    if (read(gate, &token, 1) != 1)
    {
        _exit(1);
    }
    close(gate);
    Pico_DocsSetAppDir(data);
    PicoHost app;
    memset(&app, 0, sizeof(app));
    PicoHostPreferences_Load(&app);
    _exit(0);
}

static int TestConcurrentSettingsCreationPublishesOneCompleteTemplate(void)
{
    static const char first[] = "{\n  \"winner\": \"first\",\n  \"models\": []\n}\n";
    static const char second[] = "{\n  \"winner\": \"second\",\n  \"models\": []\n}\n";
    char temp[] = "/tmp/pico-settings-concurrent-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create concurrent settings directory");
    }

    char first_data[sizeof(temp) + 16];
    char second_data[sizeof(temp) + 16];
    char first_examples[sizeof(temp) + 32];
    char second_examples[sizeof(temp) + 32];
    char first_source[sizeof(temp) + 56];
    char second_source[sizeof(temp) + 56];
    char config[sizeof(temp) + 16];
    char destination[sizeof(temp) + 48];
    snprintf(first_data, sizeof(first_data), "%s/first", temp);
    snprintf(second_data, sizeof(second_data), "%s/second", temp);
    snprintf(first_examples, sizeof(first_examples), "%s/first/examples", temp);
    snprintf(second_examples, sizeof(second_examples), "%s/second/examples", temp);
    snprintf(first_source, sizeof(first_source), "%s/first/examples/settings.json", temp);
    snprintf(second_source, sizeof(second_source), "%s/second/examples/settings.json", temp);
    snprintf(config, sizeof(config), "%s/config", temp);
    snprintf(destination, sizeof(destination), "%s/config/pico/settings.json", temp);
    Pico_MkdirP(first_examples);
    Pico_MkdirP(second_examples);
    if (WriteFile(first_source, first) || WriteFile(second_source, second))
    {
        return Fail("could not write concurrent settings fixtures");
    }

    setenv("XDG_CONFIG_HOME", config, 1);
    int gate[2];
    if (pipe(gate) != 0)
    {
        return Fail("could not create concurrent settings gate");
    }
    pid_t a = fork();
    if (a == 0)
    {
        close(gate[1]);
        CreateSettingsInChild(gate[0], first_data);
    }
    pid_t b = fork();
    if (b == 0)
    {
        close(gate[1]);
        CreateSettingsInChild(gate[0], second_data);
    }
    close(gate[0]);
    int failed = a < 0 || b < 0 || write(gate[1], "go", 2) != 2;
    close(gate[1]);
    int a_status = 0;
    int b_status = 0;
    if (a > 0 && (waitpid(a, &a_status, 0) != a || !WIFEXITED(a_status) || WEXITSTATUS(a_status) != 0))
    {
        failed = 1;
    }
    if (b > 0 && (waitpid(b, &b_status, 0) != b || !WIFEXITED(b_status) || WEXITSTATUS(b_status) != 0))
    {
        failed = 1;
    }

    size_t len = 0;
    char *published = Pico_ReadFile(destination, &len);
    bool is_first = published && len == strlen(first) && memcmp(published, first, len) == 0;
    bool is_second = published && len == strlen(second) && memcmp(published, second, len) == 0;
    if (!is_first && !is_second)
    {
        failed = 1;
    }
    free(published);

    unsetenv("XDG_CONFIG_HOME");
    unlink(destination);
    unlink(first_source);
    unlink(second_source);
    char config_pico[sizeof(temp) + 24];
    snprintf(config_pico, sizeof(config_pico), "%s/config/pico", temp);
    rmdir(config_pico);
    rmdir(config);
    rmdir(first_examples);
    rmdir(second_examples);
    rmdir(first_data);
    rmdir(second_data);
    rmdir(temp);
    return failed ? Fail("concurrent startup did not publish one complete settings template") : 0;
}

static int TestBundledSettingsTemplateIsValid(void)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/examples/settings.json", PICO_SOURCE_ROOT);
    size_t len = 0;
    char *source = Pico_ReadFile(path, &len);
    if (!source)
    {
        return Fail("could not read bundled settings template");
    }
    JsonStripComments(source, len);
    JsonDoc doc;
    int parsed = JsonParse(&doc, source, len);
    bool valid = parsed == 0 && JsonIsArray(&doc, JsonObjGet(&doc, 0, "models"));
    if (parsed == 0)
    {
        JsonFree(&doc);
    }
    free(source);
    return valid ? 0 : Fail("bundled settings template is not valid JSONC with a models array");
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
    PicoWorkspace ws;
    memset(&app, 0, sizeof(app));
    memset(&ws, 0, sizeof(ws));
    ws.host = &app;
    snprintf(ws.path, sizeof(ws.path), "%s", workspace);
    app.workspaces[0] = &ws;
    app.workspace_count = 1;
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
    PicoWorkspace ws;
    memset(&app, 0, sizeof(app));
    memset(&ws, 0, sizeof(ws));
    ws.host = &app;
    snprintf(ws.path, sizeof(ws.path), "%s", temp);
    app.workspaces[0] = &ws;
    app.workspace_count = 1;
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
    rc = TestConcurrentHostSettingsWritePreservesUpdate();
    if (rc)
    {
        return rc;
    }
    rc = TestCreatesUserSettingsFromBundledExample();
    if (rc)
    {
        return rc;
    }
    rc = TestPreservesExistingUserSettings();
    if (rc)
    {
        return rc;
    }
    rc = TestMissingBundledSettingsIsNonFatal();
    if (rc)
    {
        return rc;
    }
    rc = TestConcurrentSettingsCreationPublishesOneCompleteTemplate();
    if (rc)
    {
        return rc;
    }
    rc = TestBundledSettingsTemplateIsValid();
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
