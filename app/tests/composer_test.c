#include "canonical.h"
#include "composer_internal.h"
#include "json.h"
#include "host_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

char *pico_files_expand_mentions(const char *workspace, const char *text, bool vision,
                                 char **parts_json_out);

static int g_failed;

void pico_status_warn(PicoHost *app, const char *msg)
{
    (void)app;
    (void)msg;
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

#if defined(__linux__)
static double WallSeconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static void TestClipboardOwnerCannotBlockUi(void)
{
    char *temp = MakeTempDir();
    if (!temp)
    {
        Check(false, "could not create clipboard-process temp dir");
        return;
    }
    char command[4096];
    snprintf(command, sizeof(command), "%s/wl-paste", temp);
    const char script[] = "#!/bin/sh\nwhile :; do :; done\n";
    if (!WriteBytes(command, script, sizeof(script) - 1) || chmod(command, 0700) != 0)
    {
        Check(false, "could not create stalled clipboard command");
        unlink(command);
        rmdir(temp);
        free(temp);
        return;
    }
    const char *old_path_value = getenv("PATH");
    char *old_path = old_path_value ? JsonDup(old_path_value) : NULL;
    setenv("PATH", temp, 1);
    PicoHost app;
    memset(&app, 0, sizeof(app));
    PicoHost_SetPath(&app, temp);

    double started = WallSeconds();
    PicoComposer_BeginClipboardPaste(&app);
    double begin_elapsed = WallSeconds() - started;
    bool running = PicoComposer_ClipboardPasteBusy();
    started = WallSeconds();
    PicoComposer_CancelClipboardPaste();
    double cancel_elapsed = WallSeconds() - started;
    Check(running && begin_elapsed < 0.25 && cancel_elapsed < 0.5 &&
              !PicoComposer_ClipboardPasteBusy(),
          "a stalled clipboard owner starts asynchronously and cancels promptly");

    if (old_path)
    {
        setenv("PATH", old_path, 1);
    }
    else
    {
        unsetenv("PATH");
    }
    free(old_path);
    unlink(command);
    rmdir(temp);
    free(temp);
}
#endif

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

int main(void)
{
    TestAttachMergeImagePart();
    TestMergeKeepsMentionAndPaste();
    TestRemoveDropsPath();
    TestWhitespaceSubmitReady();
    TestMalformedPartsAreRejected();
#if defined(__linux__)
    TestClipboardOwnerCannotBlockUi();
#endif
    TestOwnedAttachmentLifetime();
    PicoComposer_DiscardAttachments();
    return g_failed;
}
