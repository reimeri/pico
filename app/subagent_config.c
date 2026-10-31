#define _POSIX_C_SOURCE 200809L

#include "subagent_config.h"
#include "host_internal.h"

#include "workspace_internal.h"
#include "json.h"
#include "path.h"
#include "settings.h"
#include "posix_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool ProfileNameAlnum(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static bool ValidProfileName(const char *name)
{
    if (!name || !ProfileNameAlnum(name[0])) return false;
    for (const char *p = name + 1; *p; p++)
        if (!ProfileNameAlnum(*p) && *p != '.' && *p != '_' && *p != '-') return false;
    return strlen(name) <= 64;
}

static bool ToolRegistered(const PicoWorkspace *workspace, const char *name)
{
    for (int i = 0; workspace && i < workspace->tool_count; i++)
    {
        if (workspace->tools[i].name && strcmp(workspace->tools[i].name, name) == 0)
        {
            return true;
        }
    }
    return false;
}

static void ProfileWarning(PicoHost *app, const char *path, const char *reason)
{
    char line[4608];
    snprintf(line, sizeof(line), "%s: %s", path, reason);
    pico_status_warn(app, line);
}

static bool ProfileError(char *error, size_t cap, const char *format, ...)
{
    if (error && cap) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, cap, format, args);
        va_end(args);
    }
    return false;
}

/* Work on a copy: token offsets still address the original JSONC. */
static char *ProfileJson(const char *source)
{
    if (!source || !JsonValidUtf8(source, strlen(source))) return NULL;
    char *copy = JsonDup(source);
    if (!copy) return NULL;
    bool quoted = false, escaped = false;
    size_t len = strlen(copy);
    for (size_t i = 0; i < len; i++) {
        char c = copy[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '/' && i + 1 < len && copy[i + 1] == '/') {
            while (i < len && copy[i] != '\n') i++;
        } else if (c == '/' && i + 1 < len && copy[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(copy[i] == '*' && copy[i + 1] == '/')) i++;
            if (i + 1 >= len) { free(copy); return NULL; }
            i++;
        }
    }
    JsonStripComments(copy, len);
    /* JSONC trailing commas, blanked without changing token offsets. */
    quoted = escaped = false;
    for (size_t i = 0; i < len; i++) {
        char c = copy[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == ',') {
            size_t next = i + 1;
            while (next < len && isspace((unsigned char)copy[next])) next++;
            size_t previous = i;
            while (previous && isspace((unsigned char)copy[previous - 1])) previous--;
            if (previous && copy[previous - 1] != '{' && copy[previous - 1] != '[' &&
                copy[previous - 1] != ':' && copy[previous - 1] != ',' &&
                next < len && (copy[next] == '}' || copy[next] == ']')) copy[i] = ' ';
        }
    }
    if (!JsonValidSyntax(copy, len)) { free(copy); return NULL; }
    return copy;
}

