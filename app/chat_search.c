#include "chat_search.h"

#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

static void FreeFold(PicoSearchFold *f)
{
    free(f->text);
    free(f->from);
    free(f->to);
    memset(f, 0, sizeof(*f));
}

static bool Fold(const char *text, PicoSearchFold *out)
{
    PicoSearchFold f = {0};
    int len = (int)strlen(text);
    int capacity = len + 1;
    f.text = malloc((size_t)capacity);
    if (!f.text) return false;
    for (int pos = 0; pos < len;)
    {
        utf8proc_int32_t cp;
        int bytes = (int)utf8proc_iterate((const utf8proc_uint8_t *)text + pos, len - pos, &cp);
        if (bytes <= 0) { cp = 0xfffd; bytes = 1; }
        utf8proc_int32_t folded[4];
        int n = (int)utf8proc_decompose_char(cp, folded, 4, UTF8PROC_CASEFOLD, NULL);
        if (n < 0 || n > 4) { FreeFold(&f); return false; }
        char encoded[16];
        int encoded_length = 0;
        for (int i = 0; i < n; i++)
            encoded_length += (int)utf8proc_encode_char(folded[i], (utf8proc_uint8_t *)encoded + encoded_length);
        if (f.count + encoded_length + 1 > capacity)
        {
            capacity = (f.count + encoded_length + 1) * 2;
            char *next = realloc(f.text, (size_t)capacity);
            if (!next) { FreeFold(&f); return false; }
            f.text = next;
            if (f.from)
            {
                int *from = realloc(f.from, (size_t)capacity * sizeof(*from));
                if (!from) { FreeFold(&f); return false; }
                f.from = from;
                int *to = realloc(f.to, (size_t)capacity * sizeof(*to));
                if (!to) { FreeFold(&f); return false; }
                f.to = to;
            }
        }
        /* Ordinary case changes preserve UTF-8 byte offsets. Only expansions
         * or width changes need maps; an ASCII transcript costs one byte per
         * input byte, not three arrays of UTF-32 upper bounds. */
        if (!f.from && (n != 1 || encoded_length != bytes))
        {
            f.from = malloc((size_t)capacity * sizeof(*f.from));
            f.to = malloc((size_t)capacity * sizeof(*f.to));
            if (!f.from || !f.to) { FreeFold(&f); return false; }
            for (int i = 0; i < f.count; i++) { f.from[i] = i; f.to[i] = i + 1; }
        }
        memcpy(f.text + f.count, encoded, (size_t)encoded_length);
        if (f.from)
            for (int i = 0; i < encoded_length; i++)
            {
                f.from[f.count + i] = pos;
                f.to[f.count + i] = pos + bytes;
            }
        f.count += encoded_length;
        pos += bytes;
    }
    f.text[f.count] = 0;
    FreeFold(out);
    *out = f;
    return true;
}

static void FreeMessage(PicoSearchMessage *m)
{
    free(m->text);
    FreeFold(&m->folded);
    free(m->matches);
    memset(m, 0, sizeof(*m));
}

void PicoChatSearch_Free(PicoChatSearch *s)
{
    for (int i = 0; i < s->message_count; i++) FreeMessage(&s->messages[i]);
    free(s->messages);
    free(s->matches);
    free(s->prefix);
    FreeFold(&s->query);
    memset(s, 0, sizeof(*s));
    s->active = -1;
}

bool PicoChatSearch_Resize(PicoChatSearch *s, int count)
{
    if (count < 0) count = 0;
    if (count == s->message_count) return true;
    for (int i = count; i < s->message_count; i++) FreeMessage(&s->messages[i]);
    if (count > s->message_count)
    {
        PicoSearchMessage *next = realloc(s->messages, (size_t)count * sizeof(*next));
        if (!next) return !(s->failed = true);
        memset(next + s->message_count, 0, (size_t)(count - s->message_count) * sizeof(*next));
        s->messages = next;
    }
    s->message_count = count;
    s->dirty = true;
    return true;
}

