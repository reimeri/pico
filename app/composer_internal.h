#ifndef PICO_COMPOSER_INTERNAL_H
#define PICO_COMPOSER_INTERNAL_H

#include "pico/app.h"

bool PicoComposer_HasAttachments(const PicoHost *app);
bool PicoComposer_PointerOverAttachments(void);
bool PicoComposer_PointerOverAttachmentRemove(void);
bool PicoComposer_ApplyAttachments(PicoHost *app);
void PicoComposer_ReleaseAttachments(void);
void PicoComposer_DiscardAttachments(void);
char *pico_composer_display_message(const char *text);

/* Focused test seams for the builtin attachment model. */
bool pico_composer_attach_path(const char *path, bool owned);
bool pico_composer_remove_at(int index);
int pico_composer_attachment_count(void);
char *pico_composer_merge_parts(const char *text, const char *existing_parts);
bool pico_composer_submit_ready(const char *text, int length);

#if defined(__linux__)
void PicoComposer_BeginClipboardPaste(PicoHost *app);
void PicoComposer_PumpClipboardPaste(PicoHost *app);
void PicoComposer_CancelClipboardPaste(void);
bool PicoComposer_ClipboardPasteBusy(void);
#endif

#endif
