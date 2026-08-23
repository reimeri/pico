#define _POSIX_C_SOURCE 200809L

#include "settings.h"
#include "theme_internal.h"

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

static int TestNonFiniteSetting(void)
{
    char temp[] = "/tmp/pico-font-scale-XXXXXX";
    if (!mkdtemp(temp))
    {
        return Fail("could not create isolated settings directory");
    }

    PicoApp app;
    memset(&app, 0, sizeof(app));
    snprintf(app.workspace, sizeof(app.workspace), "%s", temp);
    setenv("XDG_CONFIG_HOME", temp, 1);
    setenv("PICO_FONT_SCALE", "2.0", 1);
    PicoSettings_Load(&app);
    int valid_failed = app.settings.font_scale != 2.0 || Pico_FontScale() != 2.0f;

    setenv("PICO_FONT_SCALE", "nan", 1);
    PicoSettings_Load(&app);
    int non_finite_failed = app.settings.font_scale != 1.0 || Pico_FontScale() != 1.0f;

    free(app.models);
    unsetenv("PICO_FONT_SCALE");
    unsetenv("XDG_CONFIG_HOME");
    char config_dir[4096];
    snprintf(config_dir, sizeof(config_dir), "%s/pico", temp);
    rmdir(config_dir);
    rmdir(temp);

    if (valid_failed)
    {
        return Fail("valid setting was not applied");
    }
    return non_finite_failed ? Fail("non-finite setting preserved the previous scale") : 0;
}

int main(void)
{
    int rc = TestScaleHelpers();
    return rc != 0 ? rc : TestNonFiniteSetting();
}