bool PicoChatSearch_SetText(PicoChatSearch *s, int message, const char *text)
{
    if (message < 0 || message >= s->message_count) return false;
    PicoSearchMessage *m = &s->messages[message];
    if (!text) text = "";
    if (m->text && strcmp(m->text, text) == 0) return true;
    char *copy = strdup(text);
    if (!copy) return !(s->failed = true);
    free(m->text);
    m->text = copy;
    FreeFold(&m->folded);
    m->dirty = s->dirty = true;
    return true;
}

bool PicoChatSearch_Query(PicoChatSearch *s, const char *query)
{
    if (!Fold(query ? query : "", &s->query)) return !(s->failed = true);
    int n = s->query.count;
    int *prefix = n ? malloc((size_t)n * sizeof(*prefix)) : NULL;
    if (n && !prefix) return !(s->failed = true);
    free(s->prefix);
    s->prefix = prefix;
    if (n) prefix[0] = 0;
    for (int i = 1, j = 0; i < n; i++)
    {
        while (j && s->query.text[i] != s->query.text[j]) j = prefix[j - 1];
        if (s->query.text[i] == s->query.text[j]) j++;
        prefix[i] = j;
    }
    for (int i = 0; i < s->message_count; i++) s->messages[i].dirty = true;
    s->active = -1;
    s->dirty = true;
    s->failed = false;
    return true;
}

static bool MatchMessage(PicoChatSearch *s, int message)
{
    PicoSearchMessage *m = &s->messages[message];
    m->count = 0;
    int qn = s->query.count;
    if (!qn || !m->text)
    {
        FreeFold(&m->folded);
        free(m->matches);
        m->matches = NULL;
        return true;
    }
    if (!m->folded.text && !Fold(m->text, &m->folded)) return false;
    int capacity = 0;
    free(m->matches);
    m->matches = NULL;
    for (int i = 0, j = 0, end = 0; i < m->folded.count; i++)
    {
        if ((m->folded.from ? m->folded.from[i] : i) < end) continue;
        while (j && m->folded.text[i] != s->query.text[j]) j = s->prefix[j - 1];
        if (m->folded.text[i] == s->query.text[j]) j++;
        if (j != qn) continue;
        if (m->count == capacity)
        {
            capacity = capacity ? capacity * 2 : 8;
            PicoChatMatch *next = realloc(m->matches, (size_t)capacity * sizeof(*next));
            if (!next) return false;
            m->matches = next;
        }
        end = m->folded.to ? m->folded.to[i] : i + 1;
        int from = m->folded.from ? m->folded.from[i - qn + 1] : i - qn + 1;
        m->matches[m->count++] = (PicoChatMatch){message, from, end};
        j = 0; /* Non-overlapping in original-text coordinates, including folds. */
    }
    return true;
}

bool PicoChatSearch_Refresh(PicoChatSearch *s)
{
    if (s->failed) { s->count = 0; s->active = -1; return false; }
    if (!s->dirty) return true;
    PicoChatMatch old = s->active >= 0 && s->active < s->count
                            ? s->matches[s->active] : (PicoChatMatch){-1, 0, 0};
    int total = 0;
    for (int i = 0; i < s->message_count; i++)
    {
        if (s->messages[i].dirty && !MatchMessage(s, i))
        {
            s->failed = true;
            s->count = 0;
            s->active = -1;
            return false;
        }
        s->messages[i].dirty = false;
        total += s->messages[i].count;
    }
    PicoChatMatch *next = total ? malloc((size_t)total * sizeof(*next)) : NULL;
    if (total && !next) { s->failed = true; s->count = 0; s->active = -1; return false; }
    int at = 0;
    int active = -1;
    for (int i = 0; i < s->message_count; i++)
    {
        PicoSearchMessage *m = &s->messages[i];
        for (int j = 0; j < m->count; j++)
        {
            next[at] = m->matches[j];
            if (old.message >= 0 && active < 0 &&
                (next[at].message > old.message ||
                 (next[at].message == old.message && next[at].from >= old.from))) active = at;
            at++;
        }
    }
    free(s->matches);
    s->matches = next;
    s->count = total;
    s->active = total ? (active >= 0 ? active : total - 1) : -1;
    s->dirty = false;
    return true;
}

void PicoChatSearch_Step(PicoChatSearch *s, int direction)
{
    if (s->count <= 0) { s->active = -1; return; }
    s->active = (s->active + (direction < 0 ? -1 : 1) + s->count) % s->count;
}
