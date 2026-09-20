#include "diff_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PicoDiffModel_Signature backs the worker's capture dedup: an unchanged
 * working tree must produce the same signature (so the worker skips
 * re-highlighting and re-publishing), and any change visible in the UI must
 * change it. The tests below pin both directions. */

static int g_fails;

#define CHECK(cond)                                                           \
    do                                                                        \
    {                                                                         \
        if (!(cond))                                                          \
        {                                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_fails++;                                                        \
        }                                                                     \
    } while (0)

typedef struct RowSpec {
    DiffRowKind kind;
    const char *text;
} RowSpec;

/* Builds a model with one file; rows borrow from the string literals. */
static DiffModel *MakeModel(const char *workspace, int adds, int dels, int untracked, bool is_repo,
                            const char *label, const RowSpec *specs, int spec_count)
{
    DiffModel *m = calloc(1, sizeof(*m));
    if (!m)
    {
        return NULL;
    }
    snprintf(m->workspace, sizeof(m->workspace), "%s", workspace);
    m->adds = adds;
    m->dels = dels;
    m->untracked = untracked;
    m->is_repo = is_repo;

    m->files = calloc(1, sizeof(DiffFile));
    m->file_count = m->file_cap = 1;
    DiffFile *f = &m->files[0];
    f->label = label;
    f->label_len = (int)strlen(label);
    f->row_cap = spec_count;
    f->rows = calloc((size_t)spec_count, sizeof(DiffRow));
    f->row_count = spec_count;
    for (int i = 0; i < spec_count; i++)
    {
        f->rows[i].kind = specs[i].kind;
        f->rows[i].text = specs[i].text;
        f->rows[i].len = (int)strlen(specs[i].text);
        f->rows[i].img_off = -1;
    }
    return m;
}

int main(void)
{
    const RowSpec rows[] = {
        {ROW_HEADER, "src/main.c"},
        {ROW_HUNK, "@@ -1,3 +1,4 @@"},
        {ROW_CTX, "int main(void)"},
        {ROW_DEL, "    return 1;"},
        {ROW_ADD, "    return 0;"},
    };
    const int n = (int)(sizeof(rows) / sizeof(rows[0]));

    DiffModel *a = MakeModel("/ws", 1, 1, 0, true, "src/main.c", rows, n);
    DiffModel *b = MakeModel("/ws", 1, 1, 0, true, "src/main.c", rows, n);
    CHECK(a && b);
    CHECK(PicoDiffModel_Signature(a) == PicoDiffModel_Signature(b));

    /* Any UI-visible change must flip the signature. */
    struct {
        const char *workspace;
        int adds, dels, untracked;
        bool is_repo;
        const char *label;
        DiffRowKind kind2;   /* replacement kind for row 2 */
        const char *text2;   /* replacement text for row 2 */
    } mutations[] = {
        {"/other", 1, 1, 0, true, "src/main.c", ROW_CTX, "int main(void)"}, /* workspace */
        {"/ws", 2, 1, 0, true, "src/main.c", ROW_CTX, "int main(void)"},    /* adds */
        {"/ws", 1, 2, 0, true, "src/main.c", ROW_CTX, "int main(void)"},    /* dels */
        {"/ws", 1, 1, 1, true, "src/main.c", ROW_CTX, "int main(void)"},    /* untracked */
        {"/ws", 1, 1, 0, false, "src/main.c", ROW_CTX, "int main(void)"},   /* is_repo */
        {"/ws", 1, 1, 0, true, "src/other.c", ROW_CTX, "int main(void)"},   /* label */
        {"/ws", 1, 1, 0, true, "src/main.c", ROW_ADD, "int main(void)"},    /* row kind */
        {"/ws", 1, 1, 0, true, "src/main.c", ROW_CTX, "int main(int argc)"}, /* row text */
    };
    uint64_t base = PicoDiffModel_Signature(a);
    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++)
    {
        RowSpec mut_rows[5];
        memcpy(mut_rows, rows, sizeof(mut_rows));
        mut_rows[2].kind = mutations[i].kind2;
        mut_rows[2].text = mutations[i].text2;
        DiffModel *m = MakeModel(mutations[i].workspace, mutations[i].adds, mutations[i].dels,
                                 mutations[i].untracked, mutations[i].is_repo, mutations[i].label,
                                 mut_rows, n);
        CHECK(m != NULL);
        if (m)
        {
            if (PicoDiffModel_Signature(m) == base)
            {
                fprintf(stderr, "FAIL: mutation %zu kept signature\n", i);
                g_fails++;
            }
            PicoDiffModel_Free(m);
        }
    }

    /* Row count changes are visible too. */
    DiffModel *shorter = MakeModel("/ws", 1, 1, 0, true, "src/main.c", rows, n - 1);
    CHECK(shorter && PicoDiffModel_Signature(shorter) != base);

    /* Signature is stable for NULL (worker polls before the first capture). */
    CHECK(PicoDiffModel_Signature(NULL) == PicoDiffModel_Signature(NULL));

    PicoDiffModel_Free(shorter);
    PicoDiffModel_Free(a);
    PicoDiffModel_Free(b);

    if (g_fails)
    {
        fprintf(stderr, "%d checks failed\n", g_fails);
        return 1;
    }
    puts("diff_model: ok");
    return 0;
}
