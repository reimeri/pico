#ifndef PICO_IMAGE_SETTINGS_H
#define PICO_IMAGE_SETTINGS_H

#include <stdbool.h>

typedef struct PicoImageSettings {
    bool use_api;
    char codex_quality[16];
    char generation_model[128];
    char editing_model[128];
    char generation_quality[16];
    char editing_quality[16];
} PicoImageSettings;

static inline PicoImageSettings PicoImageSettings_Default(void)
{
    return (PicoImageSettings){.codex_quality = "auto",
        .generation_model = "gpt-image-2.6-sunburst", .editing_model = "gpt-image-2.6-sunburst",
        .generation_quality = "auto", .editing_quality = "auto"};
}

struct PicoAgentContext;
/* Private builtin worker helper: copies settings under the host settings lock. */
PicoImageSettings PicoImageSettings_CopyContext(const struct PicoAgentContext *ctx);
void PicoImageSettings_Read(PicoImageSettings *out);
#endif
