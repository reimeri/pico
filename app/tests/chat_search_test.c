#include "chat_search.h"
#include <stdio.h>
#include <string.h>

#define CHECK(condition, message) do { if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); return 1; } } while (0)

int main(void)
{
    PicoChatSearch search = {0};
    CHECK(PicoChatSearch_Resize(&search, 3), "create display index");
    CHECK(PicoChatSearch_SetText(&search, 0, "Straße STRASSE σςΣ Éé e\nblocks"), "retain displayed text");
    CHECK(PicoChatSearch_SetText(&search, 1, "banana"), "retain second message");
    CHECK(PicoChatSearch_SetText(&search, 2, "blocks"), "retain third message");
    CHECK(PicoChatSearch_Query(&search, "STRASSE") && PicoChatSearch_Refresh(&search), "Unicode query");
    CHECK(search.count == 2 && search.matches[0].from == 0 && search.matches[0].to == (int)strlen("Straße"),
          "case expansion matches and highlights original UTF-8 bytes");
    CHECK(PicoChatSearch_Query(&search, "σ") && PicoChatSearch_Refresh(&search) && search.count == 3,
          "case folding includes final sigma");
    CHECK(PicoChatSearch_Query(&search, "é") && PicoChatSearch_Refresh(&search) && search.count == 2,
          "case folding preserves accents");
    CHECK(PicoChatSearch_Query(&search, "e\xcc\x81") && PicoChatSearch_Refresh(&search) && search.count == 0,
          "case folding does not normalize composed and decomposed accents");
    CHECK(PicoChatSearch_Query(&search, "s") && PicoChatSearch_Refresh(&search), "single-character expansion query");
    int sharp_s = 0;
    for (int i = 0; i < search.count; i++)
        if (search.matches[i].message == 0 && search.matches[i].from == 4) sharp_s++;
    CHECK(sharp_s == 1, "a folding expansion produces one non-overlapping original-text match");
    CHECK(PicoChatSearch_Query(&search, "ana") && PicoChatSearch_Refresh(&search) && search.count == 1,
          "substring occurrences do not overlap");
    CHECK(PicoChatSearch_Query(&search, "e blocks") && PicoChatSearch_Refresh(&search) && search.count == 0,
          "phrases do not cross block boundaries");
    CHECK(PicoChatSearch_Query(&search, "bananablocks") && PicoChatSearch_Refresh(&search) && search.count == 0,
          "phrases do not cross message boundaries");
    CHECK(PicoChatSearch_Query(&search, "blocks") && PicoChatSearch_Refresh(&search) && search.count == 2,
          "all retained messages contribute even without rendering them again");
    search.active = 0;
    PicoChatSearch_Step(&search, -1);
    CHECK(search.active == 1, "previous wraps to final result");
    PicoChatSearch_Step(&search, 1);
    CHECK(search.active == 0, "next wraps to first result");
    CHECK(PicoChatSearch_SetText(&search, 2, "blocks blocks") && PicoChatSearch_Refresh(&search) &&
          search.count == 3 && search.active == 0, "streaming updates preserve an unchanged active match");
    CHECK(PicoChatSearch_Query(&search, "") && PicoChatSearch_Refresh(&search) && search.count == 0,
          "empty query has no results");
    PicoChatSearch_Step(&search, 1);
    CHECK(search.active == -1, "navigation is inert without results");
    PicoChatSearch_Free(&search);
    return 0;
}
