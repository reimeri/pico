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
    uint32_t *mount_stamps = (uint32_t *)calloc((size_t)capacity, sizeof(*mount_stamps));
    int *mount_indices = (int *)malloc((size_t)capacity * sizeof(*mount_indices));
    int *dirty_next = (int *)malloc((size_t)capacity * sizeof(*dirty_next));
    int *dirty_prev = (int *)malloc((size_t)capacity * sizeof(*dirty_prev));
    int *timed_indices = (int *)malloc((size_t)capacity * sizeof(*timed_indices));
    int *timed_positions = (int *)calloc((size_t)capacity, sizeof(*timed_positions));
    if (!heights || !tree || !revisions || !dirty || !mount_stamps ||
        !mount_indices || !dirty_next || !dirty_prev ||
        !timed_indices || !timed_positions)
    {
        free(heights);
        free(tree);
        free(revisions);
        free(dirty);
        free(mount_stamps);
        free(mount_indices);
        free(dirty_next);
        free(dirty_prev);
        free(timed_indices);
        free(timed_positions);
        return false;
    }
    if (cache->capacity > 0)
    {
        memcpy(heights, cache->heights, (size_t)cache->capacity * sizeof(float));
        memcpy(revisions, cache->revisions, (size_t)cache->capacity * sizeof(uint64_t));
        memcpy(dirty, cache->dirty, (size_t)cache->capacity);
        memcpy(mount_stamps, cache->mount_stamps, (size_t)cache->capacity * sizeof(*mount_stamps));
        memcpy(mount_indices, cache->mount_indices, (size_t)cache->mount_count * sizeof(*mount_indices));
        memcpy(dirty_next, cache->dirty_next, (size_t)cache->capacity * sizeof(*dirty_next));
        memcpy(dirty_prev, cache->dirty_prev, (size_t)cache->capacity * sizeof(*dirty_prev));
        memcpy(timed_indices, cache->timed_indices, (size_t)cache->timed_count * sizeof(*timed_indices));
        memcpy(timed_positions, cache->timed_positions,
               (size_t)cache->capacity * sizeof(*timed_positions));
    }
    free(cache->heights);
    free(cache->height_tree);
    free(cache->revisions);
    free(cache->dirty);
    free(cache->mount_stamps);
    free(cache->mount_indices);
    free(cache->dirty_next);
    free(cache->dirty_prev);
    free(cache->timed_indices);
    free(cache->timed_positions);
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
    cache->mount_stamps = mount_stamps;
    cache->mount_indices = mount_indices;
    cache->dirty_next = dirty_next;
    cache->dirty_prev = dirty_prev;
    cache->timed_indices = timed_indices;
    cache->timed_positions = timed_positions;
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
    free(cache->mount_stamps);
    free(cache->mount_indices);
    free(cache->dirty_next);
    free(cache->dirty_prev);
    free(cache->timed_indices);
    free(cache->timed_positions);
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

static void DirtyAdd(PicoTranscriptVirtual *cache, int index)
{
    if (cache->dirty[index]) return;
    cache->dirty[index] = 1;
    cache->dirty_count++;
    cache->dirty_prev[index] = cache->dirty_tail;
    cache->dirty_next[index] = -1;
    if (cache->dirty_tail >= 0) cache->dirty_next[cache->dirty_tail] = index;
    else cache->dirty_head = index;
    cache->dirty_tail = index;
}