static bool ParseSource(PicoHost *warning_host, const char *source, const char *name,
                        PicoSubagentProfileInfo *out, char *reason, size_t reason_cap,
                        const char *warning_path)
{
    char *stripped = ProfileJson(source);
    JsonDoc doc;
    if (!stripped) return ProfileError(reason, reason_cap, "Profile must be a valid UTF-8 JSONC object.");
    if (JsonParse(&doc, stripped, strlen(stripped)) != 0) {
        free(stripped);
        return ProfileError(reason, reason_cap, "Could not parse profile.");
    }
    if (!JsonIsObject(&doc, 0)) {
        JsonFree(&doc); free(stripped);
        return ProfileError(reason, reason_cap, "Profile must be a JSON object.");
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    char *purpose = JsonObjStr(&doc, 0, "purpose");
    char *description = JsonObjStr(&doc, 0, "description");
    char *model = JsonObjStr(&doc, 0, "model");
    char *effort = JsonObjStr(&doc, 0, "effort");
    /* Even invalid documents can be repaired in the structured editor. */
    snprintf(out->purpose, sizeof(out->purpose), "%s", purpose ? purpose : "");
    snprintf(out->description, sizeof(out->description), "%s", description ? description : "");
    out->has_model = model != NULL;
    out->has_effort = effort != NULL;
    snprintf(out->model, sizeof(out->model), "%s", model ? model : "");
    snprintf(out->effort, sizeof(out->effort), "%s", effort ? effort : "");
    /* Extraction is independent of validation: repairing a model/purpose must
     * never silently widen an unrelated tool policy or erase concurrency. */
    out->fast = JsonEq(&doc, JsonObjGet(&doc, 0, "fast"), "true");
    out->parallel_safe = JsonEq(&doc, JsonObjGet(&doc, 0, "parallel_safe"), "true");
    out->max_parallel_tools = JsonObjInt(&doc, 0, "max_parallel_tools", 0);
    int extracted_tools = JsonObjGet(&doc, 0, "tools");
    out->restricted_tools = extracted_tools >= 0; /* invalid restrictions fail closed */
    if (JsonIsArray(&doc, extracted_tools)) {
        int count = JsonArrayLen(&doc, extracted_tools);
        for (int i = 0; i < count && i < PICO_MAX_TOOLS; i++) {
            char *tool = JsonStrDup(&doc, JsonArrayAt(&doc, extracted_tools, i));
            snprintf(out->tools[out->tool_count++], sizeof(out->tools[0]), "%s", tool ? tool : "");
            free(tool);
        }
    }
    const char *error = ValidProfileName(name) ? NULL : "Invalid profile name (use 1–64 letters, digits, dots, underscores or hyphens; start with a letter or digit).";
    if (!purpose || !purpose[0] || strlen(purpose) > 1024)
    {
        error = "purpose must be a non-empty string of at most 1024 bytes";
    }
    else if (JsonObjGet(&doc, 0, "description") >= 0 && !description)
    {
        error = "description must be a string";
    }
    else if (description && strlen(description) > 256)
    {
        error = "description exceeds 256 bytes";
    }
    else if (JsonObjGet(&doc, 0, "model") >= 0 && !model)
    {
        error = "model must be a string";
    }
    else if (model && (!model[0] || strlen(model) >= sizeof(out->model)))
    {
        error = "model must be a non-empty string shorter than 128 bytes";
    }
    else if (JsonObjGet(&doc, 0, "effort") >= 0 && !effort)
    {
        error = "effort must be a string";
    }
    else if (effort && strlen(effort) >= sizeof(out->effort))
    {
        error = "effort is too long";
    }

    int fast_tok = JsonObjGet(&doc, 0, "fast");
    if (!error && fast_tok >= 0)
    {
        int start = JsonTokStart(&doc, fast_tok);
        bool quoted = start > 0 && doc.src[start - 1] == '"';
        if (quoted || (!JsonEq(&doc, fast_tok, "true") && !JsonEq(&doc, fast_tok, "false")))
            error = "fast must be a boolean";
        else
            out->fast = JsonEq(&doc, fast_tok, "true");
    }

    int safe_tok = JsonObjGet(&doc, 0, "parallel_safe");
    if (!error && safe_tok >= 0)
    {
        int start = JsonTokStart(&doc, safe_tok);
        bool quoted = start > 0 && doc.src[start - 1] == '"';
        if (quoted || (!JsonEq(&doc, safe_tok, "true") && !JsonEq(&doc, safe_tok, "false")))
        {
            error = "parallel_safe must be a boolean";
        }
        else
        {
            out->parallel_safe = JsonEq(&doc, safe_tok, "true");
        }
    }

    int parallel_tok = JsonObjGet(&doc, 0, "max_parallel_tools");
    if (!error && parallel_tok >= 0)
    {
        char *raw = JsonRawDup(&doc, parallel_tok);
        int start = JsonTokStart(&doc, parallel_tok);
        /* String token bounds exclude quotes; raw duplication alone cannot distinguish "2" from 2. */
        bool quoted = start > 0 && doc.src[start - 1] == '"';
        char *end = NULL;
        long value = raw ? strtol(raw, &end, 10) : 0;
        bool valid = raw && !quoted && end && end != raw && *end == '\0' &&
                     value >= 1 && value <= PICO_MAX_PARALLEL_TOOLS;
        free(raw);
        if (!valid)
        {
            error = "max_parallel_tools must be an integer from 1 to 16";
        }
        else
        {
            out->max_parallel_tools = (int)value;
        }
    }
    int tools_tok = JsonObjGet(&doc, 0, "tools");
    if (!error && tools_tok >= 0)
    {
        if (!JsonIsArray(&doc, tools_tok) || JsonArrayLen(&doc, tools_tok) > PICO_MAX_TOOLS)
        {
            error = "tools must be an array within the tool limit";
        }
        else
        {
            out->restricted_tools = true;
            int count = JsonArrayLen(&doc, tools_tok);
            for (int i = 0; i < count && !error; i++)
            {
                char *tool = JsonStrDup(&doc, JsonArrayAt(&doc, tools_tok, i));
                if (!tool || !tool[0] || strlen(tool) >= sizeof(out->tools[0]))
                {
                    error = "tool names must be non-empty strings shorter than 128 bytes";
                }
                for (int j = 0; tool && !error && j < i; j++)
                {
                    if (strcmp(out->tools[j], tool) == 0)
                    {
                        error = "tools contains a duplicate name";
                    }
                }
                free(tool);
            }
        }
    }

    for (int i = 0; i < JsonObjLen(&doc, 0); i++)
    {
        int key_tok = -1;
        int value_tok = -1;
        if (!JsonObjPair(&doc, 0, i, &key_tok, &value_tok))
        {
            continue;
        }
        char *key = JsonStrDup(&doc, key_tok);
        if (key && strcmp(key, "purpose") != 0 && strcmp(key, "description") != 0 &&
            strcmp(key, "model") != 0 && strcmp(key, "effort") != 0 &&
            strcmp(key, "fast") != 0 && strcmp(key, "tools") != 0 &&
            strcmp(key, "max_parallel_tools") != 0 && strcmp(key, "parallel_safe") != 0)
        {
            char reason[256];
            snprintf(reason, sizeof(reason), "unknown profile key `%s`", key);
            if (warning_host && warning_path) ProfileWarning(warning_host, warning_path, reason);
        }
        free(key);
        (void)value_tok;
    }

    if (!error)
    {
        snprintf(out->purpose, sizeof(out->purpose), "%s", purpose);
        snprintf(out->description, sizeof(out->description), "%s", description ? description : "");
        if (model)
        {
            out->has_model = true;
            snprintf(out->model, sizeof(out->model), "%s", model);
        }
        if (effort)
        {
            out->has_effort = true;
            snprintf(out->effort, sizeof(out->effort), "%s", effort);
        }
    }
    else
    {
        ProfileError(reason, reason_cap, "%s", error);
    }
    free(purpose);
    free(description);
    free(model);
    free(effort);
    JsonFree(&doc);
    free(stripped);
    return error == NULL;
}

/* Stored definitions are global. Only loading them for use depends on a
 * workspace's resolved model catalog and registered tool names. */
static const char *ProfileCompatibility(const PicoWorkspace *workspace,
                                        const PicoSubagentProfileInfo *profile)
{
    const PicoModel *model = profile->has_model ? PicoSettings_FindModelConst(workspace, profile->model) : NULL;
    if (profile->has_model && !model) return "model is not in the model catalog";
    if (profile->has_effort && model && !PicoSettings_EffortAllowed(model, profile->effort))
        return "effort is not supported by the configured model";
    for (int i = 0; i < profile->tool_count; i++)
        if (!ToolRegistered(workspace, profile->tools[i])) return "tools contains an unknown tool name";
    return NULL;
}

static bool ParseProfile(PicoWorkspace *workspace, const char *path, const char *name,
                         PicoSubagentProfileInfo *out)
{
    size_t len = 0;
    char *source = Pico_ReadFile(path, &len);
    char error[512] = "Could not read profile, or profile contains NUL bytes.";
    bool ok = source && !memchr(source, '\0', len) && ParseSource(workspace->host, source, name, out, error, sizeof(error), path);
    if (ok) {
        const char *compatibility_error = ProfileCompatibility(workspace, out);
        if (compatibility_error) ok = ProfileError(error, sizeof(error), "%s", compatibility_error);
    }
    if (!ok) ProfileWarning(workspace->host, path, error);
    free(source);
    return ok;
}

void PicoSubagentConfig_Load(PicoWorkspace *workspace)
{
    if (!workspace || !workspace->host)
    {
        return;
    }
    char config[4096];
    char dir[4096];
    bool directory_path_ok = Pico_ConfigDir(config, sizeof(config)) &&
                             PicoPath_Format(dir, sizeof(dir), "%s/subagents", config);
    if (directory_path_ok)
    {
        Pico_MkdirP(dir);
    }

    PicoSubagentProfileInfo loaded[PICO_MAX_SUBAGENT_PROFILES];
    int count = 0;
    DIR *directory = directory_path_ok ? opendir(dir) : NULL;
    if (directory)
    {
        struct dirent *entry;
        while ((entry = readdir(directory)) && count < PICO_MAX_SUBAGENT_PROFILES)
        {
            size_t name_len = strlen(entry->d_name);
            if (entry->d_name[0] == '.' || name_len <= 5 ||
                strcmp(entry->d_name + name_len - 5, ".json") != 0)
            {
                continue;
            }
            char profile_name[65];
            size_t stem_len = name_len - 5;
            if (stem_len >= sizeof(profile_name))
            {
                continue;
            }
            memcpy(profile_name, entry->d_name, stem_len);
            profile_name[stem_len] = '\0';
            char path[4096];
            if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= sizeof(path))
            {
                continue;
            }
            struct stat st;
            if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
            {
                continue;
            }
            if (!ValidProfileName(profile_name))
            {
                ProfileWarning(workspace->host, path, "invalid profile filename");
                continue;
            }
            if (ParseProfile(workspace, path, profile_name, &loaded[count]))
            {
                count++;
            }
        }
        closedir(directory);
    }

    memset(workspace->profiles, 0, sizeof(workspace->profiles));
    memcpy(workspace->profiles, loaded, (size_t)count * sizeof(loaded[0]));
    workspace->profile_count = count;
}

