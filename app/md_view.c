#include "pico/md_view.h"

#include "pico/theme.h"
#include "richtext.h"
#include "chat_sel.h"
#include "md_view_internal.h"

#include "raylib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHAT_IMAGE_MAX_WIDTH 360.0f
#define CHAT_IMAGE_MAX_HEIGHT 240.0f

static uint16_t HeadingSizes[7] = {0, 32, 28, 24, PICO_FONT_TITLE, PICO_FONT_BODY, PICO_FONT_BODY};

static RichTextStyle BaseStyle = {
    .font_regular = FONT_REGULAR,
    .font_bold = FONT_BOLD,
    .font_italic = FONT_ITALIC,
    .font_bold_italic = FONT_BOLD_ITALIC,
    .font_mono = FONT_MONO,
    .font_size = CHAT_BODY_FONT_SIZE,
    .line_height = PICO_FONT_BODY_LINE,
    .text_color = COLOR_TEXT,
    .code_text_color = COLOR_CODE_TEXT,
    .code_bg_color = COLOR_CODE_BG,
    .link_color = COLOR_LINK,
    .link_hover_color = COLOR_LINK_HOVER,
};

static const char *hovered_link = NULL;
static char image_base_dir[4096] = ".";
static Clay_ElementId *horizontal_scroll_ids;
static int horizontal_scroll_count;
static int horizontal_scroll_capacity;

static Clay_ElementId HorizontalScrollId(int id_base, int block_index)
{
    return Clay_GetElementIdWithIndex(CLAY_STRING("MdHorizontalScroll"),
                                      (uint32_t)id_base + (uint32_t)block_index);
}

static void TrackHorizontalScroller(Clay_ElementId id)
{
    if (horizontal_scroll_count >= horizontal_scroll_capacity)
    {
        int capacity = horizontal_scroll_capacity == 0 ? 64 : horizontal_scroll_capacity * 2;
        Clay_ElementId *ids = (Clay_ElementId *)realloc(horizontal_scroll_ids,
                                                         (size_t)capacity * sizeof(Clay_ElementId));
        if (!ids)
        {
            return;
        }
        horizontal_scroll_ids = ids;
        horizontal_scroll_capacity = capacity;
    }
    horizontal_scroll_ids[horizontal_scroll_count++] = id;
}

bool MdView_ScrollHoveredHorizontal(float delta_x)
{
    if (delta_x == 0.0f)
    {
        return false;
    }
    for (int i = horizontal_scroll_count - 1; i >= 0; i--)
    {
        Clay_ElementId id = horizontal_scroll_ids[i];
        if (!Clay_PointerOver(id))
        {
            continue;
        }
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(id);
        if (!data.found || !data.scrollPosition || !data.config.horizontal)
        {
            continue;
        }
        float overflow = data.contentDimensions.width - data.scrollContainerDimensions.width;
        if (overflow <= 0.5f)
        {
            continue;
        }
        float x = data.scrollPosition->x + delta_x * 10.0f;
        if (x > 0.0f)
        {
            x = 0.0f;
        }
        else if (x < -overflow)
        {
            x = -overflow;
        }
        data.scrollPosition->x = x;
        return true;
    }
    return false;
}

typedef struct {
    char path[8192];
    Texture2D texture;
    bool loaded;
} CachedImage;

static CachedImage image_cache[64];
static int image_cache_count = 0;

void MdView_SetImageBaseDir(const char *dir)
{
    if (!dir || dir[0] == '\0')
    {
        strcpy(image_base_dir, ".");
        return;
    }
    snprintf(image_base_dir, sizeof(image_base_dir), "%s", dir);
}

static void ChatImageFit(int src_w, int src_h, float max_w, float max_h, int *out_w, int *out_h)
{
    if (src_w < 1)
    {
        src_w = 1;
    }
    if (src_h < 1)
    {
        src_h = 1;
    }
    if (max_w < 1.0f)
    {
        max_w = 1.0f;
    }
    if (max_h < 1.0f)
    {
        max_h = 1.0f;
    }
    float scale = 1.0f;
    if ((float)src_w > max_w)
    {
        scale = max_w / (float)src_w;
    }
    if ((float)src_h * scale > max_h)
    {
        scale = max_h / (float)src_h;
    }
    int w = (int)((float)src_w * scale + 0.5f);
    int h = (int)((float)src_h * scale + 0.5f);
    if (w < 1)
    {
        w = 1;
    }
    if (h < 1)
    {
        h = 1;
    }
    *out_w = w;
    *out_h = h;
}

