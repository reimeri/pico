#include "subagent_settings.h"
#include "host_internal.h"
#include "subagent_config.h"
#include "text_field_ui.h"
#include "settings_controls.h"
#include "path.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SA_NAME, SA_DESCRIPTION, SA_PURPOSE, SA_MODEL, SA_EFFORT, SA_TOOLS, SA_LIMIT, SA_FIELDS };
enum { SA_CONFIRM_NONE, SA_CONFIRM_REFRESH, SA_CONFIRM_LEAVE, SA_CONFIRM_DELETE, SA_CONFIRM_REPLACE };

typedef struct SubagentDraft
{
    PicoSubagentDocument original; /* Owns source bytes, including for duplicate templates. */
    PicoSubagentProfileInfo profile;
    char purpose[4097]; /* Escaped newlines/backslashes for the single-line field. */
    char tools[PICO_MAX_TOOLS * 129];
    char limit[32], error[512];
    bool persisted, expanded, dirty, deleted, replacement_confirmed;
} SubagentDraft;

typedef struct SubagentChange
{
    int index;
    bool has_original, deleting;
    PicoSubagentDocument original, updated;
    char error[512];
} SubagentChange;

struct PicoSubagentSettingsSave
{
    SubagentChange *changes;
    int count, completed;
};

struct PicoSubagentSettings
{
    PicoHost *host;
    SubagentDraft **drafts;
    int count;
    bool loaded, apply_requested;
    int focus_card, focus, confirm, confirm_card, leave_decision, reveal_card;
    char error[512];
    PicoTextField field;
};

static bool SaChanged(const SubagentDraft *d)
{
    return d->dirty || (d->persisted && d->deleted);
}

static bool SaDirty(const PicoSubagentSettings *s)
{
    for (int i = 0; i < s->count; i++)
        if (SaChanged(s->drafts[i])) return true;
    return false;
}

static void SaSyncReload(PicoSubagentSettings *s)
{
    s->host->subagent_settings_reload_blocked = SaDirty(s) || s->confirm != SA_CONFIRM_NONE;
    if (s->confirm != SA_CONFIRM_NONE) {
        if (!pico_ui_modal_has(s->host, "settings-confirm"))
            (void)pico_ui_modal_push(s->host, "settings-confirm");
    } else {
        (void)pico_ui_modal_pop(s->host, "settings-confirm");
    }
}

static Clay_String SaString(const char *s)
{
    return (Clay_String){.chars = s, .length = (int32_t)strlen(s)};
}

static void SaText(const char *text, Clay_Color color)
{
    CLAY_TEXT(SaString(text), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_CAPTION,
                                              .textColor = color, .wrapMode = CLAY_TEXT_WRAP_WORDS}));
}

static void SaButton(Clay_ElementId id, const char *label, bool primary)
{
    PicoSettingsControl_Button(id, label, true, primary, false);
}

static char *SaBuffer(SubagentDraft *s, int index, int *capacity)
{
    switch (index)
    {
    case SA_NAME: *capacity = sizeof(s->profile.name); return s->profile.name;
    case SA_DESCRIPTION: *capacity = sizeof(s->profile.description); return s->profile.description;
    case SA_PURPOSE: *capacity = sizeof(s->purpose); return s->purpose;
    case SA_MODEL: *capacity = sizeof(s->profile.model); return s->profile.model;
    case SA_EFFORT: *capacity = sizeof(s->profile.effort); return s->profile.effort;
    case SA_TOOLS: *capacity = sizeof(s->tools); return s->tools;
    case SA_LIMIT: *capacity = sizeof(s->limit); return s->limit;
    default: *capacity = 0; return NULL;
    }
}

static void SaFocus(PicoSubagentSettings *s, int card, int field)
{
    PicoTextField_Unbind(&s->field);
    s->focus_card = card;
    s->focus = field;
    if (card >= 0 && field >= 0)
    {
        int cap;
        char *buffer = SaBuffer(s->drafts[card], field, &cap);
        PicoTextField_Bind(&s->field, buffer, cap);
    }
}

void PicoSubagentSettings_Blur(PicoSubagentSettings *s)
{
    if (s) SaFocus(s, -1, -1);
}

