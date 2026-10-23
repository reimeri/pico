#define GL_GLEXT_PROTOTYPES
#include "gpu.h"
#include <GL/gl.h>
#include <GL/glext.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

static bool Error(PbGpu *g, const char *message) {
    snprintf(g->error, sizeof(g->error), "%s (EGL 0x%x, GL 0x%x)", message, eglGetError(), glGetError());
    return false;
}
static bool Extension(const char *list, const char *name) {
    if (!list) return false;
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
    return false;
}
bool PbGpuCurrent(PbGpu *g) {
    if (!g->display || !g->context) return false;
    if (!g->owned) return eglGetCurrentContext() == g->context && eglGetCurrentDisplay() == g->display;
    return eglMakeCurrent(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE, g->context) || Error(g, "Cannot activate browser GPU context");
}
bool PbGpuInit(PbGpu *g, bool helper) {
    memset(g, 0, sizeof(*g)); g->owned = helper;
    if (helper) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (get_display) g->display = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (!g->display || !eglInitialize(g->display, NULL, NULL)) return Error(g, "Browser requires a hardware EGL display");
        EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLConfig config; EGLint count = 0;
        if (!eglBindAPI(EGL_OPENGL_API) || !eglChooseConfig(g->display, attrs, &config, 1, &count) || !count) return Error(g, "No browser OpenGL EGL configuration");
        EGLint context[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
        g->context = eglCreateContext(g->display, config, EGL_NO_CONTEXT, context);
        if (!g->context || !PbGpuCurrent(g)) return Error(g, "Cannot create browser GPU context");
    } else {
        g->display = eglGetCurrentDisplay(); g->context = eglGetCurrentContext();
        if (!g->display || !g->context) { snprintf(g->error, sizeof(g->error), "Browser GPU presentation requires Pico's EGL OpenGL context (GLX is unsupported)."); return false; }
    }
    const char *extensions = eglQueryString(g->display, EGL_EXTENSIONS);
    if (!Extension(extensions, "EGL_EXT_image_dma_buf_import") ||
        !Extension(extensions, "EGL_EXT_image_dma_buf_import_modifiers") ||
        !Extension(extensions, "EGL_ANDROID_native_fence_sync") ||
        !Extension(extensions, "EGL_KHR_wait_sync") ||
        (helper && !Extension(extensions, "EGL_MESA_image_dma_buf_export")))
        return Error(g, "Driver lacks DMA-BUF import/export or native GPU fences; CPU rendering is not supported");
#define LOAD(field, type, name) g->field = (type)eglGetProcAddress(name)
    LOAD(create_image, PFNEGLCREATEIMAGEKHRPROC, "eglCreateImageKHR");
    LOAD(destroy_image, PFNEGLDESTROYIMAGEKHRPROC, "eglDestroyImageKHR");
    LOAD(create_sync, PFNEGLCREATESYNCKHRPROC, "eglCreateSyncKHR");
    LOAD(destroy_sync, PFNEGLDESTROYSYNCKHRPROC, "eglDestroySyncKHR");
    LOAD(wait_sync, PFNEGLWAITSYNCKHRPROC, "eglWaitSyncKHR");
    LOAD(dup_fence, PFNEGLDUPNATIVEFENCEFDANDROIDPROC, "eglDupNativeFenceFDANDROID");
    LOAD(export_query, PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC, "eglExportDMABUFImageQueryMESA");
    LOAD(export_image, PFNEGLEXPORTDMABUFIMAGEMESAPROC, "eglExportDMABUFImageMESA");
    LOAD(image_target, void (*)(unsigned, void *), "glEGLImageTargetTexture2DOES");
#undef LOAD
    if (!g->create_image || !g->destroy_image || !g->create_sync || !g->destroy_sync || !g->wait_sync || !g->dup_fence || !g->image_target ||
        (helper && (!g->export_query || !g->export_image))) return Error(g, "Missing EGL DMA-BUF entry points");
    return true;
}
void PbGpuShutdown(PbGpu *g) {
    if (g->owned && g->display) {
        eglMakeCurrent(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g->context) eglDestroyContext(g->display, g->context);
        eglTerminate(g->display);
    }
    memset(g, 0, sizeof(*g));
}
void PbGpuDestroy(PbGpu *g, PbGpuImage *im) {
    if (im->framebuffer) glDeleteFramebuffers(1, &im->framebuffer);
    if (im->texture) glDeleteTextures(1, &im->texture);
    if (im->image) g->destroy_image(g->display, im->image);
    memset(im, 0, sizeof(*im));
}
static bool Attach(PbGpu *g, PbGpuImage *im) {
    GLint read, draw;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGenFramebuffers(1, &im->framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, im->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, im->texture, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    return ok || Error(g, "DMA-BUF is not a renderable OpenGL image");
}
static void TextureParameters(void) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
bool PbGpuAllocate(PbGpu *g, PbGpuImage *im, int w, int h) {
    if (im->texture && im->width == w && im->height == h) return true;
    PbGpuDestroy(g, im);
    if (w < 1 || h < 1 || w > PB_MAX_DIMENSION || h > PB_MAX_DIMENSION) return Error(g, "Invalid GPU surface size");
    GLint binding; glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    glGenTextures(1, &im->texture); glBindTexture(GL_TEXTURE_2D, im->texture); TextureParameters();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, binding); im->width = w; im->height = h;
    if (!Attach(g, im)) { PbGpuDestroy(g, im); return false; }
    return true;
}
bool PbFrameValid(const PbFrame *f, const PbFds *fds) {
    if (!f->width || !f->height || f->width > PB_MAX_DIMENSION || f->height > PB_MAX_DIMENSION ||
        f->plane_count < 1 || f->plane_count > PB_MAX_PLANES || fds->count != f->plane_count + 1 ||
        f->slot >= PB_POOL_SIZE || !f->serial) return false;
    for (unsigned i = 0; i < f->plane_count; ++i)
        if (!f->stride[i] || f->stride[i] > INT_MAX || f->offset[i] > INT_MAX || fds->fd[i] < 0) return false;
    return fds->fd[f->plane_count] >= 0;
}
bool PbGpuImport(PbGpu *g, PbGpuImage *im, const PbFrame *f, const PbFds *fds) {
    /* CEF imports carry only planes; public wire validation additionally checks
     * the lease and acquire-fence descriptor. */
    if (!f->width || !f->height || f->width > PB_MAX_DIMENSION || f->height > PB_MAX_DIMENSION ||
        !f->plane_count || f->plane_count > PB_MAX_PLANES || fds->count < f->plane_count) return Error(g, "Invalid DMA-BUF dimensions or planes");
    EGLint attrs[64]; int n = 0;
#define ATTR(k, v) do { attrs[n++] = (k); attrs[n++] = (EGLint)(v); } while (0)
    ATTR(EGL_WIDTH, f->width); ATTR(EGL_HEIGHT, f->height); ATTR(EGL_LINUX_DRM_FOURCC_EXT, f->format);
    const EGLint fd_keys[] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    const EGLint lo_keys[] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    for (unsigned i = 0; i < f->plane_count; ++i) {
        if (!f->stride[i] || f->stride[i] > INT_MAX || f->offset[i] > INT_MAX) return Error(g, "Invalid DMA-BUF plane layout");
        ATTR(fd_keys[i], fds->fd[i]); ATTR(fd_keys[i] + 1, f->offset[i]); ATTR(fd_keys[i] + 2, f->stride[i]);
        /* DRM_FORMAT_MOD_INVALID means implicit layout, not an explicit modifier. */
        if (f->modifier != UINT64_C(0x00ffffffffffffff)) { ATTR(lo_keys[i], (uint32_t)f->modifier); ATTR(lo_keys[i] + 1, f->modifier >> 32); }
    }
    attrs[n++] = EGL_NONE;
#undef ATTR
    PbGpuDestroy(g, im);
    im->image = g->create_image(g->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (!im->image) return Error(g, "Cannot import DMA-BUF (format/modifier/device unsupported)");
    GLint binding; glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    glGenTextures(1, &im->texture); glBindTexture(GL_TEXTURE_2D, im->texture); TextureParameters();
    g->image_target(GL_TEXTURE_2D, im->image); glBindTexture(GL_TEXTURE_2D, binding);
    im->width = f->width; im->height = f->height;
    if (!Attach(g, im)) { PbGpuDestroy(g, im); return false; }
    return true;
}
bool PbGpuExport(PbGpu *g, PbGpuImage *im, PbFrame *f, PbFds *fds) {
    if (!im->image) {
        EGLint attrs[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        im->image = g->create_image(g->display, g->context, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)im->texture, attrs);
    }
    int format, planes;
    /* Query writes one modifier per plane, even though their values match. */
    EGLuint64KHR modifiers[PB_MAX_PLANES];
    if (!im->image || !g->export_query(g->display, im->image, &format, &planes, modifiers) || planes < 1 || planes > PB_MAX_PLANES)
        return Error(g, "Cannot export owned browser texture as DMA-BUF");
    int descriptors[PB_MAX_PLANES] = {-1,-1,-1,-1}, strides[PB_MAX_PLANES], offsets[PB_MAX_PLANES];
    bool ok = g->export_image(g->display, im->image, descriptors, strides, offsets);
    for (int i = 0; i < planes; ++i) if (descriptors[i] >= 0) {
        if (fcntl(descriptors[i], F_SETFD, FD_CLOEXEC) < 0) ok = false;
        fds->fd[fds->count++] = descriptors[i];
    }
    if (!ok || fds->count != (unsigned)planes) { PbFdsClose(fds); return Error(g, "Cannot export DMA-BUF planes"); }
    f->width = im->width; f->height = im->height; f->format = format; f->plane_count = planes; f->modifier = modifiers[0];
    for (int i = 0; i < planes; ++i) { f->stride[i] = strides[i]; f->offset[i] = offsets[i]; }
    int fence = PbGpuFence(g);
    if (fence < 0) { PbFdsClose(fds); return false; }
    fds->fd[fds->count++] = fence;
    return true;
}
bool PbGpuWait(PbGpu *g, int fd) {
    EGLint attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
    EGLSyncKHR sync = g->create_sync(g->display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
    if (!sync) { close(fd); return Error(g, "Cannot import GPU fence"); }
    bool ok = g->wait_sync(g->display, sync, 0); g->destroy_sync(g->display, sync);
    return ok || Error(g, "Cannot wait for GPU fence");
}
int PbGpuFence(PbGpu *g) {
    EGLint attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE};
    EGLSyncKHR sync = g->create_sync(g->display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
    if (!sync) { Error(g, "Cannot create GPU fence"); return -1; }
    glFlush(); int fd = g->dup_fence(g->display, sync); g->destroy_sync(g->display, sync);
    if (fd < 0) Error(g, "Cannot export GPU fence");
    return fd;
}
bool PbGpuCopy(PbGpu *g, const PbGpuImage *src, PbGpuImage *dst, int sx, int sy, int w, int h, int dx, int dy) {
    GLint read, draw; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->framebuffer); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->framebuffer);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST); glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(sx, sy, sx+w, sy+h, dx, dy, dx+w, dy+h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    return glGetError() == GL_NO_ERROR || Error(g, "Browser GPU copy failed");
}
bool PbGpuRead(PbGpu *g, const PbGpuImage *im, void *pixels) {
    GLint read, pack; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read); glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, im->framebuffer); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, im->width, im->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_PACK_ALIGNMENT, pack); glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
    return glGetError() == GL_NO_ERROR || Error(g, "Browser observation readback failed");
}
bool PbGpuComplete(PbGpu *g) { glFinish(); return glGetError() == GL_NO_ERROR || Error(g, "Browser GPU completion failed"); }
