#include "highlight.h"

#include <ctype.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Approximate, table-driven syntax highlighting. Single pass over the whole
 * text; tokens that span lines (block comments, long strings) are consumed
 * in one go, so no resumable state is needed. Only styled text produces
 * spans; gaps render as default text. */

typedef enum PicoHlKind {
    PICO_HL_KIND_TOKEN = 0,
    PICO_HL_KIND_MARKDOWN,
    PICO_HL_KIND_DIFF,
} PicoHlKind;

struct PicoHlLang {
    PicoHlKind kind;
    const char *line_comment;    /* "//", "#", "--"; NULL if none */
    const char *block_open;      /* checked before line_comment (lua --[[) */
    const char *block_close;
    bool block_nesting;          /* rust: block comments nest */
    const char *long_open[2];    /* python """ and ''', lua [[ */
    const char *long_close[2];
    char quotes[4];              /* NUL-terminated quote characters */
    unsigned escape_mask;        /* bit i: quotes[i] honors \ escapes */
    unsigned multiline_mask;     /* bit i: quotes[i] may cross newlines */
    bool hash_directive;         /* c/cpp: # directive at line start */
    bool string_field;           /* json: "key" followed by ':' */
    bool lifetime_quote;         /* rust: disambiguate 'a vs 'a' */
    bool raw_prefix;             /* python/rust: r"..." */
    bool raw_hash;               /* rust: r#"..."# */
    bool raw_paren;              /* c++: R"(...)" */
    bool fn_paren;               /* identifier '(' => FUNC */
    char var_sigil;              /* '$' for bash, 0 otherwise */
    const char *const *keywords;
    const char *const *keywords2; /* second keyword tier (typescript) */
    const char *const *builtins;
};

/* ------------------------------------------------------------------ */
/* Span emission                                                       */

typedef struct HlOut {
    PicoHlSpan *spans;
    int cap;
    int count;
} HlOut;

static void HlEmit(HlOut *out, int start, int end, PicoHlClass class)
{
    if (end <= start)
    {
        return;
    }
    if (out->spans && out->count < out->cap)
    {
        out->spans[out->count] = (PicoHlSpan){start, end, class};
    }
    out->count++;
}

/* ------------------------------------------------------------------ */
/* Generic tokenizer                                                   */

static bool HlIsIdentStart(int c)
{
    return isalpha(c) || c == '_';
}

static bool HlIsIdent(int c)
{
    return isalnum(c) || c == '_';
}

static bool HlMatchAt(const char *s, int len, int pos, const char *delim)
{
    int dlen = (int)strlen(delim);
    return dlen > 0 && pos + dlen <= len && memcmp(s + pos, delim, (size_t)dlen) == 0;
}

static bool HlInList(const char *const *list, const char *word, int word_len)
{
    if (!list)
    {
        return false;
    }
    for (int i = 0; list[i]; i++)
    {
        if ((int)strlen(list[i]) == word_len && memcmp(list[i], word, (size_t)word_len) == 0)
        {
            return true;
        }
    }
    return false;
}

/* Consumes a quoted string starting at s[pos] == quote; returns the end
 * offset (exclusive). Stops at the matching quote, at an unescaped newline
 * for single-line quotes, or at end of text. */
static int HlScanString(const PicoHlLang *L, const char *s, int len, int pos)
{
    char quote = s[pos];
    int qi = -1;
    for (int i = 0; L->quotes[i]; i++)
    {
        if (L->quotes[i] == quote)
        {
            qi = i;
            break;
        }
    }
    bool escapes = qi >= 0 && (L->escape_mask & (1u << qi));
    bool multiline = qi >= 0 && (L->multiline_mask & (1u << qi));
    int i = pos + 1;
    while (i < len)
    {
        char c = s[i];
        if (escapes && c == '\\' && i + 1 < len)
        {
            i += 2;
            continue;
        }
        if (c == quote)
        {
            return i + 1;
        }
        if (c == '\n' && !multiline)
        {
            return i;
        }
        i++;
    }
    return len;
}