static void SaField(PicoSubagentSettings *s, int card, int index, const char *label, const char *placeholder)
{
    int capacity;
    char *text = SaBuffer(s->drafts[card], index, &capacity);
    SETTINGS_ROW_BEGIN
        PicoSettingsControl_Label(label);
        PicoSettingsControl_Field(CLAY_IDI("SubagentField", card * SA_FIELDS + index), text, placeholder,
                                  &s->field, s->focus_card == card && s->focus == index);
    }
}

static void SaToggle(Clay_ElementId id, const char *label, bool on)
{
    SETTINGS_ROW_BEGIN
        PicoSettingsControl_Toggle(id, label, on);
    }
}

static void SaEncodePurpose(SubagentDraft *s)
{
    size_t at = 0;
    for (const char *p = s->profile.purpose; *p && at + 2 < sizeof(s->purpose); p++)
    {
        if (*p == '\n' || *p == '\r' || *p == '\\' || *p == '\t')
        {
            s->purpose[at++] = '\\';
            s->purpose[at++] = *p == '\n' ? 'n' : *p == '\r' ? 'r'
                                              : *p == '\t'   ? 't'
                                                             : '\\';
        }
        else
            s->purpose[at++] = *p;
    }
    s->purpose[at] = '\0';
}

static bool SaDecodePurpose(SubagentDraft *s)
{
    size_t at = 0;
    for (const char *p = s->purpose; *p; p++)
    {
        char c = *p;
        if (c == '\\')
        {
            p++;
            if (*p == 'n')
                c = '\n';
            else if (*p == 'r')
                c = '\r';
            else if (*p == 't')
                c = '\t';
            else if (*p == '\\')
                c = '\\';
            else
            {
                snprintf(s->error, sizeof(s->error), "Purpose: use \\n, \\r, \\t or \\\\ for escapes.");
                return false;
            }
        }
        if (at + 1 >= sizeof(s->profile.purpose))
        {
            snprintf(s->error, sizeof(s->error), "Purpose exceeds 1024 UTF-8 bytes.");
            return false;
        }
        s->profile.purpose[at++] = c;
    }
    s->profile.purpose[at] = '\0';
    return true;
}


static void SaFreeDraft(SubagentDraft *d)
{
    free(d->original.source);
    free(d);
}

void PicoSubagentSettings_Discard(PicoSubagentSettings *s)
{
    if (!s) return;
    SaFocus(s, -1, -1);
    for (int i = 0; i < s->count; i++) SaFreeDraft(s->drafts[i]);
    free(s->drafts);
    s->drafts = NULL;
    s->count = 0;
    s->loaded = s->apply_requested = false;
    s->confirm = SA_CONFIRM_NONE;
    s->leave_decision = 0;
    s->reveal_card = -1;
    s->error[0] = '\0';
    SaSyncReload(s);
}

static void SaLoadList(PicoSubagentSettings *s)
{
    PicoSubagentSettings_Discard(s);
    PicoSubagentDocument *documents = NULL;
    int count = 0;
    PicoSubagentConfig_List(&documents, &count, s->error, sizeof(s->error));
    s->loaded = true;
    s->drafts = calloc((size_t)count, sizeof(*s->drafts));
    if (count && !s->drafts) goto oom;
    for (int i = 0; i < count; i++)
    {
        SubagentDraft *d = calloc(1, sizeof(*d));
        if (!d) goto oom;
        d->original = documents[i];
        documents[i].source = NULL;
        d->profile = d->original.profile;
        d->persisted = true;
        SaEncodePurpose(d);
        size_t at = 0;
        for (int j = 0; j < d->profile.tool_count; j++)
            at += (size_t)snprintf(d->tools + at, sizeof(d->tools) - at, "%s%s", j ? ", " : "", d->profile.tools[j]);
        if (d->profile.max_parallel_tools)
            snprintf(d->limit, sizeof(d->limit), "%d", d->profile.max_parallel_tools);
        s->drafts[s->count++] = d;
    }
    PicoSubagentConfig_FreeDocuments(documents, count);
    return;
oom:
    PicoSubagentConfig_FreeDocuments(documents, count);
    snprintf(s->error, sizeof(s->error), "Out of memory loading profiles.");
}