static const char *ResolveImagePath(const char *src)
{
    static char resolved[8192];
    if (strncmp(src, "http://", 7) == 0 || strncmp(src, "https://", 8) == 0 || src[0] == '/')
    {
        return src;
    }
    snprintf(resolved, sizeof(resolved), "%s/%s", image_base_dir, src);
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
    memset(&entry->texture, 0, sizeof(entry->texture));
    entry->loaded = false;
    Image img = LoadImage(path);
    if (img.data && img.width > 0 && img.height > 0)
    {
        int w = img.width;
        int h = img.height;
        ChatImageFit(img.width, img.height, CHAT_IMAGE_MAX_WIDTH, CHAT_IMAGE_MAX_HEIGHT, &w, &h);
        if (w != img.width || h != img.height)
        {
            ImageResize(&img, w, h);
        }
        entry->texture = LoadTextureFromImage(img);
        entry->loaded = entry->texture.id != 0;
    }
    if (img.data)
    {
        UnloadImage(img);
    }
    return entry;
}

void MdView_ClearImages(void)
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

void MdView_BeginFrame(void)
{
    RichText_BeginLayout();
    hovered_link = NULL;
    horizontal_scroll_count = 0;
}

const char *MdView_HoveredLink(void)
{
    return hovered_link;
}

#define TABLE_CELL_PAD_X 8
#define TABLE_CELL_PAD_Y 6
#define TABLE_BORDER 1
#define QUOTE_PAD_X 16
#define LIST_INDENT_X 24
#define LIST_CONTENT_GAP 8

static float MarkdownContentWidth(float available_width, float consumed_width)
{
    float width = available_width - consumed_width;
    return width < 10.0f ? 10.0f : width;
}

typedef struct {
    float available_width;
    float content_width;
    uint16_t font_size;
    float font_scale;
    int col_count;
    float *col_widths;
} MdTableWrapCache;

static Clay_LayoutAlignmentX CellAlignX(MdCellAlign align)
{
    switch (align)
    {
        case MD_CELL_ALIGN_CENTER:
            return CLAY_ALIGN_X_CENTER;
        case MD_CELL_ALIGN_RIGHT:
            return CLAY_ALIGN_X_RIGHT;
        default:
            return CLAY_ALIGN_X_LEFT;
    }
}

static float *TableColWidths(MdBlock *block, MdArena *arena, float available_width,
                             const RichTextStyle *base_style, float *content_width)
{
    MdTable *table = &block->table;
    MdTableWrapCache *cache = (MdTableWrapCache *)block->wrap_cache;
    if (cache && cache->available_width == available_width && cache->font_size == base_style->font_size &&
        cache->font_scale == Pico_FontScale() && cache->col_count == table->col_count)
    {
        *content_width = cache->content_width;
        return cache->col_widths;
    }

    int cols = table->col_count;
    int rows = table->row_count;
    if (cols <= 0)
    {
        return NULL;
    }

    float *preferred = (float *)malloc((size_t)cols * sizeof(float));
    float *minw = (float *)malloc((size_t)cols * sizeof(float));
    if (!preferred || !minw)
    {
        free(preferred);
        free(minw);
        return NULL;
    }
    for (int c = 0; c < cols; c++)
    {
        preferred[c] = 0;
        minw[c] = 0;
    }

    for (int r = 0; r < rows; r++)
    {
        for (int c = 0; c < cols; c++)
        {
            MdTableCell *cell = &table->cells[r * cols + c];
            RichTextStyle style = *base_style;
            style.force_bold = cell->header || r < table->header_row_count;
            float pref = 0;
            float min = 0;
            RichText_MeasureUnwrapped(cell->chunks, cell->chunk_count, &style, &pref, &min);
            if (pref > preferred[c])
            {
                preferred[c] = pref;
            }
            if (min > minw[c])
            {
                minw[c] = min;
            }
        }
    }

    float borders = 2.0f * TABLE_BORDER + (float)(cols > 0 ? cols - 1 : 0) * TABLE_BORDER;
    float padding = (float)cols * (2.0f * TABLE_CELL_PAD_X);
    float budget = available_width - borders - padding;
    if (budget < (float)cols)
    {
        budget = (float)cols;
    }

    float sum_pref = 0;
    float sum_min = 0;
    for (int c = 0; c < cols; c++)
    {
        if (preferred[c] < minw[c])
        {
            preferred[c] = minw[c];
        }
        sum_pref += preferred[c];
        sum_min += minw[c];
    }

    float *widths = (float *)MdArena_Alloc(arena, (size_t)cols * sizeof(float), 8);
    if (sum_pref <= budget)
    {
        float extra = budget - sum_pref;
        float each = extra / (float)cols;
        float used = 0;
        for (int c = 0; c < cols; c++)
        {
            widths[c] = preferred[c] + each;
            used += widths[c];
        }
        widths[cols - 1] += budget - used;
    }
    else if (sum_min <= 0)
    {
        float each = budget / (float)cols;
        for (int c = 0; c < cols; c++)
        {
            widths[c] = each;
        }
    }
    else if (sum_min >= budget)
    {
        for (int c = 0; c < cols; c++)
        {
            widths[c] = minw[c];
        }
    }
    else
    {
        float extra_needed = sum_pref - budget;
        float shrinkable = sum_pref - sum_min;
        float used = 0;
        for (int c = 0; c < cols; c++)
        {
            float room = preferred[c] - minw[c];
            widths[c] = preferred[c] - extra_needed * (room / shrinkable);
            used += widths[c];
        }
        widths[cols - 1] += budget - used;
    }

    float width = borders + padding;
    for (int c = 0; c < cols; c++)
    {
        width += widths[c];
    }

    free(preferred);
    free(minw);

    cache = (MdTableWrapCache *)MdArena_Alloc(arena, sizeof(MdTableWrapCache), 8);
    cache->available_width = available_width;
    cache->content_width = width;
    cache->font_size = base_style->font_size;
    cache->font_scale = Pico_FontScale();
    cache->col_count = cols;
    cache->col_widths = widths;
    block->wrap_cache = cache;
    *content_width = width;
    return widths;
}