const PicoSubagentProfileInfo *PicoSubagentConfig_Find(const PicoWorkspace *workspace,
                                                       const char *name)
{
    for (int i = 0; workspace && name && i < workspace->profile_count; i++)
    {
        if (strcmp(workspace->profiles[i].name, name) == 0)
        {
            return &workspace->profiles[i];
        }
    }
    return NULL;
}

bool PicoSubagentConfig_Resolve(const PicoHost *app, const PicoAgent *parent,
                                const PicoSubagentProfileInfo *profile,
                                char *model, size_t model_cap,
                                char *effort, size_t effort_cap)
{
    if (!app || !parent || !profile || !model || model_cap == 0 || !effort || effort_cap == 0)
    {
        return false;
    }
    const char *resolved_model = profile->has_model ? profile->model : parent->model;
    const PicoModel *catalog = PicoSettings_FindModelConst(parent->workspace, resolved_model);
    if (!catalog)
    {
        return false;
    }
    if (profile->fast && !PicoSettings_ModelSupportsFast(parent->workspace, catalog))
        return false;
    const char *resolved_effort = NULL;
    if (profile->has_effort)
    {
        resolved_effort = profile->effort;
    }
    else if (strcmp(resolved_model, parent->model) == 0)
    {
        resolved_effort = parent->effort;
    }
    else if (PicoSettings_EffortAllowed(catalog, catalog->default_effort))
    {
        resolved_effort = catalog->default_effort;
    }
    else if (catalog->effort_count > 0)
    {
        resolved_effort = catalog->effort[0];
    }
    else
    {
        resolved_effort = "none";
    }
    if (!PicoSettings_EffortAllowed(catalog, resolved_effort) &&
        !(catalog->effort_count == 0 && strcmp(resolved_effort, "none") == 0))
    {
        return false;
    }
    snprintf(model, model_cap, "%s", resolved_model);
    snprintf(effort, effort_cap, "%s", resolved_effort);
    return true;
}

