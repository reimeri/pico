#define _POSIX_C_SOURCE 200809L

#include "settings.h"
#include "host_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int Fail(const char *message)
{
    fprintf(stderr, "chat width: %s\n", message);
    return 1;
}

static int TestClamp(void)
{
    if (Pico_ClampChatWidth(1200.0f, 800.0f) != 800.0f)
    {
        return Fail("wide pane should use the character cap");
    }
    if (Pico_ClampChatWidth(400.0f, 800.0f) != 400.0f)
    {
        return Fail("narrow pane should use available width");
    }
    if (Pico_ClampChatWidth(1200.0f, 0.0f) != 1200.0f)
    {
        return Fail("unlimited cap should keep available width");
    }
    return 0;
}

static void CleanupDir(const char *dir)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/pico/settings.json", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/pico", dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/.pico/settings.json", dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.pico", dir);
    rmdir(path);
    rmdir(dir);
}

static bool WriteFile(const char *path, const char *body)
{
    FILE *f = fopen(path, "w");
    if (!f)
    {
        return false;
    }
    int rc = fputs(body, f);
    fclose(f);
    return rc >= 0;
}

typedef struct LoadedPreferences {
    int chat_width;
    double font_scale;
    int disabled_host_extension_count;
    char first_disabled_host_extension[PICO_DISABLED_EXT_NAME];
} LoadedPreferences;

static int LoadPreferences(const char *json_body, const char *workspace_json_body, LoadedPreferences *out)
{
    char temp[] = "/tmp/pico-chat-width-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create isolated settings directory");
    }

    if (json_body)
    {
        char dir[4096];
        char path[4096];
        snprintf(dir, sizeof(dir), "%s/pico", temp);
        if (mkdir(dir, 0755) != 0 ||
            snprintf(path, sizeof(path), "%s/settings.json", dir) < 0 ||
            !WriteFile(path, json_body))
        {
            CleanupDir(temp);
            return Fail("could not write settings.json");
        }
    }
    if (workspace_json_body)
    {
        char dir[4096];
        char path[4096];
        snprintf(dir, sizeof(dir), "%s/.pico", temp);
        if (mkdir(dir, 0755) != 0 ||
            snprintf(path, sizeof(path), "%s/settings.json", dir) < 0 ||
            !WriteFile(path, workspace_json_body))
        {
            CleanupDir(temp);
            return Fail("could not write workspace settings.json");
        }
    }

    PicoHost app;
    memset(&app, 0, sizeof(app));
    PicoHost_SetPath(&app, temp);
    setenv("XDG_CONFIG_HOME", temp, 1);
    PicoHostPreferences_Load(&app);
    out->chat_width = app.preferences.chat_width;
    out->font_scale = app.preferences.font_scale;
    out->disabled_host_extension_count = app.preferences.disabled_host_extension_count;
    snprintf(out->first_disabled_host_extension, sizeof(out->first_disabled_host_extension), "%s",
             app.preferences.disabled_host_extension_count > 0
                 ? app.preferences.disabled_host_extensions[0]
                 : "");
    unsetenv("XDG_CONFIG_HOME");
    CleanupDir(temp);
    return 0;
}

static int LoadChatWidth(const char *json_body, const char *workspace_json_body, int *out)
{
    LoadedPreferences preferences;
    int rc = LoadPreferences(json_body, workspace_json_body, &preferences);
    if (rc == 0)
    {
        *out = preferences.chat_width;
    }
    return rc;
}

static int TestValidOverride(void)
{
    int width = -1;
    int rc = LoadChatWidth("{ \"chat_width\": 100 }\n", NULL, &width);
    if (rc != 0)
    {
        return rc;
    }
    return width == 100 ? 0 : Fail("valid override was not applied");
}

static int TestUnlimited(void)
{
    int width = -1;
    int rc = LoadChatWidth("{ \"chat_width\": 0 }\n", NULL, &width);
    if (rc != 0)
    {
        return rc;
    }
    if (width != 0)
    {
        return Fail("explicit 0 is not unlimited");
    }
    return 0;
}

static int TestEnvironmentOverride(void)
{
    int width = -1;
    setenv("PICO_CHAT_WIDTH", "120", 1);
    int rc = LoadChatWidth("{ \"chat_width\": 100 }\n", NULL, &width);
    unsetenv("PICO_CHAT_WIDTH");
    if (rc != 0)
    {
        return rc;
    }
    return width == 120 ? 0 : Fail("environment did not override settings.json chat width");
}

static int TestWorkspaceOverrideIgnored(void)
{
    LoadedPreferences preferences;
    int rc = LoadPreferences("{ \"chat_width\": 100, \"font_scale\": 1.5, "
                             "\"disabled_host_extensions\": [\"composer\"] }\n",
                             "{ \"chat_width\": 40, \"font_scale\": 2.0, "
                             "\"disabled_host_extensions\": [\"footer\"] }\n", &preferences);
    if (rc != 0)
    {
        return rc;
    }
    return preferences.chat_width == 100 && preferences.font_scale == 1.5 &&
                   preferences.disabled_host_extension_count == 1 &&
                   strcmp(preferences.first_disabled_host_extension, "composer") == 0
               ? 0
               : Fail("workspace settings overrode host-wide settings");
}

static int TestInvalidIgnored(void)
{
    int default_width = -1;
    int rc = LoadChatWidth(NULL, NULL, &default_width);
    if (rc != 0)
    {
        return rc;
    }

    int width = -1;
    rc = LoadChatWidth("{ \"chat_width\": 300 }\n", NULL, &width);
    if (rc != 0)
    {
        return rc;
    }
    if (width != default_width)
    {
        return Fail("out-of-range value did not keep the default");
    }
    rc = LoadChatWidth("{ \"chat_width\": 90.5 }\n", NULL, &width);
    if (rc != 0)
    {
        return rc;
    }
    return width == default_width ? 0 : Fail("non-integer value did not keep the default");
}

int main(void)
{
    unsetenv("PICO_CHAT_WIDTH");
    int rc = TestClamp();
    if (rc != 0)
    {
        return rc;
    }
    rc = TestValidOverride();
    if (rc != 0)
    {
        return rc;
    }
    rc = TestUnlimited();
    if (rc != 0)
    {
        return rc;
    }
    rc = TestEnvironmentOverride();
    if (rc != 0)
    {
        return rc;
    }
    rc = TestWorkspaceOverrideIgnored();
    if (rc != 0)
    {
        return rc;
    }
    return TestInvalidIgnored();
}
