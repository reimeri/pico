#ifndef PICO_CANONICAL_H
#define PICO_CANONICAL_H

#include "pico/app.h"
#include "json.h"

#include <stddef.h>

void pico_canonical_free_parts(PicoLlmPart *parts, int n);
bool pico_canonical_parse_parts(const JsonDoc *doc, int obj, PicoLlmPart **out, int *n);
bool pico_canonical_normalize_user_parts(const char *json, char **canonical_out);
char *pico_canonical_parts_json(const PicoLlmPart *parts, int n);
char *pico_canonical_user_json(const PicoLlmPart *parts, int n);
char *pico_canonical_user_text(const char *text);
char *pico_canonical_assistant_json(const PicoLlmPart *parts, int n, const char *thinking,
                                    const char *signature);
char *pico_canonical_tool_call_json(const char *call_id, const char *name, const char *arguments,
                                    const char *item_id);
char *pico_canonical_tool_result_json(const char *call_id, const char *name, const char *output,
                                      bool is_error);
char *pico_canonical_item_json(const PicoLlmItem *item);
char *pico_canonical_display(const PicoLlmPart *parts, int n);
char *pico_canonical_compact_text(const PicoLlmPart *parts, int n);
char *pico_canonical_plain_text(const PicoLlmPart *parts, int n);
bool pico_canonical_parts_need_log(const PicoLlmPart *parts, int n);
bool pico_canonical_parts_have_media(const PicoLlmPart *parts, int n);
bool pico_canonical_json_has_media(const char *json);
bool pico_canonical_is_image_path(const char *path);
bool pico_canonical_is_audio_path(const char *path);
const char *pico_canonical_mime_for_path(const char *path);
char *pico_canonical_data_url(const char *path, const char *mime);
char *pico_canonical_file_base64(const char *path, size_t *out_len);
char *pico_canonical_audio_format(const char *path, const char *mime);
char *pico_canonical_persist_bytes(const char *dir, const char *ext, const void *data, size_t n);
char *pico_canonical_persist_data_url(const char *dir, const char *data_url);

#endif