static bool SaCopyDocument(PicoSubagentDocument *to, const PicoSubagentDocument *from)
{
    *to = *from;
    to->source = NULL;
    if (from->source)
    {
        to->source = malloc(from->source_len + 1);
        if (!to->source) return false;
        memcpy(to->source, from->source, from->source_len);
        to->source[from->source_len] = '\0';
    }
    return true;
}

static void SaNew(PicoSubagentSettings *s, int duplicate)
{
    SubagentDraft *d = calloc(1, sizeof(*d));
    if (!d) goto oom;
    if (duplicate >= 0)
    {
        *d = *s->drafts[duplicate];
        if (!SaCopyDocument(&d->original, &s->drafts[duplicate]->original)) { free(d); goto oom; }
        char base[65];
        snprintf(base, sizeof(base), "%s", d->profile.name);
        for (int n = 1; n < 10000; n++)
        {
            snprintf(d->profile.name, sizeof(d->profile.name), "%.*s-copy%d", 52, base, n);
            bool used = false;
            for (int i = 0; i < s->count; i++)
                if (!strcmp(d->profile.name, s->drafts[i]->profile.name)) used = true;
            if (!used) break;
        }
    }
    SubagentDraft **next = realloc(s->drafts, (size_t)(s->count + 1) * sizeof(*next));
    if (!next) { SaFreeDraft(d); goto oom; }
    SaFocus(s, -1, -1);
    s->drafts = next;
    memmove(next + 1, next, (size_t)s->count * sizeof(*next));
    next[0] = d;
    s->count++;
    d->persisted = d->deleted = d->replacement_confirmed = false;
    d->expanded = d->dirty = true;
    d->error[0] = '\0';
    s->error[0] = '\0';
    s->reveal_card = 0;
    SaFocus(s, 0, SA_NAME);
    SaSyncReload(s);
    return;
oom:
    snprintf(s->error, sizeof(s->error), "Out of memory creating profile.");
}

PicoSubagentSettings *PicoSubagentSettings_Create(PicoHost *host)
{
    PicoSubagentSettings *s = calloc(1, sizeof(*s));
    if (s)
    {
        s->host = host;
        s->focus_card = s->focus = s->reveal_card = -1;
    }
    return s;
}

void PicoSubagentSettings_Free(PicoSubagentSettings *s)
{
    if (!s) return;
    PicoSubagentSettings_Discard(s);
    PicoTextField_Free(&s->field);
    free(s);
}

void PicoSubagentSettings_Open(PicoSubagentSettings *s)
{
    if (s && !s->loaded) SaLoadList(s);
}

bool PicoSubagentSettings_RequestLeave(PicoSubagentSettings *s)
{
    if (!s) return true;
    if (!SaDirty(s)) return true;
    s->confirm = SA_CONFIRM_LEAVE;
    SaSyncReload(s);
    return false;
}

int PicoSubagentSettings_TakeLeaveDecision(PicoSubagentSettings *s)
{
    int result = s ? s->leave_decision : 0;
    if (s) s->leave_decision = 0;
    return result;
}

bool PicoSubagentSettings_TakeApplyRequest(PicoSubagentSettings *s)
{
    bool result = s && s->apply_requested;
    if (s) s->apply_requested = false;
    return result;
}

static bool SaReadFields(SubagentDraft *s)
{
    if (!SaDecodePurpose(s))
        return false;
    s->profile.has_model = s->profile.model[0] != '\0';
    s->profile.has_effort = s->profile.effort[0] != '\0';
    s->profile.max_parallel_tools = 0;
    if (s->limit[0])
    {
        char *end;
        long n = strtol(s->limit, &end, 10);
        if (*end || n < 1 || n > PICO_MAX_PARALLEL_TOOLS)
        {
            snprintf(s->error, sizeof(s->error), "Tool concurrency must be blank (workspace default) or an integer from 1 to 16.");
            return false;
        }
        s->profile.max_parallel_tools = (int)n;
    }
    s->profile.tool_count = 0;
    const char *p = s->tools;
    while (*p && s->profile.restricted_tools)
    {
        while (*p == ',' || isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != ',' && !isspace((unsigned char)*p))
            p++;
        size_t len = (size_t)(p - start);
        if (len >= sizeof(s->profile.tools[0]) || s->profile.tool_count >= PICO_MAX_TOOLS)
        {
            snprintf(s->error, sizeof(s->error), "Tool allowlist is too large.");
            return false;
        }
        memcpy(s->profile.tools[s->profile.tool_count], start, len);
        s->profile.tools[s->profile.tool_count++][len] = '\0';
    }
    return true;
}


