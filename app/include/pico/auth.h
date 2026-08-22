#ifndef PICO_AUTH_H
#define PICO_AUTH_H

#include "pico/app.h"

#define PICO_AUTH_API_KEY "api_key"
#define PICO_AUTH_OAUTH "oauth"

typedef struct PicoAuthEntry {
    char provider[64];
    char active[32];
    char *api_key;
    char *access_token;
    char *refresh_token;
    char *account_id;
    long expires_at;
} PicoAuthEntry;

void pico_auth_entry_free(PicoAuthEntry *e);
/* Copies the named provider's credentials. `api_key` is the effective key (env overlay). */
bool pico_auth_copy(PicoApp *app, const char *provider, PicoAuthEntry *out);
/* Worker callback variant. ctx is valid only for the callback that received it. */
bool pico_auth_copy_ctx(PicoAgentContext *ctx, const char *provider, PicoAuthEntry *out);
/* Registers an environment-supplied API key, which overrides a stored one and is
 * never written to disk. Pass NULL to clear. */
void pico_auth_set_env_key(PicoApp *app, const char *provider, const char *key);
/* The mutators update memory unconditionally and return whether auth.json was
 * also written; false means the change is lost on restart. */
bool pico_auth_set_oauth(PicoApp *app, const char *provider, const char *access, const char *refresh,
                         const char *account_id, long expires_at);
/* Synchronized worker callback variant used by providers refreshing credentials. */
bool pico_auth_set_oauth_ctx(PicoAgentContext *ctx, const char *provider, const char *access,
                             const char *refresh, const char *account_id, long expires_at);
bool pico_auth_set_active(PicoApp *app, const char *provider, const char *active);
bool pico_auth_clear_oauth(PicoApp *app, const char *provider);

void PicoAuth_Load(PicoApp *app);
void PicoAuth_Free(PicoApp *app);

#endif
