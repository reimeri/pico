#ifndef MD_APP_H
#define MD_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Document IR built from md4c parsing.
//
// The markdown source is parsed exactly once (on load / on change) into a
// flat list of blocks. Every allocation (blocks, chunks, strings, wrap
// caches) lives in the document arena, so a reload is just "throw the arena
// away and parse again".
// ---------------------------------------------------------------------------

typedef struct MdArenaSegment {
    char *memory;
    size_t capacity;
    size_t offset;
} MdArenaSegment;

// Bump allocator with pointer-stable growth: when the primary segment is
// full, a new overflow segment is allocated (old segments are never moved),
// so pointers into arena memory remain valid for the arena's lifetime.
typedef struct MdArena {
    MdArenaSegment primary;
    MdArenaSegment *overflow;
    int overflow_count;
    int overflow_capacity;
} MdArena;

MdArena MdArena_Init(size_t capacity);
void MdArena_Free(MdArena *arena);
void MdArena_Reset(MdArena *arena);
// Never returns NULL unless the process is out of memory (in which case the
// program is in trouble anyway).
void *MdArena_Alloc(MdArena *arena, size_t size, size_t align);
char *MdArena_Dup(MdArena *arena, const char *text, size_t length);
char *MdArena_DupCstr(MdArena *arena, const char *cstr);

// One contiguous run of text sharing a style. A paragraph is a list of these.
typedef struct MdChunk {
    char *text;     // arena owned, NUL terminated
    int length;
    bool bold;
    bool italic;
    bool code;      // inline code span (`...`)
    char *link_url; // set when the run is inside a link, else NULL
} MdChunk;

typedef enum MdBlockType {
    MDB_PARAGRAPH,
    MDB_HEADING,
    MDB_CODE,    // fenced/indented code block (raw text, mono font)
    MDB_IMAGE,   // image that is the sole content of a paragraph
    MDB_LIST_ITEM,
    MDB_QUOTE,   // paragraph(s) inside a blockquote
    MDB_HR,
    MDB_HTML,    // raw HTML rendered verbatim in a mono box
} MdBlockType;

typedef struct MdBlock {
    MdBlockType type;
    int heading_level;   // 1-6, headings only
    int list_indent;     // nesting depth (list items, quotes)
    char *list_marker;   // "•" or "1." etc. (list items)
    bool list_item_task; // GFM task list item (- [ ] / - [x])
    bool list_item_done; // checkbox state
    MdChunk *chunks;     // paragraphs, headings, list items, quotes
    int chunk_count;
    char *raw_text;      // code blocks, html blocks
    char *image_path;    // BLOCK_IMAGE: source path (relative or absolute)
    char *image_alt;     // BLOCK_IMAGE: alt text fallback
    // Rich text wrap cache (filled in by richtext.c on first layout at a
    // given container width; invalidated when the width changes). Lives in
    // the document arena, so it is freed automatically on reload.
    void *wrap_cache;
} MdBlock;

typedef struct MdDocument {
    MdBlock *blocks;
    int block_count;
    char *load_error; // set when loading/parsing failed; NULL otherwise
    MdArena arena;
} MdDocument;

// Parses markdown from memory into the document IR.
// `src` is not required to be NUL-terminated. Never fails to return a
// document; on failure doc->load_error describes the problem and
// doc->block_count == 0. The document must be freed with MdDocument_Free.
MdDocument MdDocument_Parse(const char *src, size_t length);

// Reads a file and parses it with MdDocument_Parse.
MdDocument MdDocument_LoadFile(const char *path);
void MdDocument_Free(MdDocument *doc);

#endif // MD_APP_H
