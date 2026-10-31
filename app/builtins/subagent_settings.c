#include "subagent_settings.h"
#include "host_internal.h"
#include "subagent_config.h"
#include "text_field_ui.h"
#include "settings_controls.h"
#include "json.h"
#include "overlay.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    SA_NAME,
    SA_DESCRIPTION,
    SA_PURPOSE,
    SA_MODEL,
    SA_EFFORT,
    SA_TOOLS,
    SA_LIMIT,
    SA_FIELDS
};
enum
{
    SA_CONFIRM_NONE,
    SA_CONFIRM_DISCARD,
    SA_CONFIRM_LEAVE,
    SA_CONFIRM_DELETE,
    SA_CONFIRM_REPLACE
};
enum
{
    SA_ACTION_SELECT,
    SA_ACTION_NEW,
    SA_ACTION_DUPLICATE,
    SA_ACTION_REFRESH
};

typedef struct ProfileSave
{
    PicoSubagentDocument original;
    bool has_original, deleting, ok;
    char name[65];
    char *source;
    char error[512];
} ProfileSave;

struct PicoSubagentSettings
{
    PicoHost *host;
    PicoSubagentDocument *documents;
    int count, selected;
    PicoSubagentDocument *original; /* borrowed from documents until selection changes */
    PicoSubagentProfileInfo profile;
    char purpose[4097]; /* Escaped newlines/backslashes for the single-line field. */
    char tools[PICO_MAX_TOOLS * 129];
    char limit[32];
    char error[512];
    bool editing, dirty, reveal_editor, reveal_error;
    int focus, confirm, action, action_index, leave_decision;
    PicoTextField field;
    ProfileSave *save;
};

static void SaSyncReload(PicoSubagentSettings *s)
{
    s->host->subagent_settings_reload_blocked = s->dirty || s->save || s->confirm != SA_CONFIRM_NONE;
}

static Clay_String SaString(const char *s)
{
    return (Clay_String){.chars = s, .length = (int32_t)strlen(s)};
}

static void SaText(const char *text, Clay_Color color)
{
    CLAY_TEXT(SaString(text), CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_CAPTION, .textColor = color, .wrapMode = CLAY_TEXT_WRAP_WORDS}));
}

static void SaButton(PicoSubagentSettings *s, Clay_ElementId id, const char *label, bool primary)
{
    bool saving = s->save && id.id == CLAY_ID("SubagentSave").id;
    PicoSettingsControl_Button(id, label, !s->save, primary, saving);
}

static char *SaBuffer(PicoSubagentSettings *s, int index, int *capacity)
{
    switch (index)
    {
    case SA_NAME:
        *capacity = sizeof(s->profile.name);
        return s->profile.name;
    case SA_DESCRIPTION:
        *capacity = sizeof(s->profile.description);
        return s->profile.description;
    case SA_PURPOSE:
        *capacity = sizeof(s->purpose);
        return s->purpose;
    case SA_MODEL:
        *capacity = sizeof(s->profile.model);
        return s->profile.model;
    case SA_EFFORT:
        *capacity = sizeof(s->profile.effort);
        return s->profile.effort;
    case SA_TOOLS:
        *capacity = sizeof(s->tools);
        return s->tools;
    case SA_LIMIT:
        *capacity = sizeof(s->limit);
        return s->limit;
    default:
        *capacity = 0;
        return NULL;
    }
}

static void SaFocus(PicoSubagentSettings *s, int index)
{
    PicoTextField_Unbind(&s->field);
    s->focus = index;
    if (index >= 0)
    {
        int cap;
        char *buffer = SaBuffer(s, index, &cap);
        PicoTextField_Bind(&s->field, buffer, cap);
    }
}

static void SaField(PicoSubagentSettings *s, int index, const char *label, const char *placeholder)
{
    int capacity;
    char *text = SaBuffer(s, index, &capacity);
    (void)capacity;
    SETTINGS_ROW_BEGIN
        PicoSettingsControl_Label(label);
        PicoSettingsControl_Field(CLAY_IDI("SubagentField", index), text, placeholder, &s->field, s->focus == index);
    }
}

static void SaToggle(Clay_ElementId id, const char *label, bool on)
{
    SETTINGS_ROW_BEGIN
        PicoSettingsControl_Toggle(id, label, on);
    }
}

