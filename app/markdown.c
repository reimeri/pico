// markdown.c - parses a markdown file with md4c into the block/chunk IR
// declared in markdown.h.
//
// md4c is SAX-style: enter_block / leave_block / enter_span / leave_span /
// text callbacks. We keep a small stack of open blocks (code, html, quote,
// list) and spans (em, strong, code, link, img) and flatten everything into
// a linear list of MdBlocks.
//
// One wrinkle: md4c emits "tight" list item text directly under the LI
// block (no inner paragraph block), so a collector is pushed for LI blocks
// as well as P/H blocks, and text is always appended to the innermost
// collector.

#include "markdown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md4c/entity.h"
#include "md4c/md4c.h"

// ---------------------------------------------------------------------------
// Arena (pointer-stable segmented bump allocator)

static void SegmentInit(MdArenaSegment *segment, size_t capacity)
{
    segment->memory = (char *)malloc(capacity);
    segment->capacity = segment->memory ? capacity : 0;
    segment->offset = 0;
}

MdArena MdArena_Init(size_t capacity)
{
    MdArena arena = {0};
    SegmentInit(&arena.primary, capacity);
    return arena;
}

void MdArena_Free(MdArena *arena)
{
    free(arena->primary.memory);
    for (int i = 0; i < arena->overflow_count; i++)
    {
        free(arena->overflow[i].memory);
    }
    free(arena->overflow);
    memset(arena, 0, sizeof(*arena));
}

void MdArena_Reset(MdArena *arena)
{
    arena->primary.offset = 0;
    for (int i = 0; i < arena->overflow_count; i++)
    {
        arena->overflow[i].offset = 0;
    }
}

// Allocates from a segment. Returns NULL when there is no room.
static void *SegmentAlloc(MdArenaSegment *segment, size_t size, size_t align)
{
    size_t aligned = (segment->offset + (align - 1)) & ~(align - 1);
    if (aligned + size > segment->capacity)
    {
        return NULL;
    }
    void *result = segment->memory + aligned;
    segment->offset = aligned + size;
    return result;
}

void *MdArena_Alloc(MdArena *arena, size_t size, size_t align)
{
    void *result = SegmentAlloc(&arena->primary, size, align);
    if (result)
    {
        return result;
    }
    // Try the most recent overflow segment (older ones are left to their
    // existing occupants to keep all previous pointers valid).
    if (arena->overflow_count > 0)
    {
        result = SegmentAlloc(&arena->overflow[arena->overflow_count - 1], size, align);
        if (result)
        {
            return result;
        }
    }
    // Allocate a new overflow segment big enough for this request.
    size_t capacity = size + align;
    if (capacity < (1 << 16))
    {
        capacity = 1 << 16;
    }
    if (arena->overflow_count >= arena->overflow_capacity)
    {
        int new_capacity = arena->overflow_capacity == 0 ? 8 : arena->overflow_capacity * 2;
        MdArenaSegment *new_overflow =
            (MdArenaSegment *)realloc(arena->overflow, (size_t)new_capacity * sizeof(MdArenaSegment));
        if (!new_overflow)
        {
            return NULL;
        }
        arena->overflow = new_overflow;
        arena->overflow_capacity = new_capacity;
    }
    MdArenaSegment *segment = &arena->overflow[arena->overflow_count++];
    SegmentInit(segment, capacity);
    return SegmentAlloc(segment, size, align);
}

