/* Included by md_scroll_layout_test.c. Exercise public layout commands with
 * synthetic textures, without requiring a GPU or exposing the cache internals. */
#include <sys/stat.h>
#include <unistd.h>

static unsigned image_next_id;
static unsigned image_unloads;
static bool *image_alive;
static size_t image_alive_cap;

Image __wrap_LoadImage(const char *path)
{
    FILE *file = fopen(path, "rb");
    int value = file ? fgetc(file) : EOF;
    if (file) fclose(file);
    if (value != 'A' && value != 'B') return (Image){0};
    return (Image){.data = malloc(1), .width = value == 'A' ? 10 : 20, .height = 10};
}
Texture2D __wrap_LoadTextureFromImage(Image image)
{
    unsigned id = ++image_next_id;
    if (id >= image_alive_cap)
    {
        size_t capacity = image_alive_cap ? image_alive_cap * 2 : 128;
        bool *next = realloc(image_alive, capacity * sizeof(*next));
        if (!next) return (Texture2D){0};
        memset(next + image_alive_cap, 0, (capacity - image_alive_cap) * sizeof(*next));
        image_alive = next; image_alive_cap = capacity;
    }
    image_alive[id] = true;
    return (Texture2D){.id = id, .width = image.width, .height = image.height};
}
void __wrap_UnloadImage(Image image) { free(image.data); }
void __wrap_UnloadTexture(Texture2D texture)
{
    if (texture.id < image_alive_cap && image_alive[texture.id])
    { image_alive[texture.id] = false; image_unloads++; }
}

static bool WriteImageFixture(const char *path, const char *contents)
{
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fputs(contents, file) >= 0;
    return fclose(file) == 0 && ok;
}

static Clay_RenderCommandArray RenderImages(MdDocument *documents, int count)
{
    float height = (float)(count + 1) * 64.0f;
    Clay_SetLayoutDimensions((Clay_Dimensions){640, height});
    MdView_BeginFrame();
    PicoChatSel_BeginFrame(0);
    PicoChatSel_SetMessage(-1);
    Clay_BeginLayout();
    CLAY(CLAY_ID("ImageTestRoot"), {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
          .sizing = {.width = CLAY_SIZING_FIXED(640), .height = CLAY_SIZING_FIXED(height)}}})
    {
        for (int i = 0; i < count; i++) MdView_RenderDocument(&documents[i], 500 + i, 640);
    }
    return Clay_EndLayout(0);
}

static Texture2D *ImageCommand(Clay_RenderCommandArray commands, int index)
{
    for (int i = 0; i < commands.length; i++)
    {
        Clay_RenderCommand *command = Clay_RenderCommandArray_Get(&commands, i);
        if (command->commandType == CLAY_RENDER_COMMAND_TYPE_IMAGE && index-- == 0)
            return command->renderData.image.imageData;
    }
    return NULL;
}

static MdDocument ImageDocument(const char *name)
{
    char markdown[512];
    snprintf(markdown, sizeof(markdown), "![fixture](%s)", name);
    return MdDocument_Parse(markdown, strlen(markdown));
}

static int TestImageCache(void)
{
    char root[] = "/tmp/pico-image-cache-XXXXXX", path[4096], name[64];
    if (!mkdtemp(root)) return Fail("image cache fixture");
    MdView_ClearImages();
    MdView_SetImageBaseDir(root);
    unsigned initial_unloads = image_unloads;
    int count = 0;
    /* Discover eviction through behavior, not the private/configurable cache
     * capacity. A cache that permanently refuses new images fails immediately. */
    PICO_TEST_WAIT_LOOP("unused images become evictable")
    {
        snprintf(name, sizeof(name), "image-%d.png", count++);
        snprintf(path, sizeof(path), "%s/%s", root, name);
        if (!WriteImageFixture(path, "A")) return Fail("image fixture write");
        MdDocument document = ImageDocument(name);
        Texture2D *texture = ImageCommand(RenderImages(&document, 1), 0);
        bool shown = texture && texture->id && image_alive[texture->id];
        MdDocument_Free(&document);
        if (!shown) return Fail("new images must display after cache pressure");
        if (image_unloads > initial_unloads) break;
    }
    MdDocument first = ImageDocument("image-0.png");
    Texture2D *texture = ImageCommand(RenderImages(&first, 1), 0);
    if (!texture || !image_alive[texture->id]) return Fail("evicted image must reload");
    unsigned original = texture->id;
    snprintf(path, sizeof(path), "%s/image-0.png", root);
    if (!WriteImageFixture(path, "B changed")) return Fail("changed image fixture");
    /* Earlier Clay commands still borrow the old texture until the next pass. */
    if (texture->id != original || !image_alive[original]) return Fail("pending image command changed prematurely");
    PICO_TEST_WAIT(!MdView_ImagesChanged()) {}
    texture = ImageCommand(RenderImages(&first, 1), 0);
    if (!texture || texture->id == original || texture->width != 20 || image_alive[original])
        return Fail("changed image must replace its texture at the next layout");
    unsigned replaced = texture->id;
    if (!WriteImageFixture(path, "invalid")) return Fail("invalid replacement fixture");
    texture = ImageCommand(RenderImages(&first, 1), 0);
    if (!texture || texture->id != replaced || !image_alive[replaced])
        return Fail("failed refresh must keep the last usable texture");
    if (!WriteImageFixture(path, "A repaired")) return Fail("repaired image fixture");
    texture = ImageCommand(RenderImages(&first, 1), 0);
    if (!texture || texture->id == replaced || texture->width != 10)
        return Fail("fixed files must not remain negatively cached");
    MdDocument_Free(&first);

    /* More simultaneous images than the retained cache must keep every pending
     * command alive, then reuse that working set on identical relayouts. */
    MdDocument *documents = calloc((size_t)count + 1, sizeof(*documents));
    if (!documents) return Fail("image documents allocation");
    for (int i = 0; i < count; i++)
    { snprintf(name, sizeof(name), "image-%d.png", i); documents[i + 1] = ImageDocument(name); }
    Clay_RenderCommandArray commands = RenderImages(documents + 1, count);
    for (int i = 0; i < count; i++)
    {
        texture = ImageCommand(commands, i);
        if (!texture || !image_alive[texture->id]) return Fail("active images were evicted before render consumption");
    }
    unsigned loads = image_next_id, unloads = image_unloads;
    commands = RenderImages(documents + 1, count);
    if (image_next_id != loads || image_unloads != unloads)
        return Fail("repeated identical image layouts churned textures");
    snprintf(path, sizeof(path), "%s/prepended.png", root);
    if (!WriteImageFixture(path, "A")) return Fail("prepended image fixture");
    documents[0] = ImageDocument("prepended.png");
    commands = RenderImages(documents, count + 1);
    if (image_next_id != loads + 1 || image_unloads != unloads)
        return Fail("a new leading image evicted the previous layout's working set");
    MdDocument_Free(&documents[0]);
    unlink(path);
    for (int i = 0; i < count; i++)
    {
        texture = ImageCommand(commands, i + 1);
        if (!texture || !image_alive[texture->id]) return Fail("relayout lost an image");
        MdDocument_Free(&documents[i + 1]);
        snprintf(path, sizeof(path), "%s/image-%d.png", root, i); unlink(path);
    }
    free(documents);
    MdView_ClearImages();
    for (unsigned i = 1; i <= image_next_id; i++)
        if (image_alive[i]) return Fail("image clear leaked a texture");
    MdView_SetImageBaseDir(NULL);
    free(image_alive); image_alive = NULL; image_alive_cap = 0; image_next_id = 0;
    rmdir(root);
    return 0;
}
