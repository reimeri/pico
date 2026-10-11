#include "canonical.h"
#include "composer_internal.h"
#include "session_ref.h"
#include "json.h"
#include "host_internal.h"

void PicoChat_ResetBottomSpace(PicoHost *host) { (void)host; }

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

char *pico_files_expand_mentions(const char *workspace, const char *text, bool vision,
                                 char **parts_json_out);
PicoExt pico_ext_composer(void);

static int g_failed;
static void *g_composer_state = NULL;

void *PicoPlugins_HostState(const PicoHost *host, const char *name)
{
    (void)host;
    if (name && strcmp(name, "composer") == 0)
    {
        return g_composer_state;
    }
    return NULL;
}

void *PicoPlugins_WorkspaceState(const PicoWorkspace *workspace, const char *name)
{
    (void)workspace;
    (void)name;
    return NULL;
}

void PicoPlugins_InitWorkspace(PicoHost *host, PicoWorkspace *workspace)
{
    (void)host;
    (void)workspace;
}

void PicoPlugins_CancelCompiles(PicoHost *host) { (void)host; }

void PicoPlugins_Poll(PicoHost *host) { (void)host; }

bool PicoPlugins_LoadWorkspaceSources(PicoHost *host, PicoWorkspace *workspace)
{
    (void)host;
    (void)workspace;
    return true;
}

static void Check(bool ok, const char *message)
{
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        g_failed = 1;
    }
}

static bool WriteBytes(const char *path, const void *body, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        return false;
    }
    bool ok = fwrite(body, 1, n, f) == n;
    fclose(f);
    return ok;
}

static char *MakeTempDir(void)
{
    char *temp = (char *)malloc(32);
    if (!temp)
    {
        return NULL;
    }
    snprintf(temp, 32, "/tmp/pico-composer-XXXXXX");
    if (!mkdtemp(temp))
    {
        free(temp);
        return NULL;
    }
    return temp;
}

