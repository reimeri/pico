#define _POSIX_C_SOURCE 200809L

#include "builtins/skill_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
            g_failures++;                                                                      \
        }                                                                                      \
    } while (0)

static bool ParseOk(const char *text, const char *dir_name, PicoSkill *out)
{
    char err[256];
    bool ok = PicoSkill_Parse(text, strlen(text), dir_name, out, err, sizeof(err));
    if (!ok)
    {
        fprintf(stderr, "  parse error: %s\n", err);
    }
    return ok;
}

static bool ParseFails(const char *text, const char *dir_name)
{
    PicoSkill skill;
    char err[256];
    bool ok = PicoSkill_Parse(text, strlen(text), dir_name, &skill, err, sizeof(err));
    if (ok)
    {
        PicoSkill_Free(&skill);
    }
    return !ok;
}

static void TestMinimal(void)
{
    PicoSkill s;
    CHECK(ParseOk("---\nname: my-skill\ndescription: Does things.\n---\n# Body\n", "my-skill", &s));
    CHECK(s.name && strcmp(s.name, "my-skill") == 0);
    CHECK(s.description && strcmp(s.description, "Does things.") == 0);
    CHECK(s.license == NULL && s.compatibility == NULL && s.allowed_tools == NULL);
    CHECK(s.meta_count == 0);
    PicoSkill_Free(&s);
}

static void TestAllFields(void)
{
    const char *text = "---\n"
                       "name: pdf-processing\n"
                       "description: Extract PDF text, fill forms, merge files.\n"
                       "license: Apache-2.0\n"
                       "compatibility: Requires git and docker\n"
                       "allowed-tools: Bash(git:*) Read\n"
                       "metadata:\n"
                       "  author: example-org\n"
                       "  version: \"1.0\"\n"
                       "---\n"
                       "Body text.\n";
    PicoSkill s;
    CHECK(ParseOk(text, "pdf-processing", &s));
    CHECK(strcmp(s.license, "Apache-2.0") == 0);
    CHECK(strcmp(s.compatibility, "Requires git and docker") == 0);
    CHECK(strcmp(s.allowed_tools, "Bash(git:*) Read") == 0);
    CHECK(s.meta_count == 2);
    CHECK(strcmp(s.meta[0].key, "author") == 0 && strcmp(s.meta[0].value, "example-org") == 0);
    CHECK(strcmp(s.meta[1].key, "version") == 0 && strcmp(s.meta[1].value, "1.0") == 0);
    PicoSkill_Free(&s);
}

static void TestQuotedScalars(void)
{
    PicoSkill s;
    CHECK(ParseOk("---\n"
                  "name: quoted\n"
                  "description: \"Extracts: tables # not a comment\"\n"
                  "license: 'It''s ours'\n"
                  "---\n",
                  "quoted", &s));
    CHECK(strcmp(s.description, "Extracts: tables # not a comment") == 0);
    CHECK(strcmp(s.license, "It's ours") == 0);
    PicoSkill_Free(&s);
}

static void TestContinuationFolding(void)
{
    PicoSkill s;
    CHECK(ParseOk("---\n"
                  "name: folded\n"
                  "description: Extracts text\n"
                  "  and tables from PDFs.\n"
                  "license: MIT\n"
                  "---\n",
                  "folded", &s));
    CHECK(strcmp(s.description, "Extracts text and tables from PDFs.") == 0);
    PicoSkill_Free(&s);

    /* Empty rest with an indented plain scalar on the next line. */
    CHECK(ParseOk("---\n"
                  "name: folded\n"
                  "description:\n"
                  "  Extracts text.\n"
                  "---\n",
                  "folded", &s));
    CHECK(strcmp(s.description, "Extracts text.") == 0);
    PicoSkill_Free(&s);
}

static void TestBlockScalars(void)
{
    PicoSkill s;
    CHECK(ParseOk("---\n"
                  "name: blocked\n"
                  "description: |\n"
                  "  line one\n"
                  "  line two\n"
                  "---\n",
                  "blocked", &s));
    CHECK(strcmp(s.description, "line one\nline two") == 0);
    PicoSkill_Free(&s);

    CHECK(ParseOk("---\n"
                  "name: blocked\n"
                  "description: >\n"
                  "  line one\n"
                  "  line two\n"
                  "---\n",
                  "blocked", &s));
    CHECK(strcmp(s.description, "line one line two") == 0);
    PicoSkill_Free(&s);
}

static void TestInlineComment(void)
{
    PicoSkill s;
    CHECK(ParseOk("---\nname: commented # trailing\ndescription: Does things. # why\n---\n",
                  "commented", &s));
    CHECK(strcmp(s.name, "commented") == 0);
    CHECK(strcmp(s.description, "Does things.") == 0);
    PicoSkill_Free(&s);
}

