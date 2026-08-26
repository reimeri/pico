#include "markdown.h"

#include <stdio.h>
#include <string.h>

static int Fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(void)
{
    const char *src = "What does the text in the picture say?\n\n![image](/tmp/shot.png)";
    MdDocument doc = MdDocument_ParseEx(src, strlen(src), MD_PARSE_PRESERVE_NEWLINES);
    int ok = doc.block_count == 2 && doc.blocks[0].type == MDB_PARAGRAPH &&
             doc.blocks[1].type == MDB_IMAGE && doc.blocks[1].image_path &&
             strcmp(doc.blocks[1].image_path, "/tmp/shot.png") == 0;
    MdDocument_Free(&doc);
    return ok ? 0 : Fail("user text plus a markdown image is a paragraph then an image, not a blank spacer");
}
