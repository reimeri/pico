/* ABI 22 image result example. See docs/extend/tools.md#image-bearing-results
 * and contracts.md. Workers transfer encoded bytes; never call Raylib here.
 * For a real file-reading tool add the desired path/permission policy. */
#include "pico/plugin.h"
#include "pico/agent.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>

static void ReadImage(PicoAgentContext *ctx, const char *args, PicoToolResult *out, void *state)
{
    (void)state;
    if (!pico_agent_context_accepts_tool_images(ctx)) {
        out->is_error = true;
        out->output = JsonDup("This model/provider cannot receive tool images.");
        return;
    }
    JsonDoc doc;
    if (JsonParse(&doc, args, strlen(args))) {
        out->is_error = true; out->output = JsonDup("Invalid arguments."); return;
    }
    char *path = JsonObjStr(&doc, 0, "path"); JsonFree(&doc);
    size_t size = 0;
    unsigned char *png = path && path[0] == '/' ?
        (unsigned char *)Pico_ReadFileLimited(path, 64 * 1024 * 1024, &size) : NULL;
    free(path);
    out->images = png ? calloc(1, sizeof(*out->images)) : NULL;
    if (!out->images) {
        free(png); out->is_error = true; out->output = JsonDup("Cannot read PNG."); return;
    }
    out->images[0] = (PicoToolImage){.png=png, .size=size};
    out->image_count = 1;
    out->output = JsonDup("Image supplied by read_image_example.");
    /* Default namespace tools: core validates PNG and creates its owned copy
     * in .pico/media/tools/<session-id>/. It never takes ownership of path. */
}
static int Init(PicoWorkspace *workspace, void **state)
{
    (void)state;
    pico_add_tool(workspace, "read_image_example", "Read an absolute PNG path as a model-visible image.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        ReadImage, NULL, PICO_TOOL_PARALLEL);
    return 0;
}
PicoExt pico_ext(void)
{
    return (PicoExt){.abi=PICO_EXT_ABI, .name="image_example", .workspace_init=Init};
}
