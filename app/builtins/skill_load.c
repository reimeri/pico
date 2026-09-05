#define _POSIX_C_SOURCE 200809L

#include "skill_load.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SKILL_ERR_CAP 256

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static char *DupRange(const char *s, size_t len)
{
    char *out = (char *)malloc(len + 1);
    if (!out)
    {
        return NULL;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static char *DupTrimmed(const char *s, size_t len)
{
    while (len > 0 && (s[0] == ' ' || s[0] == '\t'))
    {
        s++;
        len--;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
    {
        len--;
    }
    return DupRange(s, len);
}

/* Trims trailing spaces, tabs, and newlines in place. */
static void RTrim(char *s)
{
    size_t n = s ? strlen(s) : 0;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
    {
        s[--n] = '\0';
    }
}

static char *ReadFileLimited(const char *path, size_t limit, size_t *out_len)
{
    if (out_len)
    {
        *out_len = 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size > limit)
    {
        fclose(f);
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    char *buf = (char *)malloc(size + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len)
    {
        *out_len = n;
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* Frontmatter extraction                                              */
/* ------------------------------------------------------------------ */

/* A line qualifies as a "---" fence when it contains only hyphens and
 * trailing whitespace. */
static bool IsFence(const char *line, size_t len)
{
    size_t i = 0;
    while (i < len && line[i] == '-')
    {
        i++;
    }
    if (i != 3)
    {
        return false;
    }
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
    {
        i++;
    }
    return i == len;
}

/* Splits text into frontmatter and body regions. The file must open with a
 * "---" fence line and contain a closing fence line. */
static bool SplitFrontmatter(const char *text, size_t len, size_t *fm_off, size_t *fm_len,
                             size_t *body_off, char *err, size_t err_cap)
{
    size_t pos = 0;
    size_t line_no = 0;
    bool opened = false;
    while (pos <= len)
    {
        const char *line = text + pos;
        const char *nl = (const char *)memchr(line, '\n', len - pos);
        size_t line_len = nl ? (size_t)(nl - line) : len - pos;
        size_t next = nl ? pos + line_len + 1 : len + 1;
        size_t trimmed = line_len;
        if (trimmed > 0 && line[trimmed - 1] == '\r')
        {
            trimmed--;
        }
        line_no++;
        if (!opened)
        {
            if (!IsFence(line, trimmed))
            {
                snprintf(err, err_cap, "line %zu: SKILL.md must start with a --- frontmatter fence",
                         line_no);
                return false;
            }
            opened = true;
            *fm_off = next;
        }
        else if (IsFence(line, trimmed))
        {
            *fm_len = pos - *fm_off;
            *body_off = next <= len ? next : len;
            return true;
        }
        if (!nl)
        {
            break;
        }
        pos = next;
    }
    snprintf(err, err_cap, "missing closing --- frontmatter fence");
    return false;
}

/* ------------------------------------------------------------------ */
/* YAML-subset parser                                                  */
/* ------------------------------------------------------------------ */

typedef struct SkillParser {
    const char *cur;
    const char *end;
    int line;
    char *err;
    size_t err_cap;
} SkillParser;

static void SetErr(SkillParser *p, const char *fmt, ...)
{
    if (p->err[0])
    {
        return; /* keep the first error */
    }
    char msg[SKILL_ERR_CAP];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    snprintf(p->err, p->err_cap, "line %d: %s", p->line, msg);
}

/* Returns the current line without its newline and advances. */
static bool NextLine(SkillParser *p, const char **line, size_t *len)
{
    if (p->cur >= p->end)
    {
        return false;
    }
    const char *start = p->cur;
    const char *nl = (const char *)memchr(start, '\n', (size_t)(p->end - start));
    const char *line_end = nl ? nl : p->end;
    p->cur = nl ? nl + 1 : p->end;
    p->line++;
    size_t n = (size_t)(line_end - start);
    if (n > 0 && start[n - 1] == '\r')
    {
        n--;
    }
    *line = start;
    *len = n;
    return true;
}

static SkillParser Save(const SkillParser *p)
{
    return *p;
}

static void Restore(SkillParser *p, const SkillParser *saved)
{
    p->cur = saved->cur;
    p->line = saved->line;
}

/* Leading spaces of a line; -1 when the indent contains a tab. */
static int IndentOf(const char *line, size_t len)
{
    int n = 0;
    while ((size_t)n < len && line[n] == ' ')
    {
        n++;
    }
    if ((size_t)n < len && line[n] == '\t')
    {
        return -1;
    }
    return n;
}

static bool IsBlank(const char *line, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (line[i] != ' ' && line[i] != '\t')
        {
            return false;
        }
    }
    return true;
}

static bool IsComment(const char *line, size_t len)
{
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
    {
        i++;
    }
    return i < len && line[i] == '#';
}

/* True when the trimmed line looks like a "key:" mapping entry. */
static bool IsMappingLine(const char *line, size_t len)
{
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
    {
        i++;
    }
    bool saw_key = false;
    for (; i < len; i++)
    {
        if (line[i] == ':')
        {
            return saw_key && (i + 1 >= len || line[i + 1] == ' ' || line[i + 1] == '\t');
        }
        if (!isalnum((unsigned char)line[i]) && line[i] != '-' && line[i] != '_' && line[i] != '.' &&
            line[i] != ' ' && line[i] != '\t')
        {
            return false; /* quoted or otherwise exotic key: treat as scalar */
        }
        if (line[i] != ' ' && line[i] != '\t')
        {
            saw_key = true;
        }
    }
    return false;
}

/* Strips a " # comment" suffix from a plain scalar range. */
static size_t StripComment(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (s[i] == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))
        {
            len = i;
            break;
        }
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
    {
        len--;
    }
    return len;
}

/* Grows *buf by appending text; returns false on allocation failure. */
static bool Append(char **buf, size_t *len, size_t *cap, const char *text, size_t text_len)
{
    if (*len + text_len + 1 > *cap)
    {
        size_t want = (*cap ? *cap : 64);
        while (*len + text_len + 1 > want)
        {
            want *= 2;
        }
        char *next = (char *)realloc(*buf, want);
        if (!next)
        {
            return false;
        }
        *buf = next;
        *cap = want;
    }
    memcpy(*buf + *len, text, text_len);
    *len += text_len;
    (*buf)[*len] = '\0';
    return true;
}

/* Folds deeper-indented continuation lines into a plain scalar, joined by
 * single spaces. A deeper-indented mapping line is an error. */
static bool FoldContinuation(SkillParser *p, char **buf, size_t *len, size_t *cap, int key_indent)
{
    for (;;)
    {
        SkillParser saved = Save(p);
        const char *line;
        size_t line_len;
        if (!NextLine(p, &line, &line_len))
        {
            return true;
        }
        int indent = IndentOf(line, line_len);
        if (indent < 0)
        {
            SetErr(p, "tab indentation is not supported");
            return false;
        }
        if (IsBlank(line, line_len) || indent <= key_indent)
        {
            Restore(p, &saved);
            return true;
        }
        if (IsComment(line, line_len))
        {
            continue;
        }
        if (IsMappingLine(line, line_len))
        {
            SetErr(p, "nested mappings are not supported here");
            return false;
        }
        size_t trimmed = StripComment(line, line_len);
        const char *text = line;
        size_t text_len = trimmed;
        while (text_len > 0 && (*text == ' ' || *text == '\t'))
        {
            text++;
            text_len--;
        }
        if (*len > 0 && !Append(buf, len, cap, " ", 1))
        {
            SetErr(p, "out of memory");
            return false;
        }
        if (!Append(buf, len, cap, text, text_len))
        {
            SetErr(p, "out of memory");
            return false;
        }
    }
}

/* Parses a single- or double-quoted scalar that must close on the same
 * line. s[0] is the opening quote. */
static char *ParseQuoted(SkillParser *p, const char *s, size_t len)
{
    char quote = s[0];
    char *out = NULL;
    size_t out_len = 0, out_cap = 0;
    size_t i = 1;
    bool closed = false;
    while (i < len)
    {
        char c = s[i];
        if (c == quote)
        {
            if (quote == '\'' && i + 1 < len && s[i + 1] == '\'')
            {
                if (!Append(&out, &out_len, &out_cap, "'", 1))
                {
                    goto oom;
                }
                i += 2;
                continue;
            }
            closed = true;
            i++;
            break;
        }
        if (quote == '"' && c == '\\')
        {
            if (i + 1 >= len)
            {
                break;
            }
            char esc = s[i + 1];
            char decoded;
            switch (esc)
            {
            case 'n': decoded = '\n'; break;
            case 't': decoded = '\t'; break;
            case 'r': decoded = '\r'; break;
            case '"': decoded = '"'; break;
            case '\'': decoded = '\''; break;
            case '\\': decoded = '\\'; break;
            case '/': decoded = '/'; break;
            default:
                SetErr(p, "unsupported escape \\%c in double-quoted scalar", esc);
                free(out);
                return NULL;
            }
            if (!Append(&out, &out_len, &out_cap, &decoded, 1))
            {
                goto oom;
            }
            i += 2;
            continue;
        }
        if (!Append(&out, &out_len, &out_cap, &c, 1))
        {
            goto oom;
        }
        i++;
    }
    if (!closed)
    {
        SetErr(p, "unterminated quoted scalar (multi-line quoted scalars are not supported)");
        free(out);
        return NULL;
    }
    /* Only trailing whitespace and an optional comment may follow. */
    size_t rest = StripComment(s + i, len - i);
    for (size_t j = 0; j < rest; j++)
    {
        if (s[i + j] != ' ' && s[i + j] != '\t')
        {
            SetErr(p, "unexpected content after quoted scalar");
            free(out);
            return NULL;
        }
    }
    if (!out)
    {
        out = DupRange("", 0);
    }
    return out;
oom:
    SetErr(p, "out of memory");
    free(out);
    return NULL;
}

/* Parses a "|" or ">" block scalar. rest points at the indicator. */
static char *ParseBlock(SkillParser *p, const char *rest, size_t rest_len, int key_indent)
{
    bool literal = rest[0] == '|';
    char chomp = '\0';
    size_t i = 1;
    if (i < rest_len && (rest[i] == '-' || rest[i] == '+'))
    {
        chomp = rest[i];
        i++;
    }
    size_t tail = StripComment(rest + i, rest_len - i);
    for (size_t j = 0; j < tail; j++)
    {
        if (rest[i + j] != ' ' && rest[i + j] != '\t')
        {
            SetErr(p, "unsupported block scalar indicator");
            return NULL;
        }
    }

    char *out = NULL;
    size_t out_len = 0, out_cap = 0;
    int content_indent = -1;
    bool last_was_text = false;
    bool any = false;
    for (;;)
    {
        SkillParser saved = Save(p);
        const char *line;
        size_t line_len;
        if (!NextLine(p, &line, &line_len))
        {
            break;
        }
        if (IsBlank(line, line_len))
        {
            /* Harmless even when the block has ended: the outer loop would
             * skip blank lines anyway. Only record as content once the block
             * has started and more content may follow. */
            if (content_indent >= 0)
            {
                if (!Append(&out, &out_len, &out_cap, "\n", 1))
                {
                    goto oom;
                }
                last_was_text = false;
            }
            continue;
        }
        int indent = IndentOf(line, line_len);
        if (indent < 0)
        {
            SetErr(p, "tab indentation is not supported");
            goto fail;
        }
        if (indent <= key_indent)
        {
            Restore(p, &saved);
            break;
        }
        if (content_indent < 0)
        {
            content_indent = indent;
        }
        if (indent < content_indent)
        {
            SetErr(p, "inconsistent block scalar indentation");
            goto fail;
        }
        const char *text = line + content_indent;
        size_t text_len = line_len - (size_t)content_indent;
        if (!literal && last_was_text)
        {
            if (!Append(&out, &out_len, &out_cap, " ", 1))
            {
                goto oom;
            }
        }
        if (!Append(&out, &out_len, &out_cap, text, text_len))
        {
            goto oom;
        }
        if (literal && !Append(&out, &out_len, &out_cap, "\n", 1))
        {
            goto oom;
        }
        last_was_text = true;
        any = true;
    }
    if (!out && !(out = DupRange("", 0)))
    {
        goto oom;
    }
    /* Chomping: strip removes trailing newlines, clip keeps exactly one. */
    if (chomp != '+')
    {
        while (out_len > 0 && out[out_len - 1] == '\n')
        {
            out[--out_len] = '\0';
        }
        if (chomp != '-' && any)
        {
            if (!Append(&out, &out_len, &out_cap, "\n", 1))
            {
                goto oom;
            }
        }
    }
    return out;
oom:
    SetErr(p, "out of memory");
fail:
    free(out);
    return NULL;
}

/* Parses the scalar value of a key line. rest is the text after "key:". */
static char *ParseScalar(SkillParser *p, const char *rest, size_t rest_len, int key_indent)
{
    while (rest_len > 0 && (*rest == ' ' || *rest == '\t'))
    {
        rest++;
        rest_len--;
    }
    if (rest_len > 0 && (*rest == '"' || *rest == '\''))
    {
        return ParseQuoted(p, rest, rest_len);
    }
    if (rest_len > 0 && (*rest == '|' || *rest == '>'))
    {
        return ParseBlock(p, rest, rest_len, key_indent);
    }
    size_t init_len = StripComment(rest, rest_len);
    char *out = DupTrimmed(rest, init_len);
    if (!out)
    {
        SetErr(p, "out of memory");
        return NULL;
    }
    size_t len = strlen(out), cap = len + 1;
    if (!FoldContinuation(p, &out, &len, &cap, key_indent))
    {
        free(out);
        return NULL;
    }
    return out;
}

/* Consumes the value of an unknown key: any deeper-indented content. */
static void SkipUnknown(SkillParser *p, int key_indent)
{
    for (;;)
    {
        SkillParser saved = Save(p);
        const char *line;
        size_t line_len;
        if (!NextLine(p, &line, &line_len))
        {
            return;
        }
        if (IsBlank(line, line_len))
        {
            continue;
        }
        int indent = IndentOf(line, line_len);
        if (indent <= key_indent)
        {
            Restore(p, &saved);
            return;
        }
    }
}

static bool MetaPush(SkillParser *p, PicoSkill *out, char *key, char *value)
{
    for (int i = 0; i < out->meta_count; i++)
    {
        if (strcmp(out->meta[i].key, key) == 0)
        {
            SetErr(p, "duplicate metadata key '%s'", key);
            free(key);
            free(value);
            return false;
        }
    }
    PicoSkillMeta *next =
        (PicoSkillMeta *)realloc(out->meta, sizeof(PicoSkillMeta) * (size_t)(out->meta_count + 1));
    if (!next)
    {
        SetErr(p, "out of memory");
        free(key);
        free(value);
        return false;
    }
    out->meta = next;
    out->meta[out->meta_count].key = key;
    out->meta[out->meta_count].value = value;
    out->meta_count++;
    return true;
}

/* Parses the nested string map after "metadata:". */
static bool ParseMetaMap(SkillParser *p, int key_indent, PicoSkill *out)
{
    int map_indent = -1;
    for (;;)
    {
        SkillParser saved = Save(p);
        const char *line;
        size_t line_len;
        if (!NextLine(p, &line, &line_len))
        {
            return true;
        }
        if (IsBlank(line, line_len) || IsComment(line, line_len))
        {
            continue;
        }
        int indent = IndentOf(line, line_len);
        if (indent < 0)
        {
            SetErr(p, "tab indentation is not supported");
            return false;
        }
        if (indent <= key_indent)
        {
            Restore(p, &saved);
            return true;
        }
        if (map_indent < 0)
        {
            map_indent = indent;
        }
        else if (indent != map_indent)
        {
            SetErr(p, "inconsistent metadata indentation");
            return false;
        }
        const char *colon = (const char *)memchr(line, ':', line_len);
        if (!colon)
        {
            SetErr(p, "expected a key: value metadata entry");
            return false;
        }
        char *key = DupTrimmed(line, (size_t)(colon - line));
        if (!key || !key[0])
        {
            free(key);
            SetErr(p, "empty metadata key");
            return false;
        }
        const char *rest = colon + 1;
        size_t rest_len = line_len - (size_t)(rest - line);
        while (rest_len > 0 && (*rest == ' ' || *rest == '\t'))
        {
            rest++;
            rest_len--;
        }
        char *value;
        if (rest_len > 0 && (*rest == '"' || *rest == '\''))
        {
            value = ParseQuoted(p, rest, rest_len);
        }
        else if (rest_len > 0 && (*rest == '|' || *rest == '>'))
        {
            value = NULL;
            SetErr(p, "block scalars are not supported in metadata");
        }
        else
        {
            value = DupTrimmed(rest, StripComment(rest, rest_len));
            if (value)
            {
                RTrim(value);
            }
        }
        if (!value)
        {
            free(key);
            if (!p->err[0])
            {
                SetErr(p, "out of memory");
            }
            return false;
        }
        if (!MetaPush(p, out, key, value))
        {
            return false;
        }
    }
}

enum {
    KEY_NAME = 1 << 0,
    KEY_DESCRIPTION = 1 << 1,
    KEY_LICENSE = 1 << 2,
    KEY_COMPATIBILITY = 1 << 3,
    KEY_METADATA = 1 << 4,
    KEY_ALLOWED_TOOLS = 1 << 5,
};

static bool ParseFrontmatter(SkillParser *p, PicoSkill *out)
{
    int seen = 0;
    const char *line;
    size_t line_len;
    while (NextLine(p, &line, &line_len))
    {
        if (IsBlank(line, line_len) || IsComment(line, line_len))
        {
            continue;
        }
        int indent = IndentOf(line, line_len);
        if (indent < 0)
        {
            SetErr(p, "tab indentation is not supported");
            return false;
        }
        if (indent != 0)
        {
            SetErr(p, "unexpected indentation");
            return false;
        }
        const char *colon = (const char *)memchr(line, ':', line_len);
        if (!colon)
        {
            SetErr(p, "expected a key: value entry");
            return false;
        }
        char *key = DupTrimmed(line, (size_t)(colon - line));
        if (!key || !key[0])
        {
            free(key);
            SetErr(p, "empty key");
            return false;
        }
        const char *rest = colon + 1;
        size_t rest_len = line_len - (size_t)(rest - line);
        if (rest_len > 0 && *rest != ' ' && *rest != '\t')
        {
            SetErr(p, "expected a space after ':'");
            free(key);
            return false;
        }

        struct {
            const char *name;
            int bit;
            char **slot;
        } fields[] = {
            {"name", KEY_NAME, &out->name},
            {"description", KEY_DESCRIPTION, &out->description},
            {"license", KEY_LICENSE, &out->license},
            {"compatibility", KEY_COMPATIBILITY, &out->compatibility},
            {"allowed-tools", KEY_ALLOWED_TOOLS, &out->allowed_tools},
        };
        bool handled = false;
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        {
            if (strcmp(key, fields[i].name) != 0)
            {
                continue;
            }
            handled = true;
            if (seen & fields[i].bit)
            {
                SetErr(p, "duplicate key '%s'", key);
                free(key);
                return false;
            }
            seen |= fields[i].bit;
            char *value = ParseScalar(p, rest, rest_len, indent);
            if (!value)
            {
                free(key);
                return false;
            }
            RTrim(value);
            *fields[i].slot = value;
            break;
        }
        if (handled)
        {
            free(key);
            continue;
        }
        if (strcmp(key, "metadata") == 0)
        {
            free(key);
            if (seen & KEY_METADATA)
            {
                SetErr(p, "duplicate key 'metadata'");
                return false;
            }
            seen |= KEY_METADATA;
            const char *trimmed = rest;
            size_t trimmed_len = rest_len;
            while (trimmed_len > 0 && (*trimmed == ' ' || *trimmed == '\t'))
            {
                trimmed++;
                trimmed_len--;
            }
            if (trimmed_len > 0)
            {
                SetErr(p, "metadata must be a nested key: value map");
                return false;
            }
            if (!ParseMetaMap(p, indent, out))
            {
                return false;
            }
            continue;
        }
        /* Unknown keys are ignored so newer spec fields stay loadable. */
        free(key);
        SkipUnknown(p, indent);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool PicoSkill_NameValid(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (len < 1 || len > 64)
    {
        return false;
    }
    if (name[0] == '-' || name[len - 1] == '-')
    {
        return false;
    }
    for (size_t i = 0; i < len; i++)
    {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok)
        {
            return false;
        }
        if (c == '-' && i + 1 < len && name[i + 1] == '-')
        {
            return false;
        }
    }
    return true;
}

static bool Validate(PicoSkill *skill, const char *dir_name, char *err, size_t err_cap)
{
    if (!skill->name || !PicoSkill_NameValid(skill->name))
    {
        snprintf(err, err_cap, "invalid or missing skill name (1-64 chars of [a-z0-9-], no "
                               "leading/trailing/consecutive hyphens)");
        return false;
    }
    if (dir_name && strcmp(skill->name, dir_name) != 0)
    {
        snprintf(err, err_cap, "skill name '%s' does not match directory name '%s'", skill->name,
                 dir_name);
        return false;
    }
    if (!skill->description || !skill->description[0])
    {
        snprintf(err, err_cap, "skill '%s' is missing a description", skill->name);
        return false;
    }
    if (strlen(skill->description) > 1024)
    {
        snprintf(err, err_cap, "skill '%s' description exceeds 1024 characters", skill->name);
        return false;
    }
    size_t compat_len = skill->compatibility ? strlen(skill->compatibility) : 0;
    if (skill->compatibility && (compat_len < 1 || compat_len > 500))
    {
        snprintf(err, err_cap, "skill '%s' compatibility must be 1-500 characters", skill->name);
        return false;
    }
    return true;
}

bool PicoSkill_Parse(const char *text, size_t len, const char *dir_name, PicoSkill *out, char *err,
                     size_t err_cap)
{
    memset(out, 0, sizeof(*out));
    if (err_cap > 0)
    {
        err[0] = '\0';
    }
    size_t fm_off = 0, fm_len = 0, body_off = 0;
    if (!SplitFrontmatter(text, len, &fm_off, &fm_len, &body_off, err, err_cap))
    {
        return false;
    }
    SkillParser p = {
        .cur = text + fm_off,
        .end = text + fm_off + fm_len,
        .line = 1,
        .err = err,
        .err_cap = err_cap,
    };
    if (!ParseFrontmatter(&p, out) || !Validate(out, dir_name, err, err_cap))
    {
        PicoSkill_Free(out);
        return false;
    }
    return true;
}

bool PicoSkill_Load(const char *dir, PicoSkill *out, char *err, size_t err_cap)
{
    char path[4096];
    size_t dir_len = strlen(dir);
    while (dir_len > 0 && dir[dir_len - 1] == '/')
    {
        dir_len--;
    }
    if (dir_len == 0 || dir_len + sizeof("/SKILL.md") > sizeof(path))
    {
        snprintf(err, err_cap, "invalid skill directory path");
        return false;
    }
    int n = snprintf(path, sizeof(path), "%.*s/SKILL.md", (int)dir_len, dir);
    if (n < 0 || (size_t)n >= sizeof(path))
    {
        snprintf(err, err_cap, "skill path too long");
        return false;
    }
    size_t len = 0;
    char *text = ReadFileLimited(path, PICO_SKILL_FILE_MAX, &len);
    if (!text)
    {
        snprintf(err, err_cap, "cannot read SKILL.md (missing, unreadable, or over %d bytes)",
                 PICO_SKILL_FILE_MAX);
        return false;
    }
    const char *dir_name = dir + dir_len;
    while (dir_name > dir && dir_name[-1] != '/')
    {
        dir_name--;
    }
    char name_buf[256];
    size_t name_len = (size_t)(dir + dir_len - dir_name);
    if (name_len >= sizeof(name_buf))
    {
        free(text);
        snprintf(err, err_cap, "skill directory name too long");
        return false;
    }
    memcpy(name_buf, dir_name, name_len);
    name_buf[name_len] = '\0';
    bool ok = PicoSkill_Parse(text, len, name_buf, out, err, err_cap);
    free(text);
    if (ok)
    {
        out->dir = DupRange(dir, dir_len);
        if (!out->dir)
        {
            PicoSkill_Free(out);
            snprintf(err, err_cap, "out of memory");
            return false;
        }
    }
    return ok;
}

static int FindIndex(const PicoSkillCatalog *cat, const char *name)
{
    for (int i = 0; i < cat->count; i++)
    {
        if (strcmp(cat->skills[i].name, name) == 0)
        {
            return i;
        }
    }
    return -1;
}

const PicoSkill *PicoSkillCatalog_Find(const PicoSkillCatalog *cat, const char *name)
{
    int idx = cat ? FindIndex(cat, name) : -1;
    return idx >= 0 ? &cat->skills[idx] : NULL;
}

static int CompareSkills(const void *a, const void *b)
{
    return strcmp(((const PicoSkill *)a)->name, ((const PicoSkill *)b)->name);
}

static void ScanDir(PicoSkillCatalog *cat, const char *dir, bool workspace, PicoSkillWarnFn warn,
                    void *ctx)
{
    DIR *d = opendir(dir);
    if (!d)
    {
        return;
    }
    struct dirent *de;
    char path[4096];
    while ((de = readdir(d)) != NULL)
    {
        if (de->d_name[0] == '.')
        {
            continue;
        }
        int n = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
        {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        {
            continue;
        }
        char md[4096];
        n = snprintf(md, sizeof(md), "%s/SKILL.md", path);
        if (n < 0 || (size_t)n >= sizeof(md))
        {
            continue;
        }
        struct stat md_st;
        if (stat(md, &md_st) != 0 || !S_ISREG(md_st.st_mode))
        {
            continue;
        }
        PicoSkill skill;
        char err[SKILL_ERR_CAP];
        if (!PicoSkill_Load(path, &skill, err, sizeof(err)))
        {
            if (warn)
            {
                warn(path, err, ctx);
            }
            continue;
        }
        skill.workspace = workspace;
        int idx = FindIndex(cat, skill.name);
        if (idx >= 0)
        {
            if (workspace && !cat->skills[idx].workspace)
            {
                PicoSkill_Free(&cat->skills[idx]);
                cat->skills[idx] = skill;
            }
            else
            {
                if (warn)
                {
                    warn(path, "duplicate skill name; ignoring", ctx);
                }
                PicoSkill_Free(&skill);
            }
            continue;
        }
        if (cat->count >= PICO_SKILLS_MAX)
        {
            if (warn)
            {
                warn(path, "skill limit reached; ignoring", ctx);
            }
            PicoSkill_Free(&skill);
            continue;
        }
        cat->skills[cat->count++] = skill;
    }
    closedir(d);
}

int PicoSkillCatalog_Scan(PicoSkillCatalog *cat, const char *global_dir, const char *workspace_dir,
                          PicoSkillWarnFn warn, void *ctx)
{
    PicoSkillCatalog_Clear(cat);
    if (global_dir)
    {
        ScanDir(cat, global_dir, false, warn, ctx);
    }
    if (workspace_dir)
    {
        ScanDir(cat, workspace_dir, true, warn, ctx);
    }
    qsort(cat->skills, (size_t)cat->count, sizeof(PicoSkill), CompareSkills);
    return cat->count;
}

void PicoSkill_Free(PicoSkill *skill)
{
    if (!skill)
    {
        return;
    }
    free(skill->name);
    free(skill->description);
    free(skill->license);
    free(skill->compatibility);
    free(skill->allowed_tools);
    for (int i = 0; i < skill->meta_count; i++)
    {
        free(skill->meta[i].key);
        free(skill->meta[i].value);
    }
    free(skill->meta);
    free(skill->dir);
    memset(skill, 0, sizeof(*skill));
}

void PicoSkillCatalog_Clear(PicoSkillCatalog *cat)
{
    if (!cat)
    {
        return;
    }
    for (int i = 0; i < cat->count; i++)
    {
        PicoSkill_Free(&cat->skills[i]);
    }
    cat->count = 0;
}

char *PicoSkill_ReadBody(const PicoSkill *skill, size_t *out_len)
{
    if (out_len)
    {
        *out_len = 0;
    }
    if (!skill || !skill->dir)
    {
        return NULL;
    }
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/SKILL.md", skill->dir);
    if (n < 0 || (size_t)n >= sizeof(path))
    {
        return NULL;
    }
    size_t len = 0;
    char *text = ReadFileLimited(path, PICO_SKILL_FILE_MAX, &len);
    if (!text)
    {
        return NULL;
    }
    char err[SKILL_ERR_CAP];
    size_t fm_off = 0, fm_len = 0, body_off = 0;
    const char *body = text;
    size_t body_len = len;
    if (SplitFrontmatter(text, len, &fm_off, &fm_len, &body_off, err, sizeof(err)))
    {
        body = text + body_off;
        body_len = len - body_off;
    }
    /* Skip leading blank lines between the fence and the content. */
    while (body_len > 0 && (*body == '\n' || *body == '\r' || *body == ' ' || *body == '\t'))
    {
        body++;
        body_len--;
    }
    char *out = DupRange(body, body_len);
    if (out && out_len)
    {
        *out_len = body_len;
    }
    free(text);
    return out;
}
