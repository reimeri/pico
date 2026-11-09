#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "pico/plugin.h"
#include "canonical.h"
#include "complete_internal.h"
#include "session_ref.h"
#include "file_mention.h"
#include "file_index.h"
#include "json.h"
#include "path.h"
#include "settings.h"
#include "host_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILES_MAX_BYTES (1024 * 1024)

typedef struct FilesState {
    PicoWorkspace *workspace;
    PicoFileIndex *index;
} FilesState;

int pico_files_complete(PicoWorkspace *workspace, const char *prefix, PicoCompleteItem *out, int max, void *state)
{
    FilesState *s = state ? state : PicoPlugins_WorkspaceState(workspace, "files");
    if (!s || max <= 0) return 0;
    char query[512];
    if (!PicoFileMention_Query(prefix, query, sizeof(query))) return 0;
    if (!s->index) s->index = PicoFileIndex_Open(PicoWorkspace_Path(s->workspace ? s->workspace : workspace));
    PicoFileIndexResult result;
    PicoFileIndex_Query(s->index, PicoComplete_TokenId(), query, &result);
    PicoComplete_SetStatus(result.pending ? "Searching workspace files…" : result.status);
    if (result.pending) return -1;
    int count = 0;
    for (int n = 0; n < result.count && count < max; n++)
    {
        if (!PicoFileMention_Encode(result.paths[n], out[count].insert, sizeof(out[count].insert))) continue;
        snprintf(out[count].label, sizeof(out[count].label), "%s", result.paths[n]);
        out[count].detail[0] = 0;
        count++;
    }
    return count;
}

static bool AlreadyAdded(char **paths, int n, const char *path)
{
    for (int i = 0; i < n; i++)
    {
        if (strcmp(paths[i], path) == 0)
        {
            return true;
        }
    }
    return false;
}

static void AppendFileBlock(JsonBuf *b, const char *abs, const char *body)
{
    JsonBuf_Puts(b, "\n\n<file name=\"");
    for (const char *p = abs; *p; p++)
    {
        if (*p == '&') JsonBuf_Puts(b, "&amp;");
        else if (*p == '"') JsonBuf_Puts(b, "&quot;");
        else if (*p == '<') JsonBuf_Puts(b, "&lt;");
        else if (*p == '>') JsonBuf_Puts(b, "&gt;");
        else JsonBuf_Putc(b, *p);
    }
    JsonBuf_Puts(b, "\">\n");
    JsonBuf_Puts(b, body);
    if (body[0] && body[strlen(body) - 1] != '\n')
    {
        JsonBuf_Putc(b, '\n');
    }
    JsonBuf_Puts(b, "</file>");
}

