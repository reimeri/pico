#include "diff_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static bool Git(const char *root, const char *args)
{
    char command[8192];
    snprintf(command, sizeof(command), "git -C '%s' -c user.name=PicoTest -c user.email=test@example.invalid %s >/dev/null 2>&1", root, args);
    return system(command) == 0;
}

static void WriteBytes(const char *root, const char *name, const char *body, size_t size)
{
    char path[4096]; snprintf(path, sizeof(path), "%s/%s", root, name);
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file) { CHECK(fwrite(body, 1, size, file) == size); fclose(file); }
}

static bool HasNote(const DiffFile *file, const char *needle)
{
    for (int i = 0; file && i < file->row_count; i++)
    {
        const DiffRow *row = &file->rows[i];
        if (row->kind != ROW_NOTE) continue;
        for (int at = 0; at + (int)strlen(needle) <= row->len; at++)
            if (!memcmp(row->text + at, needle, strlen(needle))) return true;
    }
    return false;
}

static void TestNonTextCapture(void)
{
    char root[] = "/tmp/pico-diff-metadata-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    CHECK(Git(root, "init -q"));
    CHECK(Git(root, "config core.filemode true"));
    const char *binary = "photo \"sample\".png";
    WriteBytes(root, binary, "old\0bytes", 9);
    WriteBytes(root, "old name.txt", "same\n", 5);
    WriteBytes(root, "run.sh", "echo hello\n", 11);
    CHECK(Git(root, "add -A"));
    CHECK(Git(root, "-c commit.gpgsign=false -c core.hooksPath=/dev/null commit -qm initial"));
    WriteBytes(root, binary, "new\0bytes", 9);
    DiffModel *model = PicoDiffModel_Capture(root);
    CHECK(model && model->file_count == 1 && model->adds == 0 && model->dels == 0 &&
          model->files[0].label_len == (int)strlen(binary) &&
          !memcmp(model->files[0].label, binary, strlen(binary)) && HasNote(&model->files[0], "Binary"));
    PicoDiffModel_Free(model);
    CHECK(Git(root, "reset --hard -q HEAD"));
    char old[4096], renamed[4096];
    snprintf(old, sizeof(old), "%s/old name.txt", root);
    snprintf(renamed, sizeof(renamed), "%s/renamed\nfile.txt", root);
    CHECK(rename(old, renamed) == 0);
    CHECK(Git(root, "add -A"));
    model = PicoDiffModel_Capture(root);
    CHECK(model && model->file_count == 1 && model->adds == 0 && model->dels == 0 &&
          model->files[0].label_len == (int)strlen("renamed\nfile.txt") &&
          !memcmp(model->files[0].label, "renamed\nfile.txt", strlen("renamed\nfile.txt")) &&
          HasNote(&model->files[0], "Renamed from old name.txt"));
    PicoDiffModel_Free(model);
    CHECK(Git(root, "reset --hard -q HEAD"));
    snprintf(old, sizeof(old), "%s/run.sh", root);
    CHECK(chmod(old, 0755) == 0);
    model = PicoDiffModel_Capture(root);
    CHECK(model && model->file_count == 1 && model->adds == 0 && model->dels == 0 &&
          HasNote(&model->files[0], "Mode changed"));
    PicoDiffModel_Free(model);
    /* Adding a textual edit must not erase the metadata-only entry. */
    WriteBytes(root, "old name.txt", "different\n", 10);
    model = PicoDiffModel_Capture(root);
    bool metadata = false, text = false;
    for (int i = 0; model && i < model->file_count; i++)
    {
        metadata |= HasNote(&model->files[i], "Mode changed");
        for (int r = 0; r < model->files[i].row_count; r++)
            text |= model->files[i].rows[r].kind == ROW_ADD;
    }
    CHECK(model && model->file_count == 2 && metadata && text && model->adds > 0 && model->dels > 0);
    PicoDiffModel_Free(model);
    CHECK(Git(root, "reset --hard -q HEAD"));
    snprintf(old, sizeof(old), "%s/old name.txt", root);
    CHECK(unlink(old) == 0 && symlink("target", old) == 0);
    WriteBytes(root, "run.sh", "echo changed\n", 13);
    model = PicoDiffModel_Capture(root);
    bool type_note = false, target_added = false, script_added = false;
    for (int i = 0; model && i < model->file_count; i++)
    {
        DiffFile *file = &model->files[i];
        type_note |= HasNote(file, "File type changed");
        for (int r = 0; r < file->row_count; r++)
        {
            DiffRow *row = &file->rows[r];
            if (row->kind != ROW_ADD) continue;
            if (file->label_len == (int)strlen("old name.txt") && !memcmp(file->label, "old name.txt", file->label_len))
                target_added |= row->len == 6 && !memcmp(row->text, "target", 6);
            if (file->label_len == 6 && !memcmp(file->label, "run.sh", 6))
                script_added |= row->len == 12 && !memcmp(row->text, "echo changed", 12);
        }
    }
    CHECK(model && model->file_count == 2 && !model->partial && type_note && target_added && script_added);
    PicoDiffModel_Free(model);
    CHECK(Git(root, "reset --hard -q HEAD"));
    char submodule[4096]; snprintf(submodule, sizeof(submodule), "%s/a-sub", root);
    CHECK(mkdir(submodule, 0700) == 0 && Git(submodule, "init -q"));
    WriteBytes(submodule, "nested.txt", "before\n", 7);
    CHECK(Git(submodule, "add -A"));
    CHECK(Git(submodule, "-c commit.gpgsign=false -c core.hooksPath=/dev/null commit -qm initial"));
    CHECK(Git(root, "add -A"));
    CHECK(Git(root, "-c commit.gpgsign=false -c core.hooksPath=/dev/null commit -qm gitlink"));
    WriteBytes(submodule, "nested.txt", "after\n", 6);
    CHECK(Git(submodule, "add -A"));
    CHECK(Git(submodule, "-c commit.gpgsign=false -c core.hooksPath=/dev/null commit -qm changed"));
    CHECK(Git(root, "config diff.submodule log"));
    WriteBytes(root, "run.sh", "echo changed\n", 13);
    model = PicoDiffModel_Capture(root);
    script_added = false;
    for (int i = 0; model && i < model->file_count; i++)
    {
        DiffFile *file = &model->files[i];
        if (file->label_len != 6 || memcmp(file->label, "run.sh", 6)) continue;
        for (int r = 0; r < file->row_count; r++)
            script_added |= file->rows[r].kind == ROW_ADD && file->rows[r].len == 12 &&
                            !memcmp(file->rows[r].text, "echo changed", 12);
    }
    CHECK(model && model->file_count == 2 && !model->partial && script_added);
    PicoDiffModel_Free(model);
    char cleanup[8192]; snprintf(cleanup, sizeof(cleanup), "rm -rf -- '%s'", root);
    CHECK(system(cleanup) == 0);
}

int main(void)
{
    TestNonTextCapture();
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
    b->partial = true;
    CHECK(PicoDiffModel_Signature(b) != base);
    b->partial = false;
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
