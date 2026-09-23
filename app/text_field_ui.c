#include "text_field_ui.h"

#include "pico/app.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PICO_TEXT_FIELD_BLINK_HZ 2.0

static bool CtrlDown(void)
{
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

static bool ShiftDown(void)
{
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}

static bool KeyHit(int key)
{
    return IsKeyPressed(key) || IsKeyPressedRepeat(key);
}

Clay_TextElementConfig PicoTextField_Config(uint16_t font_id)
{
    return (Clay_TextElementConfig){.fontId = font_id,
                                    .fontSize = PICO_FONT_UI,
                                    .textColor = COLOR_TEXT,
                                    .wrapMode = CLAY_TEXT_WRAP_NONE};
}

static float MeasureSlice(const PicoTextField *f, int start, int n, uint16_t font_id)
{
    if (!f->text || n <= 0)
    {
        return 0.0f;
    }
    Clay_TextElementConfig config = PicoTextField_Config(font_id);
    Clay_StringSlice slice = {.chars = f->text + start, .length = n, .baseChars = f->text};
    return Pico_MeasureTextUtf8(slice, &config, NULL).width;
}

float PicoTextField_MeasureText(const PicoTextField *f, int n, uint16_t font_id)
{
    if (!f)
    {
        return 0.0f;
    }
    if (n > f->length)
    {
        n = f->length;
    }
    return MeasureSlice(f, 0, n, font_id);
}

/* Byte offset whose caret position is nearest local_x, using midpoint
 * boundaries measured from the text start so kerning stays exact. */
static int OffsetAtX(const PicoTextField *f, float local_x, uint16_t font_id)
{
    if (local_x <= 0.0f || !f->text)
    {
        return 0;
    }
    int pos = 0;
    while (pos < f->length)
    {
        int next = PicoText_Utf8Next(f->text, f->length, pos);
        float here = MeasureSlice(f, 0, pos, font_id);
        float there = MeasureSlice(f, 0, next, font_id);
        if (local_x < (here + there) * 0.5f)
        {
            return pos;
        }
        pos = next;
    }
    return f->length;
}

static void NoteActivity(PicoTextField *f, int cursor, int anchor, unsigned revision)
{
    if (f->cursor != cursor || f->anchor != anchor || f->revision != revision)
    {
        f->blink_at = GetTime();
    }
}

bool PicoTextField_HandleKeys(PicoTextField *f)
{
    if (!f || !f->bound)
    {
        return false;
    }
    int cursor = f->cursor;
    int anchor = f->anchor;
    unsigned revision = f->revision;
    bool ctrl = CtrlDown();
    bool shift = ShiftDown();
    bool used = false;
    const char *text = f->text ? f->text : "";

    if (ctrl && Pico_ShortcutPressed('a'))
    {
        PicoTextField_SelectAll(f);
        used = true;
    }
    if (ctrl && (Pico_ShortcutPressed('c') || Pico_ShortcutPressed('x')))
    {
        used = true;
        if (PicoTextField_HasSelection(f))
        {
            int n = PicoTextField_SelTo(f) - PicoTextField_SelFrom(f);
            char *copy = (char *)malloc((size_t)n + 1);
            if (copy)
            {
                PicoTextField_CopyText(f, copy, n + 1);
                SetClipboardText(copy);
                free(copy);
            }
            if (Pico_ShortcutPressed('x'))
            {
                PicoTextField_DeleteSelection(f);
            }
        }
    }
    if (ctrl && Pico_ShortcutPressed('v'))
    {
        const char *clip = GetClipboardText();
        if (clip && clip[0])
        {
            PicoTextField_PasteText(f, clip);
        }
        used = true;
    }
    if (ctrl && Pico_ShortcutRepeat('w'))
    {
        PicoTextField_DeleteBackward(f, true);
        used = true;
    }
    else if (KeyHit(KEY_BACKSPACE))
    {
        PicoTextField_DeleteBackward(f, ctrl);
        used = true;
    }
    if (KeyHit(KEY_DELETE))
    {
        PicoTextField_DeleteForward(f, ctrl);
        used = true;
    }
    if (KeyHit(KEY_LEFT))
    {
        /* An unshifted arrow collapses the selection to its edge first. */
        int pos;
        if (!shift && PicoTextField_HasSelection(f))
        {
            pos = PicoTextField_SelFrom(f);
        }
        else
        {
            pos = ctrl ? PicoText_PrevWord(text, f->cursor) : PicoText_Utf8Prev(text, f->cursor);
        }
        PicoTextField_Move(f, pos, shift);
        used = true;
    }
    if (KeyHit(KEY_RIGHT))
    {
        int pos;
        if (!shift && PicoTextField_HasSelection(f))
        {
            pos = PicoTextField_SelTo(f);
        }
        else
        {
            pos = ctrl ? PicoText_NextWord(text, f->length, f->cursor)
                       : PicoText_Utf8Next(text, f->length, f->cursor);
        }
        PicoTextField_Move(f, pos, shift);
        used = true;
    }
    if (IsKeyPressed(KEY_HOME))
    {
        PicoTextField_Move(f, 0, shift);
        used = true;
    }
    if (IsKeyPressed(KEY_END))
    {
        PicoTextField_Move(f, f->length, shift);
        used = true;
    }
    if (!ctrl)
    {
        int cp;
        while ((cp = GetCharPressed()) != 0)
        {
            if (cp < 32 || cp == 127)
            {
                continue;
            }
            char bytes[4];
            int n = PicoText_Utf8Encode(cp, bytes);
            if (n > 0)
            {
                PicoTextField_Insert(f, bytes, n);
                used = true;
            }
        }
    }
    NoteActivity(f, cursor, anchor, revision);
    return used;
}

static bool PointerOverBox(Clay_ElementId id, Clay_BoundingBox *out)
{
    Clay_ElementData element = Clay_GetElementData(id);
    if (!element.found)
    {
        return false;
    }
    Vector2 point = GetMousePosition();
    Clay_BoundingBox box = element.boundingBox;
    if (out)
    {
        *out = box;
    }
    return point.x >= box.x && point.y >= box.y && point.x < box.x + box.width &&
           point.y < box.y + box.height;
}

bool PicoTextField_HandlePointer(PicoTextField *f, Clay_ElementId id, float pad_x, uint16_t font_id)
{
    if (!f || !f->bound)
    {
        return false;
    }
    Clay_BoundingBox box = {0};
    bool over = PointerOverBox(id, &box);
    Vector2 mouse = GetMousePosition();
    int cursor = f->cursor;
    int anchor = f->anchor;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && over)
    {
        float local_x = mouse.x - box.x - pad_x + f->scroll_x;
        int pos = OffsetAtX(f, local_x, font_id);
        int count = PicoClickSeq_Press(&f->click_seq, GetTime(), mouse.x, mouse.y);
        PicoTextField_SelectUnit(f, pos, count, ShiftDown());
        f->dragging = true;
        f->blink_at = GetTime();
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        f->dragging = false;
    }
    else if (f->dragging)
    {
        float local_x = mouse.x - box.x - pad_x + f->scroll_x;
        PicoTextField_ExtendUnit(f, OffsetAtX(f, local_x, font_id));
    }
    NoteActivity(f, cursor, anchor, f->revision);
    return over;
}