char *pico_files_expand_mentions(const char *workspace, const char *text, bool vision,
                                 char **parts_json_out)
{
    if (parts_json_out)
    {
        *parts_json_out = NULL;
    }
    if (!text || !text[0])
    {
        return NULL;
    }
    char *paths[32];
    int path_n = 0;
    int text_len = (int)strlen(text);
    PicoSessionRef ref;
    bool has_refs = PicoSessionRef_Next(text,text_len,0,&ref);
    for (int i = 0; text[i] && path_n < 32; i++)
    {
        /* Search the original input, never generated titles/paths or hidden
         * reference payloads. Referencing a session must not attach its log. */
        if (PicoSessionRef_Parse(text,text_len,i,&ref)) { i=ref.end-1; continue; }
        if (text[i] != '@' || (i > 0 && !isspace((unsigned char)text[i - 1]) && text[i - 1] != '\n'))
        {
            continue;
        }
        int end;
        char rel[4096];
        bool valid = PicoFileMention_Parse(text, text_len, i, &end, rel, sizeof(rel));
        i = end - 1;
        if (!valid) continue;
        char joined[4096];
        if (rel[0] == '/')
        {
            snprintf(joined, sizeof(joined), "%s", rel);
        }
        else if (!PicoPath_Format(joined, sizeof(joined), "%s/%s", workspace ? workspace : ".", rel))
        {
            continue;
        }
        char resolved[4096];
        if (!realpath(joined, resolved))
        {
            continue;
        }
        if (AlreadyAdded(paths, path_n, resolved))
        {
            continue;
        }
        char *copy = JsonDup(resolved);
        if (copy) paths[path_n++] = copy;
    }
    if (path_n == 0 && !has_refs) return NULL;
    char *projected = has_refs ? PicoSessionRef_Project(text) : NULL;
    if (has_refs && !projected)
    {
        for (int i=0; i<path_n; i++) free(paths[i]);
        return NULL;
    }
    JsonBuf b;
    JsonBuf_Init(&b);
    JsonBuf_Puts(&b, projected ? projected : text);
    free(projected);
    PicoLlmPart media[32];
    memset(media, 0, sizeof(media));
    int media_n = 0;
    for (int i = 0; i < path_n; i++)
    {
        bool image = pico_canonical_is_image_path(paths[i]);
        bool audio = pico_canonical_is_audio_path(paths[i]);
        if (vision && (image || audio) && media_n < 32)
        {
            media[media_n].kind = image ? PICO_LLM_PART_IMAGE : PICO_LLM_PART_AUDIO;
            media[media_n].path = paths[i];
            media[media_n].mime = (char *)pico_canonical_mime_for_path(paths[i]);
            media_n++;
            continue;
        }
        size_t len = 0;
        errno = 0;
        char *src = Pico_ReadFileLimited(paths[i], FILES_MAX_BYTES, &len);
        if (!src)
        {
            if (errno == EFBIG)
            {
                char note[80];
                if (len > 0)
                {
                    snprintf(note, sizeof(note), "(file too large, omitted, %.1f MB)",
                             (double)len / (1024.0 * 1024.0));
                }
                else
                {
                    snprintf(note, sizeof(note), "(file too large, omitted)");
                }
                AppendFileBlock(&b, paths[i], note);
            }
            free(paths[i]);
            paths[i] = NULL;
            continue;
        }
        if (memchr(src, '\0', len > 4096 ? 4096 : len))
        {
            AppendFileBlock(&b, paths[i], "(binary file omitted)");
        }
        else
        {
            AppendFileBlock(&b, paths[i], src);
        }
        free(src);
        free(paths[i]);
        paths[i] = NULL;
    }
    char *inline_text = JsonBuf_Steal(&b);
    if (media_n > 0 && parts_json_out)
    {
        PicoLlmPart parts[33];
        memset(parts, 0, sizeof(parts));
        int n = 0;
        parts[n].kind = PICO_LLM_PART_TEXT;
        parts[n].text = inline_text ? inline_text : (char *)"";
        n++;
        for (int i = 0; i < media_n; i++)
        {
            parts[n++] = media[i];
        }
        *parts_json_out = pico_canonical_parts_json(parts, n);
    }
    for (int i = 0; i < media_n; i++)
    {
        free(media[i].path);
    }
    return inline_text;
}

static void FilesBeforeSubmit(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    PicoHost *app = workspace ? workspace->host : NULL;
    PicoAgent *agent = PicoHost_FindAgent(app, event ? event->agent_id : 0);
    const char *root;
    (void)state;
    if (!app || app->submit_cancel || !app->composer.text)
    {
        return;
    }
    bool vision = false;
    PicoModel *model = event && event->submit_kind == PICO_SUBMIT_STEERING
                           ? PicoSettings_ActiveModel(agent) : PicoSettings_SelectedModel(agent);
    if (model)
    {
        vision = model->vision;
    }
    root = PicoAgent_WorkspacePath(agent);
    if (!root[0])
    {
        root = PicoWorkspace_Path(workspace);
    }
    char *parts = NULL;
    const char *text = app->agent_input && app->agent_input[0] ? app->agent_input : app->composer.text;
    char *expanded = pico_files_expand_mentions(root[0] ? root : ".", text, vision, &parts);
    if (!expanded)
    {
        return;
    }
    pico_host_set_agent_input(app, expanded);
    pico_host_set_agent_parts(app, parts);
}

static int FilesWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    FilesState *s = (FilesState *)calloc(1, sizeof(FilesState));
    if (!s)
    {
        return 1;
    }
    if (!state_out)
    {
        free(s);
        return 1;
    }
    s->workspace = workspace;
    *state_out = s;
    pico_workspace_add_completer(workspace, '@', false, pico_files_complete, NULL);
    pico_workspace_add_hook(workspace, PICO_HOOK_BEFORE_SUBMIT, FilesBeforeSubmit);
    return 0;
}

static void FilesWorkspaceShutdown(PicoWorkspace *workspace, void *state)
{
    (void)workspace;
    FilesState *s = (FilesState *)state;
    if (s)
    {
        PicoFileIndex_Close(s->index);
        free(s);
    }
}

PicoExt pico_ext_files(void)
{
    return (PicoExt){
        .abi = PICO_EXT_ABI,
        .name = "files",
        .description = "Workspace file completion",
        .workspace_init = FilesWorkspaceInit,
        .workspace_shutdown = FilesWorkspaceShutdown,
    };
}
