#ifndef PICO_SETTINGS_CONTROLS_H
#define PICO_SETTINGS_CONTROLS_H

/* Private presentation shared by the Settings sections. */
#include "text_field_ui.h"

#include <math.h>
#include <string.h>

enum { SETTINGS_BUTTON_PAD_X = 14 };

#define SETTINGS_ROW_BEGIN                                                                                             \
    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,                                                    \
                             .childGap = 4,                                                                            \
                             .padding = {.bottom = 12},                                                                \
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})                                               \
    {

static inline Clay_String PicoSettingsControl_String(const char *text)
{
    return (Clay_String){.chars = text ? text : "", .length = text ? (int32_t)strlen(text) : 0};
}

static inline void PicoSettingsControl_Label(const char *label)
{
    CLAY_TEXT(PicoSettingsControl_String(label),
              CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_CAPTION,
                                .textColor = COLOR_MUTED, .wrapMode = CLAY_TEXT_WRAP_NONE}));
}

static inline void PicoSettingsControl_Field(Clay_ElementId id, const char *value, const char *placeholder,
                                              const PicoTextField *field, bool focused)
{
    bool hover = Clay_PointerOver(id);
    Clay_Color bg = focused ? (Clay_Color){54, 54, 66, 255} : (hover ? COLOR_CODE_BG : COLOR_COMPOSER_BG);
    CLAY(id, {.layout = {.padding = {8, 8, 6, 6}, .sizing = {.width = CLAY_SIZING_GROW(0)}},
              .clip = {.horizontal = true, .childOffset = {.x = focused && field ? -field->scroll_x : 0.0f}},
              .backgroundColor = bg,
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        Clay_TextElementConfig config = PicoTextField_Config(FONT_REGULAR);
        if (!value || !value[0]) config.textColor = COLOR_MUTED;
        CLAY_TEXT(PicoSettingsControl_String(value && value[0] ? value : placeholder), CLAY_TEXT_CONFIG(config));
    }
}

static inline Clay_TextElementConfig PicoSettingsControl_ButtonText(Clay_Color color)
{
    return (Clay_TextElementConfig){.fontId = FONT_BOLD, .fontSize = PICO_FONT_UI,
                                    .textColor = color, .wrapMode = CLAY_TEXT_WRAP_NONE};
}

static inline float PicoSettingsControl_ButtonWidth(const char *label)
{
    Clay_TextElementConfig config = PicoSettingsControl_ButtonText(COLOR_TEXT);
    return Pico_MeasureTextUtf8((Clay_StringSlice){.chars = label, .length = (int32_t)strlen(label)},
                               &config, NULL).width + 2.0f * SETTINGS_BUTTON_PAD_X;
}

static inline void PicoSettingsControl_Button(Clay_ElementId id, const char *label, bool enabled,
                                               bool primary, bool saving)
{
    bool hover = enabled && Clay_PointerOver(id);
    Clay_Color bg = primary ? (Clay_Color){74, 104, 180, 255} : COLOR_FOOTER_BG;
    Clay_Color text = enabled ? COLOR_TEXT : COLOR_MUTED;
    if (!enabled) bg = (Clay_Color){38, 38, 44, 255};
    else if (hover) bg = primary ? (Clay_Color){92, 126, 210, 255} : COLOR_CODE_BG;
    if (saving)
    {
        float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 5.0f);
        bg = (Clay_Color){54 + 28 * pulse, 74 + 32 * pulse, 126 + 54 * pulse, 255};
        text = COLOR_TEXT;
    }
    CLAY(id, {.layout = {.padding = {SETTINGS_BUTTON_PAD_X, SETTINGS_BUTTON_PAD_X, 8, 8}}, .backgroundColor = bg, .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        Clay_TextElementConfig config = PicoSettingsControl_ButtonText(text);
        CLAY_TEXT(PicoSettingsControl_String(label), CLAY_TEXT_CONFIG(config));
    }
}

static inline void PicoSettingsControl_Toggle(Clay_ElementId id, const char *label, bool on)
{
    bool hover = Clay_PointerOver(id);
    CLAY(id, {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                         .childGap = 8,
                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                         .padding = {8, 8, 6, 6},
                         .sizing = {.width = CLAY_SIZING_GROW(0)}},
              .backgroundColor = hover ? COLOR_CODE_BG : COLOR_COMPOSER_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
    {
        CLAY_TEXT(PicoSettingsControl_String(label),
                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI,
                                    .textColor = COLOR_TEXT, .wrapMode = CLAY_TEXT_WRAP_NONE}));
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {}
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(10), .height = CLAY_SIZING_FIXED(10)}},
                      .backgroundColor = on ? COLOR_STATUS_ON : COLOR_STATUS_OFF,
                      .cornerRadius = CLAY_CORNER_RADIUS(5)}) {}
    }
}

#endif
