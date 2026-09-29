#include "transcript_virtual.h"

#include <stdlib.h>
#include <string.h>

#define PICO_TRANSCRIPT_ESTIMATED_HEIGHT 96.0f
#define PICO_TRANSCRIPT_BACKGROUND_BATCH 32

static bool SameGeometry(const PicoTranscriptVirtual *cache, float width, float font_scale)
{
    return cache->width == width && cache->font_scale == font_scale;
}

static bool Reserve(PicoTranscriptVirtual *cache, int count)
{
    if (count <= cache->capacity)
    {
        return true;
    }
    int capacity = cache->capacity == 0 ? 32 : cache->capacity;
    while (capacity < count)
    {
        capacity *= 2;
    }
    float *heights = (float *)calloc((size_t)capacity, sizeof(float));
    double *tree = (double *)calloc((size_t)capacity + 1, sizeof(double));
    uint64_t *revisions = (uint64_t *)calloc((size_t)capacity, sizeof(uint64_t));
    unsigned char *dirty = (unsigned char *)calloc((size_t)capacity, 1);
    unsigned char *mounted = (unsigned char *)calloc((size_t)capacity, 1);
    if (!heights || !tree || !revisions || !dirty || !mounted)
    {
        free(heights);
        free(tree);
        free(revisions);
        free(dirty);
        free(mounted);
        return false;
    }
    if (cache->capacity > 0)
    {
        memcpy(heights, cache->heights, (size_t)cache->capacity * sizeof(float));
        memcpy(revisions, cache->revisions, (size_t)cache->capacity * sizeof(uint64_t));
        memcpy(dirty, cache->dirty, (size_t)cache->capacity);
        memcpy(mounted, cache->mounted, (size_t)cache->capacity);
    }
    free(cache->heights);
    free(cache->height_tree);
    free(cache->revisions);
    free(cache->dirty);
    free(cache->mounted);
    cache->heights = heights;
    cache->height_tree = tree;
    /* Fenwick nodes at a new capacity depend on heights in the old prefix. */
    for (int i = 0; i < cache->count; i++)
    {
        double h = heights[i] > 0.5f ? heights[i] : PICO_TRANSCRIPT_ESTIMATED_HEIGHT;
        for (int j = i + 1; j <= capacity; j += j & -j) tree[j] += h;
    }
    cache->revisions = revisions;
    cache->dirty = dirty;
    cache->mounted = mounted;
    cache->capacity = capacity;
    return true;
}

void PicoTranscriptVirtual_Free(PicoTranscriptVirtual *cache)
{
    if (!cache)
    {
        return;
    }
    free(cache->heights);
    free(cache->height_tree);
    free(cache->revisions);
    free(cache->dirty);
    free(cache->mounted);
    memset(cache, 0, sizeof(*cache));
}

static void HeightChange(PicoTranscriptVirtual *cache, int index, double delta)
{
    for (int j = index + 1; j <= cache->capacity; j += j & -j)
        cache->height_tree[j] += delta;
}

static double HeightPrefix(const PicoTranscriptVirtual *cache, int end)
{
    double sum = 0;
    for (int j = end; j > 0; j -= j & -j) sum += cache->height_tree[j];
    return sum;
}

void PicoTranscriptVirtual_Begin(PicoTranscriptVirtual *cache, uint64_t identity,
                                 int count, float width, float font_scale)
{
    if (!cache)
    {
        return;
    }
    if (count < 0)
    {
        count = 0;
    }
    if (!Reserve(cache, count))
    {
        cache->count = 0;
        return;
    }

    bool reset = !cache->configured || cache->identity != identity ||
                 count < cache->count || !SameGeometry(cache, width, font_scale);
    int old_count = reset ? 0 : cache->count;
    if (reset)
    {
        /* Buffers stay NULL when nothing was ever reserved; zero-length
         * memsets must not pass those NULL pointers to memset. */
        if (count > 0)
        {
            memset(cache->heights, 0, (size_t)count * sizeof(float));
            memset(cache->revisions, 0, (size_t)count * sizeof(uint64_t));
            memset(cache->dirty, 1, (size_t)count);
        }
        cache->measure_all = count > 0;
        cache->measure_cursor = 0;
        cache->dirty_count = count;
        if (cache->height_tree)
            memset(cache->height_tree, 0, ((size_t)cache->capacity + 1) * sizeof(double));
        for (int i = 0; i < count; i++)
            HeightChange(cache, i, PICO_TRANSCRIPT_ESTIMATED_HEIGHT);
    }
    else if (count > old_count)
    {
        memset(cache->heights + old_count, 0,
               (size_t)(count - old_count) * sizeof(float));
        memset(cache->revisions + old_count, 0,
               (size_t)(count - old_count) * sizeof(uint64_t));
        memset(cache->dirty + old_count, 1, (size_t)(count - old_count));
        cache->dirty_count += count - old_count;
        for (int i = old_count; i < count; i++)
            HeightChange(cache, i, PICO_TRANSCRIPT_ESTIMATED_HEIGHT);
    }
    if (count > 0)
    {
        memset(cache->mounted, 0, (size_t)count);
    }
    cache->identity = identity;
    cache->width = width;
    cache->font_scale = font_scale;
    cache->count = count;
    cache->configured = true;
}

