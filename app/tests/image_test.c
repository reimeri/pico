#define _POSIX_C_SOURCE 200809L
#include "pico/plugin.h"
#include "pico/agent.h"
#include "json.h"
#include "path.h"
#include "image_fixtures.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct PicoAgentContext { const char *workspace; bool accepts_images; };
static PicoToolFn tool_run;
static int failed;

static void Check(bool ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); failed = 1; }
}

bool pico_add_tool(PicoWorkspace *workspace, const char *name, const char *description,
                   const char *params, PicoToolFn run, PicoToolApplyFn apply, PicoToolExecution execution)
{
    (void)workspace; (void)description; (void)params; (void)apply; (void)execution;
    if (!strcmp(name, "view_image")) tool_run = run;
    return true;
}
void pico_add_llm_hook(PicoWorkspace *workspace, PicoLlmHookFn hook) { (void)workspace; (void)hook; }
PicoHost *pico_workspace_host(PicoWorkspace *workspace) { (void)workspace; return NULL; }
PicoAgent *PicoHost_FindAgent(PicoHost *host, PicoAgentId id) { (void)host; (void)id; return NULL; }
bool PicoAgent_AcceptsToolImages(const PicoAgent *agent) { (void)agent; return false; }
const char *pico_agent_context_workspace(const PicoAgentContext *ctx) { return ctx->workspace; }
bool pico_agent_context_accepts_tool_images(const PicoAgentContext *ctx) { return ctx->accepts_images; }

static bool WriteBytes(const char *path, const void *bytes, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fwrite(bytes, 1, size, file) == size;
    return fclose(file) == 0 && ok;
}

static void FreeResult(PicoToolResult *result)
{
    for (int i = 0; i < result->image_count; i++) free(result->images[i].data);
    free(result->images); free(result->output); free(result->details_json); free(result->media_namespace);
}

static PicoToolResult Run(PicoAgentContext *ctx, const char *path)
{
    JsonBuf args; JsonBuf_Init(&args);
    JsonBuf_Puts(&args, "{\"path\":"); JsonBuf_String(&args, path); JsonBuf_Putc(&args, '}');
    PicoToolResult result = {0}; tool_run(ctx, args.data, &result, NULL);
    JsonBuf_Free(&args); return result;
}

int main(void)
{
    char root[] = "/tmp/pico-view-image-XXXXXX", workspace[4096], path[4096];
    if (!mkdtemp(root)) return 1;
    snprintf(workspace, sizeof(workspace), "%s/workspace", root);
    if (mkdir(workspace, 0700)) return 1;
    PicoAgentContext ctx = {.workspace = workspace, .accepts_images = true};
    PicoExt ext = pico_ext_image(); void *state = NULL;
    Check(ext.workspace_init(NULL, &state) == 0 && tool_run, "view_image is registered");
    if (!tool_run) return 1;
    for (size_t i = 0; i < sizeof(image_fixtures) / sizeof(*image_fixtures); ++i) {
        const TestImageFixture *fixture = &image_fixtures[i];
        /* JPEG is outside the workspace, with a misleading extension. The
         * other files exercise workspace-relative resolution independent of cwd. */
        bool absolute = !strcmp(fixture->mime, "image/jpeg");
        PicoPath_Format(path, sizeof(path), "%s/fixture.bin", absolute ? root : workspace);
        Check(WriteBytes(path, fixture->data, fixture->size), "write image fixture");
        PicoToolResult result = Run(&ctx, absolute ? path : "fixture.bin");
        Check(!result.is_error && result.image_count == 1 && result.images &&
              result.images[0].size == fixture->size && !memcmp(result.images[0].data, fixture->data, fixture->size) &&
              result.output && strstr(result.output, fixture->mime),
              "PNG, JPEG, static/animated WebP are visible as original bytes, detected from content");
        FreeResult(&result); unlink(path);
    }
    PicoPath_Format(path, sizeof(path), "%s/boundary.png", workspace);
    Check(WriteBytes(path, image_wide_png, sizeof(image_wide_png)), "write dimension boundary fixture");
    PicoToolResult result = Run(&ctx, "boundary.png");
    Check(!result.is_error && result.image_count == 1, "image at the documented dimension boundary is accepted");
    FreeResult(&result);
    Check(WriteBytes(path, image_oversized_png, sizeof(image_oversized_png)), "write oversized dimension fixture");
    result = Run(&ctx, "boundary.png");
    Check(result.is_error && !result.image_count, "oversized dimensions are rejected rather than converted");
    FreeResult(&result); unlink(path);

    PicoPath_Format(path, sizeof(path), "%s/invalid", workspace);
    for (size_t i = 0; i < sizeof(image_fixtures) / sizeof(*image_fixtures); ++i) {
        Check(WriteBytes(path, image_fixtures[i].data, 16), "write truncated image fixture");
        result = Run(&ctx, "invalid");
        Check(result.is_error && !result.image_count, "truncated encoded images are not returned as visual content");
        FreeResult(&result);
    }
    /* JPEG decoders may recover from missing scan data and synthesize an EOI.
     * Reject truncation both after dimensions and during the compressed scan. */
    const size_t jpeg_prefixes[] = {sizeof(image_jpeg) / 2, sizeof(image_jpeg) - 8};
    for (size_t i = 0; i < sizeof(jpeg_prefixes) / sizeof(*jpeg_prefixes); ++i) {
        Check(WriteBytes(path, image_jpeg, jpeg_prefixes[i]), "write incomplete JPEG stream");
        result = Run(&ctx, "invalid");
        Check(result.is_error && !result.image_count, "incomplete JPEGs cannot succeed through decoder recovery");
        FreeResult(&result);
    }
    Check(WriteBytes(path, "not an image", 12), "write text fixture");
    result = Run(&ctx, "invalid");
    Check(result.is_error && !result.image_count, "non-image files are rejected");
    FreeResult(&result); unlink(path);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    Check(fd >= 0 && ftruncate(fd, 64 * 1024 * 1024 + 1) == 0, "create oversized encoded file");
    if (fd >= 0) close(fd);
    result = Run(&ctx, "invalid");
    Check(result.is_error && !result.image_count && result.output && strstr(result.output, "64 MiB"),
          "oversized encoded files fail explicitly without loading their bytes");
    FreeResult(&result); unlink(path);

    result = Run(&ctx, "missing.png");
    Check(result.is_error && !result.image_count, "missing files are controlled tool errors");
    FreeResult(&result);
    result = Run(&ctx, ".");
    Check(result.is_error && !result.image_count, "directories are not image files");
    FreeResult(&result);
    Check(mkfifo(path, 0600) == 0, "create FIFO fixture");
    result = Run(&ctx, "invalid");
    Check(result.is_error && !result.image_count, "non-regular paths cannot block image reading");
    FreeResult(&result); unlink(path);

    ctx.accepts_images = false;
    result = Run(&ctx, "missing.png");
    Check(result.is_error && !result.image_count && result.output && strstr(result.output, "model/provider"),
          "unsupported model/provider fails before file access");
    FreeResult(&result);
    ctx.accepts_images = true;
    const char *invalid_args[] = {"{", "{}", "{\"path\":42}", "{\"path\":\"\"}"};
    for (size_t i = 0; i < sizeof(invalid_args) / sizeof(*invalid_args); ++i) {
        result = (PicoToolResult){0}; tool_run(&ctx, invalid_args[i], &result, NULL);
        Check(result.is_error && !result.image_count, "invalid path arguments are controlled errors");
        FreeResult(&result);
    }
    rmdir(workspace); rmdir(root);
    return failed;
}