void PicoSubagentSettings_FreeSave(PicoSubagentSettingsSave *save)
{
    if (!save) return;
    for (int i = 0; i < save->count; i++)
    {
        free(save->changes[i].original.source);
        free(save->changes[i].updated.source);
    }
    free(save->changes);
    free(save);
}

/* Validate every changed card before any part of the shared Apply is queued.
 * The returned snapshot owns all worker inputs; the UI never writes from a worker. */
bool PicoSubagentSettings_PrepareSave(PicoSubagentSettings *s, PicoSubagentSettingsSave **out)
{
    *out = NULL;
    if (!s || !SaDirty(s)) return true;
    PicoSubagentSettingsSave *save = calloc(1, sizeof(*save));
    if (!save) goto oom;
    save->changes = calloc((size_t)s->count, sizeof(*save->changes));
    if (!save->changes) goto oom;
    /* Deletes precede writes so a staged deletion can free a destination name. */
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < s->count; i++)
    {
        SubagentDraft *d = s->drafts[i];
        if (!SaChanged(d) || d->deleted != (pass == 0)) continue;
        d->error[0] = '\0';
        SubagentChange *change = &save->changes[save->count++];
        change->index = i;
        change->has_original = d->persisted;
        change->deleting = d->deleted;
        if (d->persisted && !SaCopyDocument(&change->original, &d->original)) goto oom;
        if (!d->deleted)
        {
            if (!SaReadFields(d)) goto invalid;
            change->updated.source = PicoSubagentConfig_Build(d->original.source ? &d->original : NULL,
                                                             &d->profile, d->error, sizeof(d->error));
            if (!change->updated.source) goto invalid;
            for (int j = 0; j < s->count; j++)
                if (j != i && !s->drafts[j]->deleted && !strcmp(d->profile.name, s->drafts[j]->profile.name))
                {
                    snprintf(d->error, sizeof(d->error), "Another profile has this name.");
                    goto invalid;
                }
            if (d->persisted && !d->original.parseable && !d->replacement_confirmed)
            {
                s->confirm = SA_CONFIRM_REPLACE;
                s->confirm_card = i;
                goto invalid;
            }
            char directory[4096];
            if (!PicoSubagentConfig_Directory(directory, sizeof(directory)) ||
                !PicoPath_Format(change->updated.path, sizeof(change->updated.path), "%s/%s.json", directory, d->profile.name))
            {
                snprintf(d->error, sizeof(d->error), "Invalid profile path.");
                goto invalid;
            }
            snprintf(change->updated.filename, sizeof(change->updated.filename), "%s.json", d->profile.name);
            change->updated.profile = d->profile;
            change->updated.valid = change->updated.parseable = true;
            change->updated.source_len = strlen(change->updated.source);
        }
        continue;
invalid:
        d->expanded = true;
        s->reveal_card = i;
        PicoSubagentSettings_FreeSave(save);
        SaSyncReload(s);
        s->host->ui_relayout_requested = true;
        return false;
    }
    /* A rename must free its old filename before another card can reuse it.
     * Never resolve a cycle by overwriting a destination or temporary UI state. */
    for (int i = 0; i < save->count; i++)
    {
        if (save->changes[i].deleting) continue;
        int ready = -1;
        for (int j = i; j < save->count; j++)
        {
            bool blocked = false;
            for (int k = i; k < save->count; k++)
                if (j != k && save->changes[k].has_original &&
                    !strcmp(save->changes[j].updated.path, save->changes[k].original.path)) blocked = true;
            if (!blocked) { ready = j; break; }
        }
        if (ready < 0)
        {
            int card = save->changes[i].index;
            SubagentDraft *d = s->drafts[card];
            snprintf(d->error, sizeof(d->error), "Profile renames form a cycle. Apply a temporary name first, then finish the renames.");
            d->expanded = true;
            s->reveal_card = card;
            PicoSubagentSettings_FreeSave(save);
            s->host->ui_relayout_requested = true;
            return false;
        }
        if (ready != i)
        {
            SubagentChange next = save->changes[ready];
            memmove(save->changes + i + 1, save->changes + i, (size_t)(ready - i) * sizeof(*save->changes));
            save->changes[i] = next;
        }
    }
    *out = save;
    return true;
