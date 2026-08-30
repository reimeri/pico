#ifndef PICO_DOCS_PATH_H
#define PICO_DOCS_PATH_H

#include <stdbool.h>
#include <stddef.h>

void Pico_PathsInit(const char *application_dir);
void Pico_DocsSetAppDir(const char *dir);
const char *Pico_DocsAppDir(void);
bool Pico_DataPath(const char *relative, char *out, size_t cap);
bool Pico_SdkIncludeDir(char *out, size_t cap);
bool Pico_DocsRelPath(const char *topic, char *out, size_t cap);
bool Pico_DocsJoin(const char *app_dir, const char *rel, char *out, size_t cap);
bool Pico_DocsFile(const char *topic, char *out, size_t cap);

#endif
