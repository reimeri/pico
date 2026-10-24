#include "session.h"
#include "agent.h"
#include "canonical.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

bool PicoSession_RememberMissingImage(PicoAgent *agent, const char *path, const char *notice)
{
    for (int i = 0; i < agent->missing_image_warning_count; ++i)
        if (!strcmp(agent->missing_image_warnings[i].path, path)) return true;
    char *owned_path = JsonDup(path), *owned_notice = JsonDup(notice);
    PicoMissingImageWarning *next = realloc(agent->missing_image_warnings,
        (size_t)(agent->missing_image_warning_count + 1) * sizeof(*next));
    if (next) agent->missing_image_warnings = next;
    if (!owned_path || !owned_notice || !next) { free(owned_path); free(owned_notice); return false; }
    next[agent->missing_image_warning_count++] = (PicoMissingImageWarning){owned_path, owned_notice};
    return true;
}

void PicoSession_ClearMissingImageWarnings(PicoAgent *agent)
{
    for (int i = 0; i < agent->missing_image_warning_count; ++i) {
        free(agent->missing_image_warnings[i].path); free(agent->missing_image_warnings[i].notice);
    }
    free(agent->missing_image_warnings); agent->missing_image_warnings = NULL;
    agent->missing_image_warning_count = 0;
}

bool PicoSession_PrepareRequestImages(PicoHost *app, PicoAgent *agent, char **input, int count)
{
    PicoAgentRt *rt = agent->runtime;
    char **paths = NULL; int missing = 0;
    bool ok = true;
    for (int i = 0; ok && i < count; ++i)
        ok = pico_canonical_omit_missing_images(&input[i], &paths, &missing);
    for (int i = 0; ok && i < missing; ++i) {
        bool warned = false;
        for (int j = 0; j < agent->missing_image_warning_count; ++j)
            if (!strcmp(agent->missing_image_warnings[j].path, paths[i])) warned = true;
        if (warned) continue;
        JsonBuf notice; JsonBuf_Init(&notice);
        JsonBuf_Puts(&notice, "**Missing image** — Continuing without this image in model requests. "
            "Restore the file at its original path "
            "to include it again; saved history has not been changed.\n\n    ");
        JsonBuf_String(&notice, paths[i]);
        char *text = JsonBuf_Steal(&notice);
        ok = text && PicoSession_RememberMissingImage(agent, paths[i], text);
        if (ok && !PicoAgent_AddNoticePersisted(app, agent, PICO_NOTICE_WARNING, text)) {
            /* No notice was accepted and no message hook ran; allow a retry. */
            PicoMissingImageWarning *warning = &agent->missing_image_warnings[--agent->missing_image_warning_count];
            free(warning->path); free(warning->notice);
            ok = false;
        }
        free(text);
        if (agent->runtime != rt || PicoAgent_CancelRequested(agent)) { ok = false; break; }
    }
    for (int i = 0; i < missing; ++i) free(paths[i]);
    free(paths); return ok;
}