oom:
    PicoSubagentSettings_FreeSave(save);
    snprintf(s->error, sizeof(s->error), "Out of memory preparing profiles.");
    return false;
}

bool PicoSubagentSettings_RunSave(PicoSubagentSettingsSave *save)
{
    if (!save) return true;
    for (; save->completed < save->count; save->completed++)
    {
        SubagentChange *c = &save->changes[save->completed];
        bool ok = c->deleting ? PicoSubagentConfig_Delete(&c->original, c->error, sizeof(c->error)) :
            PicoSubagentConfig_Save(c->has_original ? &c->original : NULL, c->updated.profile.name,
                                   c->updated.source, c->error, sizeof(c->error));
        if (!ok) return false;
    }
    return true;
}

void PicoSubagentSettings_CompleteSave(PicoSubagentSettings *s, PicoSubagentSettingsSave *save)
{
    if (!s || !save) return;
    /* Rebase successful operations even on partial failure. Retrying must not
     * repeat a completed create, rename or delete against its old snapshot. */
    for (int i = 0; i < save->completed; i++)
    {
        SubagentChange *c = &save->changes[i];
        SubagentDraft *d = s->drafts[c->index];
        free(d->original.source);
        d->original = c->updated;
        c->updated.source = NULL;
        d->persisted = !c->deleting;
        d->dirty = false;
        d->error[0] = '\0';
    }
    if (save->completed < save->count)
    {
        SubagentChange *c = &save->changes[save->completed];
        SubagentDraft *d = s->drafts[c->index];
        snprintf(d->error, sizeof(d->error), "%s", c->error);
        d->expanded = true;
        s->reveal_card = c->index;
    }
    if (save->completed) PicoSubagentConfig_RequestRefresh(s->host);
    SaSyncReload(s);
}

static void SaEditor(PicoSubagentSettings *s, int i)
{
    SubagentDraft *d = s->drafts[i];
    if (d->original.path[0]) SaButton(CLAY_IDI("SubagentOpenFile", i), "Open in text editor", false);
    SaField(s, i, SA_NAME, "Name", "explore");
    SaField(s, i, SA_DESCRIPTION, "Description", "Optional");
    SaField(s, i, SA_PURPOSE, "Purpose", "Required");
    SaField(s, i, SA_MODEL, "Model", "Inherit");
    SaField(s, i, SA_EFFORT, "Effort", "Automatic");
    SaToggle(CLAY_IDI("SubagentFast", i), "Fast", d->profile.fast);
    SaToggle(CLAY_IDI("SubagentParallel", i), "Parallel-safe", d->profile.parallel_safe);
    SaToggle(CLAY_IDI("SubagentRestricted", i), "Restrict tools", d->profile.restricted_tools);
    if (d->profile.restricted_tools) SaField(s, i, SA_TOOLS, "Tools", "None");
    SaField(s, i, SA_LIMIT, "Max parallel tools", "Default");
    CLAY(CLAY_IDI("SubagentActions", i), {.layout = {.childGap = 8, .sizing = {.width = CLAY_SIZING_GROW(0)}}})
    {
        SaButton(CLAY_IDI("SubagentDuplicate", i), "Duplicate", false);
        SaButton(CLAY_IDI("SubagentDelete", i), "Delete…", false);
    }
}

