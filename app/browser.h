#ifndef PICO_BROWSER_H
#define PICO_BROWSER_H
#include "pico/plugin.h"
#include <time.h>
/* Core-owned, not extension state. No CEF process starts during registration,
 * layout, session creation, selection, or reload. */
void PicoBrowser_Init(PicoHost *host);
void PicoBrowser_Pump(PicoHost *host);
void PicoBrowser_Shutdown(PicoHost *host, const struct timespec *deadline);
void PicoBrowser_CloseAgent(PicoHost *host, PicoAgentId id);
void PicoBrowser_BeginLayout(PicoHost *host);
void PicoBrowser_Layout(PicoHost *host);
float PicoBrowser_ShellWidth(const PicoHost *host);
void PicoBrowser_Input(PicoHost *host);
void PicoBrowser_AfterLayout(PicoHost *host);
void PicoBrowser_AfterRender(PicoHost *host);
bool PicoBrowser_BlocksKeyboard(const PicoHost *host);
bool PicoBrowser_Key(PicoHost *host, int key, int scan, int action, int mods);
bool PicoBrowser_Char(PicoHost *host, unsigned int codepoint);
char *PicoBrowser_ToolOpen(PicoAgentContext *ctx, const char *url, bool *error);
PicoExt pico_ext_browser(void);
#endif
