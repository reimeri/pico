#define _POSIX_C_SOURCE 200809L

#include "auth.h"
#include "agent.h"
#include "json.h"
#include "path.h"
#include "settings.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct StoredAuth {
    char provider[64];
    char active[32];
    char *api_key;
    char *env_api_key;
    char *access_token;
    char *refresh_token;
    char *account_id;
    long expires_at;
} StoredAuth;

/* Entries are individually allocated so a `StoredAuth *` stays valid across
 * later Ensure() growth. */
typedef struct PicoAuthStore {
    pthread_mutex_t mu;
    StoredAuth **entries;
    int count;
    int cap;
} PicoAuthStore;

static void FreeStrings(StoredAuth *e)
{
    if (!e)
    {
        return;
    }
    free(e->api_key);
    free(e->env_api_key);
    free(e->access_token);
    free(e->refresh_token);
    free(e->account_id);
    e->api_key = NULL;
    e->env_api_key = NULL;
    e->access_token = NULL;
    e->refresh_token = NULL;
    e->account_id = NULL;
}

static void SetStr(char **slot, const char *v)
{
    free(*slot);
    *slot = (v && v[0]) ? JsonDup(v) : NULL;
}

static const char *EffectiveKey(const StoredAuth *e)
{
    if (e->env_api_key && e->env_api_key[0])
    {
        return e->env_api_key;
    }
    if (e->api_key && e->api_key[0])
    {
        return e->api_key;
    }
    return NULL;
}

static bool AuthPath(char *out, size_t cap)
{
    char dir[4096];
    return Pico_ConfigDir(dir, sizeof(dir)) &&
           PicoPath_Format(out, cap, "%s/auth.json", dir);
}

static StoredAuth *Find(PicoAuthStore *s, const char *provider)
{
    if (!s || !provider || !provider[0])
    {
        return NULL;
    }
    for (int i = 0; i < s->count; i++)
    {
        if (strcmp(s->entries[i]->provider, provider) == 0)
        {
            return s->entries[i];
        }
    }
    return NULL;
}

static StoredAuth *Ensure(PicoAuthStore *s, const char *provider)
{
    StoredAuth *e = Find(s, provider);
    if (e)
    {
        return e;
    }
    if (s->count >= s->cap)
    {
        int cap = s->cap == 0 ? 4 : s->cap * 2;
        StoredAuth **next = (StoredAuth **)realloc(s->entries, (size_t)cap * sizeof(StoredAuth *));
        if (!next)
        {
            return NULL;
        }
        s->entries = next;
        s->cap = cap;
    }
    e = (StoredAuth *)calloc(1, sizeof(StoredAuth));
    if (!e)
    {
        return NULL;
    }
    snprintf(e->provider, sizeof(e->provider), "%s", provider);
    s->entries[s->count++] = e;
    return e;
}

static void FillActive(StoredAuth *e)
{
    if (!e || e->active[0])
    {
        return;
    }
    if (e->access_token && e->access_token[0] && e->refresh_token && e->refresh_token[0])
    {
        snprintf(e->active, sizeof(e->active), "%s", PICO_AUTH_OAUTH);
    }
    else if (EffectiveKey(e))
    {
        snprintf(e->active, sizeof(e->active), "%s", PICO_AUTH_API_KEY);
    }
}

/* Credentials must never exist at the umask's default mode, not even briefly, so
 * create the temporary file 0600 up front and rename it over the target. */
static bool WriteSecret(const char *path, const char *data, size_t len)
{
    if (!path || !path[0] || !data)
    {
        return false;
    }
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
    {
        return false;
    }
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
    {
        return false;
    }
    bool ok = true;
    for (size_t off = 0; ok && off < len;)
    {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0)
        {
            ok = false;
            break;
        }
        off += (size_t)n;
    }
    if (ok && fsync(fd) != 0)
    {
        ok = false;
    }
    if (close(fd) != 0)
    {
        ok = false;
    }
    if (!ok || rename(tmp, path) != 0)
    {
        unlink(tmp);
        return false;
    }
    return true;
}