void PicoTextField_KeepCaretVisible(PicoTextField *f, float view_width, uint16_t font_id)
{
    if (!f || !f->bound)
    {
        return;
    }
    if (view_width < 1.0f)
    {
        view_width = 1.0f;
    }
    float caret = PicoTextField_MeasureText(f, f->cursor, font_id);
    if (caret < f->scroll_x)
    {
        f->scroll_x = caret;
    }
    if (caret > f->scroll_x + view_width)
    {
        f->scroll_x = caret - view_width;
    }
    float full = PicoTextField_MeasureText(f, f->length, font_id);
    if (f->scroll_x > full)
    {
        f->scroll_x = full;
    }
    if (f->scroll_x < 0.0f)
    {
        f->scroll_x = 0.0f;
    }
}

void PicoTextField_Draw(const PicoTextField *f, Clay_ElementId id, float pad_x, float pad_y,
                        uint16_t font_id)
{
    if (!f || !f->bound)
    {
        return;
    }
    Clay_ElementData element = Clay_GetElementData(id);
    if (!element.found)
    {
        return;
    }
    Clay_BoundingBox box = element.boundingBox;
    float inner_h = box.height - 2.0f * pad_y;
    float font_px = Pico_FontPx(PICO_FONT_UI);
    if (inner_h < 1.0f)
    {
        inner_h = font_px;
    }
    float origin_x = box.x + pad_x - f->scroll_x;
    float origin_y = box.y + pad_y;
    BeginScissorMode((int)box.x, (int)box.y, (int)box.width, (int)box.height);
    if (PicoTextField_HasSelection(f))
    {
        float x0 = MeasureSlice(f, 0, PicoTextField_SelFrom(f), font_id);
        float x1 = MeasureSlice(f, 0, PicoTextField_SelTo(f), font_id);
        Color fill = {(unsigned char)COLOR_SELECTION.r, (unsigned char)COLOR_SELECTION.g,
                      (unsigned char)COLOR_SELECTION.b, (unsigned char)COLOR_SELECTION.a};
        DrawRectangle((int)(origin_x + x0), (int)origin_y,
                      (int)(x1 - x0 < 2.0f ? 2.0f : x1 - x0), (int)inner_h, fill);
    }
    double elapsed = GetTime() - f->blink_at;
    if (elapsed < 0.0)
    {
        elapsed = 0.0;
    }
    if (((int)(elapsed * PICO_TEXT_FIELD_BLINK_HZ) & 1) == 0)
    {
        float x = origin_x + MeasureSlice(f, 0, f->cursor, font_id);
        Color caret = {(unsigned char)COLOR_CURSOR.r, (unsigned char)COLOR_CURSOR.g,
                       (unsigned char)COLOR_CURSOR.b, 255};
        DrawRectangle((int)x, (int)origin_y, 2, (int)inner_h, caret);
    }
    EndScissorMode();
}
