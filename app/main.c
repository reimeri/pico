// main.c - Clay markdown reader.
//
// Usage: clay_markdown_reader <file.md>
//
// The file is parsed once with md4c into a block-based IR (markdown.c) and
// rendered every frame with Clay. Paragraphs with inline styles are
// pre-wrapped by richtext.c. The file is re-parsed when it changes on disk.

#define CLAY_IMPLEMENTATION
#include "../clay/clay.h"
#include "../clay/renderers/raylib/clay_renderer_raylib.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "markdown.h"
#include "richtext.h"

// ---------------------------------------------------------------------------
// Fonts and theme

enum {
    FONT_REGULAR = 0,
    FONT_BOLD,
    FONT_ITALIC,
    FONT_BOLD_ITALIC,
    FONT_MONO,
    FONT_COUNT,
};

#define COLOR_BG (Clay_Color){24, 24, 28, 255}
#define COLOR_CONTENT_BG (Clay_Color){30, 30, 36, 255}
#define COLOR_TEXT (Clay_Color){222, 222, 228, 255}
#define COLOR_MUTED (Clay_Color){140, 140, 150, 255}
#define COLOR_LINK (Clay_Color){120, 160, 255, 255}
#define COLOR_LINK_HOVER (Clay_Color){170, 200, 255, 255}
#define COLOR_CODE_BG (Clay_Color){42, 42, 52, 255}
#define COLOR_CODE_TEXT (Clay_Color){230, 200, 140, 255}
#define COLOR_QUOTE_BG (Clay_Color){36, 38, 48, 255}
#define COLOR_QUOTE_BORDER (Clay_Color){120, 160, 255, 255}
#define COLOR_HR (Clay_Color){64, 64, 74, 255}
#define COLOR_SCROLLBAR (Clay_Color){120, 120, 160, 150}
#define COLOR_SCROLLBAR_HOVER (Clay_Color){100, 100, 140, 150}
#define COLOR_ERROR_BG (Clay_Color){70, 40, 40, 255}

#define CONTENT_PADDING 32
#define SCROLLBAR_WIDTH 14
#define BLOCK_SPACING 14

static uint16_t HeadingSizes[7] = {0, 32, 27, 23, 20, 17, 15};

static RichTextStyle BaseStyle = {
    .font_regular = FONT_REGULAR,
    .font_bold = FONT_BOLD,
    .font_italic = FONT_ITALIC,
    .font_bold_italic = FONT_BOLD_ITALIC,
    .font_mono = FONT_MONO,
    .font_size = 18,
    .text_color = COLOR_TEXT,
    .code_text_color = COLOR_CODE_TEXT,
    .code_bg_color = COLOR_CODE_BG,
    .link_color = COLOR_LINK,
    .link_hover_color = COLOR_LINK_HOVER,
};

// ---------------------------------------------------------------------------
// App state

static MdDocument doc = {0};
static const char *md_path = NULL;
static char md_dir[4096] = {0};
static time_t md_mtime = 0;
static double last_poll_time = 0;

static const char *hovered_link = NULL;

static bool reinitialize_clay = false;
static bool debug_enabled = false;

typedef struct {
    Clay_Vector2 click_origin;
    Clay_Vector2 position_origin;
    bool mouse_down;
} ScrollbarDragData;

static ScrollbarDragData scrollbar_drag = {0};

// ---------------------------------------------------------------------------
// Image cache

typedef struct {
    char path[8192];
    Texture2D texture;
    bool loaded;
} CachedImage;

static CachedImage image_cache[64];
static int image_cache_count = 0;

static const char *ResolveImagePath(const char *src)
{
    static char resolved[8192];
    if (strncmp(src, "http://", 7) == 0 || strncmp(src, "https://", 8) == 0 ||
        src[0] == '/')
    {
        return src;
    }
    snprintf(resolved, sizeof(resolved), "%s/%s", md_dir, src);
    return resolved;
}

