#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "host_internal.h"
#include "spell_engine.h"
#include "spell_internal.h"

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
    char lang_override[32];   /* settings spell_lang; empty = derive from env */
} SpellLoader;

typedef enum SpellStatus {
    SPELL_LOADING,
    SPELL_READY,
    SPELL_UNAVAILABLE,
} SpellStatus;

/* Last-checked text and misspelling ranges for one text field. A single
 * slot is enough: fields that share the screen (composer behind the ask
 * modal) never draw in the same frame, and a key mismatch just rechecks. */
typedef struct SpellFieldCache {
    const void *key;
    char *text; /* copy of the checked text, not NUL-terminated */
    int length;
    PicoSpellRanges ranges;
    /* Not part of the text cache key. A caret move reveals a committed
     * word without rechecking; only an edit arms a new span. */
    PicoSpellPending pending;
} SpellFieldCache;

typedef struct SpellState {
    SpellStatus status;
    SpellLoader *loader;        /* non-NULL until the load resolves */
    PicoSpellBackend backend;   /* valid when status == SPELL_READY */
    SpellFieldCache cache;
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
        char full[32] = {0};
        char lang[32] = {0};
        if (loader->lang_override[0])
        {
            snprintf(full, sizeof(full), "%s", loader->lang_override);
        }
        else
        {
            pico_spell_dict_tags(LocaleEnv(), full, lang);
        }
        const char *tag = NULL;
        if (full[0] && dict_exists(b->broker, full))
        {
            tag = full;
        }
        else if (lang[0] && dict_exists(b->broker, lang))
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
    SpellState *s = (SpellState *)calloc(1, sizeof(*s));
    SpellLoader *loader = (SpellLoader *)calloc(1, sizeof(*loader));
    if (!s || !loader)
    {
        free(s);
        free(loader);
        return -1;
    }
    pthread_mutex_init(&loader->lock, NULL);
    if (host)
    {
        snprintf(loader->lang_override, sizeof(loader->lang_override), "%s", host->preferences.spell_lang);
    }
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
    free(s->cache.text);
    pico_spell_ranges_free(&s->cache.ranges);
    FreeBackend(s->backend.ctx);
    free(s);
}

/* Same temporarily-NUL-terminate measuring trick as the composer; the view
 * text is the caller's live editable buffer. */
static float MeasureSlice(Font font, const char *s, int start, int length, float font_size)
{
    if (length <= 0)
    {
        return 0;
    }
    char saved = ((char *)s)[start + length];
    ((char *)s)[start + length] = '\0';
    Vector2 size = MeasureTextEx(font, s + start, font_size, 0);
    ((char *)s)[start + length] = saved;
    return size.x;
}

static void DrawSquiggle(float x0, float x1, float y, Color color)
{
    const float half_period = 2.0f;
    const float amplitude = 1.5f;
    float px = x0;
    float py = y;
    bool up = true;
    float x = x0;
    while (x < x1)
    {
        float nx = x + half_period < x1 ? x + half_period : x1;
        float ny = up ? y - amplitude : y;
        DrawLineEx((Vector2){px, py}, (Vector2){nx, ny}, 1.0f, color);
        px = nx;
        py = ny;
        up = !up;
        x = nx;
    }
}

static bool CacheMatches(const SpellFieldCache *cache, const void *key, const char *text, int length)
{
    return cache->key == key && cache->length == length &&
           (length == 0 || (cache->text && memcmp(cache->text, text, (size_t)length) == 0));
}

static void RecheckField(SpellState *s, const void *key, const char *text, int length)
{
    pico_spell_check_text(&s->backend, text, length, &s->cache.ranges);
    if (length <= 0)
    {
        free(s->cache.text);
        s->cache.text = NULL;
        s->cache.length = 0;
        s->cache.key = key;
        return;
    }
    char *copy = (char *)realloc(s->cache.text, (size_t)length);
    if (copy)
    {
        memcpy(copy, text, (size_t)length);
        s->cache.text = copy;
        s->cache.length = length;
        s->cache.key = key;
    }
    else
    {
        /* OOM: drop the copy so the next frame rechecks instead of trusting
         * ranges computed from text the cache can no longer identify. */
        free(s->cache.text);
        s->cache.text = NULL;
        s->cache.length = 0;
        s->cache.key = NULL;
    }
}

void PicoSpell_DrawSquiggles(PicoHost *host, const void *field_key, const PicoSpellView *view)
{
    SpellState *s = (SpellState *)PicoPlugins_HostState(host, "spell");
    if (!s || s->status != SPELL_READY || !host || !host->preferences.spell || !view)
    {
        return;
    }
    const char *text = view->text ? view->text : "";
    int length = view->text ? view->length : 0;
    if (length < 0)
    {
        length = 0;
    }
    bool text_changed = !CacheMatches(&s->cache, field_key, text, length);
    if (text_changed)
    {
        RecheckField(s, field_key, text, length);
    }
    /* Filter at draw time. The cached ranges still include the in-progress
     * word so leaving it can show the underline without another check. */
    pico_spell_pending_update(&s->cache.pending, text, length, view->cursor, text_changed);
    if (length <= 0 || !view->lines)
    {
        return;
    }
    Color miss = {(unsigned char)COLOR_SPELL_MISS.r, (unsigned char)COLOR_SPELL_MISS.g,
                  (unsigned char)COLOR_SPELL_MISS.b, (unsigned char)COLOR_SPELL_MISS.a};
    for (int i = 0; i < view->line_count; i++)
    {
        float line_y = view->origin_y + (float)i * view->line_height + view->scroll_y;
        if (line_y + view->line_height < view->clip.y || line_y > view->clip.y + view->clip.height)
        {
            continue;
        }
        int line_start = view->lines[i].start;
        int line_end = line_start + view->lines[i].length;
        float y = line_y + view->line_height - 2.0f;
        for (int r = 0; r < s->cache.ranges.count; r++)
        {
            int rs = s->cache.ranges.items[r].start;
            int re = s->cache.ranges.items[r].end;
            if (pico_spell_pending_hides(&s->cache.pending, rs, re))
            {
                continue;
            }
            int a = rs > line_start ? rs : line_start;
            int b = re < line_end ? re : line_end;
            if (a >= b)
            {
                continue;
            }
            float x0 = view->origin_x + MeasureSlice(view->font, view->text, line_start, a - line_start,
                                                     view->font_px);
            float x1 = view->origin_x + MeasureSlice(view->font, view->text, line_start, b - line_start,
                                                     view->font_px);
            DrawSquiggle(x0, x1, y, miss);
        }
    }
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
