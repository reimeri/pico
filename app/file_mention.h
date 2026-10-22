#ifndef PICO_FILE_MENTION_H
#define PICO_FILE_MENTION_H

#include <stdbool.h>
#include <stddef.h>

/* Internal text representation: @bare/path or @"escaped path". */
bool PicoFileMention_Encode(const char *path, char *out, size_t cap);
/* Decode the prefix after @, allowing an unfinished quoted query. */
bool PicoFileMention_Query(const char *prefix, char *out, size_t cap);
/* End of the quote, including its closing quote, or len if still open. */
int PicoFileMention_QuotedEnd(const char *text, int len, int start, bool *closed);
/* Parse one complete mention. end advances past malformed tokens too, so their
 * interior cannot accidentally expand another file mention. */
bool PicoFileMention_Parse(const char *text, int len, int start, int *end,
                            char *path, size_t cap);

#endif