/* Rust ': a char literal when a closing quote follows one character or one
 * escape sequence, otherwise a lifetime (left unstyled). Returns the end
 * offset for a char literal, -1 for a lifetime. */
static int HlScanLifetime(const char *s, int len, int pos)
{
    int i = pos + 1;
    if (i >= len)
    {
        return -1;
    }
    if (s[i] == '\\')
    {
        i += 2;
        while (i < len && s[i] != '\'' && s[i] != '\n' && i - pos < 8)
        {
            i++;
        }
        return (i < len && s[i] == '\'') ? i + 1 : -1;
    }
    if (HlIsIdent((unsigned char)s[i]))
    {
        int j = i;
        while (j < len && HlIsIdent((unsigned char)s[j]))
        {
            j++;
        }
        /* 'a' is a char; 'a or 'static is a lifetime */
        return (j - i == 1 && j < len && s[j] == '\'') ? j + 1 : -1;
    }
    /* '*' and friends: a single non-identifier character then a quote */
    return (i + 1 < len && s[i + 1] == '\'') ? i + 2 : -1;
}

static int HlScanBlockComment(const PicoHlLang *L, const char *s, int len, int pos)
{
    int open_len = (int)strlen(L->block_open);
    int close_len = (int)strlen(L->block_close);
    int depth = 1;
    int i = pos + open_len;
    while (i < len)
    {
        if (HlMatchAt(s, len, i, L->block_close))
        {
            i += close_len;
            depth--;
            if (depth == 0)
            {
                return i;
            }
            continue;
        }
        if (L->block_nesting && HlMatchAt(s, len, i, L->block_open))
        {
            depth++;
            i += open_len;
            continue;
        }
        i++;
    }
    return len;
}

