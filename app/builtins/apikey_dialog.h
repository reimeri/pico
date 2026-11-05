#ifndef PICO_APIKEY_DIALOG_H
#define PICO_APIKEY_DIALOG_H

/* Private UI owned by the commands builtin; secrets never enter the composer. */
#include "auth.h"
#include "text_field_ui.h"

#define APIKEY_MODAL "apikey"
typedef struct ApiKeyDialog {
    PicoAgentId agent_id;
    char provider[16];
    char title[64];
    char key[4096];
    char mask[4096];
    PicoTextField field;
    bool opening;
    const char *error;
} ApiKeyDialog;

static void ApiKeyClose(PicoHost *host, ApiKeyDialog *d)
{
    pico_ui_modal_pop(host, APIKEY_MODAL);
    PicoTextField_Unbind(&d->field);
    /* Do not leave a cancelled/submitted key in the reusable dialog buffer. */
    volatile unsigned char *p = (volatile unsigned char *)d;
    for (size_t i = 0; i < sizeof(*d); i++) p[i] = 0;
}

static PicoTextField ApiKeyMaskedField(ApiKeyDialog *d)
{
    memset(d->mask, '*', (size_t)d->field.length);
    d->mask[d->field.length] = '\0';
    PicoTextField view = d->field;
    view.text = d->mask;
    return view;
}

static void ApiKeySave(PicoHost *host, ApiKeyDialog *d)
{
    if (!d->key[0]) { d->error = "Enter an API key."; return; }
    for (const unsigned char *p = (unsigned char *)d->key; *p; p++)
    {
        if (*p <= 32 || *p == 127)
        {
            d->error = "API keys cannot contain whitespace or control characters.";
            return;
        }
    }
    bool overridden = false;
    bool saved = PicoAuth_SetApiKey(host, d->provider, d->key, &overridden);
    char message[512];
    snprintf(message, sizeof(message),
             saved ? "Saved %s API key. Authentication selection is unchanged. Use `/login %s key` to select API-key authentication.%s"
                   : "Could not persist %s API key to auth.json; it may be available only until restart. Authentication selection is unchanged. Retry `/apikey %s` after fixing file permissions.%s",
             d->provider, d->provider,
             overridden ? " An environment API key overrides this saved key; unset it and restart Pico to use the saved key." : "");
    PicoHost_AddNotice(host, d->agent_id, saved && !overridden ? PICO_NOTICE_INFO : PICO_NOTICE_WARNING, message);
    ApiKeyClose(host, d);
}

static void ApiKeyFrame(PicoHost *host, ApiKeyDialog *d)
{
    if (!pico_ui_modal_is_top(host, APIKEY_MODAL)) return;
    if (d->opening) { d->opening = false; return; }
    if (IsKeyPressed(KEY_ESCAPE)) { ApiKeyClose(host, d); return; }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) { ApiKeySave(host, d); return; }
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && Pico_ShortcutPressed('v'))
    {
        const char *clip = GetClipboardText();
        if (!clip) return;
        size_t length = strlen(clip);
        int retained = d->field.length - (PicoTextField_SelTo(&d->field) - PicoTextField_SelFrom(&d->field));
        if (length >= sizeof(d->key) - (size_t)retained)
        {
            d->error = "Key is too long; nothing was pasted.";
            return;
        }
        PicoTextField_Insert(&d->field, clip, (int)length);
        d->field.blink_at = GetTime();
        d->error = NULL;
        return;
    }
    /* Keep paste and normal editing, but never export the secret to clipboard. */
    if (!(ctrl && (Pico_ShortcutPressed('c') || Pico_ShortcutPressed('x'))))
        PicoTextField_HandleKeys(&d->field);
}