static CachedImage *GetCachedImage(const char *path)
{
    for (int i = 0; i < image_cache_count; i++)
    {
        if (strcmp(image_cache[i].path, path) == 0)
        {
            return &image_cache[i];
        }
    }
    if (image_cache_count >= 64)
    {
        return NULL;
    }
    CachedImage *entry = &image_cache[image_cache_count++];
    snprintf(entry->path, sizeof(entry->path), "%s", path);
    entry->texture = LoadTexture(path);
    entry->loaded = entry->texture.id != 0;
    return entry;
}

static void ClearImageCache(void)
{
    for (int i = 0; i < image_cache_count; i++)
    {
        if (image_cache[i].loaded)
        {
            UnloadTexture(image_cache[i].texture);
        }
    }
    image_cache_count = 0;
}

// ---------------------------------------------------------------------------
// Layout

static float TextAreaWidth(void)
{
    float width = (float)GetScreenWidth() - CONTENT_PADDING * 2 - SCROLLBAR_WIDTH;
    if (width < 50)
    {
        width = 50;
    }
    return width;
}

static void RenderBlock(int index, float available_width, RichTextEmitState *emit)
{
    MdBlock *block = &doc.blocks[index];
    switch (block->type)
    {
        case MDB_PARAGRAPH:
        case MDB_HEADING:
        case MDB_LIST_ITEM:
        case MDB_QUOTE:
        {
            RichTextStyle style = BaseStyle;
            if (block->type == MDB_HEADING)
            {
                int level = block->heading_level;
                if (level < 1)
                {
                    level = 1;
                }
                if (level > 6)
                {
                    level = 6;
                }
                style.font_size = HeadingSizes[level];
            }

            if (block->type == MDB_QUOTE)
            {
                CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                         .padding = {16, 16, 10, 10},
                                         .childGap = 8,
                                         .sizing = {.width = CLAY_SIZING_GROW(0)}},
                              .backgroundColor = COLOR_QUOTE_BG,
                              .border = {.color = COLOR_QUOTE_BORDER,
                                         .width = {.left = 4}}})
                {
                    RichText_RenderParagraph(block, &doc.arena, available_width, &style, emit);
                }
            }
            else if (block->type == MDB_LIST_ITEM)
            {
                int indent = block->list_indent;
                if (indent < 0)
                {
                    indent = 0;
                }
                CLAY(CLAY_IDI("DbgLiRow", index), {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                         .padding = {.left = (uint16_t)(indent * 24)},
                                         .childGap = 8,
                                         .sizing = {.width = CLAY_SIZING_GROW(0)},
                                         .childAlignment = {.y = CLAY_ALIGN_Y_TOP}}})
                {
                    if (block->list_item_task)
                    {
                        const char *checkbox = block->list_item_done ? "\xE2\x98\x91" : "\xE2\x98\x90";
                        Clay_String checkbox_string = {.length = (int32_t)strlen(checkbox), .chars = checkbox};
                        CLAY_TEXT(checkbox_string,
                                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                    .fontSize = style.font_size,
                                                    .textColor = block->list_item_done ? COLOR_MUTED : COLOR_LINK,
                                                    .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                    else
                    {
                        Clay_String marker_string = {.length = (int32_t)strlen(block->list_marker), .chars = block->list_marker};
                        CLAY_TEXT(marker_string,
                                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR,
                                                    .fontSize = style.font_size,
                                                    .textColor = COLOR_MUTED,
                                                    .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                    CLAY(CLAY_IDI("DbgLiInner", index), {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
                    {
                        RichText_RenderParagraph(block, &doc.arena, available_width, &style, emit);
                    }
                }
            }
            else
            {
                RichText_RenderParagraph(block, &doc.arena, available_width, &style, emit);
            }
            break;
        }
        case MDB_CODE:
        case MDB_HTML:
        {
            CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                     .padding = {14, 14, 12, 12},
                                     .childGap = 2,
                                     .sizing = {.width = CLAY_SIZING_GROW(0)}},
                          .backgroundColor = COLOR_CODE_BG,
                          .cornerRadius = CLAY_CORNER_RADIUS(6)})
            {
                char *line = block->raw_text;
                int line_count = 0;
                if (strlen(block->raw_text) == 0)
                {
                    line = NULL;
                }
                while (line)
                {
                    char *newline = strchr(line, '\n');
                    int length = newline ? (int)(newline - line) : (int)strlen(line);
                    Clay_String text = {.length = length, .chars = line};
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
                    {
                        CLAY_TEXT(text, CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                                          .fontSize = 16,
                                                          .textColor = COLOR_CODE_TEXT,
                                                          .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                    line = newline ? newline + 1 : NULL;
                    line_count++;
                }
                if (line_count == 0)
                {
                    CLAY_TEXT(CLAY_STRING(" "), CLAY_TEXT_CONFIG({.fontId = FONT_MONO,
                                                                  .fontSize = 16,
                                                                  .textColor = COLOR_CODE_TEXT,
                                                                  .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
            }
            break;
        }
        case MDB_IMAGE:
        {
            const char *path = ResolveImagePath(block->image_path);
            bool remote = strncmp(block->image_path, "http", 4) == 0;
            CachedImage *image = (!remote && FileExists(path)) ? GetCachedImage(path) : NULL;
            if (image && image->loaded)
            {
                float width = (float)image->texture.width;
                if (width > available_width)
                {
                    width = available_width;
                }
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
                {
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(width)}},
                                  .aspectRatio = (float)image->texture.width / (float)image->texture.height,
                                  .image = {.imageData = &image->texture}})
                    {
                    }
                }
            }
            else
            {
                CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                         .padding = {12, 12, 8, 8},
                                         .childGap = 8,
                                         .sizing = {.width = CLAY_SIZING_GROW(0)}},
                              .backgroundColor = COLOR_CODE_BG,
                              .cornerRadius = CLAY_CORNER_RADIUS(6)})
                {
                    char message[2400];
                    if (remote)
                    {
                        snprintf(message, sizeof(message), "[image unavailable, remote URLs are not loaded] %s",
                                 block->image_alt);
                    }
                    else
                    {
                        snprintf(message, sizeof(message), "[image not found] %s", block->image_alt);
                    }
                    Clay_String message_string = {.length = (int32_t)strlen(message), .chars = message};
                    CLAY_TEXT(message_string, CLAY_TEXT_CONFIG({.fontId = FONT_ITALIC,
                                                                .fontSize = 15,
                                                                .textColor = COLOR_MUTED}));
                }
            }
            break;
        }
        case MDB_HR:
        {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                                .height = CLAY_SIZING_FIXED(2)}},
                          .backgroundColor = COLOR_HR})
            {
            }
            break;
        }
    }
}