bool PicoSubagentConfig_Directory(char *out, size_t cap)
{
    char config[4096];
    return Pico_ConfigDir(config, sizeof(config)) &&
           PicoPath_Format(out, cap, "%s/subagents", config);
}

void PicoSubagentConfig_FreeDocuments(PicoSubagentDocument *documents, int count)
{
    for (int i = 0; i < count; i++) free(documents[i].source);
    free(documents);
}

static int CompareDocuments(const void *a, const void *b)
{
    return strcmp(((const PicoSubagentDocument *)a)->filename,
                  ((const PicoSubagentDocument *)b)->filename);
}

bool PicoSubagentConfig_List(PicoSubagentDocument **out,
                              int *count, char *error, size_t error_cap)
{
    *out = NULL;
    *count = 0;
    char dir[4096];
    if (!PicoSubagentConfig_Directory(dir, sizeof(dir)))
        return ProfileError(error, error_cap, "Profile directory path is too long.");
    Pico_MkdirP(dir);
    DIR *directory = opendir(dir);
    if (!directory) return ProfileError(error, error_cap, "Could not open profiles: %s", strerror(errno));
    struct dirent *entry;
    bool ok = true;
    while ((entry = readdir(directory))) {
        size_t len = strlen(entry->d_name);
        if (entry->d_name[0] == '.' || len <= 5 || strcmp(entry->d_name + len - 5, ".json")) continue;
        char path[4096];
        struct stat st;
        if (!PicoPath_Format(path, sizeof(path), "%s/%s", dir, entry->d_name) ||
            lstat(path, &st) || !S_ISREG(st.st_mode)) continue;
        PicoSubagentDocument *next = realloc(*out, (size_t)(*count + 1) * sizeof(**out));
        if (!next) { ok = ProfileError(error, error_cap, "Out of memory listing profiles."); break; }
        *out = next;
        PicoSubagentDocument *d = &next[(*count)++];
        memset(d, 0, sizeof(*d));
        snprintf(d->filename, sizeof(d->filename), "%s", entry->d_name);
        snprintf(d->path, sizeof(d->path), "%s", path);
        char name[256];
        memcpy(name, entry->d_name, len - 5);
        name[len - 5] = '\0';
        snprintf(d->profile.name, sizeof(d->profile.name), "%.*s", 64, name);
        d->source = Pico_ReadFileLimited(path, 1024 * 1024, &d->source_len);
        if (!d->source) {
            snprintf(d->error, sizeof(d->error), "Could not read profile: %s", strerror(errno));
            continue;
        }
        if (memchr(d->source, '\0', d->source_len)) {
            snprintf(d->error, sizeof(d->error), "Profile contains NUL bytes and cannot be parsed.");
            continue;
        }
        char *stripped = ProfileJson(d->source);
        JsonDoc doc;
        if (stripped && JsonParse(&doc, stripped, strlen(stripped)) == 0) {
            d->parseable = JsonIsObject(&doc, 0);
            JsonFree(&doc);
        }
        free(stripped);
        d->valid = ParseSource(NULL, d->source, name, &d->profile,
                               d->error, sizeof(d->error), NULL);
    }
    closedir(directory);
    if (*count > 1) qsort(*out, (size_t)*count, sizeof(**out), CompareDocuments);
    return ok;
}