static void TestAttachMergeImagePart(void)
{
    PicoComposer_DiscardAttachments();
    char *temp = MakeTempDir();
    if (!temp)
    {
        Check(false, "could not create temp dir");
        return;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s/shot.png", temp);
    if (!WriteBytes(path, "PNG\0bin", 7))
    {
        Check(false, "could not write shot.png");
        rmdir(temp);
        free(temp);
        return;
    }
    Check(pico_composer_attach_path(path, false), "attaching a local png succeeds");
    Check(pico_composer_attachment_count() == 1, "one attachment is stored");
    char *parts = pico_composer_merge_parts("look", NULL);
    Check(parts && strstr(parts, "\"type\":\"image\"") && strstr(parts, "shot.png") &&
              strstr(parts, "\"type\":\"text\"") && strstr(parts, "look"),
          "merge of typed text and a pasted image produces text plus image parts");
    free(parts);
    PicoComposer_DiscardAttachments();
    unlink(path);
    rmdir(temp);
    free(temp);
}

static void TestMergeKeepsMentionAndPaste(void)
{
    PicoComposer_DiscardAttachments();
    char *temp = MakeTempDir();
    if (!temp)
    {
        Check(false, "could not create mention temp dir");
        return;
    }
    char mention[4096];
    char paste[4096];
    snprintf(mention, sizeof(mention), "%s/pic.png", temp);
    snprintf(paste, sizeof(paste), "%s/shot.png", temp);
    if (!WriteBytes(mention, "PNG\0bin", 7) || !WriteBytes(paste, "PNG\0bin", 7))
    {
        Check(false, "could not write mention and paste images");
        unlink(mention);
        unlink(paste);
        rmdir(temp);
        free(temp);
        return;
    }
    char *mention_parts = NULL;
    char *expanded = pico_files_expand_mentions(temp, "see @pic.png", true, &mention_parts);
    Check(mention_parts && strstr(mention_parts, "pic.png"),
          "@pic.png on a vision model produces an image part");
    Check(pico_composer_attach_path(paste, false), "pasted image attaches beside a mention");
    char *merged = pico_composer_merge_parts(expanded, mention_parts);
    Check(merged && strstr(merged, "pic.png") && strstr(merged, "shot.png"),
          "merge after @ expansion keeps the mention image and the pasted image");
    free(merged);
    free(expanded);
    free(mention_parts);
    PicoComposer_DiscardAttachments();
    unlink(mention);
    unlink(paste);
    rmdir(temp);
    free(temp);
}

static void TestRemoveDropsPath(void)
{
    PicoComposer_DiscardAttachments();
    char *temp = MakeTempDir();
    if (!temp)
    {
        Check(false, "could not create remove temp dir");
        return;
    }
    char first[4096];
    char second[4096];
    snprintf(first, sizeof(first), "%s/a.png", temp);
    snprintf(second, sizeof(second), "%s/b.png", temp);
    if (!WriteBytes(first, "PNG\0bin", 7) || !WriteBytes(second, "PNG\0bin", 7))
    {
        Check(false, "could not write images for remove");
        unlink(first);
        unlink(second);
        rmdir(temp);
        free(temp);
        return;
    }
    Check(pico_composer_attach_path(first, false) && pico_composer_attach_path(second, false),
          "two images attach");
    Check(pico_composer_remove_at(0), "remove the first thumbnail");
    char *parts = pico_composer_merge_parts("x", NULL);
    Check(parts && strstr(parts, "b.png") && !strstr(parts, "a.png"),
          "remove drops that path from a later merge");
    free(parts);
    PicoComposer_DiscardAttachments();
    unlink(first);
    unlink(second);
    rmdir(temp);
    free(temp);
}

static void TestWhitespaceSubmitReady(void)
{
    PicoComposer_DiscardAttachments();
    Check(!pico_composer_submit_ready(NULL, 0), "empty composer without attachments is not ready");
    Check(!pico_composer_submit_ready("   \n\t", 5), "whitespace without attachments is not ready");
    Check(pico_composer_submit_ready("hi", 2), "typed text is ready");
    char *temp = MakeTempDir();
    if (!temp)
    {
        Check(false, "could not create ready temp dir");
        return;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s/shot.png", temp);
    if (!WriteBytes(path, "PNG\0bin", 7))
    {
        Check(false, "could not write image for ready check");
        rmdir(temp);
        free(temp);
        return;
    }
    Check(pico_composer_attach_path(path, false), "attach for whitespace submit");
    Check(pico_composer_submit_ready("   ", 3), "whitespace-only composer with attachments is ready");
    Check(pico_composer_submit_ready("", 0), "empty composer with attachments is ready");
    char *display = pico_composer_display_message("");
    Check(display && strstr(display, "![image](") && strstr(display, "shot.png"),
          "display message for an image-only send includes a markdown image");
    free(display);
    char *with_text = pico_composer_display_message("look");
    Check(with_text && strstr(with_text, "look") && strstr(with_text, "![image](") &&
              strstr(with_text, "shot.png"),
          "typed text plus a pasted image is shown as text then a markdown image");
    free(with_text);
    PicoComposer_DiscardAttachments();
    unlink(path);
    rmdir(temp);
    free(temp);
}

static void TestMalformedPartsAreRejected(void)
{
    PicoComposer_DiscardAttachments();
    char *temp = MakeTempDir();
    if (!temp)
    {
        Check(false, "could not create malformed-parts temp dir");
        return;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s/shot.png", temp);
    Check(WriteBytes(path, "PNG\0bin", 7) && pico_composer_attach_path(path, false),
          "attach for malformed-parts merge");
    char *syntax = pico_composer_merge_parts("look", "[{bad]");
    char *empty = pico_composer_merge_parts("look", "[]");
    char *unknown = pico_composer_merge_parts("look", "[{\"type\":\"unknown\"}]");
    Check(!syntax && !empty && !unknown,
          "attachments do not replace malformed or empty hook parts");
    free(syntax);
    free(empty);
    free(unknown);
    PicoComposer_DiscardAttachments();
    unlink(path);
    rmdir(temp);
    free(temp);
}


static void TestOwnedAttachmentLifetime(void)
{
    PicoComposer_DiscardAttachments();
    char *temp = MakeTempDir();
    if (!temp)
    {
        Check(false, "could not create ownership temp dir");
        return;
    }
    char discarded[4096];
    char sent[4096];
    snprintf(discarded, sizeof(discarded), "%s/discarded.png", temp);
    snprintf(sent, sizeof(sent), "%s/sent.png", temp);
    Check(WriteBytes(discarded, "PNG\0bin", 7) && pico_composer_attach_path(discarded, true),
          "attach owned draft image");
    PicoComposer_DiscardAttachments();
    Check(access(discarded, F_OK) != 0, "discard deletes an owned draft image");

    Check(WriteBytes(sent, "PNG\0bin", 7) && pico_composer_attach_path(sent, true),
          "attach owned image for send");
    PicoComposer_ReleaseAttachments();
    Check(access(sent, F_OK) == 0, "successful-send release retains the persisted image");
    unlink(sent);
    rmdir(temp);
    free(temp);
}

static PicoSessionRef ExampleReference(void)
{
    return (PicoSessionRef){.title="Auth cleanup — [retry] ~~old~~ & *new*",
        .id="auth-session",.checkout="/tmp/project/worktree",.path="/tmp/missing session @data.jsonl"};
}

static void TestReferencePortableTextAndEditing(PicoHost *host)
{
    PicoSessionRef original = ExampleReference(), parsed;
    char *portable = PicoSessionRef_Serialize(&original);
    Check(portable && PicoSessionRef_Parse(portable,(int)strlen(portable),0,&parsed) &&
        !strcmp(parsed.title,original.title) && !strcmp(parsed.id,original.id) &&
        !strcmp(parsed.checkout,original.checkout) && !strcmp(parsed.path,original.path),
        "portable reference restores title and exact identity, including punctuation and spaces");
    if (!portable) return;
    PicoComposer_SetText(host,portable);
    /* A caller replacing a selection inside the visible chip must remove the
     * entire reference, never leave a hidden path fragment in the composer. */
    PicoComposer_ReplaceRange(host,3,4,"replacement");
    Check(!strcmp(host->composer.text,"replacement"),"partial replacement removes the entire session chip");
    PicoComposer_SetText(host,portable);
    PicoComposer_ReplaceRange(host,0,0,"before ");
    Check(PicoSessionRef_Next(host->composer.text,host->composer.length,0,&parsed) && parsed.start==7 &&
          !strcmp(parsed.path,original.path),"editing before a chip preserves its reference identity");
    PicoComposer_SetText(host,"abc");
    PicoComposer_ReplaceRange(host,-1,0,"x");
    PicoComposer_ReplaceRange(host,100,200,"y");
    Check(!strcmp(host->composer.text,"xabcy") && host->composer.cursor==host->composer.length,
          "negative and past-end replacement ranges clamp safely");
    PicoComposer_SetText(host,"");
    free(portable);
}

static void TestReferenceProjectionAndFileMentions(void)
{
    PicoSessionRef original=ExampleReference();
    char *portable=PicoSessionRef_Serialize(&original);
    char *expanded=pico_files_expand_mentions("/tmp",portable,false,NULL);
    Check(expanded && strstr(expanded,original.path) && !strstr(expanded,"pico-session:") &&
        !strstr(expanded,"<file"),"an unavailable session passes its absolute path without reading or attaching it");
    free(expanded); free(portable);

    char *temp=MakeTempDir();
    if (!temp) { Check(false,"reference/file mention fixture"); return; }
    char path[4096]; snprintf(path,sizeof(path),"%s/input.txt",temp);
    Check(WriteBytes(path,"file body",9),"reference/file fixture exists");
    snprintf(original.title,sizeof(original.title),"Review @input.txt");
    portable=PicoSessionRef_Serialize(&original);
    size_t cap=strlen(portable)+32;
    char *text=malloc(cap); snprintf(text,cap,"Compare %s @input.txt",portable);
    expanded=pico_files_expand_mentions(temp,text,false,NULL);
    Check(expanded && strstr(expanded,original.path) && strstr(expanded,"file body") &&
        !strstr(strstr(expanded,"<file")+1,"<file"),"session references and file mentions project once without scanning chip metadata");
    free(expanded); free(text); free(portable);
    unlink(path); rmdir(temp); free(temp);
}

static void TestReferenceControlEscaping(void)
{
    PicoSessionRef ref=ExampleReference();
    memset(ref.title,'\t',sizeof(ref.title)-1); ref.title[sizeof(ref.title)-1]='\0';
    char *portable=PicoSessionRef_Serialize(&ref);
    char *projected=PicoSessionRef_Project(portable);
    Check(projected && strstr(projected,"\\u0009") && !strchr(projected,'\t'),
        "portable reference control bytes are safely quoted in model input");
    free(portable); free(projected);
    projected=PicoSessionRef_Project("#heading [#broken](pico-session:1:x:%00:%2Ftmp)");
    Check(projected && !strcmp(projected,"#heading [#broken](pico-session:1:x:%00:%2Ftmp)"),
        "ordinary hashtags and malformed reference text remain literal text");
    free(projected);
}

int main(void)
{
    PicoExt comp_ext = pico_ext_composer();
    PicoHost app;
    memset(&app, 0, sizeof(app));
    comp_ext.host_init(&app, &g_composer_state);

    TestAttachMergeImagePart();
    TestMergeKeepsMentionAndPaste();
    TestRemoveDropsPath();
    TestWhitespaceSubmitReady();
    TestMalformedPartsAreRejected();
    TestOwnedAttachmentLifetime();
    TestReferencePortableTextAndEditing(&app);
    TestReferenceProjectionAndFileMentions();
    TestReferenceControlEscaping();
    free(app.composer.text);
    PicoComposer_DiscardAttachments();

    comp_ext.host_shutdown(&app, g_composer_state);
    g_composer_state = NULL;
    return g_failed;
}
