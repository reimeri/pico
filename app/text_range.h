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
int PicoText_Utf8Next(const char *s, int length, int pos);
int PicoText_Utf8Prev(const char *s, int pos);
int PicoText_Utf8Encode(int cp, char out[4]);
int PicoText_PrevWord(const char *s, int pos);
int PicoText_NextWord(const char *s, int length, int pos);
void PicoText_WordRange(const char *s, int len, int pos, int *from, int *to);
void PicoText_ParaRange(const char *s, int len, int pos, int *from, int *to);
void PicoText_UnionRange(int a0, int a1, int b0, int b1, int *from, int *to);
int PicoClickSeq_Press(PicoClickSeq *seq, double now, float x, float y);
void PicoClickSeq_Reset(PicoClickSeq *seq);

#endif
