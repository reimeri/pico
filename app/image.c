#include "image.h"

#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>
#include <webp/demux.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS PICO_IMAGE_MAX_DIMENSION
#include "external/stb_image.h"

static bool DimensionsValid(int width, int height)
{
    return width > 0 && height > 0 && width <= PICO_IMAGE_MAX_DIMENSION && height <= PICO_IMAGE_MAX_DIMENSION;
}

static bool DecodeWebP(const void *bytes, size_t size, PicoImageInfo *info, unsigned char **preview)
{
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(bytes, size, &features) != VP8_STATUS_OK ||
        !DimensionsValid(features.width, features.height)) return false;
    WebPData data = {.bytes = bytes, .size = size};
    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options)) return false;
    options.color_mode = MODE_RGBA;
    WebPAnimDecoder *decoder = WebPAnimDecoderNew(&data, &options);
    if (!decoder) return false;
    WebPAnimInfo animation;
    bool ok = WebPAnimDecoderGetInfo(decoder, &animation) && animation.frame_count > 0 &&
              DimensionsValid((int)animation.canvas_width, (int)animation.canvas_height);
    uint32_t frames = 0;
    while (ok && WebPAnimDecoderHasMoreFrames(decoder)) {
        uint8_t *pixels = NULL; int timestamp = 0;
        ok = WebPAnimDecoderGetNext(decoder, &pixels, &timestamp) != 0;
        if (!ok) break;
        frames++;
        if (preview) {
            size_t length = (size_t)animation.canvas_width * animation.canvas_height * 4;
            *preview = malloc(length);
            ok = *preview != NULL;
            if (ok) memcpy(*preview, pixels, length);
            break;
        }
    }
    ok = ok && frames > 0 && (preview || frames == animation.frame_count);
    if (ok && info) *info = (PicoImageInfo){"image/webp", "webp", (int)animation.canvas_width, (int)animation.canvas_height};
    WebPAnimDecoderDelete(decoder);
    return ok;
}

typedef struct JpegError {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} JpegError;

static void JpegFailure(j_common_ptr decoder)
{
    JpegError *error = (JpegError *)decoder->err;
    longjmp(error->jump, 1);
}

static void JpegMessage(j_common_ptr decoder, int level)
{
    /* libjpeg normally recovers from premature EOF by synthesizing EOI.
     * Warnings must instead fail validation, without writing to stderr. */
    if (level < 0) JpegFailure(decoder);
}

static bool ValidateJpeg(const void *bytes, size_t size, PicoImageInfo *info)
{
    /* Keep mutable decoder state off the stack so it remains defined after
     * the error manager longjmps, even in optimized builds. */
    struct jpeg_decompress_struct *decoder = calloc(1, sizeof(*decoder));
    if (!decoder) return false;
    JpegError error;
    decoder->err = jpeg_std_error(&error.base);
    error.base.error_exit = JpegFailure;
    error.base.emit_message = JpegMessage;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(decoder); free(decoder); return false;
    }
    jpeg_create_decompress(decoder);
    jpeg_mem_src(decoder, bytes, (unsigned long)size);
    bool valid = jpeg_read_header(decoder, TRUE) == JPEG_HEADER_OK &&
                 DimensionsValid((int)decoder->image_width, (int)decoder->image_height);
    if (valid) valid = jpeg_start_decompress(decoder) != 0;
    if (valid) {
        JSAMPARRAY row = (*decoder->mem->alloc_sarray)((j_common_ptr)decoder, JPOOL_IMAGE,
            decoder->output_width * decoder->output_components, 1);
        while (valid && decoder->output_scanline < decoder->output_height)
            valid = jpeg_read_scanlines(decoder, row, 1) == 1;
        if (valid) valid = jpeg_finish_decompress(decoder) != 0;
    }
    if (valid && info) *info = (PicoImageInfo){"image/jpeg", "jpg", (int)decoder->image_width, (int)decoder->image_height};
    jpeg_destroy_decompress(decoder); free(decoder);
    return valid;
}

bool PicoImage_Validate(const void *bytes, size_t size, PicoImageInfo *info)
{
    if (!bytes || size < 12 || size > PICO_IMAGE_MAX_BYTES) return false;
    const unsigned char *data = bytes;
    if (!memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) return DecodeWebP(bytes, size, info, NULL);
    if (data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) return ValidateJpeg(bytes, size, info);
    if (memcmp(data, "\x89PNG\r\n\x1a\n", 8)) return false;
    int width, height, channels;
    if (!stbi_info_from_memory(data, (int)size, &width, &height, &channels) || !DimensionsValid(width, height)) return false;
    unsigned char *pixels = stbi_load_from_memory(data, (int)size, &width, &height, &channels, 4);
    bool valid = pixels != NULL;
    stbi_image_free(pixels);
    if (valid && info) *info = (PicoImageInfo){"image/png", "png", width, height};
    return valid;
}

unsigned char *PicoImage_DecodeWebP(const void *bytes, size_t size, int *width, int *height)
{
    if (!bytes || size < 12 || size > PICO_IMAGE_MAX_BYTES) return NULL;
    PicoImageInfo info;
    unsigned char *pixels = NULL;
    if (!DecodeWebP(bytes, size, &info, &pixels)) { free(pixels); return NULL; }
    *width = info.width; *height = info.height;
    return pixels;
}
