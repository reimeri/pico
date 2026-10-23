#ifndef PICO_BROWSER_GPU_H
#define PICO_BROWSER_GPU_H
#include "protocol.h"
#include "transport.h"
#include <stdbool.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Graphics-thread only. The helper owns a surfaceless context; the host borrows
 * its actual GLFW EGL context. No CEF resource escapes an accelerated callback. */
typedef struct PbGpu {
    EGLDisplay display;
    EGLContext context;
    bool owned;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNEGLWAITSYNCKHRPROC wait_sync;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence;
    PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC export_query;
    PFNEGLEXPORTDMABUFIMAGEMESAPROC export_image;
    void (*image_target)(unsigned, void *);
    char error[256];
} PbGpu;
typedef struct PbGpuImage {
    unsigned texture, framebuffer;
    EGLImageKHR image;
    int width, height;
} PbGpuImage;
bool PbGpuInit(PbGpu *gpu, bool helper);
void PbGpuShutdown(PbGpu *gpu);
bool PbGpuCurrent(PbGpu *gpu);
void PbGpuDestroy(PbGpu *gpu, PbGpuImage *image);
bool PbGpuAllocate(PbGpu *gpu, PbGpuImage *image, int width, int height);
bool PbGpuImport(PbGpu *gpu, PbGpuImage *image, const PbFrame *frame, const PbFds *fds);
bool PbGpuExport(PbGpu *gpu, PbGpuImage *image, PbFrame *frame, PbFds *fds);
bool PbGpuWait(PbGpu *gpu, int fence); /* consumes fd, including on failure */
int PbGpuFence(PbGpu *gpu); /* caller owns sync_file fd */
bool PbGpuCopy(PbGpu *gpu, const PbGpuImage *src, PbGpuImage *dst,
               int sx, int sy, int width, int height, int dx, int dy);
bool PbGpuRead(PbGpu *gpu, const PbGpuImage *image, void *rgba);
bool PbGpuComplete(PbGpu *gpu); /* before returning CEF's borrowed buffer */
bool PbFrameValid(const PbFrame *frame, const PbFds *fds);
#ifdef __cplusplus
}
#endif
#endif
