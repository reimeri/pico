#include "builtins/xai_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failed;

static void Check(bool ok, const char *message)
{
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        g_failed = 1;
    }
}

static void TestRefreshDecision(void)
{
    time_t now = 1000;
    Check(pico_xai_oauth_refresh_needed(NULL, 2000, now, false), "missing access token refreshes");
    Check(!pico_xai_oauth_refresh_needed("token", 2000, now, false), "fresh access token is reused");
    Check(pico_xai_oauth_refresh_needed("token", 1060, now, false), "nearly expired token refreshes");
    Check(pico_xai_oauth_refresh_needed("token", 2000, now, true), "401 forces refresh");
    Check(pico_xai_oauth_token_replaced("old", "new"),
          "a waiter reuses a token refreshed by another request");
    Check(!pico_xai_oauth_token_replaced("same", "same"),
          "the request that received the 401 performs the refresh");
    Check(pico_xai_oauth_slow_down_interval(5, 3) == 10,
          "slow_down increases the interval by at least five seconds");
}

static void TestConvIdHeader(void)
{
    char buf[64];
    Check(pico_xai_conv_id_header("session-1", buf, sizeof(buf)) &&
              strcmp(buf, "x-grok-conv-id: session-1") == 0,
          "a cache key is sent as x-grok-conv-id");
    Check(!pico_xai_conv_id_header("", buf, sizeof(buf)) && buf[0] == 0,
          "an empty cache key omits the header");
}

static void TestHttpsUri(void)
{
    Check(pico_xai_https_uri_ok("https://auth.x.ai/authorize"), "https verification URI is accepted");
    Check(!pico_xai_https_uri_ok("http://auth.x.ai/authorize"), "http verification URI is rejected");
    Check(!pico_xai_https_uri_ok("not-a-url"), "a non-URI is rejected");
}

static void TestTokenParse(void)
{
    char *access = NULL;
    char *refresh = NULL;
    long expires_at = 0;
    Check(pico_xai_oauth_parse_token("{\"access_token\":\"a\",\"expires_in\":3600}", "prev", 1000,
                                     &access, &refresh, &expires_at) &&
              access && strcmp(access, "a") == 0 && refresh && strcmp(refresh, "prev") == 0 &&
              expires_at == 4600,
          "refresh response without refresh_token keeps the previous one");
    free(access);
    free(refresh);

    access = NULL;
    refresh = NULL;
    expires_at = 0;
    Check(!pico_xai_oauth_parse_token("{\"refresh_token\":\"r\",\"expires_in\":3600}", NULL, 1000,
                                      &access, &refresh, &expires_at) &&
              !access && !refresh && expires_at == 0,
          "token response without access_token is rejected");
}

int main(void)
{
    TestRefreshDecision();
    TestConvIdHeader();
    TestHttpsUri();
    TestTokenParse();
    return g_failed;
}
