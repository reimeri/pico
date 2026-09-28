#include "markdown.h"

#include <stdio.h>
#include <string.h>

static int Fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static int TestCodeBlockLang(void)
{
    const char *src = "```c\nint x;\n```\n\n```\nplain\n```\n";
    MdDocument doc = MdDocument_ParseEx(src, strlen(src), MD_PARSE_PRESERVE_NEWLINES);
    const MdBlock *tagged = NULL, *untagged = NULL;
    for (int i = 0; i < doc.block_count; i++)
    {
        if (doc.blocks[i].type == MDB_CODE)
        {
            if (tagged)
            {
                untagged = &doc.blocks[i];
            }
            else
            {
                tagged = &doc.blocks[i];
            }
        }
    }
    int ok = tagged && untagged && tagged->lang && strcmp(tagged->lang, "c") == 0 &&
             untagged->lang == NULL;
    MdDocument_Free(&doc);
    return ok ? 0 : Fail("fenced code blocks capture the language tag; untagged fences get NULL");
}

static int TestApproximateNumbersAndStrikethrough(void)
{
    // Both single tildes are approximation markers, not a deletion span.
    const char *src = "**~1.6 percentage points of idle CPU saved (~24% of the idle total)**, "
                      "matching the prediction. ~~obsolete~~";
    MdDocument doc = MdDocument_Parse(src, strlen(src));
    char bold_text[256] = {0};
    char struck_text[256] = {0};
    for (int b = 0; b < doc.block_count; b++)
    {
        const MdBlock *block = &doc.blocks[b];
        for (int c = 0; c < block->chunk_count; c++)
        {
            const MdChunk *chunk = &block->chunks[c];
            if (chunk->strike)
                strncat(struck_text, chunk->text, sizeof(struck_text) - strlen(struck_text) - 1);
            else if (chunk->bold)
                strncat(bold_text, chunk->text, sizeof(bold_text) - strlen(bold_text) - 1);
        }
    }
    MdDocument_Free(&doc);
    return strcmp(bold_text, "~1.6 percentage points of idle CPU saved "
                             "(~24% of the idle total)") == 0 &&
                   strcmp(struck_text, "obsolete") == 0
               ? 0 : Fail("single approximation tildes stay visible; double tildes strike text");
}

int main(void)
{
    int fails = TestCodeBlockLang();
    fails += TestApproximateNumbersAndStrikethrough();
    const char *src = "What does the text in the picture say?\n\n![image](/tmp/shot.png)";
    MdDocument doc = MdDocument_ParseEx(src, strlen(src), MD_PARSE_PRESERVE_NEWLINES);
    int ok = doc.block_count == 2 && doc.blocks[0].type == MDB_PARAGRAPH &&
             doc.blocks[1].type == MDB_IMAGE && doc.blocks[1].image_path &&
             strcmp(doc.blocks[1].image_path, "/tmp/shot.png") == 0;
    MdDocument_Free(&doc);
    fails += ok ? 0 : Fail("user text plus a markdown image is a paragraph then an image, not a blank spacer");
    return fails;
}
