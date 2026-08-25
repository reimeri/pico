#ifndef PICO_HTTP_CAPTURE_H
#define PICO_HTTP_CAPTURE_H

#include <stddef.h>

#ifdef PICO_DEBUG_SSE_CAPTURE
typedef struct PicoHttpCapture {
    void *file;
    size_t response_bytes;
    size_t captured_bytes;
    int write_failed;
    char directory[4096];
    char raw_name[128];
    char raw_path[4096];
    char metadata_path[4096];
    char started_at[80];
} PicoHttpCapture;
#else
typedef struct PicoHttpCapture {
    int unused;
} PicoHttpCapture;
#endif

void PicoHttpCapture_Begin(PicoHttpCapture *capture);
void PicoHttpCapture_Write(PicoHttpCapture *capture, const char *data, size_t length);
void PicoHttpCapture_Finish(PicoHttpCapture *capture, const char *url, long http_status,
                            const char *outcome, int transport_code,
                            const char *transport_error);

#endif
