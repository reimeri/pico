#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "spell_engine.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Spell checking is probed from the host at runtime: libenchant-2 is a
 * broker that fronts whatever spell backends and dictionaries the machine
 * has (hunspell, aspell, nuspell). Pico never links it; when the library
 * or a matching dictionary is absent, the feature silently disables.
 *
 * Threading: one short-lived loader thread does the dlopen and dictionary
 * setup (dictionary load can take a few hundred milliseconds, and main
 * thread callbacks must return promptly). The thread only calls enchant
 * and malloc — never host APIs, Clay, or raylib — and publishes through a
 * mutex-guarded box. The main thread adopts the result in on_frame, which
 * establishes the happens-before; the adopted backend is only ever used on
 * the main thread afterwards. The thread is detached so no callback ever
 * joins; if shutdown lands mid-load, the cancelled flag makes the thread
 * free its own partial work and the box is intentionally leaked (builtin
 * shutdown means process exit). */

typedef struct EnchantBroker EnchantBroker;
typedef struct EnchantDict EnchantDict;
typedef EnchantBroker *(*EnchantBrokerInitFn)(void);
typedef void (*EnchantBrokerFreeFn)(EnchantBroker *);
typedef int (*EnchantBrokerDictExistsFn)(EnchantBroker *, const char *);
typedef EnchantDict *(*EnchantBrokerRequestDictFn)(EnchantBroker *, const char *);
typedef void (*EnchantBrokerFreeDictFn)(EnchantBroker *, EnchantDict *);
typedef int (*EnchantDictCheckFn)(EnchantDict *, const char *, ssize_t);

typedef struct SpellBackendImpl {
    void *dl;
    EnchantBroker *broker;
    EnchantDict *dict;
    EnchantBrokerFreeFn broker_free;
    EnchantBrokerFreeDictFn dict_free;
    EnchantDictCheckFn dict_check;
    char tag[32];
} SpellBackendImpl;

/* Shared with the loader thread; every field is guarded by lock. */
typedef struct SpellLoader {
    pthread_mutex_t lock;
    bool cancelled;
    bool done;
    SpellBackendImpl *result; /* NULL on failure; see error */
    char error[160];
} SpellLoader;

typedef enum SpellStatus {
    SPELL_LOADING,
    SPELL_READY,
    SPELL_UNAVAILABLE,
} SpellStatus;

typedef struct SpellState {
    SpellStatus status;
    SpellLoader *loader;        /* non-NULL until the load resolves */
    PicoSpellBackend backend;   /* valid when status == SPELL_READY */
} SpellState;

static bool EnchantCheck(void *ctx, const char *word, int length)
{
    SpellBackendImpl *b = (SpellBackendImpl *)ctx;
    /* enchant_dict_check: 0 = correct, >0 = misspelled, <0 = error. Errors
     * count as correct so a flaky backend never paints noise. */
    return b->dict_check(b->dict, word, length) <= 0;
}

static void FreeBackend(SpellBackendImpl *b)
{
    if (!b)
    {
        return;
    }
    if (b->dict && b->dict_free)
    {
        b->dict_free(b->broker, b->dict);
    }
    if (b->broker && b->broker_free)
    {
        b->broker_free(b->broker);
    }
    if (b->dl)
    {
        dlclose(b->dl);
    }
    free(b);
}

static void *Resolve(void *dl, const char *name)
{
    void *fn = dlsym(dl, name);
    if (!fn)
    {
        fprintf(stderr, "pico spell: libenchant-2 is missing %s\n", name);
    }
    return fn;
}

static const char *LocaleEnv(void)
{
    const char *locale = getenv("LC_ALL");
    if (!locale || !*locale)
    {
        locale = getenv("LANG");
    }
    return locale;
}