static void TestNameRules(void)
{
    CHECK(PicoSkill_NameValid("a"));
    CHECK(PicoSkill_NameValid("pdf-processing-2"));

    CHECK(ParseFails("---\nname: PDF\ndescription: x\n---\n", NULL));
    CHECK(ParseFails("---\nname: -pdf\ndescription: x\n---\n", NULL));
    CHECK(ParseFails("---\nname: pdf-\ndescription: x\n---\n", NULL));
    CHECK(ParseFails("---\nname: pdf--processing\ndescription: x\n---\n", NULL));
    CHECK(ParseFails("---\nname: pdf_processing\ndescription: x\n---\n", NULL));
    CHECK(ParseFails("---\ndescription: x\n---\n", NULL));

    /* Spec boundary: 64 characters is allowed, 65 is not. */
    char text[512];
    char name65[66];
    memset(name65, 'a', 65);
    name65[65] = '\0';
    snprintf(text, sizeof(text), "---\nname: %s\ndescription: x\n---\n", name65);
    CHECK(ParseFails(text, NULL));
    name65[64] = '\0';
    PicoSkill s;
    snprintf(text, sizeof(text), "---\nname: %s\ndescription: x\n---\n", name65);
    CHECK(ParseOk(text, NULL, &s));
    PicoSkill_Free(&s);

    /* Name must match the parent directory name when one is given. */
    CHECK(ParseFails("---\nname: actual\ndescription: x\n---\n", "other"));
    CHECK(ParseOk("---\nname: actual\ndescription: x\n---\n", "actual", &s));
    PicoSkill_Free(&s);
}

static void TestDescriptionRules(void)
{
    CHECK(ParseFails("---\nname: nodesc\n---\n", NULL));
    CHECK(ParseFails("---\nname: nodesc\ndescription:\n---\n", NULL));

    /* Spec boundary: 1024 characters is allowed, 1025 is not. */
    PicoSkill s;
    char desc[1026];
    memset(desc, 'd', 1025);
    desc[1025] = '\0';
    char text[1200];
    desc[1024] = '\0';
    snprintf(text, sizeof(text), "---\nname: sized\ndescription: %s\n---\n", desc);
    CHECK(ParseOk(text, NULL, &s));
    PicoSkill_Free(&s);
    desc[1024] = 'd';
    desc[1025] = '\0';
    snprintf(text, sizeof(text), "---\nname: sized\ndescription: %s\n---\n", desc);
    CHECK(ParseFails(text, NULL));
}

static void TestCompatibilityRules(void)
{
    PicoSkill s;
    char compat[502];
    memset(compat, 'c', 501);
    compat[501] = '\0';
    char text[1600];
    compat[500] = '\0';
    snprintf(text, sizeof(text), "---\nname: compat\ndescription: x\ncompatibility: %s\n---\n",
             compat);
    CHECK(ParseOk(text, NULL, &s));
    PicoSkill_Free(&s);
    compat[500] = 'c';
    compat[501] = '\0';
    snprintf(text, sizeof(text), "---\nname: compat\ndescription: x\ncompatibility: %s\n---\n",
             compat);
    CHECK(ParseFails(text, NULL));
}

static void TestMalformed(void)
{
    CHECK(ParseFails("name: nofence\ndescription: x\n---\n", NULL));
    CHECK(ParseFails("---\nname: unclosed\ndescription: x\n", NULL));
    CHECK(ParseFails("---\n\tname: tabbed\ndescription: x\n---\n", NULL));
    CHECK(ParseFails("---\nname: dup\ndescription: x\ndescription: y\n---\n", NULL));
    CHECK(ParseFails("---\nname: badmeta\ndescription: x\nmetadata:\n  author: a\n    deeper: b\n---\n",
                     NULL));
    CHECK(ParseFails("---\nname: badnest\ndescription: x\nlicense: MIT\n  nested: nope\n---\n",
                     NULL));
}

static void TestUnknownKeysIgnored(void)
{
    PicoSkill s;
    CHECK(ParseOk("---\n"
                  "name: future\n"
                  "description: x\n"
                  "future-field: whatever\n"
                  "future-map:\n"
                  "  nested: ignored\n"
                  "  list-item: also ignored\n"
                  "---\n",
                  "future", &s));
    CHECK(strcmp(s.name, "future") == 0);
    PicoSkill_Free(&s);
}

/* ------------------------------------------------------------------ */
/* Discovery tests with temporary directories                          */
/* ------------------------------------------------------------------ */

static char g_tmp[256];

static void MakeSkill(const char *root, const char *dir, const char *body)
{
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", root, dir);
    CHECK(mkdir(path, 0755) == 0);
    char file[2048];
    snprintf(file, sizeof(file), "%s/%s/SKILL.md", root, dir);
    FILE *f = fopen(file, "wb");
    CHECK(f != NULL);
    if (f)
    {
        fputs(body, f);
        fclose(f);
    }
}