void PicoSubagentSettings_Render(PicoSubagentSettings *s)
{
    if (!s) return;
    CLAY_AUTO_ID({.layout = {.childGap = 8}})
    {
        SaButton(CLAY_ID("SubagentNew"), "New profile", false);
        SaButton(CLAY_ID("SubagentRefresh"), "Refresh", false);
    }
    if (!s->count) SaText("No profiles", COLOR_MUTED);
    for (int i = 0; i < s->count; i++)
    {
        SubagentDraft *d = s->drafts[i];
        if (d->deleted && !d->persisted) continue;
        bool hover = Clay_PointerOver(CLAY_IDI("SubagentRow", i));
        CLAY(CLAY_IDI("SubagentCard", i),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 8, .padding = {10, 10, 8, 8},
                         .sizing = {.width = CLAY_SIZING_GROW(0)}},
              .backgroundColor = hover ? (Clay_Color){54, 54, 66, 255} : COLOR_CODE_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
        {
            CLAY(CLAY_IDI("SubagentRow", i), {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 4,
                                                       .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                Clay_String name = SaString(d->profile.name[0] ? d->profile.name : "New profile");
                /* Preserve the full filename stem of untouched invalid profiles. */
                if (d->persisted && !d->dirty) { name = SaString(d->original.filename); name.length -= 5; }
                CLAY_TEXT(name, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI,
                                                 .textColor = COLOR_TEXT, .wrapMode = CLAY_TEXT_WRAP_WORDS}));
                if (d->profile.description[0]) SaText(d->profile.description, COLOR_MUTED);
                if (d->persisted && !d->original.valid) SaText(d->original.error, COLOR_STATUS_ERR);
            }
            if (d->deleted)
            {
                SaText("Will be deleted on Apply", COLOR_MUTED);
                SaButton(CLAY_IDI("SubagentUndoDelete", i), "Undo delete", false);
            }
            else if (d->expanded) SaEditor(s, i);
            if (d->error[0])
            {
                CLAY(CLAY_IDI("SubagentError", i), {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
                    SaText(d->error, COLOR_STATUS_ERR);
            }
        }
    }
    if (s->error[0]) SaText(s->error, COLOR_STATUS_ERR);
}

void PicoSubagentSettings_RenderConfirmation(PicoSubagentSettings *s)
{
    if (!s || !s->confirm) return;
    CLAY(CLAY_ID("SubagentConfirmationDim"),
         {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 45,
                       .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER}},
          .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                     .sizing = {.width = CLAY_SIZING_FIXED(GetScreenWidth()), .height = CLAY_SIZING_FIXED(GetScreenHeight())}},
          .backgroundColor = {0, 0, 0, 120}})
    {
        CLAY(CLAY_ID("SubagentConfirmation"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 8, .padding = {16, 16, 14, 14},
                         .sizing = {.width = CLAY_SIZING_FIXED(GetScreenWidth() < 600 ? GetScreenWidth() - 40 : 520)}},
              .backgroundColor = COLOR_CODE_BG, .cornerRadius = CLAY_CORNER_RADIUS(8)})
        {
            const char *message = s->confirm == SA_CONFIRM_DELETE ? "Delete this profile when you Apply settings?" :
                s->confirm == SA_CONFIRM_REPLACE ? "Replace the invalid file? Applying will overwrite its contents." :
                s->confirm == SA_CONFIRM_REFRESH ? "Discard unsaved profile edits and refresh?" : "Discard unsaved settings and profile edits?";
            SaText(message, COLOR_TEXT);
            if (s->confirm == SA_CONFIRM_REPLACE) SaText(s->drafts[s->confirm_card]->original.filename, COLOR_MUTED);
            CLAY_AUTO_ID({.layout = {.childGap = 8}})
            {
                SaButton(CLAY_ID("SubagentConfirmYes"), s->confirm == SA_CONFIRM_DELETE ? "Delete profile" :
                         s->confirm == SA_CONFIRM_REPLACE ? "Replace file" : "Discard edits", true);
                SaButton(CLAY_ID("SubagentConfirmNo"), "Keep editing", false);
            }
            if (s->confirm == SA_CONFIRM_REPLACE) SaButton(CLAY_ID("SubagentRepairFile"), "Open in text editor", false);
        }
    }
}

static bool SaOver(const char *id) { return Clay_PointerOver(Clay_GetElementId(SaString(id))); }