char *MdArena_Dup(MdArena *arena, const char *text, size_t length)
{
    char *copy = (char *)MdArena_Alloc(arena, length + 1, 8);
    if (!copy)
    {
        return NULL;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

char *MdArena_DupCstr(MdArena *arena, const char *cstr)
{
    return MdArena_Dup(arena, cstr, strlen(cstr));
}

// ---------------------------------------------------------------------------
// UTF-8 / entity helpers

static int Utf8Encode(unsigned int codepoint, char out[4])
{
    if (codepoint < 0x80)
    {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800)
    {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000)
    {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (codepoint >> 18));
    out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = (char)(0x80 | (codepoint & 0x3F));
    return 4;
}

// Decodes "&name;" / "&#1234;" style entities to UTF-8, writing into `out`
// (must have room for at least 9 bytes). Returns the number of bytes
// written, or 0 when the entity is unknown (in which case the caller should
// emit the source text verbatim).
static size_t DecodeEntity(const char *text, MD_SIZE size, char out[9])
{
    if (size < 3 || text[0] != '&')
    {
        return 0;
    }
    if (text[1] == '#')
    {
        // Numeric character reference: &#65; or &#x2603; (md4c only passes
        // these through; the entity table covers named entities only).
        unsigned long codepoint = 0;
        if (size >= 4 && (text[2] == 'x' || text[2] == 'X'))
        {
            for (MD_SIZE i = 3; i < size - 1; i++)
            {
                char ch = text[i];
                unsigned int digit;
                if (ch >= '0' && ch <= '9')
                {
                    digit = (unsigned int)(ch - '0');
                }
                else if (ch >= 'a' && ch <= 'f')
                {
                    digit = (unsigned int)(ch - 'a') + 10;
                }
                else if (ch >= 'A' && ch <= 'F')
                {
                    digit = (unsigned int)(ch - 'A') + 10;
                }
                else
                {
                    return 0;
                }
                codepoint = codepoint * 16 + digit;
                if (codepoint > 0x10FFFF)
                {
                    return 0;
                }
            }
        }
        else
        {
            for (MD_SIZE i = 2; i < size - 1; i++)
            {
                char ch = text[i];
                if (ch < '0' || ch > '9')
                {
                    return 0;
                }
                codepoint = codepoint * 10 + (unsigned long)(ch - '0');
                if (codepoint > 0x10FFFF)
                {
                    return 0;
                }
            }
        }
        // CommonMark maps NUL, noncharacters and other out-of-range values
        // to U+FFFD; UTF-8 surrogates cannot be encoded either.
        if (codepoint == 0 || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        {
            codepoint = 0xFFFD;
        }
        return (size_t)Utf8Encode((unsigned int)codepoint, out);
    }
    const ENTITY *entity = entity_lookup(text + 1, (size_t)size - 2);
    if (!entity)
    {
        return 0;
    }
    size_t written = 0;
    for (int i = 0; i < 2; i++)
    {
        if (entity->codepoints[i] == 0)
        {
            break;
        }
        written += (size_t)Utf8Encode(entity->codepoints[i], out + written);
    }
    return written;
}

// ---------------------------------------------------------------------------
// Growable plain-C buffers (realloc based; freed explicitly)

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} Buffer;

static void BufferAppend(Buffer *buf, const char *text, size_t length)
{
    if (length == 0)
    {
        return;
    }
    if (buf->length + length + 1 > buf->capacity)
    {
        size_t new_capacity = buf->capacity == 0 ? 256 : buf->capacity * 2;
        while (new_capacity < buf->length + length + 1)
        {
            new_capacity *= 2;
        }
        char *new_data = (char *)realloc(buf->data, new_capacity);
        if (!new_data)
        {
            return;
        }
        buf->data = new_data;
        buf->capacity = new_capacity;
    }
    memcpy(buf->data + buf->length, text, length);
    buf->length += length;
    buf->data[buf->length] = '\0';
}

// ---------------------------------------------------------------------------
// Builder state threaded through the md4c callbacks

#define MAX_BLOCK_DEPTH 64
#define MAX_LIST_DEPTH 32
#define MAX_LINK_DEPTH 16
#define MAX_COLLECTOR_DEPTH 16

typedef struct {
    bool ordered;
    int next_number;
} ListFrame;

// Per list item info, used when a LIST_ITEM block finalizes.
typedef struct {
    char *marker;      // arena owned ("•", "1.", ...)
    bool is_task;
    char task_mark;    // 'x' / ' ' for task lists
} ListItemFrame;

// Collects the styled chunks of one paragraph, heading or tight list item.
typedef struct {
    MdChunk *chunks;
    int count;
    int capacity;
    bool img_only; // no text so far except inside a single image span
    int img_count;
    char *img_src; // arena owned; src of the last image span seen
} Collector;

typedef struct {
    MdArena *arena;

    MdBlock *blocks;
    int block_count;
    int block_capacity;

    MD_BLOCKTYPE block_stack[MAX_BLOCK_DEPTH];
    int block_depth;

    ListFrame list_stack[MAX_LIST_DEPTH];
    int list_depth;

    ListItemFrame li_stack[MAX_LIST_DEPTH];
    int li_depth;

    int quote_depth;

    int em_depth;
    int strong_depth;
    bool in_code_span;
    bool in_img_span;
    char *link_stack[MAX_LINK_DEPTH]; // arena owned hrefs
    int link_depth;

    Collector collector_stack[MAX_COLLECTOR_DEPTH];
    int collector_depth;

    Buffer raw_buffer; // code / html block contents
} Builder;

static void AddBlock(Builder *b, MdBlock block)
{
    if (b->block_count >= b->block_capacity)
    {
        int new_capacity = b->block_capacity == 0 ? 32 : b->block_capacity * 2;
        MdBlock *new_blocks = (MdBlock *)realloc(b->blocks, (size_t)new_capacity * sizeof(MdBlock));
        if (!new_blocks)
        {
            return;
        }
        b->blocks = new_blocks;
        b->block_capacity = new_capacity;
    }
    b->blocks[b->block_count++] = block;
}

static MdBlock NewBlock(MdBlockType type)
{
    MdBlock block = {0};
    block.type = type;
    return block;
}

static Collector *TopCollector(Builder *b)
{
    if (b->collector_depth == 0)
    {
        return NULL;
    }
    return &b->collector_stack[b->collector_depth - 1];
}

static void PushCollector(Builder *b)
{
    if (b->collector_depth >= MAX_COLLECTOR_DEPTH)
    {
        return;
    }
    Collector *c = &b->collector_stack[b->collector_depth++];
    memset(c, 0, sizeof(*c));
    c->img_only = true;
}

// Appends text to the innermost collector with the current span styles,
// merging with the previous chunk when styles match.
static void CollectorAppendText(Builder *b, const char *text, size_t length)
{
    Collector *c = TopCollector(b);
    if (!c || length == 0)
    {
        return;
    }
    if (!b->in_img_span)
    {
        c->img_only = false;
    }

    char *link_url = b->link_depth > 0 ? b->link_stack[b->link_depth - 1] : NULL;

    if (c->count > 0)
    {
        MdChunk *last = &c->chunks[c->count - 1];
        if (last->bold == (b->strong_depth > 0) && last->italic == (b->em_depth > 0) &&
            last->code == b->in_code_span && last->link_url == link_url)
        {
            // Grow: old text + new text. Old chunk memory is abandoned
            // inside the arena (the arena only frees as a whole), which is
            // acceptable here.
            char *new_text = (char *)malloc((size_t)last->length + length + 1);
            if (new_text)
            {
                memcpy(new_text, last->text, (size_t)last->length);
                memcpy(new_text + last->length, text, length);
                new_text[last->length + (int)length] = '\0';
                char *arena_copy = MdArena_Dup(b->arena, new_text, (size_t)last->length + length);
                free(new_text);
                if (arena_copy)
                {
                    last->text = arena_copy;
                    last->length += (int)length;
                    return;
                }
            }
        }
    }

    if (c->count >= c->capacity)
    {
        int new_capacity = c->capacity == 0 ? 8 : c->capacity * 2;
        MdChunk *new_chunks = (MdChunk *)realloc(c->chunks, (size_t)new_capacity * sizeof(MdChunk));
        if (!new_chunks)
        {
            return;
        }
        c->chunks = new_chunks;
        c->capacity = new_capacity;
    }
    MdChunk *chunk = &c->chunks[c->count++];
    chunk->text = MdArena_Dup(b->arena, text, length);
    chunk->length = (int)length;
    chunk->bold = b->strong_depth > 0;
    chunk->italic = b->em_depth > 0;
    chunk->code = b->in_code_span;
    chunk->link_url = link_url;
}

// Copies an md4c MD_ATTRIBUTE (href / src) into arena memory, decoding any
// embedded entities.
static char *DupAttribute(Builder *b, const MD_ATTRIBUTE *attr)
{
    if (!attr || attr->size == 0)
    {
        return MdArena_DupCstr(b->arena, "");
    }
    Buffer buf = {0};
    for (size_t i = 0;; i++)
    {
        MD_OFFSET start = attr->substr_offsets[i];
        MD_OFFSET end = attr->substr_offsets[i + 1];
        if (start >= (MD_OFFSET)attr->size && i > 0)
        {
            break;
        }
        if (end > (MD_OFFSET)attr->size)
        {
            end = attr->size;
        }
        switch (attr->substr_types[i])
        {
            case MD_TEXT_ENTITY:
            {
                char decoded[9];
                size_t decoded_len = DecodeEntity(attr->text + start, end - start, decoded);
                if (decoded_len > 0)
                {
                    BufferAppend(&buf, decoded, decoded_len);
                }
                else
                {
                    BufferAppend(&buf, attr->text + start, end - start);
                }
                break;
            }
            case MD_TEXT_NULLCHAR:
            {
                BufferAppend(&buf, "\xEF\xBF\xBD", 3);
                break;
            }
            default:
            {
                BufferAppend(&buf, attr->text + start, end - start);
                break;
            }
        }
        if (end >= (MD_OFFSET)attr->size)
        {
            break;
        }
    }
    char *result = MdArena_Dup(b->arena, buf.data ? buf.data : "", buf.length);
    free(buf.data);
    return result;
}

// ---------------------------------------------------------------------------
// Collector finalization: pops the innermost collector and turns it into a
// block (or nothing, when empty).

static void PopCollector(Builder *b, MdBlockType fallback_type, int heading_level)
{
    if (b->collector_depth == 0)
    {
        return;
    }
    Collector *c = &b->collector_stack[--b->collector_depth];

    // An image that is the sole content of a paragraph becomes a block-level
    // image; everything else keeps its inline chunks.
    if (c->img_only && c->img_count == 1 && c->img_src != NULL && c->count > 0)
    {
        Buffer alt = {0};
        for (int i = 0; i < c->count; i++)
        {
            BufferAppend(&alt, c->chunks[i].text, (size_t)c->chunks[i].length);
        }
        MdBlock block = NewBlock(MDB_IMAGE);
        block.image_path = c->img_src;
        block.image_alt = alt.length > 0 ? MdArena_Dup(b->arena, alt.data, alt.length)
                                         : MdArena_DupCstr(b->arena, "(image)");
        free(alt.data);
        AddBlock(b, block);
        free(c->chunks);
        return;
    }

    if (c->count == 0)
    {
        free(c->chunks);
        return; // empty; nothing to render
    }

    MdBlockType type = fallback_type;
    if (fallback_type == MDB_PARAGRAPH)
    {
        if (b->list_depth > 0)
        {
            type = MDB_LIST_ITEM;
        }
        else if (b->quote_depth > 0)
        {
            type = MDB_QUOTE;
        }
    }

    MdBlock block = NewBlock(type);
    block.heading_level = heading_level;
    block.chunks = c->chunks; // ownership transferred to the block
    block.chunk_count = c->count;
    if (type == MDB_QUOTE)
    {
        block.list_indent = b->quote_depth - 1;
    }
    if (type == MDB_LIST_ITEM && b->li_depth > 0)
    {
        ListItemFrame *frame = &b->li_stack[b->li_depth - 1];
        block.list_indent = b->list_depth - 1;
        block.list_marker = frame->marker;
        block.list_item_task = frame->is_task;
        block.list_item_done = frame->task_mark == 'x' || frame->task_mark == 'X';
    }
    AddBlock(b, block);
    // Note: c->chunks is now owned by the block and freed in MdDocument_Free.
}

// ---------------------------------------------------------------------------
// md4c callbacks

static bool InsideVerbatimBlock(Builder *b)
{
    return b->block_depth > 0 &&
           (b->block_stack[b->block_depth - 1] == MD_BLOCK_CODE ||
            b->block_stack[b->block_depth - 1] == MD_BLOCK_HTML);
}

static int OnEnterBlock(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    Builder *b = (Builder *)userdata;

    if (b->block_depth < MAX_BLOCK_DEPTH)
    {
        b->block_stack[b->block_depth++] = type;
    }

    switch (type)
    {
        case MD_BLOCK_QUOTE:
        {
            b->quote_depth++;
            break;
        }
        case MD_BLOCK_UL:
        {
            // A nested list opens: flush the enclosing list item's pending
            // text so it is emitted before its children (matching md4c's
            // emission order).
            PopCollector(b, MDB_LIST_ITEM, 0);
            if (b->list_depth < MAX_LIST_DEPTH)
            {
                ListFrame frame = {0};
                frame.ordered = false;
                b->list_stack[b->list_depth++] = frame;
            }
            break;
        }
        case MD_BLOCK_OL:
        {
            PopCollector(b, MDB_LIST_ITEM, 0);
            MD_BLOCK_OL_DETAIL *d = (MD_BLOCK_OL_DETAIL *)detail;
            if (b->list_depth < MAX_LIST_DEPTH)
            {
                ListFrame frame = {0};
                frame.ordered = true;
                frame.next_number = d ? d->start : 1;
                b->list_stack[b->list_depth++] = frame;
            }
            break;
        }
        case MD_BLOCK_LI:
        {
            MD_BLOCK_LI_DETAIL *d = (MD_BLOCK_LI_DETAIL *)detail;
            if (b->li_depth < MAX_LIST_DEPTH && b->list_depth > 0)
            {
                ListFrame *frame = &b->list_stack[b->list_depth - 1];
                char marker[16];
                if (frame->ordered)
                {
                    snprintf(marker, sizeof(marker), "%d.", frame->next_number++);
                }
                else
                {
                    snprintf(marker, sizeof(marker), "\xE2\x80\xA2"); // "•"
                }
                ListItemFrame li = {0};
                li.marker = MdArena_DupCstr(b->arena, marker);
                li.is_task = d && d->is_task;
                li.task_mark = d ? d->task_mark : 0;
                b->li_stack[b->li_depth++] = li;
            }
            // Tight list items carry their text directly; loose ones have
            // inner P blocks. Push a collector either way and drop it if
            // unused.
            PushCollector(b);
            break;
        }
        case MD_BLOCK_CODE:
        case MD_BLOCK_HTML:
        {
            memset(&b->raw_buffer, 0, sizeof(b->raw_buffer));
            break;
        }
        case MD_BLOCK_P:
        case MD_BLOCK_H:
        {
            PushCollector(b);
            break;
        }
        default:
            break;
    }
    return 0;
}

static int OnLeaveBlock(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    Builder *b = (Builder *)userdata;

    switch (type)
    {
        case MD_BLOCK_QUOTE:
        {
            if (b->quote_depth > 0)
            {
                b->quote_depth--;
            }
            break;
        }
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
        {
            if (b->list_depth > 0)
            {
                b->list_depth--;
            }
            break;
        }
        case MD_BLOCK_LI:
        {
            PopCollector(b, MDB_LIST_ITEM, 0);
            if (b->li_depth > 0)
            {
                b->li_depth--;
            }
            break;
        }
        case MD_BLOCK_HR:
        {
            AddBlock(b, NewBlock(MDB_HR));
            break;
        }
        case MD_BLOCK_H:
        {
            MD_BLOCK_H_DETAIL *d = (MD_BLOCK_H_DETAIL *)detail;
            PopCollector(b, MDB_HEADING, d ? d->level : 1);
            break;
        }
        case MD_BLOCK_P:
        {
            PopCollector(b, MDB_PARAGRAPH, 0);
            break;
        }
        case MD_BLOCK_CODE:
        {
            // md4c includes the trailing newline of code blocks; strip it.
            while (b->raw_buffer.length > 0 &&
                   (b->raw_buffer.data[b->raw_buffer.length - 1] == '\n' ||
                    b->raw_buffer.data[b->raw_buffer.length - 1] == '\r'))
            {
                b->raw_buffer.data[--b->raw_buffer.length] = '\0';
            }
            MdBlock block = NewBlock(MDB_CODE);
            block.raw_text = b->raw_buffer.length > 0
                                 ? MdArena_Dup(b->arena, b->raw_buffer.data, b->raw_buffer.length)
                                 : MdArena_DupCstr(b->arena, "");
            free(b->raw_buffer.data);
            memset(&b->raw_buffer, 0, sizeof(b->raw_buffer));
            AddBlock(b, block);
            break;
        }
        case MD_BLOCK_HTML:
        {
            MdBlock block = NewBlock(MDB_HTML);
            block.raw_text = b->raw_buffer.length > 0
                                 ? MdArena_Dup(b->arena, b->raw_buffer.data, b->raw_buffer.length)
                                 : MdArena_DupCstr(b->arena, "");
            free(b->raw_buffer.data);
            memset(&b->raw_buffer, 0, sizeof(b->raw_buffer));
            AddBlock(b, block);
            break;
        }
        default:
            break;
    }

    if (b->block_depth > 0)
    {
        b->block_depth--;
    }
    return 0;
}

static int OnEnterSpan(MD_SPANTYPE type, void *detail, void *userdata)
{
    Builder *b = (Builder *)userdata;
    switch (type)
    {
        case MD_SPAN_EM:
        {
            b->em_depth++;
            break;
        }
        case MD_SPAN_STRONG:
        {
            b->strong_depth++;
            break;
        }
        case MD_SPAN_CODE:
        {
            b->in_code_span = true;
            break;
        }
        case MD_SPAN_A:
        {
            MD_SPAN_A_DETAIL *d = (MD_SPAN_A_DETAIL *)detail;
            if (b->link_depth < MAX_LINK_DEPTH)
            {
                b->link_stack[b->link_depth++] = DupAttribute(b, &d->href);
            }
            break;
        }
        case MD_SPAN_IMG:
        {
            MD_SPAN_IMG_DETAIL *d = (MD_SPAN_IMG_DETAIL *)detail;
            b->in_img_span = true;
            Collector *c = TopCollector(b);
            if (c)
            {
                c->img_count++;
                if (c->img_count > 1)
                {
                    c->img_only = false;
                }
                c->img_src = DupAttribute(b, &d->src);
            }
            break;
        }
        default:
            break; // DEL, U, LATEXMATH, WIKILINK: render as plain text
    }
    return 0;
}

static int OnLeaveSpan(MD_SPANTYPE type, void *detail, void *userdata)
{
    (void)detail;
    Builder *b = (Builder *)userdata;
    switch (type)
    {
        case MD_SPAN_EM:
        {
            if (b->em_depth > 0)
            {
                b->em_depth--;
            }
            break;
        }
        case MD_SPAN_STRONG:
        {
            if (b->strong_depth > 0)
            {
                b->strong_depth--;
            }
            break;
        }
        case MD_SPAN_CODE:
        {
            b->in_code_span = false;
            break;
        }
        case MD_SPAN_A:
        {
            if (b->link_depth > 0)
            {
                b->link_depth--;
            }
            break;
        }
        case MD_SPAN_IMG:
        {
            b->in_img_span = false;
            break;
        }
        default:
            break;
    }
    return 0;
}

static int OnText(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata)
{
    Builder *b = (Builder *)userdata;
    if (size == 0)
    {
        return 0;
    }

    // Verbatim blocks (code / html) collect raw text instead of chunks.
    if (InsideVerbatimBlock(b))
    {
        BufferAppend(&b->raw_buffer, text, (size_t)size);
        return 0;
    }

    switch (type)
    {
        case MD_TEXT_NORMAL:
        case MD_TEXT_CODE: // inline `code` span text
        case MD_TEXT_LATEXMATH:
        {
            CollectorAppendText(b, text, (size_t)size);
            break;
        }
        case MD_TEXT_SOFTBR:
        {
            CollectorAppendText(b, " ", 1);
            break;
        }
        case MD_TEXT_BR:
        {
            CollectorAppendText(b, "\n", 1);
            break;
        }
        case MD_TEXT_NULLCHAR:
        {
            CollectorAppendText(b, "\xEF\xBF\xBD", 3);
            break;
        }
        case MD_TEXT_ENTITY:
        {
            char decoded[9];
            size_t decoded_len = DecodeEntity(text, size, decoded);
            if (decoded_len > 0)
            {
                CollectorAppendText(b, decoded, decoded_len);
            }
            else
            {
                CollectorAppendText(b, text, (size_t)size);
            }
            break;
        }
        case MD_TEXT_HTML:
        {
            // Inline raw HTML is disabled via MD_FLAG_NOHTMLSPANS, so this
            // normally never fires; drop it defensively.
            break;
        }
        default:
            break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Public API

MdDocument MdDocument_LoadFile(const char *path)
{
    MdDocument doc = {0};
    doc.arena = MdArena_Init(1 << 20); // 1 MiB; grows via overflow segments

    FILE *file = fopen(path, "rb");
    if (!file)
    {
        doc.load_error = MdArena_DupCstr(&doc.arena, "Could not open file.");
        return doc;
    }
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        doc.load_error = MdArena_DupCstr(&doc.arena, "Could not seek file.");
        return doc;
    }
    long length = ftell(file);
    if (length < 0)
    {
        fclose(file);
        doc.load_error = MdArena_DupCstr(&doc.arena, "Could not determine file size.");
        return doc;
    }
    rewind(file);
    char *contents = (char *)malloc((size_t)length + 1);
    if (!contents)
    {
        fclose(file);
        doc.load_error = MdArena_DupCstr(&doc.arena, "Out of memory reading file.");
        return doc;
    }
    size_t size = fread(contents, 1, (size_t)length, file);
    fclose(file);
    contents[size] = '\0';

    // Strip a UTF-8 BOM if present.
    size_t offset = 0;
    if (size >= 3 && (unsigned char)contents[0] == 0xEF && (unsigned char)contents[1] == 0xBB &&
        (unsigned char)contents[2] == 0xBF)
    {
        offset = 3;
    }

    Builder builder = {0};
    builder.arena = &doc.arena;

    MD_PARSER parser = {0};
    parser.flags = MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_NOHTMLSPANS | MD_FLAG_TASKLISTS |
                   MD_FLAG_STRIKETHROUGH;
    parser.enter_block = OnEnterBlock;
    parser.leave_block = OnLeaveBlock;
    parser.enter_span = OnEnterSpan;
    parser.leave_span = OnLeaveSpan;
    parser.text = OnText;

    int parse_result = md_parse(contents + offset, (MD_SIZE)(size - offset), &parser, &builder);

    // Clean up anything left dangling (e.g. when a callback aborted parsing).
    for (int i = 0; i < builder.collector_depth; i++)
    {
        free(builder.collector_stack[i].chunks);
    }
    free(builder.raw_buffer.data);
    free(contents);

    if (parse_result != 0)
    {
        for (int i = 0; i < builder.block_count; i++)
        {
            free(builder.blocks[i].chunks);
        }
        free(builder.blocks);
        doc.load_error = MdArena_DupCstr(&doc.arena, "Markdown parsing failed.");
        return doc;
    }

    doc.blocks = builder.blocks;
    doc.block_count = builder.block_count;
    return doc;
}

void MdDocument_Free(MdDocument *doc)
{
    for (int i = 0; i < doc->block_count; i++)
    {
        free(doc->blocks[i].chunks);
    }
    free(doc->blocks);
    MdArena_Free(&doc->arena);
    memset(doc, 0, sizeof(*doc));
}
