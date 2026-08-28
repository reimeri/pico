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

static int LoadChatWidth(const char *json_body, int *out)
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

    PicoHost app;
    memset(&app, 0, sizeof(app));
    PicoHost_SetPath(&app, temp);
    setenv("XDG_CONFIG_HOME", temp, 1);
    PicoSettings_Load(&app);
    *out = app.settings.chat_width;
    free(app.models);
    unsetenv("XDG_CONFIG_HOME");
    CleanupDir(temp);
    return 0;
}

static int TestDefault(void)
{
    int width = -1;
    int rc = LoadChatWidth(NULL, &width);
    if (rc != 0)
    {
        return rc;
    }
    return width == 75 ? 0 : Fail("default is not 75");
}

static int TestValidOverride(void)
{
    int width = -1;
    int rc = LoadChatWidth("{ \"chat_width\": 100 }\n", &width);
    if (rc != 0)
    {
        return rc;
    }
    return width == 100 ? 0 : Fail("valid override was not applied");
}

static int TestUnlimited(void)
{
    int width = -1;
    int rc = LoadChatWidth("{ \"chat_width\": 0 }\n", &width);
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

static int TestInvalidIgnored(void)
{
    int width = -1;
    int rc = LoadChatWidth("{ \"chat_width\": 300 }\n", &width);
    if (rc != 0)
    {
        return rc;
    }
    if (width != 75)
    {
        return Fail("out-of-range value did not keep the default");
    }
    rc = LoadChatWidth("{ \"chat_width\": 75.5 }\n", &width);
    if (rc != 0)
    {
        return rc;
    }
    return width == 75 ? 0 : Fail("non-integer value did not keep the default");
}

int main(void)
{
    int rc = TestClamp();
    if (rc != 0)
    {
        return rc;
    }
    rc = TestDefault();
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
    return TestInvalidIgnored();
}