void PicoSubagentSettings_AfterLayout(PicoSubagentSettings *s)
{
    if (!s) return;
    if (s->reveal_card >= 0)
    {
        Clay_ElementData target = Clay_GetElementData(CLAY_IDI("SubagentError", s->reveal_card));
        if (!target.found) target = Clay_GetElementData(CLAY_IDI("SubagentRow", s->reveal_card));
        Clay_ElementData viewport = Clay_GetElementData(CLAY_ID("SettingsPanelScroll"));
        Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("SettingsPanelScroll"));
        if (target.found && viewport.found && scroll.found && scroll.scrollPosition)
        {
            float bottom = target.boundingBox.y + target.boundingBox.height;
            float view_bottom = viewport.boundingBox.y + viewport.boundingBox.height;
            if (target.boundingBox.y < viewport.boundingBox.y) scroll.scrollPosition->y += viewport.boundingBox.y - target.boundingBox.y;
            else if (bottom > view_bottom) scroll.scrollPosition->y -= bottom - view_bottom;
            s->reveal_card = -1;
            s->host->ui_relayout_requested = true;
        }
    }
    if (!s->confirm)
    for (int i = 0; i < s->count; i++)
    {
        if (Clay_PointerOver(CLAY_IDI("SubagentRow", i))) s->host->hovered_clickable = true;
        for (int j = 0; j < SA_FIELDS; j++)
            if (Clay_PointerOver(CLAY_IDI("SubagentField", i * SA_FIELDS + j))) s->host->hovered_text = true;
        const char *buttons[] = {"SubagentOpenFile", "SubagentFast", "SubagentParallel", "SubagentRestricted",
                                 "SubagentDuplicate", "SubagentDelete", "SubagentUndoDelete"};
        for (size_t j = 0; j < sizeof(buttons) / sizeof(buttons[0]); j++)
            if (Clay_PointerOver(Clay_GetElementIdWithIndex(SaString(buttons[j]), i))) s->host->hovered_clickable = true;
    }
    const char *buttons[] = {"SubagentNew", "SubagentRefresh", "SubagentConfirmYes", "SubagentConfirmNo", "SubagentRepairFile"};
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (SaOver(buttons[i])) s->host->hovered_clickable = true;
}

static void SaOpenFile(PicoSubagentSettings *s, int card)
{
    if (card >= 0 && card < s->count && s->drafts[card]->original.path[0] &&
        !PicoHost_OpenExternal(s->host, s->drafts[card]->original.path, true))
    {
        snprintf(s->drafts[card]->error, sizeof(s->drafts[card]->error), "Could not open the profile in the system text editor.");
        s->reveal_card = card;
    }
}

