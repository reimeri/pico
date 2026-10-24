#ifndef PICO_IMAGE_H
#define PICO_IMAGE_H

#include <stdbool.h>
#include <stddef.h>

#define PICO_IMAGE_MAX_BYTES ((size_t)64 * 1024 * 1024)
#define PICO_IMAGE_MAX_DIMENSION 4096
#define PICO_TOOL_MAX_IMAGES 4

typedef struct PicoImageInfo {
    const char *mime;
    const char *extension;
    int width;
    int height;
} PicoImageInfo;

/* Detect and fully decode supported images without changing the source bytes.
 * WebP validation includes every animation frame. info strings are static. */
bool PicoImage_Validate(const void *bytes, size_t size, PicoImageInfo *info);
/* malloc'd RGBA first-frame pixels for a transcript preview. */
unsigned char *PicoImage_DecodeWebP(const void *bytes, size_t size, int *width, int *height);

#endif
