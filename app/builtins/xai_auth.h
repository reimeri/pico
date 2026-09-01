#ifndef PICO_XAI_AUTH_H
#define PICO_XAI_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

bool pico_xai_oauth_refresh_needed(const char *access_token, long expires_at, time_t now, bool force);
bool pico_xai_oauth_token_replaced(const char *rejected_token, const char *current_token);
int pico_xai_oauth_slow_down_interval(int current, int supplied);
bool pico_xai_https_uri_ok(const char *uri);
/* Formats the Chat Completions cache-affinity header. Returns false when
 * cache_key is empty or the buffer cannot hold the full header. */
bool pico_xai_conv_id_header(const char *cache_key, char *out, size_t cap);
/* Parses an xAI token JSON body. `refresh` is the new token, or `previous_refresh`
 * when xAI omits rotation. Caller frees `*access` and `*refresh` on success. */
bool pico_xai_oauth_parse_token(const char *body, const char *previous_refresh, time_t now,
                                char **access, char **refresh, long *expires_at);

#endif
