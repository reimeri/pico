#ifndef PICO_DIFF_UI_H
#define PICO_DIFF_UI_H
#include "pico/app.h"
/* Private dock integration; review state belongs to the workspace. */
void PicoDiffUi_Layout(PicoHost *host);
void PicoDiffUi_Open(PicoHost *host);
void PicoDiffUi_Close(PicoHost *host);
float PicoDiffUi_ShellWidth(const PicoHost *host);
bool PicoDiffUi_AfterLayout(PicoHost *host);
void PicoDiffUi_Input(PicoHost *host);
bool PicoDiffUi_BlocksKeyboard(const PicoHost *host);
bool PicoDiffUi_BlocksPointer(const PicoHost *host);
bool PicoDiffUi_ScrollHovered(PicoHost *host, Clay_Vector2 wheel);
bool PicoDiffUi_ResizeHovered(const PicoHost *host);
#endif
