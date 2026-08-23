#ifndef PICO_TEXT_RANGE_H
#define PICO_TEXT_RANGE_H

#include <stdbool.h>

typedef struct PicoClickSeq {
    double last_time;
    float last_x;
    float last_y;
    int count;
} PicoClickSeq;

bool PicoText_IsWordByte(unsigned char c);
void PicoText_WordRange(const char *s, int len, int pos, int *from, int *to);
void PicoText_ParaRange(const char *s, int len, int pos, int *from, int *to);
void PicoText_UnionRange(int a0, int a1, int b0, int b1, int *from, int *to);
int PicoClickSeq_Press(PicoClickSeq *seq, double now, float x, float y);
void PicoClickSeq_Reset(PicoClickSeq *seq);

#endif