static void *SpellLoaderMain(void *arg)
{
    SpellLoader *loader = (SpellLoader *)arg;
    SpellBackendImpl *b = (SpellBackendImpl *)calloc(1, sizeof(*b));
    char error[160] = "out of memory";

    if (b)
    {
        b->dl = dlopen("libenchant-2.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!b->dl)
        {
            snprintf(error, sizeof(error), "libenchant-2 not found");
        }
    }
    EnchantBrokerInitFn broker_init = NULL;
    EnchantBrokerDictExistsFn dict_exists = NULL;
    EnchantBrokerRequestDictFn request_dict = NULL;
    if (b && b->dl)
    {
        broker_init = (EnchantBrokerInitFn)Resolve(b->dl, "enchant_broker_init");
        b->broker_free = (EnchantBrokerFreeFn)Resolve(b->dl, "enchant_broker_free");
        dict_exists = (EnchantBrokerDictExistsFn)Resolve(b->dl, "enchant_broker_dict_exists");
        request_dict = (EnchantBrokerRequestDictFn)Resolve(b->dl, "enchant_broker_request_dict");
        b->dict_free = (EnchantBrokerFreeDictFn)Resolve(b->dl, "enchant_broker_free_dict");
        b->dict_check = (EnchantDictCheckFn)Resolve(b->dl, "enchant_dict_check");
        if (!broker_init || !b->broker_free || !dict_exists || !request_dict || !b->dict_free ||
            !b->dict_check)
        {
            snprintf(error, sizeof(error), "libenchant-2 is missing expected symbols");
        }
        else
        {
            b->broker = broker_init();
            if (!b->broker)
            {
                snprintf(error, sizeof(error), "enchant_broker_init failed");
            }
        }
    }
    if (b && b->broker)
    {
        char full[32];
        char lang[32];
        int tags = pico_spell_dict_tags(LocaleEnv(), full, lang);
        const char *tag = NULL;
        if (tags >= 1 && dict_exists(b->broker, full))
        {
            tag = full;
        }
        else if (tags == 2 && dict_exists(b->broker, lang))
        {
            tag = lang;
        }
        if (tag)
        {
            b->dict = request_dict(b->broker, tag);
        }
        if (b->dict)
        {
            snprintf(b->tag, sizeof(b->tag), "%s", tag);
            error[0] = '\0';
        }
        else
        {
            snprintf(error, sizeof(error), "no enchant dictionary for locale");
        }
    }

    pthread_mutex_lock(&loader->lock);
    bool cancelled = loader->cancelled;
    if (!cancelled)
    {
        loader->result = error[0] ? NULL : b;
        snprintf(loader->error, sizeof(loader->error), "%s", error);
    }
    loader->done = true;
    pthread_mutex_unlock(&loader->lock);

    if (cancelled || error[0])
    {
        FreeBackend(b);
    }
    return NULL;
}

static int SpellHostInit(PicoHost *host, void **state_out)
{
    (void)host;
    SpellState *s = (SpellState *)calloc(1, sizeof(*s));
    SpellLoader *loader = (SpellLoader *)calloc(1, sizeof(*loader));
    if (!s || !loader)
    {
        free(s);
        free(loader);
        return -1;
    }
    pthread_mutex_init(&loader->lock, NULL);
    s->status = SPELL_LOADING;
    s->loader = loader;

    pthread_t thread;
    if (pthread_create(&thread, NULL, SpellLoaderMain, loader) != 0)
    {
        s->status = SPELL_UNAVAILABLE;
        fprintf(stderr, "pico spell: could not start loader thread\n");
        pthread_mutex_destroy(&loader->lock);
        free(loader);
        s->loader = NULL;
    }
    else
    {
        pthread_detach(thread);
    }
    *state_out = s;
    return 0;
}

/* Adopts a finished load. Returns without touching anything while the
 * loader is still running — main-thread callbacks must not wait. */
static void SpellHostFrame(PicoHost *host, void *state, float dt)
{
    (void)host;
    (void)dt;
    SpellState *s = (SpellState *)state;
    if (!s || s->status != SPELL_LOADING || !s->loader)
    {
        return;
    }
    SpellLoader *loader = s->loader;
    pthread_mutex_lock(&loader->lock);
    bool done = loader->done;
    SpellBackendImpl *result = loader->result;
    char error[160];
    snprintf(error, sizeof(error), "%s", loader->error);
    pthread_mutex_unlock(&loader->lock);
    if (!done)
    {
        return;
    }
    pthread_mutex_destroy(&loader->lock);
    free(loader);
    s->loader = NULL;
    if (result)
    {
        s->backend = (PicoSpellBackend){result, EnchantCheck};
        s->status = SPELL_READY;
    }
    else
    {
        s->status = SPELL_UNAVAILABLE;
        fprintf(stderr, "pico spell: %s; spell checking disabled\n", error[0] ? error : "unavailable");
    }
}

static void SpellHostShutdown(PicoHost *host, void *state)
{
    (void)host;
    SpellState *s = (SpellState *)state;
    if (!s)
    {
        return;
    }
    if (s->loader)
    {
        SpellLoader *loader = s->loader;
        pthread_mutex_lock(&loader->lock);
        bool done = loader->done;
        SpellBackendImpl *result = loader->result;
        if (!done)
        {
            loader->cancelled = true;
        }
        pthread_mutex_unlock(&loader->lock);
        if (done)
        {
            FreeBackend(result);
            pthread_mutex_destroy(&loader->lock);
            free(loader);
        }
        /* Otherwise the loader frees its own partial work; the box is
         * intentionally leaked because builtin shutdown is process exit. */
    }
    FreeBackend(s->backend.ctx);
    free(s);
}

PicoExt pico_ext_spell(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "spell",
        .description = "Spell checking via the host enchant library when available",
        .host_init = SpellHostInit,
        .host_shutdown = SpellHostShutdown,
        .host_on_frame = SpellHostFrame,
    };
}