static void HlTokenizeLang(const PicoHlLang *L, const char *s, HlOut *out)
{
    int len = (int)strlen(s);
    bool line_start = true; /* only whitespace so far on this line */
    int pos = 0;
    while (pos < len)
    {
        char c = s[pos];
        if (c == '\n')
        {
            line_start = true;
            pos++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r')
        {
            pos++;
            continue;
        }

        int start = pos;

        if (L->block_open && HlMatchAt(s, len, pos, L->block_open))
        {
            pos = HlScanBlockComment(L, s, len, pos);
            HlEmit(out, start, pos, PICO_HL_COMMENT);
        }
        else if (L->line_comment && HlMatchAt(s, len, pos, L->line_comment))
        {
            while (pos < len && s[pos] != '\n')
            {
                pos++;
            }
            HlEmit(out, start, pos, PICO_HL_COMMENT);
        }
        else if (L->hash_directive && c == '#' && line_start)
        {
            while (pos < len && s[pos] != '\n')
            {
                pos++;
            }
            /* a directive line ending in \ continues on the next line */
            while (pos > start && pos < len && s[pos - 1] == '\\')
            {
                pos++;
                while (pos < len && s[pos] != '\n')
                {
                    pos++;
                }
            }
            HlEmit(out, start, pos, PICO_HL_PREPROC);
        }
        else if (L->long_open[0] && (HlMatchAt(s, len, pos, L->long_open[0]) ||
                                     (L->long_open[1] && HlMatchAt(s, len, pos, L->long_open[1]))))
        {
            int idx = HlMatchAt(s, len, pos, L->long_open[0]) ? 0 : 1;
            pos += (int)strlen(L->long_open[idx]);
            while (pos < len && !HlMatchAt(s, len, pos, L->long_close[idx]))
            {
                pos++;
            }
            pos += (int)strlen(L->long_close[idx]);
            if (pos > len)
            {
                pos = len;
            }
            HlEmit(out, start, pos, PICO_HL_STRING);
        }
        else if ((c == 'r' || c == 'R') &&
                 ((L->raw_paren && c == 'R' && pos + 2 < len && s[pos + 1] == '"' && s[pos + 2] == '(') ||
                  (L->raw_hash && c == 'r' && pos + 2 < len && s[pos + 1] == '#' && s[pos + 2] == '"') ||
                  (L->raw_prefix && pos + 1 < len && s[pos + 1] == '"')))
        {
            if (L->raw_paren && c == 'R' && s[pos + 1] == '"')
            {
                /* R"(...)" */
                pos += 3;
                while (pos + 1 < len && !(s[pos] == ')' && s[pos + 1] == '"'))
                {
                    pos++;
                }
                pos = pos + 1 < len ? pos + 2 : len;
            }
            else if (L->raw_hash && c == 'r' && s[pos + 1] == '#')
            {
                /* r#"..."# */
                pos += 3;
                while (pos + 1 < len && !(s[pos] == '"' && s[pos + 1] == '#'))
                {
                    pos++;
                }
                pos = pos + 1 < len ? pos + 2 : len;
            }
            else
            {
                /* r"...": no escape processing */
                pos += 2;
                while (pos < len && s[pos] != '"' && s[pos] != '\n')
                {
                    pos++;
                }
                pos = pos < len ? pos + 1 : len;
            }
            HlEmit(out, start, pos, PICO_HL_STRING);
        }
        else if (strchr(L->quotes, c))
        {
            if (L->lifetime_quote && c == '\'')
            {
                int end = HlScanLifetime(s, len, pos);
                if (end < 0)
                {
                    pos++; /* lifetime: unstyled */
                }
                else
                {
                    pos = end;
                    HlEmit(out, start, pos, PICO_HL_STRING);
                }
            }
            else
            {
                pos = HlScanString(L, s, len, pos);
                PicoHlClass class = PICO_HL_STRING;
                if (L->string_field)
                {
                    int k = pos;
                    while (k < len && (s[k] == ' ' || s[k] == '\t'))
                    {
                        k++;
                    }
                    if (k < len && s[k] == ':')
                    {
                        class = PICO_HL_FIELD;
                    }
                }
                HlEmit(out, start, pos, class);
            }
        }
        else if (L->var_sigil && c == L->var_sigil)
        {
            pos++;
            if (pos < len && s[pos] == '{')
            {
                while (pos < len && s[pos] != '}' && s[pos] != '\n')
                {
                    pos++;
                }
                pos = pos < len ? pos + 1 : len;
            }
            else if (pos < len && HlIsIdentStart((unsigned char)s[pos]))
            {
                while (pos < len && HlIsIdent((unsigned char)s[pos]))
                {
                    pos++;
                }
            }
            else if (pos < len && s[pos] != ' ' && s[pos] != '\t' && s[pos] != '\n')
            {
                pos++; /* $1, $?, $$, $# */
            }
            HlEmit(out, start, pos, PICO_HL_VAR);
        }
        else if (isdigit((unsigned char)c))
        {
            while (pos < len && (isalnum((unsigned char)s[pos]) || s[pos] == '.' || s[pos] == '_' ||
                                 s[pos] == '\''))
            {
                pos++;
            }
            HlEmit(out, start, pos, PICO_HL_NUMBER);
        }
        else if (HlIsIdentStart((unsigned char)c))
        {
            while (pos < len && HlIsIdent((unsigned char)s[pos]))
            {
                pos++;
            }
            int word_len = pos - start;
            if (HlInList(L->keywords, s + start, word_len) ||
                HlInList(L->keywords2, s + start, word_len))
            {
                HlEmit(out, start, pos, PICO_HL_KEYWORD);
            }
            else if (HlInList(L->builtins, s + start, word_len))
            {
                HlEmit(out, start, pos, PICO_HL_TYPE);
            }
            else if (L->fn_paren)
            {
                int k = pos;
                while (k < len && (s[k] == ' ' || s[k] == '\t'))
                {
                    k++;
                }
                if (k < len && s[k] == '(')
                {
                    HlEmit(out, start, pos, PICO_HL_FUNC);
                }
            }
        }
        else
        {
            pos++;
        }

        line_start = false;
    }
}

/* ------------------------------------------------------------------ */
/* Markdown: line structure plus a few inline spans. Fenced regions    */
/* suppress inline highlighting so embedded code samples stay plain.   */

static void HlMarkdownInline(const char *line, int base, int from, int len, HlOut *out)
{
    int i = from;
    while (i < len)
    {
        if (line[i] == '`')
        {
            int run = 1;
            while (i + run < len && line[i + run] == '`')
            {
                run++;
            }
            int j = i + run;
            while (j < len)
            {
                if (line[j] == '`')
                {
                    int close = 1;
                    while (j + close < len && line[j + close] == '`')
                    {
                        close++;
                    }
                    if (close == run)
                    {
                        break;
                    }
                    j += close;
                }
                else
                {
                    j++;
                }
            }
            int end = j < len ? j + run : len;
            HlEmit(out, base + i, base + end, PICO_HL_STRING);
            i = end;
        }
        else if ((line[i] == '*' || line[i] == '_') && i + 2 < len && line[i + 1] == line[i] &&
                 line[i + 2] != ' ')
        {
            char mark = line[i];
            int j = i + 2;
            while (j + 1 < len && !(line[j] == mark && line[j + 1] == mark))
            {
                j++;
            }
            if (j + 1 < len)
            {
                HlEmit(out, base + i, base + j + 2, PICO_HL_TYPE);
                i = j + 2;
            }
            else
            {
                i++;
            }
        }
        else if (line[i] == '[')
        {
            int j = i + 1;
            while (j < len && line[j] != ']')
            {
                j++;
            }
            if (j + 1 < len && line[j] == ']' && line[j + 1] == '(')
            {
                int k = j + 2;
                while (k < len && line[k] != ')')
                {
                    k++;
                }
                HlEmit(out, base + i, base + j + 1, PICO_HL_FIELD);
                if (k < len)
                {
                    HlEmit(out, base + j + 1, base + k + 1, PICO_HL_STRING);
                    i = k + 1;
                }
                else
                {
                    i = j + 1;
                }
            }
            else
            {
                i++;
            }
        }
        else
        {
            i++;
        }
    }
}

static bool HlMdSetext(const char *line, int indent, int len)
{
    /* a line consisting solely of = or - (plus spaces) underlines a heading */
    if (indent >= len || (line[indent] != '=' && line[indent] != '-'))
    {
        return false;
    }
    char c = line[indent];
    for (int i = indent; i < len; i++)
    {
        if (line[i] != c && line[i] != ' ' && line[i] != '\t')
        {
            return false;
        }
    }
    return true;
}

static void HlTokenizeMarkdown(const char *s, HlOut *out)
{
    int len = (int)strlen(s);
    char fence_char = 0;
    int fence_len = 0;
    int pos = 0;
    while (pos < len)
    {
        int line_end = pos;
        while (line_end < len && s[line_end] != '\n')
        {
            line_end++;
        }
        int line_len = line_end - pos;
        const char *line = s + pos;

        int indent = 0;
        while (indent < line_len && (line[indent] == ' ' || line[indent] == '\t'))
        {
            indent++;
        }

        int fence_run = 0;
        if (indent < line_len && (line[indent] == '`' || line[indent] == '~'))
        {
            char fc = line[indent];
            while (indent + fence_run < line_len && line[indent + fence_run] == fc)
            {
                fence_run++;
            }
            if (fence_run < 3)
            {
                fence_run = 0;
            }
        }

        if (fence_char)
        {
            if (fence_run >= fence_len && line[indent] == fence_char)
            {
                HlEmit(out, pos + indent, pos + indent + fence_run, PICO_HL_PREPROC);
                fence_char = 0;
            }
            /* fenced content stays plain */
        }
        else if (fence_run)
        {
            HlEmit(out, pos + indent, pos + line_len, PICO_HL_PREPROC);
            fence_char = line[indent];
            fence_len = fence_run;
        }
        else if (indent < line_len && line[indent] == '#')
        {
            int hashes = 0;
            while (indent + hashes < line_len && line[indent + hashes] == '#')
            {
                hashes++;
            }
            if (hashes <= 6 && indent + hashes < line_len && line[indent + hashes] == ' ')
            {
                HlEmit(out, pos, pos + line_len, PICO_HL_KEYWORD);
            }
            else
            {
                HlMarkdownInline(line, pos, indent, line_len, out);
            }
        }
        else if (HlMdSetext(line, indent, line_len))
        {
            HlEmit(out, pos, pos + line_len, PICO_HL_KEYWORD);
        }
        else if (indent < line_len && line[indent] == '>')
        {
            HlEmit(out, pos, pos + line_len, PICO_HL_COMMENT);
        }
        else
        {
            int inline_from = indent;
            if (indent < line_len &&
                (line[indent] == '-' || line[indent] == '*' || line[indent] == '+') &&
                indent + 1 < line_len && line[indent + 1] == ' ')
            {
                HlEmit(out, pos + indent, pos + indent + 1, PICO_HL_PREPROC);
                inline_from = indent + 1;
            }
            else if (indent < line_len && isdigit((unsigned char)line[indent]))
            {
                int j = indent;
                while (j < line_len && isdigit((unsigned char)line[j]))
                {
                    j++;
                }
                if (j + 1 < line_len && (line[j] == '.' || line[j] == ')') && line[j + 1] == ' ')
                {
                    HlEmit(out, pos + indent, pos + j + 1, PICO_HL_PREPROC);
                    inline_from = j + 1;
                }
            }
            HlMarkdownInline(line, pos, inline_from, line_len, out);
        }

        pos = line_end + 1;
    }
}

/* ------------------------------------------------------------------ */
/* Diff: unified-diff line prefixes                                    */

static void HlTokenizeDiff(const char *s, HlOut *out)
{
    int len = (int)strlen(s);
    int pos = 0;
    while (pos < len)
    {
        int line_end = pos;
        while (line_end < len && s[line_end] != '\n')
        {
            line_end++;
        }
        if (line_end - pos >= 3 && (memcmp(s + pos, "+++", 3) == 0 || memcmp(s + pos, "---", 3) == 0))
        {
            HlEmit(out, pos, line_end, PICO_HL_HUNK);
        }
        else if (line_end - pos >= 2 && s[pos] == '@' && s[pos + 1] == '@')
        {
            HlEmit(out, pos, line_end, PICO_HL_HUNK);
        }
        else if (line_end > pos && s[pos] == '+')
        {
            HlEmit(out, pos, line_end, PICO_HL_ADD);
        }
        else if (line_end > pos && s[pos] == '-')
        {
            HlEmit(out, pos, line_end, PICO_HL_DEL);
        }
        pos = line_end + 1;
    }
}

/* ------------------------------------------------------------------ */
/* Language tables                                                     */

static const char *const KW_C[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
    "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
    "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
    "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Bool",
    "_Static_assert", NULL,
};
static const char *const BI_C[] = {
    "NULL", "bool", "true", "false", "size_t", "ssize_t", "ptrdiff_t", "intptr_t",
    "uintptr_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
    "uint32_t", "uint64_t", "FILE", NULL,
};

static const char *const KW_CPP[] = {
    "alignas", "alignof", "asm", "auto", "break", "case", "catch", "char", "class",
    "concept", "const", "constexpr", "const_cast", "continue", "decltype", "default",
    "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export",
    "extern", "float", "for", "friend", "goto", "if", "inline", "int", "long",
    "mutable", "namespace", "new", "noexcept", "operator", "private", "protected",
    "public", "register", "reinterpret_cast", "requires", "return", "short", "signed",
    "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "template",
    "this", "thread_local", "throw", "try", "typedef", "typeid", "typename", "union",
    "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "override",
    "final", NULL,
};
static const char *const BI_CPP[] = {
    "NULL", "nullptr", "bool", "true", "false", "size_t", "string", "string_view",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
    "uint64_t", NULL,
};

static const char *const KW_PY[] = {
    "and", "as", "assert", "async", "await", "break", "class", "continue", "def",
    "del", "elif", "else", "except", "finally", "for", "from", "global", "if",
    "import", "in", "is", "lambda", "match", "nonlocal", "not", "or", "pass",
    "raise", "return", "try", "while", "with", "yield", NULL,
};
static const char *const BI_PY[] = {
    "True", "False", "None", "self", "cls", "bool", "bytes", "dict", "float", "int",
    "list", "object", "set", "str", "tuple", "type", "print", "len", "range", "open",
    "isinstance", "issubclass", "enumerate", "zip", "map", "filter", "sorted", "sum",
    "min", "max", "abs", "repr", "super", "Exception", "ValueError", "TypeError",
    "KeyError", "IndexError", "RuntimeError", "StopIteration", "NotImplementedError",
    "__init__", "__name__", "__main__", "__file__", NULL,
};

static const char *const KW_JS[] = {
    "async", "await", "break", "case", "catch", "class", "const", "continue",
    "debugger", "default", "delete", "do", "else", "export", "extends", "finally",
    "for", "from", "function", "get", "if", "import", "in", "instanceof", "let",
    "new", "of", "return", "set", "static", "super", "switch", "this", "throw",
    "try", "typeof", "var", "void", "while", "with", "yield", NULL,
};
static const char *const KW_TS[] = {
    "abstract", "any", "as", "asserts", "declare", "enum", "implements", "infer",
    "interface", "is", "keyof", "namespace", "never", "private", "protected",
    "public", "readonly", "satisfies", "type", "unknown", NULL,
};
static const char *const BI_JS[] = {
    "true", "false", "null", "undefined", "NaN", "Infinity", "console", "window",
    "document", "Math", "JSON", "Object", "Array", "String", "Number", "Boolean",
    "Promise", "Error", "TypeError", "Symbol", "BigInt", "Map", "Set", "Date",
    "RegExp", "globalThis", "require", "module", "exports", "process", NULL,
};

static const char *const KW_RUST[] = {
    "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else",
    "enum", "extern", "fn", "for", "if", "impl", "in", "let", "loop", "match",
    "mod", "move", "mut", "pub", "ref", "return", "self", "Self", "static",
    "struct", "super", "trait", "type", "unsafe", "use", "where", "while", NULL,
};
static const char *const BI_RUST[] = {
    "true", "false", "bool", "char", "str", "i8", "i16", "i32", "i64", "i128",
    "isize", "u8", "u16", "u32", "u64", "u128", "usize", "f32", "f64", "String",
    "Vec", "Box", "Option", "Result", "Some", "None", "Ok", "Err", "Copy", "Clone",
    "Default", "Drop", "Send", "Sync", "Sized", "Iterator", "Into", "From", NULL,
};

static const char *const KW_GO[] = {
    "break", "case", "chan", "const", "continue", "default", "defer", "else",
    "fallthrough", "for", "func", "go", "goto", "if", "import", "interface", "map",
    "package", "range", "return", "select", "struct", "switch", "type", "var", NULL,
};
static const char *const BI_GO[] = {
    "true", "false", "nil", "iota", "bool", "byte", "rune", "string", "error", "any",
    "comparable", "int", "int8", "int16", "int32", "int64", "uint", "uint8",
    "uint16", "uint32", "uint64", "uintptr", "float32", "float64", "complex64",
    "complex128", "append", "cap", "close", "copy", "delete", "len", "make", "new",
    "panic", "print", "println", "recover", NULL,
};

static const char *const KW_BASH[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done",
    "case", "esac", "function", "in", "select", "time", NULL,
};
static const char *const BI_BASH[] = {
    "echo", "cd", "exit", "return", "export", "local", "readonly", "declare",
    "unset", "shift", "source", "alias", "eval", "exec", "set", "trap", "printf",
    "read", "true", "false", "test", NULL,
};

static const char *const KW_LUA[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
    "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
    "true", "until", "while", NULL,
};
static const char *const BI_LUA[] = {
    "print", "pairs", "ipairs", "next", "type", "tostring", "tonumber", "string",
    "table", "math", "io", "os", "coroutine", "require", "pcall", "xpcall", "error",
    "setmetatable", "getmetatable", "rawget", "rawset", "rawequal", "select",
    "unpack", "_G", "_VERSION", NULL,
};

static const char *const BI_JSON[] = {
    "true", "false", "null", NULL,
};

static const PicoHlLang LANG_C = {
    .kind = PICO_HL_KIND_TOKEN,
    .line_comment = "//", .block_open = "/*", .block_close = "*/",
    .quotes = "\"'", .escape_mask = 0x3,
    .hash_directive = true, .fn_paren = true,
    .keywords = KW_C, .builtins = BI_C,
};
static const PicoHlLang LANG_CPP = {
    .kind = PICO_HL_KIND_TOKEN,
    .line_comment = "//", .block_open = "/*", .block_close = "*/",
    .quotes = "\"'", .escape_mask = 0x3,
    .hash_directive = true, .raw_paren = true, .fn_paren = true,
    .keywords = KW_CPP, .builtins = BI_CPP,
};
static const PicoHlLang LANG_PY = {
    .kind = PICO_HL_KIND_TOKEN,
    .line_comment = "#",
    .long_open = {"\"\"\"", "'''"}, .long_close = {"\"\"\"", "'''"},
    .quotes = "\"'", .escape_mask = 0x3,
    .raw_prefix = true, .fn_paren = true,
    .keywords = KW_PY, .builtins = BI_PY,
};
static const PicoHlLang LANG_JS = {
    .kind = PICO_HL_KIND_TOKEN,
    .line_comment = "//", .block_open = "/*", .block_close = "*/",
    .quotes = "\"'`", .escape_mask = 0x7, .multiline_mask = 0x4,
    .fn_paren = true,
    .keywords = KW_JS, .builtins = BI_JS,
};
static const PicoHlLang LANG_TS = {
    .kind = PICO_HL_KIND_TOKEN,
    .line_comment = "//", .block_open = "/*", .block_close = "*/",
    .quotes = "\"'`", .escape_mask = 0x7, .multiline_mask = 0x4,
    .fn_paren = true,
    .keywords = KW_JS, .keywords2 = KW_TS, .builtins = BI_JS,
};
static const PicoHlLang LANG_RUST = {
    .kind = PICO_HL_KIND_TOKEN,
    .line_comment = "//", .block_open = "/*", .block_close = "*/",
    .block_nesting = true,
    .quotes = "\"'", .escape_mask = 0x3,
    .lifetime_quote = true, .raw_prefix = true, .raw_hash = true, .fn_paren = true,
    .keywords = KW_RUST, .builtins = BI_RUST,
};
static const PicoHlLang LANG_GO = {
    .kind = PICO_HL_KIND_TOKEN,
    .line_comment = "//", .block_open = "/*", .block_close = "*/",
    .quotes = "\"'`", .escape_mask = 0x3, .multiline_mask = 0x4,
    .fn_paren = true,
    .keywords = KW_GO, .builtins = BI_GO,
};
static const PicoHlLang LANG_BASH = {
    .kind = PICO_HL_KIND_TOKEN,
    .line_comment = "#",
    .quotes = "\"'", .escape_mask = 0x3,
    .var_sigil = '$',
    .keywords = KW_BASH, .builtins = BI_BASH,
};
static const PicoHlLang LANG_LUA = {
    .kind = PICO_HL_KIND_TOKEN,
    .line_comment = "--", .block_open = "--[[", .block_close = "]]",
    .long_open = {"[[", NULL}, .long_close = {"]]", NULL},
    .quotes = "\"'", .escape_mask = 0x3,
    .fn_paren = true,
    .keywords = KW_LUA, .builtins = BI_LUA,
};
static const PicoHlLang LANG_JSON = {
    .kind = PICO_HL_KIND_TOKEN,
    .quotes = "\"", .escape_mask = 0x1,
    .string_field = true,
    .builtins = BI_JSON,
};
static const PicoHlLang LANG_MD = { .kind = PICO_HL_KIND_MARKDOWN };
static const PicoHlLang LANG_DIFF = { .kind = PICO_HL_KIND_DIFF };

/* ------------------------------------------------------------------ */
/* Lookup tables                                                       */

typedef struct HlAlias {
    const char *tag;
    const PicoHlLang *lang;
} HlAlias;

static const HlAlias ALIASES[] = {
    { "c", &LANG_C }, { "h", &LANG_C },
    { "cpp", &LANG_CPP }, { "c++", &LANG_CPP }, { "cxx", &LANG_CPP },
    { "cc", &LANG_CPP }, { "hpp", &LANG_CPP }, { "hxx", &LANG_CPP },
    { "python", &LANG_PY }, { "py", &LANG_PY },
    { "js", &LANG_JS }, { "javascript", &LANG_JS }, { "jsx", &LANG_JS },
    { "mjs", &LANG_JS }, { "cjs", &LANG_JS },
    { "ts", &LANG_TS }, { "typescript", &LANG_TS }, { "tsx", &LANG_TS },
    { "rust", &LANG_RUST }, { "rs", &LANG_RUST },
    { "go", &LANG_GO }, { "golang", &LANG_GO },
    { "json", &LANG_JSON },
    { "bash", &LANG_BASH }, { "sh", &LANG_BASH }, { "shell", &LANG_BASH },
    { "zsh", &LANG_BASH },
    { "lua", &LANG_LUA },
    { "markdown", &LANG_MD }, { "md", &LANG_MD },
    { "diff", &LANG_DIFF }, { "patch", &LANG_DIFF },
    { NULL, NULL },
};

const PicoHlLang *PicoHl_Lookup(const char *tag)
{
    if (!tag)
    {
        return NULL;
    }
    while (*tag == ' ' || *tag == '\t')
    {
        tag++;
    }
    char word[32];
    int n = 0;
    while (tag[n] && tag[n] != ' ' && tag[n] != '\t' && n < (int)sizeof(word) - 1)
    {
        word[n] = (char)tolower((unsigned char)tag[n]);
        n++;
    }
    word[n] = '\0';
    if (n == 0)
    {
        return NULL;
    }
    for (int i = 0; ALIASES[i].tag; i++)
    {
        if (strcmp(ALIASES[i].tag, word) == 0)
        {
            return ALIASES[i].lang;
        }
    }
    return NULL;
}

const PicoHlLang *PicoHl_LangForPath(const char *path)
{
    if (!path)
    {
        return NULL;
    }
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path || !dot[1])
    {
        return NULL;
    }
    return PicoHl_Lookup(dot + 1);
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */

static int HlTokenize(const PicoHlLang *lang, const char *text, PicoHlSpan *spans, int cap)
{
    if (!lang || !text || strlen(text) > INT32_MAX)
    {
        /* spans are int offsets; inputs beyond that render unstyled */
        return 0;
    }
    HlOut out = { spans, cap, 0 };
    switch (lang->kind)
    {
        case PICO_HL_KIND_TOKEN:
            HlTokenizeLang(lang, text, &out);
            break;
        case PICO_HL_KIND_MARKDOWN:
            HlTokenizeMarkdown(text, &out);
            break;
        case PICO_HL_KIND_DIFF:
            HlTokenizeDiff(text, &out);
            break;
    }
    return out.count;
}

int PicoHl_Count(const PicoHlLang *lang, const char *text)
{
    return HlTokenize(lang, text, NULL, 0);
}

int PicoHl_Fill(const PicoHlLang *lang, const char *text, PicoHlSpan *out, int cap)
{
    int count = HlTokenize(lang, text, out, cap);
    return count < cap ? count : cap;
}
