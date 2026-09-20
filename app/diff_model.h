#ifndef PICO_DIFF_MODEL_H
#define PICO_DIFF_MODEL_H

#include <stdbool.h>
#include <stdint.h>

/* Data model for the diff viewer: the working-tree diff against HEAD (or the
 * empty tree when unborn), parsed into per-file rows, plus lazily built
 * syntax-highlight spans. Built on the diff extension's worker thread by
 * PicoDiffModel_Capture(); rendered read-only on the main thread. */

typedef enum DiffRowKind {
    ROW_HEADER = 0, /* file section header */
    ROW_HUNK,       /* @@ ... @@ */
    ROW_CTX,
    ROW_ADD,
    ROW_DEL,
    ROW_NOTE,       /* untracked file we could not render (empty/binary/oversized) */
} DiffRowKind;

typedef struct DiffRow {
    DiffRowKind kind;
    const char *text; /* borrowed from the owning model buffer */
    int len;
    /* Byte offset of this row in its syntax-highlight image: the post-image
     * (context + added lines) for ROW_CTX/ROW_ADD, the pre-image (context +
     * deleted lines) for ROW_DEL. -1 for non-source rows. */
    int img_off;
} DiffRow;

/* Opaque highlight span (defined by highlight.h). */
struct PicoHlSpan;

typedef struct DiffFile {
    const char *label; /* borrowed from the owning model buffer */
    int label_len;
    DiffRow *rows;     /* malloc'd */
    int row_count;
    int row_cap;
    /* Lazily built syntax spans over the pre/post image (see DiffRow). */
    bool hl_built;
    struct PicoHlSpan *old_spans; /* malloc'd */
    int old_count;
    struct PicoHlSpan *new_spans; /* malloc'd */
    int new_count;
} DiffFile;

/* Borrowed storage the model keeps alive for rows/labels outside `patch`. */
typedef struct DiffStash {
    char *buf;
    struct DiffStash *next;
} DiffStash;

typedef struct DiffModel {
    DiffFile *files;
    int file_count;
    int file_cap;
    int adds;
    int dels;
    int untracked;  /* files with no tracked counterpart (empty/binary/oversized included) */
    bool is_repo;
    char workspace[4096]; /* captured from; AdoptPending drops models for other workspaces */
    char *patch;    /* malloc'd; tracked file labels and rows borrow from this */
    DiffStash *stash; /* malloc'd buffers for untracked file labels/rows */
} DiffModel;

/* Runs git (numstat + diff + ls-files) and parses the result. NULL on
 * allocation failure. Highlight spans are NOT built here; call
 * PicoDiffModel_HighlightAll only for models that will be published, so
 * unchanged re-captures stay cheap (see PicoDiffModel_Signature). */
DiffModel *PicoDiffModel_Capture(const char *workspace);

/* Builds syntax-highlight spans for every file. Idempotent. */
void PicoDiffModel_HighlightAll(DiffModel *m);

/* Content hash over everything the UI renders from the model: workspace,
 * counters, and every file's label and row kinds/text. Two captures with
 * equal signatures are interchangeable; the worker uses this to skip
 * re-highlighting and re-publishing when nothing changed. */
uint64_t PicoDiffModel_Signature(const DiffModel *m);

void PicoDiffModel_Free(DiffModel *m);

#endif