static void RenderTable(MdDocument *doc, MdBlock *block, Clay_ElementId scroll_id,
                        float available_width, RichTextEmitState *emit)
{
    MdTable *table = &block->table;
    if (!table->cells || table->col_count <= 0 || table->row_count <= 0)
    {
        return;
    }

    int indent = block->list_indent;
    if (indent < 0)
    {
        indent = 0;
    }

    float inner_width = available_width - (float)(indent * 24);
    if (inner_width < 32.0f)
    {
        inner_width = 32.0f;
    }

    float content_width = 0.0f;
    float *col_widths = TableColWidths(block, &doc->arena, inner_width, &BaseStyle, &content_width);
    if (!col_widths)
    {
        return;
    }

    int cols = table->col_count;
    int rows = table->row_count;

    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .padding = {.left = (uint16_t)(indent * 24)},
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
    {
        TrackHorizontalScroller(scroll_id);
        CLAY(scroll_id,
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .sizing = {.width = CLAY_SIZING_GROW(0)}},
              .clip = {.horizontal = true, .childOffset = Clay_GetScrollOffset()}})
        {
            CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                     .sizing = {.width = CLAY_SIZING_FIXED(content_width)}},
                          .border = {.color = COLOR_TABLE_BORDER,
                                     .width = {.left = TABLE_BORDER,
                                               .right = TABLE_BORDER,
                                               .top = TABLE_BORDER,
                                               .bottom = TABLE_BORDER,
                                               .betweenChildren = TABLE_BORDER}}})
            {
                for (int r = 0; r < rows; r++)
                {
                    bool header_row = r < table->header_row_count;
                    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                             .sizing = {.width = CLAY_SIZING_GROW(0)}},
                                  .backgroundColor = header_row ? COLOR_TABLE_HEADER : (Clay_Color){0, 0, 0, 0},
                                  .border = {.color = COLOR_TABLE_BORDER,
                                             .width = {.betweenChildren = TABLE_BORDER}}})
                    {
                        for (int c = 0; c < cols; c++)
                        {
                            MdTableCell *cell = &table->cells[r * cols + c];
                            float cw = col_widths[c];
                            Clay_LayoutAlignmentX ax = CellAlignX(cell->align);
                            CLAY_AUTO_ID({.layout = {
                                              .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                              .padding = {TABLE_CELL_PAD_X, TABLE_CELL_PAD_X, TABLE_CELL_PAD_Y,
                                                          TABLE_CELL_PAD_Y},
                                              .sizing = {.width = CLAY_SIZING_FIXED(cw + 2.0f * TABLE_CELL_PAD_X)},
                                              .childAlignment = {.x = ax}}})
                            {
                                MdBlock view = {0};
                                view.type = MDB_PARAGRAPH;
                                view.chunks = cell->chunks;
                                view.chunk_count = cell->chunk_count;
                                view.wrap_cache = cell->wrap_cache;
                                RichTextStyle style = BaseStyle;
                                style.force_bold = cell->header || header_row;
                                style.text_align = ax;
                                RichText_RenderParagraph(&view, &doc->arena, cw, &style, emit);
                                cell->wrap_cache = view.wrap_cache;
                            }
                            if (c + 1 < cols)
                            {
                                PicoChatSel_Glue("\t");
                            }
                        }
                    }
                    PicoChatSel_Break();
                }
            }
        }
    }
}

