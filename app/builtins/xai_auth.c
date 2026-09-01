#include "builtins/xai_auth.h"

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool pico_xai_oauth_refresh_needed(const char *access_token, long expires_at, time_t now, bool force)
{
    if (force || !access_token || !access_token[0])
    {
        return true;
    }
    return expires_at > 0 && (long)now + 60 >= expires_at;
}

bool pico_xai_oauth_token_replaced(const char *rejected_token, const char *current_token)
{
    return rejected_token && rejected_token[0] && current_token && current_token[0] &&
           strcmp(rejected_token, current_token) != 0;
}

int pico_xai_oauth_slow_down_interval(int current, int supplied)
{
    int next = current + 5;
    if (supplied > next)
    {
        next = supplied;
    }
    if (next < 1)
    {
        next = 1;
    }
    return next;
}

bool pico_xai_conv_id_header(const char *cache_key, char *out, size_t cap)
{
    if (out && cap)
    {
        out[0] = 0;
    }
    if (!cache_key || !cache_key[0] || !out || cap == 0)
    {
        return false;
    }
    int n = snprintf(out, cap, "x-grok-conv-id: %s", cache_key);
    if (n < 0 || (size_t)n >= cap)
    {
        out[0] = 0;
        return false;
    }
    return true;
}

bool pico_xai_https_uri_ok(const char *uri)
{
    static const char kHttps[] = "https://";
    if (!uri || !uri[0])
    {
        return false;
    }
    size_t i;
    for (i = 0; kHttps[i]; i++)
    {
        char c = uri[i];
        if (c >= 'A' && c <= 'Z')
        {
            c = (char)(c - 'A' + 'a');
        }
        if (c != kHttps[i])
        {
            return false;
        }
    }
    if (!uri[i] || uri[i] == '/' || uri[i] == '?' || uri[i] == '#')
    {
        return false;
    }
    for (; uri[i]; i++)
    {
        unsigned char c = (unsigned char)uri[i];
        if (c <= 32 || c == 127)
        {
            return false;
        }
    }
    return true;
}

bool pico_xai_oauth_parse_token(const char *body, const char *previous_refresh, time_t now,
                                char **access, char **refresh, long *expires_at)
{
    if (access)
    {
        *access = NULL;
    }
    if (refresh)
    {
        *refresh = NULL;
    }
    if (expires_at)
    {
        *expires_at = 0;
    }
    if (!body || !access || !refresh || !expires_at)
    {
        return false;
    }
    JsonDoc doc;
    if (JsonParse(&doc, body, strlen(body)) != 0)
    {
        return false;
    }
    char *got_access = JsonObjStr(&doc, 0, "access_token");
    char *got_refresh = JsonObjStr(&doc, 0, "refresh_token");
    bool ok = got_access && got_access[0];
    if (ok)
    {
        if (got_refresh && got_refresh[0])
        {
            *refresh = got_refresh;
            got_refresh = NULL;
        }
        else if (previous_refresh && previous_refresh[0])
        {
            *refresh = JsonDup(previous_refresh);
            if (!*refresh)
            {
                ok = false;
            }
        }
        else
        {
            ok = false;
        }
    }
    if (ok)
    {
        int tok = JsonObjGet(&doc, 0, "expires_in");
        int expires_in = 3600;
        if (tok >= 0)
        {
            expires_in = JsonInt(&doc, tok, 0);
            if (expires_in <= 0)
            {
                ok = false;
            }
        }
        if (ok)
        {
            *access = got_access;
            got_access = NULL;
            *expires_at = (long)now + expires_in;
        }
    }
    if (!ok)
    {
        free(*refresh);
        *refresh = NULL;
        *expires_at = 0;
    }
    free(got_access);
    free(got_refresh);
    JsonFree(&doc);
    return ok;
}