static void SaEncodePurpose(PicoSubagentSettings *s)
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

static bool SaDecodePurpose(PicoSubagentSettings *s)
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

static void SaLoadList(PicoSubagentSettings *s)
{
    SaFocus(s, -1);
    s->original = NULL;
    s->editing = s->dirty = false;
    s->selected = -1;
    s->error[0] = '\0';
    s->confirm = SA_CONFIRM_NONE;
    s->action = s->leave_decision = 0;
    PicoSubagentConfig_FreeDocuments(s->documents, s->count);
    s->documents = NULL;
    s->count = 0;
    PicoSubagentConfig_List(&s->documents, &s->count, s->error, sizeof(s->error));
    SaSyncReload(s);
}

static void SaEdit(PicoSubagentSettings *s, int index, bool duplicate)
{
    SaFocus(s, -1);
    memset(&s->profile, 0, sizeof(s->profile));
    s->tools[0] = s->limit[0] = s->error[0] = '\0';
    s->original = NULL;
    s->selected = index;
    if (index >= 0 && index < s->count)
    {
        s->profile = s->documents[index].profile;
        s->original = &s->documents[index];
    }
    if (duplicate)
    {
        char base[65];
        snprintf(base, sizeof(base), "%s", s->profile.name);
        for (int n = 1; n < 10000; n++)
        {
            snprintf(s->profile.name, sizeof(s->profile.name), "%.*s-copy%d", 52, base, n);
            bool used = false;
            for (int i = 0; i < s->count; i++)
                if (!strcmp(s->profile.name, s->documents[i].profile.name))
                    used = true;
            if (!used)
                break;
        }
        /* Keep original text for comment preservation, but do not move its file. */
        s->selected = -1;
    }
    SaEncodePurpose(s);
    size_t at = 0;
    for (int i = 0; i < s->profile.tool_count; i++)
        at += (size_t)snprintf(s->tools + at, sizeof(s->tools) - at, "%s%s", i ? ", " : "", s->profile.tools[i]);
    if (s->profile.max_parallel_tools)
        snprintf(s->limit, sizeof(s->limit), "%d", s->profile.max_parallel_tools);
    s->editing = true;
    s->reveal_editor = true;
    s->dirty = duplicate;
}

PicoSubagentSettings *PicoSubagentSettings_Create(PicoHost *host)
{
    PicoSubagentSettings *s = calloc(1, sizeof(*s));
    if (s)
    {
        s->host = host;
        s->selected = s->focus = -1;
    }
    return s;
}

void PicoSubagentSettings_Free(PicoSubagentSettings *s)
{
    if (!s)
        return;
    s->host->subagent_settings_reload_blocked = false;
    PicoTextField_Free(&s->field);
    PicoSubagentConfig_FreeDocuments(s->documents, s->count);
    free(s);
}

void PicoSubagentSettings_Open(PicoSubagentSettings *s)
{
    if (s && !s->save)
        SaLoadList(s);
}

bool PicoSubagentSettings_RequestLeave(PicoSubagentSettings *s)
{
    if (!s)
        return true;
    if (s->save)
        return false;
    if (!s->dirty)
    {
        s->confirm = SA_CONFIRM_NONE;
        SaFocus(s, -1);
        SaSyncReload(s);
        return true;
    }
    s->confirm = SA_CONFIRM_LEAVE;
    SaSyncReload(s);
    return false;
}

int PicoSubagentSettings_TakeLeaveDecision(PicoSubagentSettings *s)
{
    int result = s ? s->leave_decision : 0;
    if (s)
        s->leave_decision = 0;
    return result;
}

static void SaSaveDestroy(void *arg)
{
    ProfileSave *save = arg;
    free(save->original.source);
    free(save->source);
    free(save);
}

static void SaSaveRun(void *arg)
{
    ProfileSave *save = arg;
    save->ok = save->deleting ? PicoSubagentConfig_Delete(&save->original, save->error, sizeof(save->error)) : PicoSubagentConfig_Save(save->has_original ? &save->original : NULL, save->name, save->source, save->error, sizeof(save->error));
}

/* Settings owns this editor for its entire host-instance lifetime. Reload is
 * blocked while the persistence job uses its completion callback. */