static Clay_RenderCommandArray CreateLayout(void)
{
    Clay_BeginLayout();
    RichText_BeginLayout();
    hovered_link = NULL;

    CLAY(CLAY_ID("Root"), {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                                 .height = CLAY_SIZING_GROW(0)},
                                      .padding = {CONTENT_PADDING, CONTENT_PADDING, CONTENT_PADDING, CONTENT_PADDING}},
                           .backgroundColor = COLOR_BG})
    {
        CLAY(CLAY_ID("DocumentScroll"), {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                                    .sizing = {.width = CLAY_SIZING_GROW(0),
                                                               .height = CLAY_SIZING_GROW(0)}},
                                         .clip = {.vertical = true,
                                                  .horizontal = true,
                                                  .childOffset = Clay_GetScrollOffset()}})
        {
            CLAY(CLAY_ID("DocumentContent"), {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                                         .childGap = BLOCK_SPACING,
                                                         .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                if (doc.load_error)
                {
                    CLAY(CLAY_ID("LoadError"), {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                                           .padding = {16, 16, 12, 12},
                                                           .childGap = 8,
                                                           .sizing = {.width = CLAY_SIZING_GROW(0)}},
                                                .backgroundColor = COLOR_ERROR_BG,
                                                .cornerRadius = CLAY_CORNER_RADIUS(6)})
                    {
                        CLAY_TEXT(CLAY_STRING("Could not display this markdown file."),
                                  CLAY_TEXT_CONFIG({.fontId = FONT_BOLD, .fontSize = 18, .textColor = COLOR_TEXT}));
                        Clay_String error_string = {.length = (int32_t)strlen(doc.load_error), .chars = doc.load_error};
                        CLAY_TEXT(error_string,
                                  CLAY_TEXT_CONFIG({.fontId = FONT_REGULAR, .fontSize = 16, .textColor = COLOR_MUTED}));
                    }
                }

                float available_width = TextAreaWidth();
                RichTextEmitState emit = {.hovered_link = NULL};
                for (int i = 0; i < doc.block_count; i++)
                {
                    // Add breathing room above headings (but not the first block).
                    bool heading = doc.blocks[i].type == MDB_HEADING;
                    int spacing_above = i > 0 ? (heading ? BLOCK_SPACING : 0) : 0;
                    if (spacing_above > 0)
                    {
                        CLAY(CLAY_IDI("HeadingSpacer", i),
                             {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                                    .height = CLAY_SIZING_FIXED((float)spacing_above)}}})
                        {
                        }
                    }
                    RenderBlock(i, available_width, &emit);
                }
                hovered_link = emit.hovered_link;
            }
        }

        // Scrollbar indicator (positioned from the previous frame's scroll
        // data, same approach as the clay sidebar demo). Declared after the
        // scroll container so clay can resolve the attach target.
        Clay_ScrollContainerData scroll_data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("DocumentScroll")));
        if (scroll_data.found && scroll_data.contentDimensions.height > 0)
        {
            float thumb_height = (scroll_data.scrollContainerDimensions.height / scroll_data.contentDimensions.height) * scroll_data.scrollContainerDimensions.height;
            CLAY(CLAY_ID("ScrollBar"), {.floating = {.attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                                                     .offset = {.y = -(scroll_data.scrollPosition->y / scroll_data.contentDimensions.height) * scroll_data.scrollContainerDimensions.height},
                                                     .zIndex = 1,
                                                     .parentId = Clay_GetElementId(CLAY_STRING("DocumentScroll")).id,
                                                     .attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_TOP,
                                                                      .parent = CLAY_ATTACH_POINT_RIGHT_TOP}}})
            {
                CLAY(CLAY_ID("ScrollBarHandle"), {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(SCROLLBAR_WIDTH),
                                                                        .height = CLAY_SIZING_FIXED(thumb_height)}},
                                                  .backgroundColor = Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ScrollBar"))) ? COLOR_SCROLLBAR_HOVER : COLOR_SCROLLBAR,
                                                  .cornerRadius = CLAY_CORNER_RADIUS(SCROLLBAR_WIDTH / 2)})
                {
                }
            }
        }
    }
    return Clay_EndLayout(GetFrameTime());
}

