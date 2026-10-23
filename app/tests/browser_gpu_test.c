#define _GNU_SOURCE
#include "browser/gpu.h"
#include "raylib.h"
#include "rlgl.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Manual hardware integration test: the same patched Raylib/GLFW library and
 * import code as Pico, with real CEF and real cross-process sync_file fences.
 * Readback is ONLY a test oracle, never the continuous presentation path. */
extern char **environ;
static int commands = -1, events = -1;
static pid_t helper;
static PbGpu gpu;
typedef struct Frame { PbFrame metadata; PbGpuImage image; } Frame;
static void Cleanup(void) {
    if (helper > 0) { kill(-helper, SIGKILL); waitpid(helper, NULL, 0); }
    if (commands >= 0) close(commands);
    if (events >= 0) close(events);
}
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s; %s\n", __LINE__, #condition, gpu.error); exit(1); } } while (0)
static void Command(PbCommand command) { CHECK(PbSend(commands, &command, sizeof(command), NULL, 0) == sizeof(command)); }
static void Release(Frame *frame) {
    rlDrawRenderBatchActive();
    int fence = PbGpuFence(&gpu); CHECK(fence >= 0);
    PbFds fds = {.fd={fence}, .count=1};
    PbCommand command = {.token=1, .type=PB_RELEASE, .a=frame->metadata.slot, .request=frame->metadata.serial};
    CHECK(PbSend(commands, &command, sizeof(command), &fds, 0) == sizeof(command));
    PbFdsClose(&fds); PbGpuDestroy(&gpu, &frame->image);
}
static void Read(void *data, size_t size, PbFds *fds) {
    while (size) {
        struct pollfd p = {.fd=events, .events=POLLIN};
        int ready = poll(&p, 1, 100);
        CHECK(ready >= 0);
        PollInputEvents();
        if (!ready) continue;
        ssize_t n = PbReceive(events, data, size, fds, 0);
        CHECK(n > 0); size -= n; data = (char *)data + n;
    }
}
static Frame Next(void) {
    for (;;) {
        PbEvent event; PbFds fds = {0}; Read(&event, sizeof(event), &fds);
        CHECK(event.length <= PB_URL_CAP);
        unsigned char payload[PB_URL_CAP + 1]; Read(payload, event.length, &fds); payload[event.length] = 0;
        if (event.type == PB_ERROR || event.type == PB_STOPPED) { fprintf(stderr, "Helper: %s\n", payload); exit(1); }
        if (event.type != PB_FRAME) { CHECK(!fds.count); continue; }
        Frame frame = {0}; CHECK(event.length == sizeof(PbFrame)); memcpy(&frame.metadata, payload, sizeof(PbFrame));
        CHECK(PbFrameValid(&frame.metadata, &fds));
        int fence = fds.fd[--fds.count]; CHECK(PbGpuWait(&gpu, fence));
        CHECK(PbGpuImport(&gpu, &frame.image, &frame.metadata, &fds)); PbFdsClose(&fds);
        return frame;
    }
}
static bool ColorIs(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == 255; }
static bool Pixels(Frame *frame, Color top, Color bottom) {
    int w = frame->image.width, h = frame->image.height;
    Texture2D texture = {.id=frame->image.texture, .width=w, .height=h, .mipmaps=1, .format=PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    RenderTexture2D canvas = LoadRenderTexture(w, h); CHECK(canvas.id);
    BeginTextureMode(canvas); ClearBackground(BLACK); DrawTexture(texture, 0, 0, WHITE); EndTextureMode();
    Image image = LoadImageFromTexture(canvas.texture); CHECK(image.data); ImageFlipVertical(&image);
    Color a = GetImageColor(image, w/2, h/4), b = GetImageColor(image, w/2, h*3/4);
    bool ok = ColorIs(a, top) && ColorIs(b, bottom);
    UnloadImage(image); UnloadRenderTexture(canvas);
    return ok;
}
static Frame Until(int w, int h, Color top, Color bottom) {
    for (;;) {
        Frame frame = Next();
        if (frame.image.width == w && frame.image.height == h && Pixels(&frame, top, bottom)) return frame;
        Release(&frame);
    }
}
static void Navigate(const char *top, const char *bottom) {
    PbCommand command = {.token=1, .type=PB_OPEN}; strcpy(command.profile, "abcd");
    snprintf(command.text, sizeof(command.text), "data:text/html,<style>body{margin:0;background:%s}div{height:50vh;background:%s}</style><div></div>", bottom, top);
    Command(command);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    char root[] = "/tmp/pico-browser-gpu-test-XXXXXX"; CHECK(mkdtemp(root));
    fprintf(stderr, "GPU test logs: %s/cef.log\n", root);
    atexit(Cleanup);
    SetTraceLogLevel(LOG_WARNING); SetConfigFlags(FLAG_WINDOW_HIDDEN); InitWindow(400, 300, "Pico browser GPU regression");
    if (!IsWindowReady()) { fprintf(stderr, "SKIP: no graphics window\n"); return 77; }
    if (!PbGpuInit(&gpu, false)) { fprintf(stderr, "SKIP: %s\n", gpu.error); CloseWindow(); return 77; }
    int command_pair[2], event_pair[2];
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, command_pair));
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, event_pair));
    commands = command_pair[0]; events = event_pair[0];
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, command_pair[1], 198); posix_spawn_file_actions_adddup2(&actions, event_pair[1], 199);
    posix_spawnattr_t attr; posix_spawnattr_init(&attr); posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&attr, 0);
    char *args[] = {argv[1], "198", "199", root, NULL};
    CHECK(!posix_spawn(&helper, argv[1], &actions, &attr, args, environ));
    posix_spawnattr_destroy(&attr); posix_spawn_file_actions_destroy(&actions);
    close(command_pair[1]); close(event_pair[1]);
    Navigate("rgb(220,30,50)", "rgb(20,60,210)");
    Command((PbCommand){.token=1, .type=PB_RESIZE, .x=320, .y=240}); Command((PbCommand){.token=1, .type=PB_VISIBLE, .a=1});
    Frame held[PB_POOL_SIZE];
    held[0] = Until(320, 240, (Color){220,30,50,255}, (Color){20,60,210,255});
    Navigate("rgb(20,200,70)", "rgb(230,170,10)");
    held[1] = Until(320, 240, (Color){20,200,70,255}, (Color){230,170,10,255});
    CHECK(Pixels(&held[0], (Color){220,30,50,255}, (Color){20,60,210,255})); // lease must protect contents
    // Fill the pool without releasing its older images, then resize while
    // every allocation is in flight. A release must let presentation resume.
    for (unsigned i = 2; i < PB_POOL_SIZE; ++i) {
        Command((PbCommand){.token=1, .type=PB_RESIZE, .x=320+(int)i, .y=199});
        held[i] = Until(320+i, 199, (Color){20,200,70,255}, (Color){230,170,10,255});
    }
    Command((PbCommand){.token=1, .type=PB_RESIZE, .x=333, .y=211});
    Release(&held[1]);
    Frame resized = Until(333, 211, (Color){20,200,70,255}, (Color){230,170,10,255});
    CHECK(Pixels(&held[0], (Color){220,30,50,255}, (Color){20,60,210,255}));
    for (unsigned i = 0; i < PB_POOL_SIZE; ++i) if (i != 1) Release(&held[i]);
    // Imported storage must remain valid after its allocating process dies.
    CHECK(!kill(-helper, SIGKILL)); CHECK(waitpid(helper, NULL, 0) == helper); helper = 0;
    CHECK(Pixels(&resized, (Color){20,200,70,255}, (Color){230,170,10,255}));
    PbGpuDestroy(&gpu, &resized.image); PbGpuShutdown(&gpu); CloseWindow();
    puts("PASS: EGL/Raylib DMA-BUF pixels, orientation, cross-process fences, leased contents, pool backpressure, resize, helper death");
    return 0;
}