static void SaSaveCompleted(PicoHost *host, void *arg)
{
    ProfileSave *save = arg;
    PicoSubagentSettings *s = PicoSettingsUi_SubagentEditor(host);
    if (s && s->save == save)
    {
        s->save = NULL;
        if (save->ok)
            SaLoadList(s);
        else
        {
            snprintf(s->error, sizeof(s->error), "%s", save->error[0] ? save->error : "Could not save profile.");
            s->reveal_error = true;
        }
        SaSyncReload(s);
    }
    if (save->ok)
    {
        PicoSubagentConfig_RequestRefresh(host);
        PicoOverlay_Notify(host, save->deleting ? "Profile deleted." : "Profile saved.");
    }
    host->ui_relayout_requested = true;
}

static bool SaReadFields(PicoSubagentSettings *s)
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

static void SaSave(PicoSubagentSettings *s, bool deleting, bool replacement_confirmed)
{
    if (s->save)
        return;
    s->error[0] = '\0';
    if (!deleting && s->original && !s->original->parseable && !replacement_confirmed)
    {
        s->confirm = SA_CONFIRM_REPLACE;
        return;
    }
    ProfileSave *save = calloc(1, sizeof(*save));
    if (!save)
    {
        snprintf(s->error, sizeof(s->error), "Out of memory saving profile.");
        return;
    }
    save->deleting = deleting;
    save->has_original = s->selected >= 0 && s->original;
    if (save->has_original)
    {
        save->original = *s->original;
        save->original.source = s->original->source ? malloc(s->original->source_len + 1) : NULL;
        if (s->original->source && !save->original.source)
        {
            SaSaveDestroy(save);
            return;
        }
        if (save->original.source)
            memcpy(save->original.source, s->original->source, s->original->source_len + 1);
    }
    if (!deleting)
    {
        if (!SaReadFields(s))
        {
            SaSaveDestroy(save);
            return;
        }
        save->source = PicoSubagentConfig_Build(s->original, &s->profile, s->error, sizeof(s->error));
        if (!save->source)
        {
            SaSaveDestroy(save);
            return;
        }
        snprintf(save->name, sizeof(save->name), "%s", s->profile.name);
    }
    if (!PicoHost_EnqueueUiPersist(s->host, save, SaSaveRun, SaSaveCompleted, SaSaveDestroy))
    {
        SaSaveDestroy(save);
        snprintf(s->error, sizeof(s->error), "Could not queue profile save.");
        return;
    }
    s->save = save;
    SaSyncReload(s);
    SaFocus(s, -1);
}

static void SaDoAction(PicoSubagentSettings *s, int action, int index)
{
    s->confirm = SA_CONFIRM_NONE;
    if (action == SA_ACTION_REFRESH)
    {
        SaLoadList(s);
        PicoSubagentConfig_RequestRefresh(s->host);
    }
    else
        SaEdit(s, action == SA_ACTION_NEW ? -1 : index, action == SA_ACTION_DUPLICATE);
}

static void SaRequestAction(PicoSubagentSettings *s, int action, int index)
{
    if (s->dirty)
    {
        s->action = action;
        s->action_index = index;
        s->confirm = SA_CONFIRM_DISCARD;
    }
    else
        SaDoAction(s, action, index);
}