static void RenderBlock(MdDocument *doc, int index, int id_base, float available_width,
                        RichTextEmitState *emit)
{
    MdBlock *block = &doc->blocks[index];
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
                style.line_height = (uint16_t)(HeadingSizes[level] + HeadingSizes[level] / 5);
            }

            if (block->type == MDB_QUOTE)
            {
                float content_width = MarkdownContentWidth(available_width, 2.0f * QUOTE_PAD_X);
                CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                         .padding = {QUOTE_PAD_X, QUOTE_PAD_X, 10, 10},
                                         .childGap = 8,
                                         .sizing = {.width = CLAY_SIZING_GROW(0)}},
                              .backgroundColor = COLOR_QUOTE_BG,
                              .border = {.color = COLOR_QUOTE_BORDER, .width = {.left = 4}}})
                {
                    RichText_RenderParagraph(block, &doc->arena, content_width, &style, emit);
                }
            }
            else if (block->type == MDB_LIST_ITEM)
            {
                int indent = block->list_indent;
                if (indent < 0)
                {
                    indent = 0;
                }
                const char *marker = block->list_item_task
                                         ? (block->list_item_done ? "\xE2\x98\x91" : "\xE2\x98\x90")
                                         : block->list_marker;
                Clay_String marker_string = {.length = (int32_t)strlen(marker), .chars = marker};
                Clay_TextElementConfig marker_config = {
                    .fontId = FONT_REGULAR,
                    .fontSize = style.font_size,
                    .textColor = block->list_item_task
                                     ? (block->list_item_done ? COLOR_MUTED : COLOR_LINK)
                                     : COLOR_MUTED,
                    .wrapMode = CLAY_TEXT_WRAP_NONE,
                };
                float marker_width = RichText_MeasureWidth(marker_string, marker_config);
                float content_width = MarkdownContentWidth(
                    available_width, (float)(indent * LIST_INDENT_X) + marker_width + LIST_CONTENT_GAP);

                CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                         .padding = {.left = (uint16_t)(indent * LIST_INDENT_X)},
                                         .childGap = LIST_CONTENT_GAP,
                                         .sizing = {.width = CLAY_SIZING_GROW(0)},
                                         .childAlignment = {.y = CLAY_ALIGN_Y_TOP}}})
                {
                    PicoChatSel_Text(marker_string, marker_config);
                    PicoChatSel_Glue(" ");
                    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
                    {
                        RichText_RenderParagraph(block, &doc->arena, content_width, &style, emit);
                    }
                }
            }
            else
            {
                RichText_RenderParagraph(block, &doc->arena, available_width, &style, emit);
            }
            break;
        }
        case MDB_CODE:
        case MDB_HTML:
        {
            Clay_ElementId scroll_id = HorizontalScrollId(id_base, index);
            TrackHorizontalScroller(scroll_id);
            CLAY(scroll_id,
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .sizing = {.width = CLAY_SIZING_GROW(0)}},
                  .clip = {.horizontal = true, .childOffset = Clay_GetScrollOffset()}})
            {
                CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                         .padding = {14, 14, 12, 12},
                                         .childGap = 2,
                                         .sizing = {.width = CLAY_SIZING_FIT(available_width)}},
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
                        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIT()}}})
                        {
                            PicoChatSel_Text(text, (Clay_TextElementConfig){.fontId = FONT_MONO,
                                                                           .fontSize = PICO_FONT_UI,
                                                                           .lineHeight = Pico_FontPxU16(PICO_FONT_UI_LINE),
                                                                           .textColor = COLOR_CODE_TEXT,
                                                                           .wrapMode = CLAY_TEXT_WRAP_NONE});
                        }
                        PicoChatSel_Break();
                        line = newline ? newline + 1 : NULL;
                        line_count++;
                    }
                    if (line_count == 0)
                    {
                        PicoChatSel_Text(CLAY_STRING(" "), (Clay_TextElementConfig){.fontId = FONT_MONO,
                                                                                   .fontSize = PICO_FONT_UI,
                                                                                   .textColor = COLOR_CODE_TEXT,
                                                                                   .wrapMode = CLAY_TEXT_WRAP_NONE});
                    }
                }
            }
            break;
        }
        case MDB_IMAGE:
        {
            const char *path = ResolveImagePath(block->image_path);
            bool remote = strncmp(block->image_path, "http", 4) == 0;
            CachedImage *image = (!remote && FileExists(path)) ? GetCachedImage(path) : NULL;
            if (image && image->loaded && image->texture.width > 0 && image->texture.height > 0)
            {
                float max_w = available_width < CHAT_IMAGE_MAX_WIDTH ? available_width : CHAT_IMAGE_MAX_WIDTH;
                int width = 0;
                int height = 0;
                ChatImageFit(image->texture.width, image->texture.height, max_w, CHAT_IMAGE_MAX_HEIGHT, &width,
                             &height);
                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}})
                {
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED((float)width),
                                                        .height = CLAY_SIZING_FIXED((float)height)}},
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
                    PicoChatSel_Text(message_string, (Clay_TextElementConfig){.fontId = FONT_ITALIC,
                                                                             .fontSize = PICO_FONT_UI,
                                                                             .textColor = COLOR_MUTED});
                }
            }
            break;
        }
        case MDB_HR:
        {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(2)}},
                          .backgroundColor = COLOR_HR})
            {
            }
            break;
        }
        case MDB_TABLE:
        {
            Clay_ElementId scroll_id = HorizontalScrollId(id_base, index);
            RenderTable(doc, block, scroll_id, available_width, emit);
            break;
        }
    }
    PicoChatSel_Break();
}

