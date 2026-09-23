#ifndef PICO_TEXT_FIELD_UI_H
#define PICO_TEXT_FIELD_UI_H

/* Raylib/Clay glue for PicoTextField: keyboard shortcuts (composer
 * parity), mouse caret placement and selection, horizontal scroll to
 * keep the caret visible, and caret/selection drawing. Callers keep
 * ownership of Enter/Escape/Tab semantics and focus routing. */

#include "text_field.h"

#include "pico/theme.h"

/* Text config matching the fields (PICO_FONT_UI, no wrap). Pass
 * FONT_REGULAR or FONT_MONO. Use for the CLAY_TEXT of the value too so
 * measurement and rendering always agree. */
Clay_TextElementConfig PicoTextField_Config(uint16_t font_id);

/* Handles arrows (+Ctrl word jump, +Shift extend, repeat), Home/End,
 * Backspace/Delete (+Ctrl word), Ctrl+W, Ctrl+A/C/X/V, and typed text.
 * Call only while the field is focused; handle Enter/Escape first.
 * Returns true if any input was consumed. */
bool PicoTextField_HandleKeys(PicoTextField *f);

/* Press over the element places the caret (double-click selects the
 * word), dragging extends the selection. Call every frame while the
 * field is shown. Returns true while the pointer is over the element. */
bool PicoTextField_HandlePointer(PicoTextField *f, Clay_ElementId id, float pad_x, uint16_t font_id);

/* Adjusts scroll_x so the caret stays inside a view of view_width. */
void PicoTextField_KeepCaretVisible(PicoTextField *f, float view_width, uint16_t font_id);

/* Draws the selection highlight and blinking caret scissored to the
 * element's bounding box. Call from an after-render hook. */
void PicoTextField_Draw(const PicoTextField *f, Clay_ElementId id, float pad_x, float pad_y,
                        uint16_t font_id);

/* Text width of text[0, n) with the field font; exposed for callers
 * that position companions (e.g. error popovers) relative to text. */
float PicoTextField_MeasureText(const PicoTextField *f, int n, uint16_t font_id);

#endif
