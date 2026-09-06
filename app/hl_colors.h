#ifndef PICO_HL_COLORS_H
#define PICO_HL_COLORS_H

#include "highlight.h"
#include "pico/theme.h"

/* Maps highlight classes to theme colors. Shared by md_view.c (code blocks)
 * and builtins/diff.c (diff viewer). Diff classes reuse the diff text colors
 * so fenced diff blocks match the diff viewer. */
static inline Clay_Color PicoHlClassColor(PicoHlClass class)
{
    switch (class)
    {
        case PICO_HL_KEYWORD: return COLOR_HL_KEYWORD;
        case PICO_HL_TYPE: return COLOR_HL_TYPE;
        case PICO_HL_STRING: return COLOR_HL_STRING;
        case PICO_HL_NUMBER: return COLOR_HL_NUMBER;
        case PICO_HL_COMMENT: return COLOR_HL_COMMENT;
        case PICO_HL_PREPROC: return COLOR_HL_PREPROC;
        case PICO_HL_FUNC: return COLOR_HL_FUNC;
        case PICO_HL_FIELD: return COLOR_HL_FIELD;
        case PICO_HL_VAR: return COLOR_HL_VAR;
        case PICO_HL_ADD: return COLOR_DIFF_ADD_TEXT;
        case PICO_HL_DEL: return COLOR_DIFF_DEL_TEXT;
        case PICO_HL_HUNK: return COLOR_HL_HUNK;
        case PICO_HL_NORMAL: break;
    }
    return COLOR_CODE_TEXT;
}

#endif
