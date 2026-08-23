#include "builtins/hyper_auth.h"

#include <string.h>

bool pico_hyper_oauth_refresh_needed(const char *access_token, long expires_at, time_t now,
                                     bool force)
{
    if (force || !access_token || !access_token[0])
    {
        return true;
    }
    return expires_at > 0 && (long)now + 60 >= expires_at;
}

bool pico_hyper_oauth_token_replaced(const char *rejected_token, const char *current_token)
{
    return rejected_token && rejected_token[0] && current_token && current_token[0] &&
           strcmp(rejected_token, current_token) != 0;
}
