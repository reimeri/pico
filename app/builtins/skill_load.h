#ifndef PICO_SKILL_LOAD_H
#define PICO_SKILL_LOAD_H

/* Agent Skills (agentskills.io) loading: SKILL.md frontmatter parsing,
 * validation, and directory discovery. This unit is independent of the Pico
 * host so it can be unit-tested on its own. */

#include <stdbool.h>
#include <stddef.h>

#define PICO_SKILLS_MAX 64
#define PICO_SKILL_FILE_MAX (512 * 1024)

typedef struct PicoSkillMeta {
    char *key;
    char *value;
} PicoSkillMeta;

typedef struct PicoSkill {
    char *name;          /* validated per spec; equals the directory name */
    char *description;   /* validated, 1-1024 bytes */
    char *license;       /* optional */
    char *compatibility; /* optional, 1-500 bytes when present */
    char *allowed_tools; /* optional; parsed and stored, not enforced */
    PicoSkillMeta *meta; /* optional arbitrary string map */
    int meta_count;
    char *dir;           /* skill directory, as scanned */
} PicoSkill;

typedef struct PicoSkillCatalog {
    PicoSkill skills[PICO_SKILLS_MAX];
    int count;
} PicoSkillCatalog;

/* Reports a skipped skill; path is the skill directory, message the reason. */
typedef void (*PicoSkillWarnFn)(const char *path, const char *message, void *ctx);

/* Spec name rule: 1-64 chars, [a-z0-9-], no leading/trailing/consecutive '-'. */
bool PicoSkill_NameValid(const char *name);

/* Parses SKILL.md text (frontmatter + body) into out. dir_name, when not NULL,
 * must equal the skill name per spec. On success out owns malloc'd fields
 * (free with PicoSkill_Free); on failure returns false and sets err. */
bool PicoSkill_Parse(const char *text, size_t len, const char *dir_name, PicoSkill *out, char *err,
                     size_t err_cap);

/* Loads <dir>/SKILL.md; dir also sets the name-vs-directory check. */
bool PicoSkill_Load(const char *dir, PicoSkill *out, char *err, size_t err_cap);

/* Merges skill directories into cat (sorted by name). NULL or empty entries
 * are skipped. Later directories shadow earlier ones of the same name. Invalid
 * skills are skipped and reported through warn (may be NULL). Returns count. */
int PicoSkillCatalog_Scan(PicoSkillCatalog *cat, const char *const *dirs, int dir_count,
                          PicoSkillWarnFn warn, void *ctx);

const PicoSkill *PicoSkillCatalog_Find(const PicoSkillCatalog *cat, const char *name);

void PicoSkill_Free(PicoSkill *skill);
void PicoSkillCatalog_Clear(PicoSkillCatalog *cat);

/* Reads the skill's SKILL.md fresh from disk and returns the body without the
 * frontmatter (malloc'd, empty string when the body is empty). NULL on I/O
 * error. */
char *PicoSkill_ReadBody(const PicoSkill *skill, size_t *out_len);

#endif