static void SaActions(PicoSubagentSettings *s, float width)
{
    bool existing = s->selected >= 0;
    const char *save_label = s->save ? "Saving…" : "Save";
    float required = PicoSettingsControl_ButtonWidth("Cancel") + PicoSettingsControl_ButtonWidth(save_label) + 8;
    if (existing) required += PicoSettingsControl_ButtonWidth("Duplicate") + PicoSettingsControl_ButtonWidth("Delete…") + 16;
    bool stacked = width < required;
    CLAY(CLAY_ID("SubagentActions"), {.layout = {.layoutDirection = stacked ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT,
                                               .childGap = 8, .sizing = {.width = CLAY_SIZING_GROW(0)}}})
    {
        if (existing)
        {
            CLAY(CLAY_ID("SubagentManageActions"), {.layout = {.childGap = 8}})
            {
                SaButton(s, CLAY_ID("SubagentDuplicate"), "Duplicate", false);
                SaButton(s, CLAY_ID("SubagentDelete"), "Delete…", false);
            }
        }
        CLAY(CLAY_ID("SubagentEditActions"), {.layout = {.childGap = 8, .childAlignment = {.x = CLAY_ALIGN_X_RIGHT},
                                                       .sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            SaButton(s, CLAY_ID("SubagentCancel"), "Cancel", false);
            SaButton(s, CLAY_ID("SubagentSave"), save_label, true);
        }
    }
}

void PicoSubagentSettings_Render(PicoSubagentSettings *s, float width)
{
    if (!s)
        return;
    CLAY_AUTO_ID({.layout = {.childGap = 8}})
    {
        SaButton(s, CLAY_ID("SubagentNew"), "New profile", false);
        SaButton(s, CLAY_ID("SubagentRefresh"), "Refresh", false);
    }
    if (!s->count)
        SaText("No profiles", COLOR_MUTED);
    for (int i = 0; i < s->count; i++)
    {
        PicoSubagentDocument *d = &s->documents[i];
        CLAY(CLAY_IDI("SubagentRow", i), {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 4, .padding = {10, 10, 8, 8}, .sizing = {.width = CLAY_SIZING_GROW(0)}},
                                          .backgroundColor = s->selected == i || Clay_PointerOver(CLAY_IDI("SubagentRow", i)) ? COLOR_CODE_BG : COLOR_FOOTER_BG,
                                          .cornerRadius = CLAY_CORNER_RADIUS(6)})
        {
            /* Keep the full stem even for invalid or oversized profile names. */
            Clay_String name = SaString(d->filename);
            name.length -= 5; /* Profile discovery only lists *.json files. */
            CLAY_TEXT(name, CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI,
                                              .textColor = COLOR_TEXT, .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            if (!d->valid)
                SaText(d->error, COLOR_STATUS_ERR);
            else if (d->profile.description[0])
                SaText(d->profile.description, COLOR_MUTED);
        }
    }
    if (s->editing)
    {
        CLAY(CLAY_ID("SubagentEditorStart"), {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            SaText(s->selected >= 0 ? "Edit profile" : "New profile", COLOR_TEXT);
        }
        if (s->original && !s->original->valid)
        {
            SaText(s->original->error, COLOR_STATUS_ERR);
        }
        if (s->original)
            SaButton(s, CLAY_ID("SubagentOpenFile"), "Open in text editor", false);
        SaField(s, SA_NAME, "Name", "explore");
        SaField(s, SA_DESCRIPTION, "Description", "Optional");
        SaField(s, SA_PURPOSE, "Purpose", "Required");
        SaField(s, SA_MODEL, "Model", "Inherit");
        SaField(s, SA_EFFORT, "Effort", "Automatic");
        SaToggle(CLAY_ID("SubagentFast"), "Fast", s->profile.fast);
        SaToggle(CLAY_ID("SubagentParallel"), "Parallel-safe", s->profile.parallel_safe);
        SaToggle(CLAY_ID("SubagentRestricted"), "Restrict tools", s->profile.restricted_tools);
        if (s->profile.restricted_tools)
            SaField(s, SA_TOOLS, "Tools", "None");
        SaField(s, SA_LIMIT, "Max parallel tools", "Default");
        SaActions(s, width);
    }
    if (s->error[0])
    {
        CLAY(CLAY_ID("SubagentError"), {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
        {
            SaText(s->error, COLOR_STATUS_ERR);
        }
    }
    if (s->confirm)
    {
        CLAY(CLAY_ID("SubagentConfirmationDim"), {.floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 45, .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER}},
                                                  .layout = {.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                                                             .sizing = {.width = CLAY_SIZING_FIXED(GetScreenWidth()), .height = CLAY_SIZING_FIXED(GetScreenHeight())}},
                                                  .backgroundColor = {0, 0, 0, 120}})
        {
            CLAY(CLAY_ID("SubagentConfirmation"), {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 8, .padding = {16, 16, 14, 14}, .sizing = {.width = CLAY_SIZING_FIXED(GetScreenWidth() < 600 ? GetScreenWidth() - 40 : 520)}},
                                                   .backgroundColor = COLOR_CODE_BG,
                                                   .cornerRadius = CLAY_CORNER_RADIUS(8)})
            {
                const char *message = s->confirm == SA_CONFIRM_DELETE ? "Delete this profile?" :
                    s->confirm == SA_CONFIRM_REPLACE ? "Replace the invalid file? Its contents will be overwritten." :
                    "Discard unsaved profile edits?";
                SaText(message, COLOR_TEXT);
                CLAY_AUTO_ID({.layout = {.childGap = 8}})
                {
                    SaButton(s, CLAY_ID("SubagentConfirmYes"), s->confirm == SA_CONFIRM_DELETE ? "Delete profile" : s->confirm == SA_CONFIRM_REPLACE ? "Replace file"
                                                                                                                                                  : "Discard edits",
                             true);
                    SaButton(s, CLAY_ID("SubagentConfirmNo"), "Keep editing", false);
                }
                if (s->confirm == SA_CONFIRM_REPLACE)
                    SaButton(s, CLAY_ID("SubagentRepairFile"), "Open in text editor", false);
            }
        }
    }
}

static bool SaOver(const char *id) { return Clay_PointerOver(Clay_GetElementId(SaString(id))); }

void PicoSubagentSettings_AfterLayout(PicoSubagentSettings *s)
{
    if (!s)
        return;
    if (s->reveal_editor)
    {
        Clay_ElementData target = Clay_GetElementData(CLAY_ID("SubagentEditorStart"));
        Clay_ElementData viewport = Clay_GetElementData(CLAY_ID("SettingsModalScroll"));
        Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("SettingsModalScroll"));
        if (target.found && viewport.found && scroll.found && scroll.scrollPosition)
        {
            float offset = target.boundingBox.y - viewport.boundingBox.y;
            scroll.scrollPosition->y -= offset;
            s->reveal_editor = false;
            s->host->ui_relayout_requested = true;
        }
    }
    if (s->reveal_error)
    {
        Clay_ElementData error = Clay_GetElementData(CLAY_ID("SubagentError"));
        Clay_ElementData viewport = Clay_GetElementData(CLAY_ID("SettingsModalScroll"));
        Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("SettingsModalScroll"));
        if (error.found && viewport.found && scroll.found && scroll.scrollPosition)
        {
            float bottom = error.boundingBox.y + error.boundingBox.height;
            float view_bottom = viewport.boundingBox.y + viewport.boundingBox.height;
            if (bottom > view_bottom)
                scroll.scrollPosition->y -= bottom - view_bottom;
            else if (error.boundingBox.y < viewport.boundingBox.y)
                scroll.scrollPosition->y += viewport.boundingBox.y - error.boundingBox.y;
            s->reveal_error = false;
            s->host->ui_relayout_requested = true;
        }
    }
    if (s->editing && !s->confirm)
        for (int i = 0; i < SA_FIELDS; i++)
            if (Clay_PointerOver(CLAY_IDI("SubagentField", i)))
                s->host->hovered_text = true;
    const char *buttons[] = {"SubagentNew", "SubagentRefresh", "SubagentOpenFile", "SubagentFast", "SubagentParallel",
                             "SubagentRestricted", "SubagentSave", "SubagentCancel", "SubagentDuplicate", "SubagentDelete",
                             "SubagentConfirmYes", "SubagentConfirmNo", "SubagentRepairFile"};
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (SaOver(buttons[i]))
            s->host->hovered_clickable = true;
    for (int i = 0; i < s->count && !s->confirm; i++)
        if (Clay_PointerOver(CLAY_IDI("SubagentRow", i)))
            s->host->hovered_clickable = true;
}

