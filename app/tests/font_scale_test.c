#define _POSIX_C_SOURCE 200809L

#include "settings.h"
#include "theme_internal.h"
#include "host_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int Fail(const char *message)
{
    fprintf(stderr, "font scale: %s\n", message);
    return 1;
}

static int TestScaleHelpers(void)
{
    if (Pico_FontScale() != 1.0f || Pico_FontPxU16(16) != 16 || Pico_FontPx(16) != 16.0f)
    {
        return Fail("default scale is not identity");
    }

    Pico_SetFontScale(1.25f);
    if (Pico_FontScale() != 1.25f || Pico_FontPxU16(16) != 20 || Pico_FontPx(16) != 20.0f)
    {
        return Fail("1.25 does not scale design 16 to 20");
    }

    Pico_SetFontScale(1.5f);
    if (Pico_FontPxU16(13) != 20 || Pico_FontPx(13) != 20.0f)
    {
        return Fail("1.5 does not round design 13 to 20 for atlas and draw");
    }

    Pico_SetFontScale(3.0f);
    if (Pico_FontPxU16(64) != 192)
    {
        return Fail("scaled layout dimension was clamped to the font atlas limit");
    }

    Pico_SetFontScale(1.25f);
    Pico_SetFontScale(0.0f);
    Pico_SetFontScale(4.0f);
    Pico_SetFontScale(-1.0f);
    if (Pico_FontScale() != 1.25f || Pico_FontPxU16(16) != 20)
    {
        return Fail("out-of-range scale was not ignored");
    }

    Pico_SetFontScale(1.0f);
    if (Pico_FontPxU16(16) != 16)
    {
        return Fail("reset to 1.0 did not restore identity");
    }
    return 0;
}

static int TestSettingsAndEnvironment(void)
{
    char temp[] = "/tmp/pico-font-scale-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create isolated settings directory");
    }

    char config_dir[4096];
    char settings_path[4096];
    snprintf(config_dir, sizeof(config_dir), "%s/pico", temp);
    snprintf(settings_path, sizeof(settings_path), "%s/pico/settings.json", temp);
    Pico_MkdirP(config_dir);
    FILE *settings = fopen(settings_path, "w");
    if (!settings)
    {
        rmdir(config_dir);
        rmdir(temp);
        return Fail("could not write settings.json");
    }
    fputs("{ \"font_scale\": 1.5 }\n", settings);
    fclose(settings);

    PicoHost app;
    memset(&app, 0, sizeof(app));
    PicoHost_SetPath(&app, temp);
    setenv("XDG_CONFIG_HOME", temp, 1);
    unsetenv("PICO_FONT_SCALE");
    PicoHostPreferences_Load(&app);
    int file_failed = app.preferences.font_scale != 1.5 || Pico_FontScale() != 1.5f;

    setenv("PICO_FONT_SCALE", "2.0", 1);
    PicoHostPreferences_Load(&app);
    int valid_failed = app.preferences.font_scale != 2.0 || Pico_FontScale() != 2.0f;

    setenv("PICO_FONT_SCALE", "nan", 1);
    PicoHostPreferences_Load(&app);
    int non_finite_failed = app.preferences.font_scale != 1.5 || Pico_FontScale() != 1.5f;

    unsetenv("PICO_FONT_SCALE");
    unsetenv("XDG_CONFIG_HOME");
    unlink(settings_path);
    rmdir(config_dir);
    rmdir(temp);

    if (file_failed)
    {
        return Fail("settings.json font_scale was not applied");
    }
    if (valid_failed)
    {
        return Fail("valid environment override was not applied");
    }
    return non_finite_failed ? Fail("non-finite override did not keep the file setting") : 0;
}

int main(void)
{
    int rc = TestScaleHelpers();
    return rc != 0 ? rc : TestSettingsAndEnvironment();
}
