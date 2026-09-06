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

int main(void)
{
    int fails = TestCodeBlockLang();
    const char *src = "What does the text in the picture say?\n\n![image](/tmp/shot.png)";
    MdDocument doc = MdDocument_ParseEx(src, strlen(src), MD_PARSE_PRESERVE_NEWLINES);
    int ok = doc.block_count == 2 && doc.blocks[0].type == MDB_PARAGRAPH &&
             doc.blocks[1].type == MDB_IMAGE && doc.blocks[1].image_path &&
             strcmp(doc.blocks[1].image_path, "/tmp/shot.png") == 0;
    MdDocument_Free(&doc);
    fails += ok ? 0 : Fail("user text plus a markdown image is a paragraph then an image, not a blank spacer");
    return fails;
}
