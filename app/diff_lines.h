#ifndef PICO_DIFF_LINES_H
#define PICO_DIFF_LINES_H

#include <stdbool.h>

typedef enum PicoDiffOp {
    PICO_DIFF_CTX = 0,
    PICO_DIFF_ADD,
    PICO_DIFF_DEL,
} PicoDiffOp;

typedef struct PicoDiffLine {
    PicoDiffOp op;
    const char *text; /* borrowed from the input texts; newline excluded */
    int len;
} PicoDiffLine;

typedef struct PicoDiffLines {
    PicoDiffLine *lines;
    int count;
    int cap;
} PicoDiffLines;

/* Myers O(ND) shortest edit script over lines. Output borrows from old_text
 * and new_text; both must outlive `out`. Lines are emitted in order with the
 * minimal number of edits; false on allocation failure. */
bool PicoDiff_Lines(const char *old_text, const char *new_text, PicoDiffLines *out);
void PicoDiff_LinesFree(PicoDiffLines *d);

#endif