// ---------------------------------------------------------------------------
// File watching

static bool FileChanged(void)
{
    struct stat st;
    if (stat(md_path, &st) != 0)
    {
        return false;
    }
    return st.st_mtime != md_mtime;
}

static void ReloadDocument(void)
{
    MdDocument_Free(&doc);
    ClearImageCache();
    doc = MdDocument_LoadFile(md_path);
    if (!doc.load_error)
    {
        struct stat st;
        md_mtime = stat(md_path, &st) == 0 ? st.st_mtime : 0;
    }
}

// ---------------------------------------------------------------------------
// Frame

static void UpdateDrawFrame(Font *fonts)
{
    Vector2 mouse_delta = GetMouseWheelMoveV();

    if (IsKeyPressed(KEY_D))
    {
        debug_enabled = !debug_enabled;
        Clay_SetDebugModeEnabled(debug_enabled);
    }
    if (IsKeyPressed(KEY_F12))
    {
        TakeScreenshot("clay_markdown_reader_screenshot.png");
    }

    Clay_Vector2 mouse_position = {.x = GetMousePosition().x, .y = GetMousePosition().y};
    Clay_SetPointerState(mouse_position, IsMouseButtonDown(0) && !scrollbar_drag.mouse_down);
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()});

    if (!IsMouseButtonDown(0))
    {
        scrollbar_drag.mouse_down = false;
    }

    if (IsMouseButtonDown(0) && !scrollbar_drag.mouse_down &&
        Clay_PointerOver(Clay_GetElementId(CLAY_STRING("ScrollBar"))))
    {
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("DocumentScroll")));
        if (data.found)
        {
            scrollbar_drag.click_origin = mouse_position;
            scrollbar_drag.position_origin = *data.scrollPosition;
            scrollbar_drag.mouse_down = true;
        }
    }
    else if (scrollbar_drag.mouse_down)
    {
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("DocumentScroll")));
        if (data.found && data.contentDimensions.height > 0)
        {
            float ratio = data.contentDimensions.height / data.scrollContainerDimensions.height;
            data.scrollPosition->y = scrollbar_drag.position_origin.y +
                                     (scrollbar_drag.click_origin.y - mouse_position.y) * ratio;
        }
    }

    Clay_UpdateScrollContainers(true, (Clay_Vector2){mouse_delta.x, mouse_delta.y}, GetFrameTime());

    // Reload the document at most twice a second.
    double now = GetTime();
    if (now - last_poll_time > 0.5)
    {
        last_poll_time = now;
        if (FileChanged())
        {
            ReloadDocument();
        }
    }

    Clay_RenderCommandArray render_commands = CreateLayout();
    static int dbg = 0;
    if (dbg++ < 3) fprintf(stderr, "DBG blocks=%d commands=%d screen=%dx%d\n", doc.block_count, render_commands.length, GetScreenWidth(), GetScreenHeight());
    if (dbg <= 4) {
        for (int k = 0; k < doc.block_count; k++) {
            Clay_ElementData d = Clay_GetElementData(CLAY_IDI("DbgLiRow", k));
            if (!d.found) continue;
            Clay_ElementData m = Clay_GetElementData(CLAY_IDI("DbgLiInner", k));
            fprintf(stderr, "LI %d row=(%.0f,%.0f,%.0fx%.0f) inner=(%.0f,%.0f,%.0fx%.0f)\n", k, d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height, m.found ? m.boundingBox.x : -1, m.found ? m.boundingBox.y : -1, m.found ? m.boundingBox.width : -1, m.found ? m.boundingBox.height : -1);
        }
    }

    if (hovered_link && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    {
        OpenURL(hovered_link);
    }

    BeginDrawing();
    ClearBackground((Color){(unsigned char)COLOR_BG.r, (unsigned char)COLOR_BG.g, (unsigned char)COLOR_BG.b, 255});
    Clay_Raylib_Render(render_commands, fonts);
    EndDrawing();
}