static void DirtyRemove(PicoTranscriptVirtual *cache, int index)
{
    if (!cache->dirty[index]) return;
    int prev = cache->dirty_prev[index], next = cache->dirty_next[index];
    if (prev >= 0) cache->dirty_next[prev] = next;
    else cache->dirty_head = next;
    if (next >= 0) cache->dirty_prev[next] = prev;
    else cache->dirty_tail = prev;
    cache->dirty[index] = 0;
    cache->dirty_count--;
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
        cache->configured = false;
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
            memset(cache->dirty, 0, (size_t)count);
        }
        cache->measure_all = count > 0;
        cache->dirty_head = cache->dirty_tail = -1;
        cache->dirty_count = 0;
        cache->seen_change_seq = 0;
        cache->timed_count = 0;
        if (count > 0) memset(cache->timed_positions, 0, (size_t)count * sizeof(*cache->timed_positions));
        for (int i = 0; i < count; i++) DirtyAdd(cache, i);
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
        memset(cache->timed_positions + old_count, 0,
               (size_t)(count - old_count) * sizeof(*cache->timed_positions));
        memset(cache->dirty + old_count, 0, (size_t)(count - old_count));
        for (int i = old_count; i < count; i++) DirtyAdd(cache, i);
        for (int i = old_count; i < count; i++)
            HeightChange(cache, i, PICO_TRANSCRIPT_ESTIMATED_HEIGHT);
    }
    cache->mount_count = 0;
    cache->mount_sorted = true;
    if (++cache->mount_epoch == 0)
    {
        memset(cache->mount_stamps, 0, (size_t)cache->capacity * sizeof(*cache->mount_stamps));
        cache->mount_epoch = 1;
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
        DirtyAdd(cache, index);
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
        if (viewport_height > 0.5f) PicoTranscriptVirtual_ForceMount(cache, i);
        y += ItemHeight(cache, i, message_gap);
    }
    if (force_index >= 0 && force_index < cache->count)
        PicoTranscriptVirtual_ForceMount(cache, force_index);
    /* Visible/forced rows are never delayed. Measure the oldest offscreen
     * invalidations, without searching through clean history. Do not dequeue
     * until Harvest records a valid height: unsuccessful mounts retry. */
    int budget = PICO_TRANSCRIPT_BACKGROUND_BATCH;
    for (int i = cache->dirty_head; i >= 0 && budget > 0; i = cache->dirty_next[i])
    {
        if (!PicoTranscriptVirtual_Mounted(cache, i))
        {
            PicoTranscriptVirtual_ForceMount(cache, i);
            budget--;
        }
    }
}

void PicoTranscriptVirtual_ForceMount(PicoTranscriptVirtual *cache, int index)
{
    if (!cache || index < 0 || index >= cache->count ||
        cache->mount_stamps[index] == cache->mount_epoch) return;
    cache->mount_stamps[index] = cache->mount_epoch;
    cache->mount_indices[cache->mount_count++] = index;
    cache->mount_sorted = false;
}

bool PicoTranscriptVirtual_Mounted(const PicoTranscriptVirtual *cache, int index)
{
    return cache && index >= 0 && index < cache->count &&
           cache->mount_stamps[index] == cache->mount_epoch;
}

int PicoTranscriptVirtual_MountedCount(const PicoTranscriptVirtual *cache)
{
    return cache ? cache->mount_count : 0;
}

static int CompareIndices(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int PicoTranscriptVirtual_MountedIndex(PicoTranscriptVirtual *cache, int position)
{
    if (!cache || position < 0 || position >= cache->mount_count) return -1;
    if (!cache->mount_sorted)
    {
        qsort(cache->mount_indices, (size_t)cache->mount_count,
              sizeof(*cache->mount_indices), CompareIndices);
        cache->mount_sorted = true;
    }
    return cache->mount_indices[position];
}

void PicoTranscriptVirtual_WatchTimed(PicoTranscriptVirtual *cache, int index, bool watching)
{
    if (!cache || index < 0 || index >= cache->count) return;
    int slot = cache->timed_positions[index] - 1;
    if (watching && slot < 0)
    {
        cache->timed_positions[index] = ++cache->timed_count;
        cache->timed_indices[cache->timed_count - 1] = index;
    }
    else if (!watching && slot >= 0)
    {
        int last = cache->timed_indices[--cache->timed_count];
        cache->timed_indices[slot] = last;
        cache->timed_positions[last] = slot + 1;
        cache->timed_positions[index] = 0;
    }
}

int PicoTranscriptVirtual_TimedCount(const PicoTranscriptVirtual *cache)
{
    return cache ? cache->timed_count : 0;
}

int PicoTranscriptVirtual_TimedIndex(const PicoTranscriptVirtual *cache, int position)
{
    return cache && position >= 0 && position < cache->timed_count
               ? cache->timed_indices[position] : -1;
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
    DirtyRemove(cache, index);
}

void PicoTranscriptVirtual_FinishMeasure(PicoTranscriptVirtual *cache)
{
    if (!cache || !cache->measure_all)
    {
        return;
    }
    if (cache->dirty_count == 0) cache->measure_all = false;
}