static bool SaFrame(PicoSubagentSettings *s)
{
    if (!s) return false;
    bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (s->confirm)
    {
        if (IsKeyPressed(KEY_ESCAPE) || (click && SaOver("SubagentConfirmNo")))
        {
            if (s->confirm == SA_CONFIRM_LEAVE) s->leave_decision = -1;
            s->confirm = SA_CONFIRM_NONE;
        }
        else if (click && SaOver("SubagentConfirmYes"))
        {
            int confirm = s->confirm;
            s->confirm = SA_CONFIRM_NONE;
            if (confirm == SA_CONFIRM_LEAVE)
            {
                PicoSubagentSettings_Discard(s);
                s->leave_decision = 1;
            }
            else if (confirm == SA_CONFIRM_REFRESH)
            {
                SaLoadList(s);
                PicoSubagentConfig_RequestRefresh(s->host);
            }
            else if (confirm == SA_CONFIRM_REPLACE)
            {
                s->drafts[s->confirm_card]->replacement_confirmed = true;
                s->apply_requested = true;
            }
            else if (confirm == SA_CONFIRM_DELETE)
            {
                SaFocus(s, -1, -1);
                SubagentDraft *d = s->drafts[s->confirm_card];
                if (d->persisted) d->deleted = true;
                else
                {
                    SaFreeDraft(d);
                    memmove(s->drafts + s->confirm_card, s->drafts + s->confirm_card + 1,
                            (size_t)(s->count - s->confirm_card - 1) * sizeof(*s->drafts));
                    s->count--;
                }
            }
        }
        else if (click && SaOver("SubagentRepairFile")) SaOpenFile(s, s->confirm_card);
        while (GetCharPressed()) {}
        s->host->ui_relayout_requested = true;
        return true;
    }
    if (s->focus_card >= 0 && s->focus >= 0)
    {
        SubagentDraft *d = s->drafts[s->focus_card];
        if (IsKeyPressed(KEY_TAB))
        {
            int direction = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? -1 : 1;
            int next = (s->focus + direction + SA_FIELDS) % SA_FIELDS;
            if (next == SA_TOOLS && !d->profile.restricted_tools) next += direction;
            SaFocus(s, s->focus_card, next);
        }
        unsigned revision = s->field.revision;
        if (PicoTextField_HandleKeys(&s->field)) pico_host_request_redraw(s->host);
        if (revision != s->field.revision) d->dirty = true;
        Clay_ElementId id = CLAY_IDI("SubagentField", s->focus_card * SA_FIELDS + s->focus);
        PicoTextField_HandlePointer(&s->field, id, 8, FONT_REGULAR);
        Clay_ElementData field = Clay_GetElementData(id);
        if (field.found) PicoTextField_KeepCaretVisible(&s->field, field.boundingBox.width - 16, FONT_REGULAR);
    }
    if (!click) return false;
    if (SaOver("SubagentNew")) SaNew(s, -1);
    else if (SaOver("SubagentRefresh"))
    {
        if (SaDirty(s)) s->confirm = SA_CONFIRM_REFRESH;
        else { SaLoadList(s); PicoSubagentConfig_RequestRefresh(s->host); }
    }
    else for (int i = 0; i < s->count; i++)
    {
        SubagentDraft *d = s->drafts[i];
        if (Clay_PointerOver(CLAY_IDI("SubagentRow", i)))
        {
            d->expanded = !d->expanded;
            if (s->focus_card == i) SaFocus(s, -1, -1);
            break;
        }
        if (Clay_PointerOver(CLAY_IDI("SubagentUndoDelete", i)))
        {
            d->deleted = false;
            break;
        }
        if (!d->expanded || d->deleted) continue;
        if (Clay_PointerOver(CLAY_IDI("SubagentDuplicate", i))) { SaNew(s, i); break; }
        if (Clay_PointerOver(CLAY_IDI("SubagentDelete", i))) { s->confirm = SA_CONFIRM_DELETE; s->confirm_card = i; break; }
        if (Clay_PointerOver(CLAY_IDI("SubagentOpenFile", i))) { SaOpenFile(s, i); break; }
        if (Clay_PointerOver(CLAY_IDI("SubagentFast", i))) { d->profile.fast = !d->profile.fast; d->dirty = true; break; }
        if (Clay_PointerOver(CLAY_IDI("SubagentParallel", i))) { d->profile.parallel_safe = !d->profile.parallel_safe; d->dirty = true; break; }
        if (Clay_PointerOver(CLAY_IDI("SubagentRestricted", i)))
        {
            d->profile.restricted_tools = !d->profile.restricted_tools;
            d->dirty = true;
            SaFocus(s, -1, -1);
            break;
        }
        for (int j = 0; j < SA_FIELDS; j++)
        {
            Clay_ElementId id = CLAY_IDI("SubagentField", i * SA_FIELDS + j);
            if (!Clay_PointerOver(id)) continue;
            if (s->focus_card != i || s->focus != j) SaFocus(s, i, j);
            PicoTextField_HandlePointer(&s->field, id, 8, FONT_REGULAR);
            goto changed;
        }
    }
changed:
    s->host->ui_relayout_requested = true;
    return false;
}

bool PicoSubagentSettings_Frame(PicoSubagentSettings *s)
{
    bool result = SaFrame(s);
    if (s) SaSyncReload(s);
    return result;
}

void PicoSubagentSettings_Draw(PicoSubagentSettings *s)
{
    if (!s || s->focus_card < 0 || s->focus < 0 || s->confirm) return;
    PicoTextField_Draw(&s->field, CLAY_IDI("SubagentField", s->focus_card * SA_FIELDS + s->focus), 8, 6, FONT_REGULAR);
    pico_host_request_redraw_after(s->host, PicoTextField_NextBlink(&s->field));
}