void PicoTranscriptVirtual_SetRevision(PicoTranscriptVirtual *cache, int index,
                                       uint64_t revision)
{
    if (!cache || index < 0 || index >= cache->count)
    {
        return;
    }
    if (revision == 0)
    {
        revision = 1;
    }
    if (cache->revisions[index] != revision)
    {
        cache->revisions[index] = revision;
        if (!cache->dirty[index]) cache->dirty_count++;
        cache->dirty[index] = 1;
    }
}

float PicoTranscriptVirtual_ItemHeight(const PicoTranscriptVirtual *cache, int index)
{
    return cache->heights[index] > 0.5f ? cache->heights[index]
                                         : PICO_TRANSCRIPT_ESTIMATED_HEIGHT;
}

static float ItemHeight(const PicoTranscriptVirtual *cache, int index, float message_gap)
{
    float height = PicoTranscriptVirtual_ItemHeight(cache, index);
    if (index + 1 < cache->count)
    {
        height += message_gap;
    }
    return height;
}

void PicoTranscriptVirtual_Plan(PicoTranscriptVirtual *cache, float scroll_top,
                                float viewport_height, float overscan,
                                int force_index, float message_gap)
{
    if (!cache || cache->count <= 0)
    {
        return;
    }
    if (scroll_top < 0.0f)
    {
        scroll_top = 0.0f;
    }
    if (overscan < 0.0f)
    {
        overscan = 0.0f;
    }
    float visible_from = scroll_top - overscan;
    float visible_to = scroll_top + viewport_height + overscan;
    if (visible_from < 0.0f)
    {
        visible_from = 0.0f;
    }

    /* Binary-search cumulative heights, then visit only visible rows. A gap
     * belongs to its preceding row, including the boundary at visible_from. */
    int lo = 0, hi = cache->count;
    while (lo < hi)
    {
        int mid = lo + (hi - lo) / 2;
        double bottom = HeightPrefix(cache, mid + 1) +
                        (double)(mid < cache->count - 1 ? mid + 1 : mid) * message_gap;
        if (bottom < visible_from) lo = mid + 1;
        else hi = mid;
    }
    double y = HeightPrefix(cache, lo) + (double)lo * message_gap;
    for (int i = lo; i < cache->count && y <= visible_to; i++)
    {
        if (viewport_height > 0.5f) cache->mounted[i] = 1;
        y += ItemHeight(cache, i, message_gap);
    }
    if (force_index >= 0 && force_index < cache->count)
        cache->mounted[force_index] = 1;
    /* Visible/forced rows are never delayed. Spend a bounded additional budget
     * on dirty offscreen rows, continuing where the previous pass stopped. */
    int budget = PICO_TRANSCRIPT_BACKGROUND_BATCH;
    int scanned = 0;
    int cursor = cache->measure_cursor;
    while (budget > 0 && scanned < cache->count)
    {
        if (cache->dirty[cursor] && !cache->mounted[cursor])
        {
            cache->mounted[cursor] = 1;
            budget--;
        }
        cursor = (cursor + 1) % cache->count;
        scanned++;
    }
    cache->measure_cursor = cursor;
}

void PicoTranscriptVirtual_ForceMount(PicoTranscriptVirtual *cache, int index)
{
    if (cache && index >= 0 && index < cache->count) cache->mounted[index] = 1;
}

bool PicoTranscriptVirtual_Mounted(const PicoTranscriptVirtual *cache, int index)
{
    return cache && index >= 0 && index < cache->count && cache->mounted[index] != 0;
}

float PicoTranscriptVirtual_SpanHeight(const PicoTranscriptVirtual *cache,
                                       int begin, int end, float message_gap)
{
    if (!cache)
    {
        return 0.0f;
    }
    if (begin < 0)
    {
        begin = 0;
    }
    if (end > cache->count)
    {
        end = cache->count;
    }
    if (end <= begin)
    {
        return 0.0f;
    }
    double height = HeightPrefix(cache, end) - HeightPrefix(cache, begin);
    int gaps = end - begin - (end == cache->count ? 1 : 0);
    return (float)(height + (double)gaps * message_gap);
}

float PicoTranscriptVirtual_AnchorDelta(const PicoTranscriptVirtual *cache,
                                        int index, float new_height,
                                        float scroll_top, float message_gap)
{
    if (!cache || index < 0 || index >= cache->count ||
        new_height <= 0.5f)
    {
        return 0.0f;
    }
    float top = PicoTranscriptVirtual_SpanHeight(cache, 0, index, message_gap);
    float previous = cache->heights[index] > 0.5f
                         ? cache->heights[index] : PICO_TRANSCRIPT_ESTIMATED_HEIGHT;
    float bottom = top + previous;
    if (bottom > scroll_top + 0.01f)
    {
        return 0.0f;
    }
    return new_height - previous;
}

void PicoTranscriptVirtual_RecordHeight(PicoTranscriptVirtual *cache, int index,
                                        float height)
{
    if (!cache || index < 0 || index >= cache->count || height <= 0.5f)
    {
        return;
    }
    double old = PicoTranscriptVirtual_ItemHeight(cache, index);
    cache->heights[index] = height;
    HeightChange(cache, index, (double)height - old);
    if (cache->dirty[index]) cache->dirty_count--;
    cache->dirty[index] = 0;
}

void PicoTranscriptVirtual_FinishMeasure(PicoTranscriptVirtual *cache)
{
    if (!cache || !cache->measure_all)
    {
        return;
    }
    if (cache->dirty_count == 0) cache->measure_all = false;
}