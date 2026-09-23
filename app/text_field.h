#ifndef PICO_TEXT_FIELD_H
#define PICO_TEXT_FIELD_H

/* Shared single-line text field: cursor/selection editing core
 * (text_field.c, no raylib) plus raylib/Clay glue for keys, pointer,
 * and caret/selection drawing (text_field_ui.c).
 *
 * Storage is either caller-owned fixed capacity (PicoTextField_Bind)
 * or helper-owned growable (PicoTextField_BindGrowable, freed with
 * PicoTextField_Free). All offsets are byte offsets into text. */

#include "text_range.h"

#include <stdbool.h>

typedef struct PicoTextField {
    char *text;
    int capacity; /* includes the NUL */
    int length;
    int cursor;
    int anchor;
    bool owns_text;
    bool bound;
    float scroll_x;
    PicoClickSeq click_seq;
    int granularity; /* click sequence unit: 1 caret, 2 word, 3 line */
    int unit_from;
    int unit_to;
    bool dragging;
    double blink_at;
    unsigned revision; /* bumped on every text mutation */
} PicoTextField;

/* --- Core (pure, no raylib) --- */

void PicoTextField_Bind(PicoTextField *f, char *buf, int capacity);
void PicoTextField_BindGrowable(PicoTextField *f);
void PicoTextField_Unbind(PicoTextField *f); /* detach; frees owned storage */
void PicoTextField_Free(PicoTextField *f);   /* PicoTextField_Unbind + zero */
void PicoTextField_SetText(PicoTextField *f, const char *s);
/* Re-syncs length/cursor/anchor after the caller wrote the bound buffer
 * directly. Keeps click sequence and scroll state. */
void PicoTextField_Refresh(PicoTextField *f);

bool PicoTextField_HasSelection(const PicoTextField *f);
int PicoTextField_SelFrom(const PicoTextField *f);
int PicoTextField_SelTo(const PicoTextField *f);

void PicoTextField_Move(PicoTextField *f, int pos, bool extend);
void PicoTextField_SelectAll(PicoTextField *f);
void PicoTextField_DeleteRange(PicoTextField *f, int from, int to);
void PicoTextField_DeleteSelection(PicoTextField *f);
bool PicoTextField_Insert(PicoTextField *f, const char *bytes, int n);
bool PicoTextField_DeleteBackward(PicoTextField *f, bool word);
bool PicoTextField_DeleteForward(PicoTextField *f, bool word);
/* Control bytes (< 32) become spaces so the field stays single-line. */
bool PicoTextField_PasteText(PicoTextField *f, const char *text);
/* Copies the selection into out (always NUL-terminated). Returns length. */
int PicoTextField_CopyText(const PicoTextField *f, char *out, int cap);

/* Click units used by the pointer glue; granularity >= 3 selects the line. */
void PicoTextField_SelectUnit(PicoTextField *f, int pos, int granularity, bool extend);
void PicoTextField_ExtendUnit(PicoTextField *f, int pos);

#endif