static bool SaveLocked(PicoAuthStore *s)
{
    char dir[4096];
    char path[4096];
    if (!Pico_ConfigDir(dir, sizeof(dir)) || !AuthPath(path, sizeof(path)))
    {
        return false;
    }
    Pico_MkdirP(dir);

    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, "{\n");
    int wrote = 0;
    for (int i = 0; i < s->count; i++)
    {
        StoredAuth *e = s->entries[i];
        bool has_key = e->api_key && e->api_key[0];
        bool has_oauth = (e->access_token && e->access_token[0]) || (e->refresh_token && e->refresh_token[0]);
        if (!e->active[0] && !has_key && !has_oauth)
        {
            continue;
        }
        if (wrote)
        {
            JsonBuf_Puts(&b, ",\n");
        }
        wrote++;
        JsonBuf_Puts(&b, "  ");
        JsonBuf_String(&b, e->provider);
        JsonBuf_Puts(&b, ": {\n    \"active\": ");
        JsonBuf_String(&b, e->active);
        if (has_key)
        {
            JsonBuf_Puts(&b, ",\n    \"api_key\": ");
            JsonBuf_String(&b, e->api_key);
        }
        if (has_oauth)
        {
            JsonBuf_Puts(&b, ",\n    \"oauth\": {\n      \"access_token\": ");
            JsonBuf_String(&b, e->access_token ? e->access_token : "");
            JsonBuf_Puts(&b, ",\n      \"refresh_token\": ");
            JsonBuf_String(&b, e->refresh_token ? e->refresh_token : "");
            JsonBuf_Puts(&b, ",\n      \"account_id\": ");
            JsonBuf_String(&b, e->account_id ? e->account_id : "");
            JsonBuf_Puts(&b, ",\n      \"expires_at\": ");
            char nbuf[32];
            snprintf(nbuf, sizeof(nbuf), "%ld", e->expires_at);
            JsonBuf_Puts(&b, nbuf);
            JsonBuf_Puts(&b, "\n    }");
        }
        JsonBuf_Puts(&b, "\n  }");
    }
    JsonBuf_Puts(&b, wrote ? "\n}\n" : "}\n");
    char *out = JsonBuf_Steal(&b);
    bool ok = WriteSecret(path, out, out ? strlen(out) : 0);
    free(out);
    return ok;
}

static void ParseProvider(PicoAuthStore *s, const JsonDoc *doc, int obj, const char *name)
{
    StoredAuth *e = Ensure(s, name);
    if (!e || !JsonIsObject(doc, obj))
    {
        return;
    }
    char *active = JsonObjStr(doc, obj, "active");
    char *api_key = JsonObjStr(doc, obj, "api_key");
    if (active && active[0])
    {
        snprintf(e->active, sizeof(e->active), "%s", active);
    }
    SetStr(&e->api_key, api_key);
    free(active);
    free(api_key);

    int oauth = JsonObjGet(doc, obj, "oauth");
    if (JsonIsObject(doc, oauth))
    {
        char *access = JsonObjStr(doc, oauth, "access_token");
        char *refresh = JsonObjStr(doc, oauth, "refresh_token");
        char *account = JsonObjStr(doc, oauth, "account_id");
        SetStr(&e->access_token, access);
        SetStr(&e->refresh_token, refresh);
        SetStr(&e->account_id, account);
        e->expires_at = (long)JsonObjInt(doc, oauth, "expires_at", 0);
        free(access);
        free(refresh);
        free(account);
    }
}

static void LoadFile(PicoAuthStore *s, const char *path)
{
    size_t len = 0;
    char *src = Pico_ReadFile(path, &len);
    if (!src)
    {
        return;
    }
    JsonStripComments(src, len);
    JsonDoc doc;
    if (JsonParse(&doc, src, len) == 0 && JsonIsObject(&doc, 0))
    {
        int n = JsonObjLen(&doc, 0);
        for (int i = 0; i < n; i++)
        {
            int key = -1;
            int val = -1;
            if (!JsonObjPair(&doc, 0, i, &key, &val))
            {
                continue;
            }
            char *name = JsonStrDup(&doc, key);
            if (name && name[0])
            {
                ParseProvider(s, &doc, val, name);
            }
            free(name);
        }
        JsonFree(&doc);
    }
    free(src);
}

/* Providers own their environment variable names, so the key arrives here rather
 * than being read by the store. It is deliberately never persisted. */