static bool SpliceProfile(char **source, size_t start, size_t end, const char *value)
{
    size_t len = strlen(*source), added = value ? strlen(value) : 0;
    char *next = malloc(len - (end - start) + added + 1);
    if (!next) return false;
    memcpy(next, *source, start);
    if (added) memcpy(next + start, value, added);
    memcpy(next + start + added, *source + end, len - end + 1);
    free(*source);
    *source = next;
    return true;
}

/* Edit value spans, leaving unrelated keys and surrounding comments untouched.
 * Removing an optional field removes its pair and one adjacent comma. */
static bool PatchProfile(char **source, const char *key, const char *value)
{
    char *stripped = ProfileJson(*source);
    JsonDoc doc;
    if (!stripped) return false;
    if (JsonParse(&doc, stripped, strlen(stripped)) != 0) { free(stripped); return false; }
    int key_tok = -1, tok = -1;
    for (int i = 0; i < JsonObjLen(&doc, 0); i++) {
        int k, v;
        if (JsonObjPair(&doc, 0, i, &k, &v) && JsonEq(&doc, k, key)) { key_tok = k; tok = v; break; }
    }
    bool ok = true;
    if (tok >= 0) {
        size_t start = (size_t)JsonTokStart(&doc, tok);
        size_t end = (size_t)JsonTokEnd(&doc, tok);
        if (JsonIsString(&doc, tok)) { start--; end++; }
        if (!value) {
            start = (size_t)JsonTokStart(&doc, key_tok) - 1;
            size_t after = end;
            while (isspace((unsigned char)stripped[after])) after++;
            char *comments_only = JsonDup(*source);
            if (!comments_only) { JsonFree(&doc); free(stripped); return false; }
            JsonStripComments(comments_only, strlen(comments_only));
            after = end;
            while (isspace((unsigned char)comments_only[after])) after++;
            bool following_comma = comments_only[after] == ',';
            free(comments_only);
            if (following_comma) end = after + 1;
            else {
                size_t before = start;
                while (before && isspace((unsigned char)stripped[before - 1])) before--;
                if (before && stripped[before - 1] == ',') start = before - 1;
            }
        }
        ok = SpliceProfile(source, start, end, value);
    } else if (value) {
        size_t at = (size_t)JsonTokEnd(&doc, 0) - 1;
        JsonBuf addition;
        JsonBuf_Init(&addition);
        if (JsonObjLen(&doc, 0)) {
            size_t before = at;
            while (before && isspace((unsigned char)stripped[before - 1])) before--;
            /* A trailing comma may have been blanked by ProfileJson. */
            bool comma = false;
            char *comments_only = JsonDup(*source);
            if (!comments_only) { JsonBuf_Free(&addition); JsonFree(&doc); free(stripped); return false; }
            JsonStripComments(comments_only, strlen(comments_only));
            for (size_t i = before; i < at; i++) if (comments_only[i] == ',') comma = true;
            free(comments_only);
            if (!comma) JsonBuf_Putc(&addition, ',');
        }
        JsonBuf_Puts(&addition, "\n  ");
        JsonBuf_String(&addition, key);
        JsonBuf_Puts(&addition, ": ");
        JsonBuf_Puts(&addition, value);
        JsonBuf_Putc(&addition, '\n');
        ok = !addition.failed && SpliceProfile(source, at, at, addition.data);
        JsonBuf_Free(&addition);
    }
    JsonFree(&doc);
    free(stripped);
    /* Remove/update duplicate occurrences too: duplicated policy keys must not
     * allow the parser and editor to disagree. */
    if (ok && tok >= 0) {
        stripped = ProfileJson(*source);
        if (!stripped) return false;
        if (JsonParse(&doc, stripped, strlen(stripped)) != 0) { free(stripped); return false; }
        int matches = 0;
        for (int i = 0; i < JsonObjLen(&doc, 0); i++) {
            int k, v;
            if (JsonObjPair(&doc, 0, i, &k, &v) && JsonEq(&doc, k, key)) matches++;
        }
        JsonFree(&doc); free(stripped);
        if (matches > (value ? 1 : 0)) {
            /* First remove all occurrences; then insert one authoritative value. */
            if (value && !PatchProfile(source, key, NULL)) return false;
            return PatchProfile(source, key, value);
        }
    }
    return ok;
}