static void ApiKeyRender(PicoHost *host, ApiKeyDialog *d)
{
    if (!pico_ui_modal_is_top(host, APIKEY_MODAL)) return;
    float width = (float)GetScreenWidth();
    float height = (float)GetScreenHeight();
    float card_width = width < 540 ? width : 540;
    PicoTextField view = ApiKeyMaskedField(d);
    PicoTextField_KeepCaretVisible(&view, card_width - 64, FONT_MONO);
    d->field.scroll_x = view.scroll_x;
    CLAY(CLAY_ID("ApiKeyDim"), {
        .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 90,
                     .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP}},
        .layout = {.sizing = {.width = CLAY_SIZING_FIXED(width), .height = CLAY_SIZING_FIXED(height)},
                   .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
        .backgroundColor = {0, 0, 0, 160}})
    {
        CLAY(CLAY_ID("ApiKeyCard"), {
            .layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .padding = CLAY_PADDING_ALL(20), .childGap = 12,
                       .sizing = {.width = CLAY_SIZING_FIXED(card_width)}},
            .backgroundColor = COLOR_CONTENT_BG, .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            Clay_String title = {.chars = d->title, .length = (int32_t)strlen(d->title)};
            CLAY_TEXT(title, CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = PICO_FONT_TITLE, .textColor = COLOR_TEXT}));
            CLAY_TEXT(CLAY_STRING("Save a new key without changing authentication. Stored as plaintext in your private auth.json. Saving replaces the existing key."),
                      CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI, .textColor = COLOR_MUTED}));
            CLAY(CLAY_ID("ApiKeyField"), {
                .layout = {.padding = {12, 12, 10, 10}, .sizing = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(44)}},
                .clip = {.horizontal = true, .childOffset = {-d->field.scroll_x, 0}},
                .backgroundColor = {0, 0, 0, 60}, .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                Clay_String masked = {.chars = d->mask, .length = d->field.length};
                CLAY_TEXT(masked, CLAY_TEXT_CONFIG({.fontId = FONT_MONO, .fontSize = PICO_FONT_UI, .textColor = COLOR_TEXT, .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
            if (d->error)
            {
                Clay_String error = {.chars = d->error, .length = (int32_t)strlen(d->error)};
                CLAY_TEXT(error, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI, .textColor = COLOR_TEXT}));
            }
            CLAY(CLAY_ID("ApiKeyActions"), {.layout = {.childGap = 16}})
            {
                CLAY(CLAY_ID("ApiKeySave"), {.layout = {.padding = {16,16,8,8}}, .backgroundColor = {70,90,120,255}, .cornerRadius = CLAY_CORNER_RADIUS(4)})
                { CLAY_TEXT(CLAY_STRING("Save (Enter)"), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI, .textColor = COLOR_TEXT})); }
                CLAY(CLAY_ID("ApiKeyCancel"), {.layout = {.padding = {16,16,8,8}}})
                { CLAY_TEXT(CLAY_STRING("Cancel (Esc)"), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI, .textColor = COLOR_TEXT})); }
            }
        }
    }
}

static void ApiKeyAfterLayout(PicoHost *host, ApiKeyDialog *d)
{
    if (!pico_ui_modal_is_top(host, APIKEY_MODAL)) return;
    PicoTextField view = ApiKeyMaskedField(d);
    PicoTextField_HandlePointer(&view, CLAY_ID("ApiKeyField"), 12, FONT_MONO);
    /* Pointer editing uses masked glyph geometry, not secret glyph widths. */
    char *key = d->field.text;
    d->field = view;
    d->field.text = key;
    bool save = Clay_PointerOver(CLAY_ID("ApiKeySave"));
    bool cancel = Clay_PointerOver(CLAY_ID("ApiKeyCancel"));
    if (save || cancel) pico_host_set_hovered_clickable(host);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        if (save) ApiKeySave(host, d);
        else if (cancel) ApiKeyClose(host, d);
    }
}

static void ApiKeyAfterRender(PicoHost *host, ApiKeyDialog *d)
{
    if (!pico_ui_modal_is_top(host, APIKEY_MODAL)) return;
    PicoTextField view = ApiKeyMaskedField(d);
    PicoTextField_Draw(&view, CLAY_ID("ApiKeyField"), 12, 10, FONT_MONO);
    pico_host_request_redraw_after(host, PicoTextField_NextBlink(&d->field));
}
#endif