void pico_auth_set_env_key(PicoApp *app, const char *provider, const char *key)
{
    if (!app || !app->auth_store || !provider || !provider[0])
    {
        return;
    }
    pthread_mutex_lock(&app->auth_store->mu);
    StoredAuth *e = Ensure(app->auth_store, provider);
    if (e)
    {
        SetStr(&e->env_api_key, key);
        FillActive(e);
    }
    pthread_mutex_unlock(&app->auth_store->mu);
}

void pico_auth_entry_free(PicoAuthEntry *e)
{
    if (!e)
    {
        return;
    }
    free(e->api_key);
    free(e->access_token);
    free(e->refresh_token);
    free(e->account_id);
    memset(e, 0, sizeof(*e));
}

void pico_add_auth(PicoApp *app, const PicoAuth *a)
{
    if (!app || !a || !a->provider || !a->provider[0] || !a->login || app->auth_count >= PICO_MAX_AUTH)
    {
        return;
    }
    app->auths[app->auth_count] = *a;
    app->auth_count++;
}

static bool NameEq(const char *a, const char *b)
{
    if (!a || !b)
    {
        return false;
    }
    while (*a && *b)
    {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a - 'A' + 'a' : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b - 'A' + 'a' : *b;
        if (ca != cb)
        {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

const PicoAuth *pico_find_auth(const PicoApp *app, const char *provider)
{
    if (!app || !provider || !provider[0])
    {
        return NULL;
    }
    for (int i = 0; i < app->auth_count; i++)
    {
        if (NameEq(app->auths[i].provider, provider))
        {
            return &app->auths[i];
        }
    }
    return NULL;
}

static bool AuthCopyLocked(PicoAuthStore *store, const char *provider, PicoAuthEntry *out)
{
    StoredAuth *e = Find(store, provider);
    bool ok = e != NULL;
    if (ok)
    {
        snprintf(out->provider, sizeof(out->provider), "%s", e->provider);
        snprintf(out->active, sizeof(out->active), "%s", e->active);
        const char *key = EffectiveKey(e);
        out->api_key = key ? JsonDup(key) : NULL;
        out->access_token = e->access_token ? JsonDup(e->access_token) : NULL;
        out->refresh_token = e->refresh_token ? JsonDup(e->refresh_token) : NULL;
        out->account_id = e->account_id ? JsonDup(e->account_id) : NULL;
        out->expires_at = e->expires_at;
    }
    return ok;
}

static bool AuthCopyStore(PicoAuthStore *store, const char *provider, PicoAuthEntry *out)
{
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    if (!store || !provider || !out)
    {
        return false;
    }
    pthread_mutex_lock(&store->mu);
    bool ok = AuthCopyLocked(store, provider, out);
    pthread_mutex_unlock(&store->mu);
    return ok;
}

bool pico_auth_copy(PicoApp *app, const char *provider, PicoAuthEntry *out)
{
    return AuthCopyStore(app ? app->auth_store : NULL, provider, out);
}

bool pico_auth_copy_ctx(PicoAgentContext *ctx, const char *provider, PicoAuthEntry *out)
{
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    PicoAuthStore *store = PicoAgentContext_AuthStore(ctx);
    if (!store || !provider || !out)
    {
        return false;
    }
    pthread_mutex_lock(&store->mu);
    if (!PicoAgentContext_LockIfLive(ctx))
    {
        pthread_mutex_unlock(&store->mu);
        return false;
    }
    bool ok = AuthCopyLocked(store, provider, out);
    PicoAgentContext_UnlockLive(ctx);
    pthread_mutex_unlock(&store->mu);
    return ok;
}

static bool AuthSetOauthStore(PicoAuthStore *store, const char *provider, const char *access,
                              const char *refresh, const char *account_id, long expires_at)
{
    if (!store || !provider || !provider[0])
    {
        return false;
    }
    pthread_mutex_lock(&store->mu);
    StoredAuth *e = Ensure(store, provider);
    bool saved = false;
    if (e)
    {
        SetStr(&e->access_token, access);
        SetStr(&e->refresh_token, refresh);
        SetStr(&e->account_id, account_id);
        e->expires_at = expires_at;
        snprintf(e->active, sizeof(e->active), "%s", PICO_AUTH_OAUTH);
        saved = SaveLocked(store);
    }
    pthread_mutex_unlock(&store->mu);
    return saved;
}

bool pico_auth_set_oauth(PicoApp *app, const char *provider, const char *access, const char *refresh,
                         const char *account_id, long expires_at)
{
    return AuthSetOauthStore(app ? app->auth_store : NULL, provider, access, refresh,
                             account_id, expires_at);
}

bool pico_auth_set_oauth_ctx(PicoAgentContext *ctx, const char *provider, const char *access,
                             const char *refresh, const char *account_id, long expires_at)
{
    if (!provider || !provider[0])
    {
        return false;
    }
    PicoAuthStore *store = PicoAgentContext_AuthStore(ctx);
    if (!store)
    {
        return false;
    }

    /* Lock auth first, then validate/lock the runtime. Mutate while both are
     * held so retirement and the credential update have one atomic ordering.
     * Release the runtime before persistence: cancellation never waits on
     * the auth lock or filesystem I/O. */
    pthread_mutex_lock(&store->mu);
    if (!PicoAgentContext_LockIfLive(ctx))
    {
        pthread_mutex_unlock(&store->mu);
        return false;
    }
    StoredAuth *e = Ensure(store, provider);
    if (!e)
    {
        PicoAgentContext_UnlockLive(ctx);
        pthread_mutex_unlock(&store->mu);
        return false;
    }
    SetStr(&e->access_token, access);
    SetStr(&e->refresh_token, refresh);
    SetStr(&e->account_id, account_id);
    e->expires_at = expires_at;
    snprintf(e->active, sizeof(e->active), "%s", PICO_AUTH_OAUTH);
    PicoAgentContext_UnlockLive(ctx);
    bool saved = SaveLocked(store);
    pthread_mutex_unlock(&store->mu);
    return saved;
}

bool pico_auth_set_active(PicoApp *app, const char *provider, const char *active)
{
    if (!app || !app->auth_store || !provider || !provider[0] || !active)
    {
        return false;
    }
    pthread_mutex_lock(&app->auth_store->mu);
    StoredAuth *e = Ensure(app->auth_store, provider);
    bool saved = false;
    if (e)
    {
        snprintf(e->active, sizeof(e->active), "%s", active);
        saved = SaveLocked(app->auth_store);
    }
    pthread_mutex_unlock(&app->auth_store->mu);
    return saved;
}

bool pico_auth_clear_oauth(PicoApp *app, const char *provider)
{
    if (!app || !app->auth_store || !provider || !provider[0])
    {
        return false;
    }
    pthread_mutex_lock(&app->auth_store->mu);
    StoredAuth *e = Find(app->auth_store, provider);
    bool saved = true;
    if (e)
    {
        SetStr(&e->access_token, NULL);
        SetStr(&e->refresh_token, NULL);
        SetStr(&e->account_id, NULL);
        e->expires_at = 0;
        if (EffectiveKey(e))
        {
            snprintf(e->active, sizeof(e->active), "%s", PICO_AUTH_API_KEY);
        }
        else
        {
            e->active[0] = '\0';
        }
        saved = SaveLocked(app->auth_store);
    }
    pthread_mutex_unlock(&app->auth_store->mu);
    return saved;
}

void PicoAuth_Load(PicoApp *app)
{
    if (!app)
    {
        return;
    }
    PicoAuth_Free(app);
    PicoAuthStore *s = (PicoAuthStore *)calloc(1, sizeof(PicoAuthStore));
    if (!s)
    {
        return;
    }
    pthread_mutex_init(&s->mu, NULL);
    app->auth_store = s;
    char path[4096];
    if (AuthPath(path, sizeof(path)))
    {
        LoadFile(s, path);
    }
    for (int i = 0; i < s->count; i++)
    {
        FillActive(s->entries[i]);
    }
}

void PicoAuth_Free(PicoApp *app)
{
    if (!app || !app->auth_store)
    {
        return;
    }
    PicoAuthStore *s = app->auth_store;
    pthread_mutex_lock(&s->mu);
    for (int i = 0; i < s->count; i++)
    {
        FreeStrings(s->entries[i]);
        free(s->entries[i]);
    }
    free(s->entries);
    pthread_mutex_unlock(&s->mu);
    pthread_mutex_destroy(&s->mu);
    free(s);
    app->auth_store = NULL;
}