void MdView_RenderDocument(MdDocument *doc, int id_base, float available_width)
{
    RichTextEmitState emit = {.hovered_link = NULL};

    if (doc->load_error)
    {
        CLAY(CLAY_IDI("LoadError", id_base),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .padding = {16, 16, 12, 12},
                         .childGap = 8,
                         .sizing = {.width = CLAY_SIZING_GROW(0)}},
              .backgroundColor = COLOR_ERROR_BG,
              .cornerRadius = CLAY_CORNER_RADIUS(6)})
        {
            PicoChatSel_Text(CLAY_STRING("Could not display this markdown."),
                             (Clay_TextElementConfig){.fontId = FONT_BOLD, .fontSize = PICO_FONT_TITLE, .textColor = COLOR_TEXT});
            PicoChatSel_Break();
            Clay_String error_string = {.length = (int32_t)strlen(doc->load_error), .chars = doc->load_error};
            PicoChatSel_Text(error_string,
                             (Clay_TextElementConfig){.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI, .textColor = COLOR_MUTED});
        }
        if (emit.hovered_link)
        {
            hovered_link = emit.hovered_link;
        }
        return;
    }

    for (int i = 0; i < doc->block_count; )
    {
        bool heading = doc->blocks[i].type == MDB_HEADING;
        int spacing_above = i > 0 ? (heading ? BLOCK_SPACING : 0) : 0;
        if (spacing_above > 0)
        {
            CLAY(CLAY_IDI("HeadingSpacer", id_base + i),
                 {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                        .height = CLAY_SIZING_FIXED((float)spacing_above)}}})
            {
            }
        }

        if (doc->blocks[i].type == MDB_LIST_ITEM)
        {
            int start = i;
            int end = i + 1;
            while (end < doc->block_count && doc->blocks[end].type == MDB_LIST_ITEM)
            {
                end++;
            }
            CLAY(CLAY_IDI("MdList", id_base + start),
                 {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                             .childGap = 2,
                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
            {
                for (int j = start; j < end; j++)
                {
                    RenderBlock(doc, j, id_base, available_width, &emit);
                }
            }
            i = end;
            continue;
        }

        RenderBlock(doc, i, id_base, available_width, &emit);
        i++;
    }
    if (emit.hovered_link)
    {
        hovered_link = emit.hovered_link;
    }
}