// ---------------------------------------------------------------------------
// Clay error handling

static void HandleClayErrors(Clay_ErrorData error_data)
{
    printf("%s\n", error_data.errorText.chars);
    if (error_data.errorType == CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED)
    {
        reinitialize_clay = true;
        Clay_SetMaxElementCount(Clay_GetMaxElementCount() * 2);
    }
    else if (error_data.errorType == CLAY_ERROR_TYPE_TEXT_MEASUREMENT_CAPACITY_EXCEEDED)
    {
        reinitialize_clay = true;
        Clay_SetMaxMeasureTextCacheWordCount(Clay_GetMaxMeasureTextCacheWordCount() * 2);
    }
}

// Load a font with the default ASCII glyphs plus the extra Unicode glyphs
// used for list bullets, task checkboxes, em dashes and the entity sample.
// Note: raylib loads ONLY the provided codepoints when the array is non-NULL,
// so the ASCII range must be included explicitly.
static void LoadFontWithGlyphs(const char *path, Font *out)
{
    static int codepoints[256];
    static int count = 0;
    if (count == 0)
    {
        for (int c = 32; c < 127; c++)
        {
            codepoints[count++] = c;
        }
        for (int c = 160; c < 256; c++)
        {
            codepoints[count++] = c;
        }
        int extra[] = {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2603, 0x2610, 0x2611};
        for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
        {
            codepoints[count++] = extra[i];
        }
    }
    *out = LoadFontEx(path, 48, codepoints, count);
    SetTextureFilter(out->texture, TEXTURE_FILTER_BILINEAR);
}

