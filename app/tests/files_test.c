#include "canonical.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *pico_files_expand_mentions(const char *workspace, const char *text, bool vision,
                                 char **parts_json_out);

static int g_failed;

static void Check(bool ok, const char *message)
{
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        g_failed = 1;
    }
}

int main(void)
{
    char temp[] = "/tmp/pico-files-XXXXXX";
    if (!mkdtemp(temp))
    {
        fprintf(stderr, "FAIL: could not create temp dir\n");
        return 1;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s/pic.png", temp);
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        fprintf(stderr, "FAIL: could not write image\n");
        return 1;
    }
    fwrite("PNG\0bin", 1, 7, f);
    fclose(f);

    char *parts = NULL;
    char *expanded = pico_files_expand_mentions(temp, "see @pic.png", true, &parts);
    Check(parts && strstr(parts, "\"type\":\"image\"") && strstr(parts, "pic.png") &&
              expanded && !strstr(expanded, "(binary file omitted)"),
          "@ of a local image on a vision model produces a user image part, not (binary file omitted)");
    free(expanded);
    free(parts);

    char *normalized = NULL;
    bool invalid_parts = !pico_canonical_normalize_user_parts(
        "[{\"type\":\"video\",\"path\":\"clip.mp4\"}]", &normalized) && !normalized;
    Check(invalid_parts, "structured user parts reject unsupported media types");
    free(normalized);
    normalized = NULL;
    bool embedded_bytes = !pico_canonical_normalize_user_parts(
        "[{\"type\":\"image\",\"url\":\"data:image/png;base64,UE5H\"}]", &normalized) &&
                          !normalized;
    Check(embedded_bytes, "structured user parts reject embedded file bytes");
    free(normalized);
    unlink(path);
    rmdir(temp);
    return g_failed;
}
