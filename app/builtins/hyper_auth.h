#ifndef PICO_HYPER_AUTH_H
#define PICO_HYPER_AUTH_H

#include <stdbool.h>
#include <time.h>

bool pico_hyper_oauth_refresh_needed(const char *access_token, long expires_at, time_t now,
                                     bool force);
bool pico_hyper_oauth_token_replaced(const char *rejected_token, const char *current_token);

#endif