char *PicoSubagentConfig_Build(const PicoSubagentDocument *original,
                              const PicoSubagentProfileInfo *p, char *error, size_t error_cap)
{
    if (!ValidProfileName(p->name)) {
        ProfileError(error, error_cap, "Name must be 1–64 ASCII letters, digits, dots, underscores or hyphens, starting with a letter or digit.");
        return NULL;
    }
    char *source = JsonDup(original && original->parseable ? original->source : "{\n}\n");
    if (!source) return NULL;
    const char *keys[] = {"description", "purpose", "model", "effort", "fast", "parallel_safe", "tools", "max_parallel_tools"};
    bool ok = true;
    for (int i = 0; i < 8 && ok; i++) {
        JsonBuf value;
        JsonBuf_Init(&value);
        bool present = true;
        switch (i) {
        case 0: JsonBuf_String(&value, p->description); break;
        case 1: JsonBuf_String(&value, p->purpose); break;
        case 2: present = p->has_model; JsonBuf_String(&value, p->model); break;
        case 3: present = p->has_effort; JsonBuf_String(&value, p->effort); break;
        case 4: JsonBuf_Bool(&value, p->fast); break;
        case 5: JsonBuf_Bool(&value, p->parallel_safe); break;
        case 6:
            present = p->restricted_tools;
            JsonBuf_Putc(&value, '[');
            for (int j = 0; j < p->tool_count; j++) {
                if (j) JsonBuf_Puts(&value, ", ");
                JsonBuf_String(&value, p->tools[j]);
            }
            JsonBuf_Putc(&value, ']');
            break;
        case 7: present = p->max_parallel_tools != 0; JsonBuf_Int(&value, p->max_parallel_tools); break;
        }
        ok = !value.failed && PatchProfile(&source, keys[i], present ? value.data : NULL);
        JsonBuf_Free(&value);
    }
    PicoSubagentProfileInfo checked;
    if (!ok || !ParseSource(NULL, source, p->name, &checked, error, error_cap, NULL)) {
        if (!ok) ProfileError(error, error_cap, "Could not build profile document.");
        free(source);
        return NULL;
    }
    return source;
}

