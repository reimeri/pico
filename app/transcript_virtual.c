#include "transcript_virtual.h"

#include <stdlib.h>
#include <string.h>

#define PICO_TRANSCRIPT_ESTIMATED_HEIGHT 96.0f

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
    uint64_t *revisions = (uint64_t *)calloc((size_t)capacity, sizeof(uint64_t));
    unsigned char *dirty = (unsigned char *)calloc((size_t)capacity, 1);
    unsigned char *mounted = (unsigned char *)calloc((size_t)capacity, 1);
    if (!heights || !revisions || !dirty || !mounted)
    {
        free(heights);
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
    free(cache->revisions);
    free(cache->dirty);
    free(cache->mounted);
    cache->heights = heights;
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
    free(cache->revisions);
    free(cache->dirty);
    free(cache->mounted);
    memset(cache, 0, sizeof(*cache));
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
        memset(cache->heights, 0, (size_t)count * sizeof(float));
        memset(cache->revisions, 0, (size_t)count * sizeof(uint64_t));
        memset(cache->dirty, 1, (size_t)count);
        cache->measure_all = count > 0;
    }
    else if (count > old_count)
    {
        memset(cache->heights + old_count, 0,
               (size_t)(count - old_count) * sizeof(float));
        memset(cache->revisions + old_count, 0,
               (size_t)(count - old_count) * sizeof(uint64_t));
        memset(cache->dirty + old_count, 1, (size_t)(count - old_count));
    }
    memset(cache->mounted, 0, (size_t)count);
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
        cache->dirty[index] = 1;
    }
}

static float ItemHeight(const PicoTranscriptVirtual *cache, int index, float message_gap)
{
    float height = cache->heights[index] > 0.5f
                       ? cache->heights[index]
                       : PICO_TRANSCRIPT_ESTIMATED_HEIGHT;
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
    if (cache->measure_all || viewport_height <= 0.5f)
    {
        memset(cache->mounted, 1, (size_t)cache->count);
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

    float y = 0.0f;
    for (int i = 0; i < cache->count; i++)
    {
        float next = y + ItemHeight(cache, i, message_gap);
        if (cache->dirty[i] || i == force_index ||
            (next >= visible_from && y <= visible_to))
        {
            cache->mounted[i] = 1;
        }
        y = next;
    }
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
    float height = 0.0f;
    for (int i = begin; i < end; i++)
    {
        height += ItemHeight(cache, i, message_gap);
    }
    return height;
}

float PicoTranscriptVirtual_AnchorDelta(const PicoTranscriptVirtual *cache,
                                        int index, float new_height,
                                        float scroll_top, float message_gap)
{
    if (!cache || index < 0 || index >= cache->count ||
        cache->heights[index] <= 0.5f || new_height <= 0.5f)
    {
        return 0.0f;
    }
    float top = PicoTranscriptVirtual_SpanHeight(cache, 0, index, message_gap);
    float bottom = top + cache->heights[index];
    if (bottom > scroll_top + 0.01f)
    {
        return 0.0f;
    }
    return new_height - cache->heights[index];
}

void PicoTranscriptVirtual_RecordHeight(PicoTranscriptVirtual *cache, int index,
                                        float height)
{
    if (!cache || index < 0 || index >= cache->count || height <= 0.5f)
    {
        return;
    }
    cache->heights[index] = height;
    cache->dirty[index] = 0;
}

void PicoTranscriptVirtual_FinishMeasure(PicoTranscriptVirtual *cache)
{
    if (!cache || !cache->measure_all)
    {
        return;
    }
    for (int i = 0; i < cache->count; i++)
    {
        if (cache->dirty[i])
        {
            return;
        }
    }
    cache->measure_all = false;
}