// ---------------------------------------------------------------------------
// Entry

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <markdown-file>\n", argv[0]);
        return 1;
    }
    md_path = argv[1];

    // Remember the directory of the markdown file so relative image paths
    // resolve correctly regardless of where the app was started from.
    snprintf(md_dir, sizeof(md_dir), "%s", md_path);
    char *last_slash = strrchr(md_dir, '/');
    if (last_slash)
    {
        *last_slash = '\0';
    }
    else
    {
        strcpy(md_dir, ".");
    }

    uint64_t total_memory_size = Clay_MinMemorySize();
    Clay_Arena clay_memory = Clay_CreateArenaWithCapacityAndMemory(total_memory_size, malloc(total_memory_size));
    Clay_Initialize(clay_memory, (Clay_Dimensions){1280, 800}, (Clay_ErrorHandler){HandleClayErrors, 0});
    Clay_Raylib_Initialize(1280, 800, "Clay Markdown Reader", FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    ChangeDirectory(GetApplicationDirectory());

    Font fonts[FONT_COUNT];
    LoadFontWithGlyphs("resources/Roboto-Regular.ttf", &fonts[FONT_REGULAR]);
    LoadFontWithGlyphs("resources/Roboto-Bold.ttf", &fonts[FONT_BOLD]);
    LoadFontWithGlyphs("resources/Roboto-Italic.ttf", &fonts[FONT_ITALIC]);
    LoadFontWithGlyphs("resources/Roboto-BoldItalic.ttf", &fonts[FONT_BOLD_ITALIC]);
    LoadFontWithGlyphs("resources/RobotoMono-Medium.ttf", &fonts[FONT_MONO]);
    Clay_SetMeasureTextFunction(Raylib_MeasureText, fonts);
    RichText_SetMeasureFunction(Raylib_MeasureText, fonts);

    ReloadDocument();

    // Temporary test hook: with CLAY_MD_FRAMES=N set, render N frames,
    // screenshot the last one and exit. Remove before release.
    const char *frames_env = getenv("CLAY_MD_FRAMES");
    int frame_limit = frames_env ? atoi(frames_env) : 0;
    int frame_count = 0;

    while (!WindowShouldClose())
    {
        if (frame_limit > 0 && frame_count >= frame_limit)
        {
            TakeScreenshot("clay_markdown_reader_screenshot.png");
            break;
        }
        frame_count++;
        if (reinitialize_clay)
        {
            uint64_t size = Clay_MinMemorySize();
            Clay_Arena memory = Clay_CreateArenaWithCapacityAndMemory(size, malloc(size));
            Clay_Initialize(memory, (Clay_Dimensions){(float)GetScreenWidth(), (float)GetScreenHeight()},
                            (Clay_ErrorHandler){HandleClayErrors, 0});
            reinitialize_clay = false;
        }
        UpdateDrawFrame(fonts);
    }

    MdDocument_Free(&doc);
    ClearImageCache();
    Clay_Raylib_Close();
    return 0;
}