static bool DocumentUnchanged(const PicoSubagentDocument *original, char *error, size_t error_cap)
{
    struct stat st;
    if (!original || !original->source || lstat(original->path, &st) || !S_ISREG(st.st_mode))
        return ProfileError(error, error_cap, "Profile cannot be safely changed. Refresh the list and try again.");
    size_t len = 0;
    char *current = Pico_ReadFileLimited(original->path, 1024 * 1024, &len);
    bool equal = current && len == original->source_len && !memcmp(current, original->source, len);
    free(current);
    return equal || ProfileError(error, error_cap, "Profile changed outside Pico. Refresh the list before saving or deleting.");
}

static void SyncProfileDirectory(const char *path)
{
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { (void)fsync(fd); close(fd); }
}

bool PicoSubagentConfig_Save(const PicoSubagentDocument *original, const char *name,
                             const char *source, char *error, size_t error_cap)
{
    char dir[4096], path[4096], temp[4096];
    if (error && error_cap) error[0] = '\0';
    if (!source || !ValidProfileName(name) || !PicoSubagentConfig_Directory(dir, sizeof(dir)) ||
        !PicoPath_Format(path, sizeof(path), "%s/%s.json", dir, name) ||
        !PicoPath_Format(temp, sizeof(temp), "%s/.profile-XXXXXX", dir))
        return ProfileError(error, error_cap, "Invalid profile path.");
    if (original && !DocumentUnchanged(original, error, error_cap)) return false;
    Pico_MkdirP(dir);
    bool same = original && !strcmp(path, original->path);
    mode_t mode = 0600;
    struct stat st;
    if (original && !lstat(original->path, &st)) mode = st.st_mode & 0777;
    int fd = mkstemp(temp);
    if (fd < 0) return ProfileError(error, error_cap, "Could not create profile: %s", strerror(errno));
    bool ok = !fchmod(fd, mode) && PicoIO_WriteAll(fd, source, strlen(source)) && !fsync(fd);
    int code = errno;
    if (close(fd) && ok) { ok = false; code = errno; }
    if (ok && original) ok = DocumentUnchanged(original, error, error_cap);
    if (ok) {
        /* link is an atomic no-replace install for create/duplicate/rename. */
        ok = (same ? rename(temp, path) : link(temp, path)) == 0;
        code = errno;
        if (!ok) ProfileError(error, error_cap, "Could not save profile (name may already exist): %s", strerror(code));
        if (ok && original && !same && unlink(original->path)) {
            code = errno;
            unlink(path);
            ok = ProfileError(error, error_cap, "Could not rename profile: %s", strerror(code));
        }
    } else if (!error || !error[0]) ProfileError(error, error_cap, "Could not write profile: %s", strerror(code));
    unlink(temp);
    if (ok) SyncProfileDirectory(path);
    return ok;
}

bool PicoSubagentConfig_Delete(const PicoSubagentDocument *original, char *error, size_t error_cap)
{
    if (error && error_cap) error[0] = '\0';
    if (!DocumentUnchanged(original, error, error_cap)) return false;
    if (unlink(original->path)) return ProfileError(error, error_cap, "Could not delete profile: %s", strerror(errno));
    SyncProfileDirectory(original->path);
    return true;
}

void PicoSubagentConfig_RequestRefresh(PicoHost *host)
{
    for (int i = 0; host && i < host->workspace_count; i++)
        if (host->workspaces[i]) host->workspaces[i]->profiles_refresh_pending = true;
}

void PicoSubagentConfig_PumpRefresh(PicoWorkspace *workspace)
{
    if (workspace && workspace->profiles_refresh_pending && workspace->state == PICO_WORKSPACE_OPEN &&
        !PicoWorkspace_BlocksReload(workspace)) {
        PicoSubagentConfig_Load(workspace);
        workspace->profiles_refresh_pending = false;
    }
}