static int g_warnings;
static void CountWarn(const char *path, const char *message, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "  warn: %s: %s\n", path, message);
    g_warnings++;
}

static void TestDiscovery(void)
{
    snprintf(g_tmp, sizeof(g_tmp), "/tmp/pico-skills-test-XXXXXX");
    CHECK(mkdtemp(g_tmp) != NULL);
    char agents_global[512], pico_global[512], ws_agents[512], ws_pico[512];
    snprintf(agents_global, sizeof(agents_global), "%s/agents-global", g_tmp);
    snprintf(pico_global, sizeof(pico_global), "%s/pico-global", g_tmp);
    snprintf(ws_agents, sizeof(ws_agents), "%s/agents-workspace", g_tmp);
    snprintf(ws_pico, sizeof(ws_pico), "%s/pico-workspace", g_tmp);
    CHECK(mkdir(agents_global, 0755) == 0);
    CHECK(mkdir(pico_global, 0755) == 0);
    CHECK(mkdir(ws_agents, 0755) == 0);
    CHECK(mkdir(ws_pico, 0755) == 0);

    MakeSkill(agents_global, "alpha",
              "---\nname: alpha\ndescription: agents global alpha\n---\nagents global body\n");
    MakeSkill(agents_global, "gamma", "---\nname: gamma\ndescription: agents global gamma\n---\n");
    MakeSkill(pico_global, "alpha",
              "---\nname: alpha\ndescription: pico global alpha\n---\npico global body\n");
    MakeSkill(pico_global, "broken", "---\nname: WRONG\ndescription: x\n---\n");
    MakeSkill(ws_agents, "alpha",
              "---\nname: alpha\ndescription: agents workspace alpha\n---\nagents ws body\n");
    MakeSkill(ws_agents, "delta", "---\nname: delta\ndescription: agents workspace delta\n---\n");
    MakeSkill(ws_pico, "beta", "---\nname: beta\ndescription: pico workspace beta\n---\n");
    MakeSkill(ws_pico, "alpha",
              "---\nname: alpha\ndescription: pico workspace alpha\n---\nws body\n");

    /* A directory without SKILL.md and a stray file must be ignored. */
    char noskill[2048];
    snprintf(noskill, sizeof(noskill), "%s/no-skill-here", ws_pico);
    CHECK(mkdir(noskill, 0755) == 0);
    char stray[2048];
    snprintf(stray, sizeof(stray), "%s/stray-file", ws_pico);
    FILE *f = fopen(stray, "wb");
    CHECK(f != NULL);
    if (f)
    {
        fclose(f);
    }

    PicoSkillCatalog cat;
    memset(&cat, 0, sizeof(cat));
    g_warnings = 0;
    const char *dirs[] = {agents_global, pico_global, ws_agents, ws_pico};
    int n = PicoSkillCatalog_Scan(&cat, dirs, 4, CountWarn, NULL);
    CHECK(n == 4);
    CHECK(cat.count == 4);
    /* Sorted by name; later directories shadow earlier ones of the same name. */
    CHECK(strcmp(cat.skills[0].name, "alpha") == 0);
    CHECK(strcmp(cat.skills[1].name, "beta") == 0);
    CHECK(strcmp(cat.skills[2].name, "delta") == 0);
    CHECK(strcmp(cat.skills[3].name, "gamma") == 0);
    const PicoSkill *alpha = PicoSkillCatalog_Find(&cat, "alpha");
    CHECK(alpha && strcmp(alpha->description, "pico workspace alpha") == 0);
    CHECK(PicoSkillCatalog_Find(&cat, "missing") == NULL);
    CHECK(g_warnings == 1); /* only the invalid pico-global skill */

    /* The body is read without the frontmatter. */
    size_t body_len = 0;
    char *body = PicoSkill_ReadBody(alpha, &body_len);
    CHECK(body != NULL);
    CHECK(body && strcmp(body, "ws body\n") == 0);
    free(body);

    PicoSkillCatalog_Clear(&cat);
    CHECK(cat.count == 0);

    /* Cleanup. */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmp);
    CHECK(system(cmd) == 0);
}

int main(void)
{
    TestMinimal();
    TestAllFields();
    TestQuotedScalars();
    TestContinuationFolding();
    TestBlockScalars();
    TestInlineComment();
    TestNameRules();
    TestDescriptionRules();
    TestCompatibilityRules();
    TestMalformed();
    TestUnknownKeysIgnored();
    TestDiscovery();

    if (g_failures)
    {
        fprintf(stderr, "skills: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("skills: all checks passed\n");
    return 0;
}
