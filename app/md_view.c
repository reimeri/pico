#include "pico/md_view.h"

#include "pico/theme.h"
#include "richtext.h"
#include "chat_sel.h"

#include "raylib.h"

#include <stdio.h>
#include <string.h>

static uint16_t HeadingSizes[7] = {0, 32, 27, 23, 20, 17, 15};

static RichTextStyle BaseStyle = {
    .font_regular = FONT_REGULAR,
    .font_bold = FONT_BOLD,
    .font_italic = FONT_ITALIC,
    .font_bold_italic = FONT_BOLD_ITALIC,
    .font_mono = FONT_MONO,
    .font_size = 18,
    .line_height = 22,
    .text_color = COLOR_TEXT,
    .code_text_color = COLOR_CODE_TEXT,
    .code_bg_color = COLOR_CODE_BG,
    .link_color = COLOR_LINK,
    .link_hover_color = COLOR_LINK_HOVER,
};

static const char *hovered_link = NULL;
static char image_base_dir[4096] = ".";

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
    entry->texture = LoadTexture(path);
    entry->loaded = entry->texture.id != 0;
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
}

const char *MdView_HoveredLink(void)
{
    return hovered_link;
}

static void RenderBlock(MdDocument *doc, int index, float available_width, RichTextEmitState *emit)
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
                CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                         .padding = {16, 16, 10, 10},
                                         .childGap = 8,
                                         .sizing = {.width = CLAY_SIZING_GROW(0)}},
                              .backgroundColor = COLOR_QUOTE_BG,
                              .border = {.color = COLOR_QUOTE_BORDER, .width = {.left = 4}}})
                {
                    RichText_RenderParagraph(block, &doc->arena, available_width, &style, emit);
                }
            }
            else if (block->type == MDB_LIST_ITEM)
            {
                int indent = block->list_indent;
                if (indent < 0)
                {
                    indent = 0;
                }
                CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                                         .padding = {.left = (uint16_t)(indent * 24)},
                                         .childGap = 8,
                                         .sizing = {.width = CLAY_SIZING_GROW(0)},
                                         .childAlignment = {.y = CLAY_ALIGN_Y_TOP}}})
                {
                    if (block->list_item_task)
                    {
                        const char *checkbox = block->list_item_done ? "\xE2\x98\x91" : "\xE2\x98\x90";
                        Clay_String checkbox_string = {.length = (int32_t)strlen(checkbox), .chars = checkbox};
                        PicoChatSel_Text(checkbox_string,
                                         (Clay_TextElementConfig){.fontId = FONT_REGULAR,
                                                                  .fontSize = style.font_size,
                                                                  .textColor = block->list_item_done ? COLOR_MUTED
                                                                                                     : COLOR_LINK,
                                                                  .wrapMode = CLAY_TEXT_WRAP_NONE});
                    }
                    else
                    {
                        Clay_String marker_string = {.length = (int32_t)strlen(block->list_marker),
                                                     .chars = block->list_marker};
                        PicoChatSel_Text(marker_string,
                                         (Clay_TextElementConfig){.fontId = FONT_REGULAR,
                                                                  .fontSize = style.font_size,
                                                                  .textColor = COLOR_MUTED,
                                                                  .wrapMode = CLAY_TEXT_WRAP_NONE});
                    }
                    PicoChatSel_Glue(" ");
                    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                                             .sizing = {.width = CLAY_SIZING_GROW(0)}}})
                    {
                        RichText_RenderParagraph(block, &doc->arena, available_width, &style, emit);
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
                        PicoChatSel_Text(text, (Clay_TextElementConfig){.fontId = FONT_MONO,
                                                                       .fontSize = 16,
                                                                       .lineHeight = 20,
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
                                                                               .fontSize = 16,
                                                                               .textColor = COLOR_CODE_TEXT,
                                                                               .wrapMode = CLAY_TEXT_WRAP_NONE});
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
                    PicoChatSel_Text(message_string, (Clay_TextElementConfig){.fontId = FONT_ITALIC,
                                                                             .fontSize = 15,
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
                             (Clay_TextElementConfig){.fontId = FONT_BOLD, .fontSize = 18, .textColor = COLOR_TEXT});
            PicoChatSel_Break();
            Clay_String error_string = {.length = (int32_t)strlen(doc->load_error), .chars = doc->load_error};
            PicoChatSel_Text(error_string,
                             (Clay_TextElementConfig){.fontId = FONT_REGULAR, .fontSize = 16, .textColor = COLOR_MUTED});
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
                    RenderBlock(doc, j, available_width, &emit);
                }
            }
            i = end;
            continue;
        }

        RenderBlock(doc, i, available_width, &emit);
        i++;
    }
    if (emit.hovered_link)
    {
        hovered_link = emit.hovered_link;
    }
}