static void SaOpenFile(PicoSubagentSettings *s)
{
    if (s->original && !PicoHost_OpenExternal(s->host, s->original->path, true))
        snprintf(s->error, sizeof(s->error), "Could not open the profile in the system text editor.");
}

static bool SaFrame(PicoSubagentSettings *s)
{
    if (!s)
        return false;
    if (s->save)
    {
        while (GetCharPressed())
        {
        }
        return true;
    }
    bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (s->confirm)
    {
        if (IsKeyPressed(KEY_ESCAPE) || (click && SaOver("SubagentConfirmNo")))
        {
            if (s->confirm == SA_CONFIRM_LEAVE)
                s->leave_decision = -1;
            s->confirm = SA_CONFIRM_NONE;
        }
        else if (click && SaOver("SubagentConfirmYes"))
        {
            int confirm = s->confirm;
            s->confirm = SA_CONFIRM_NONE;
            if (confirm == SA_CONFIRM_LEAVE)
            {
                s->dirty = false;
                s->leave_decision = 1;
            }
            else if (confirm == SA_CONFIRM_DELETE)
                SaSave(s, true, false);
            else if (confirm == SA_CONFIRM_REPLACE)
                SaSave(s, false, true);
            else if (s->action == -1)
            {
                s->editing = s->dirty = false;
                SaFocus(s, -1);
            }
            else
                SaDoAction(s, s->action, s->action_index);
        }
        else if (click && SaOver("SubagentRepairFile"))
            SaOpenFile(s);
        while (GetCharPressed())
        {
        }
        s->host->ui_relayout_requested = true;
        return true;
    }
    if (s->editing && s->focus >= 0)
    {
        if (IsKeyPressed(KEY_TAB))
        {
            int next = (s->focus + (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? SA_FIELDS - 1 : 1)) % SA_FIELDS;
            if (next == SA_TOOLS && !s->profile.restricted_tools)
                next = SA_LIMIT;
            SaFocus(s, next);
        }
        unsigned revision = s->field.revision;
        if (PicoTextField_HandleKeys(&s->field))
            pico_host_request_redraw(s->host);
        if (revision != s->field.revision)
            s->dirty = true;
        PicoTextField_HandlePointer(&s->field, CLAY_IDI("SubagentField", s->focus), 8, FONT_REGULAR);
        Clay_ElementData field = Clay_GetElementData(CLAY_IDI("SubagentField", s->focus));
        if (field.found)
            PicoTextField_KeepCaretVisible(&s->field, field.boundingBox.width - 16, FONT_REGULAR);
    }
    if (!click)
        return false;
    if (SaOver("SubagentNew"))
        SaRequestAction(s, SA_ACTION_NEW, -1);
    else if (SaOver("SubagentRefresh"))
        SaRequestAction(s, SA_ACTION_REFRESH, -1);
    else
    {
        for (int i = 0; i < s->count; i++)
        {
            if (Clay_PointerOver(CLAY_IDI("SubagentRow", i)))
            {
                SaRequestAction(s, SA_ACTION_SELECT, i);
                goto changed;
            }
        }
        if (!s->editing)
            return false;
        if (SaOver("SubagentSave"))
            SaSave(s, false, false);
        else if (SaOver("SubagentCancel"))
        {
            if (s->dirty)
            {
                s->action = -1;
                s->confirm = SA_CONFIRM_DISCARD;
            }
            else
            {
                s->editing = false;
                SaFocus(s, -1);
            }
        }
        else if (SaOver("SubagentDuplicate"))
            SaRequestAction(s, SA_ACTION_DUPLICATE, s->selected);
        else if (SaOver("SubagentDelete"))
            s->confirm = SA_CONFIRM_DELETE;
        else if (SaOver("SubagentOpenFile"))
            SaOpenFile(s);
        else if (SaOver("SubagentFast"))
        {
            s->profile.fast = !s->profile.fast;
            s->dirty = true;
        }
        else if (SaOver("SubagentParallel"))
        {
            s->profile.parallel_safe = !s->profile.parallel_safe;
            s->dirty = true;
        }
        else if (SaOver("SubagentRestricted"))
        {
            s->profile.restricted_tools = !s->profile.restricted_tools;
            s->dirty = true;
            SaFocus(s, -1);
        }
        else
            for (int i = 0; i < SA_FIELDS; i++)
            {
                if (Clay_PointerOver(CLAY_IDI("SubagentField", i)))
                {
                    if (s->focus != i)
                        SaFocus(s, i);
                    PicoTextField_HandlePointer(&s->field, CLAY_IDI("SubagentField", i), 8, FONT_REGULAR);
                    break;
                }
            }
    }
changed:
    s->host->ui_relayout_requested = true;
    return false;
}

bool PicoSubagentSettings_Frame(PicoSubagentSettings *s)
{
    if (!s)
        return false;
    char previous_error[sizeof(s->error)];
    memcpy(previous_error, s->error, sizeof(previous_error));
    bool consumed = SaFrame(s);
    if (s->error[0] && strcmp(previous_error, s->error))
        s->reveal_error = true;
    SaSyncReload(s);
    return consumed;
}

void PicoSubagentSettings_Draw(PicoSubagentSettings *s)
{
    if (!s || s->focus < 0 || s->confirm || s->save)
        return;
    PicoTextField_Draw(&s->field, CLAY_IDI("SubagentField", s->focus), 8, 6, FONT_REGULAR);
    pico_host_request_redraw_after(s->host, PicoTextField_NextBlink(&s->field));
}
