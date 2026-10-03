#include "test_wait.h"
#ifdef PICO_TEST_CLOCK
#include "test_clock.h"
#include "test_io.h"
#endif
#include "theme_internal.h"
#include "pico/host.h"
#include "pico/plugin.h"
#include "pico/auth.h"
#include "host_internal.h"
#include "sanitizer_detect.h"
#include "workspace_internal.h"
#include "settings.h"
#include "session.h"
#include "json.h"
#include "path.h"
#include "scrollbar.h"
#include "trace_group.h"
#include "richtext.h"
#include "chat_sel.h"
#include "builtins/chat.h"
#include "builtins/background_model.h"
#include "builtins/sidebar.h"
#include "agent_internal.h"
#include "agent.h"
#include "clarification.h"
#include "overlay.h"
#include "worktree.h"
#include "docs_path.h"
#include "clay/clay.h"
#include <utf8proc.h>

#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int pico_files_complete(PicoWorkspace *workspace, const char *prefix, PicoCompleteItem *out, int max, void *state);

#ifdef PICO_CLAY_FRAME_FAULT_TESTS
static bool g_find_input_test;
static int g_find_key;
static bool g_find_ctrl;
static bool g_find_shift;
static int g_find_character;
static bool g_find_press;
static bool g_find_down;
static bool g_find_released;
static int g_find_button = MOUSE_BUTTON_LEFT;
static Vector2 g_find_pointer;
static Vector2 g_find_wheel;

bool __real_IsKeyDown(int key);
bool __wrap_IsKeyDown(int key)
{
    if (!g_find_input_test) return __real_IsKeyDown(key);
    return (key == KEY_LEFT_CONTROL && g_find_ctrl) || (key == KEY_LEFT_SHIFT && g_find_shift);
}
bool __real_IsKeyPressed(int key);
bool __wrap_IsKeyPressed(int key) { return g_find_input_test ? key == g_find_key : __real_IsKeyPressed(key); }
int __real_GetCharPressed(void);
int __wrap_GetCharPressed(void)
{
    if (!g_find_input_test) return __real_GetCharPressed();
    int cp = g_find_character;
    g_find_character = 0;
    return cp;
}
bool __real_IsMouseButtonPressed(int button);
bool __wrap_IsMouseButtonPressed(int button)
{
    return g_find_input_test ? button == g_find_button && g_find_press : __real_IsMouseButtonPressed(button);
}
bool __real_IsMouseButtonDown(int button);
bool __wrap_IsMouseButtonDown(int button)
{
    return g_find_input_test ? button == g_find_button && g_find_down : __real_IsMouseButtonDown(button);
}
bool __real_IsMouseButtonReleased(int button);
bool __wrap_IsMouseButtonReleased(int button)
{
    return g_find_input_test ? button == g_find_button && g_find_released : __real_IsMouseButtonReleased(button);
}
Vector2 __real_GetMousePosition(void);
Vector2 __wrap_GetMousePosition(void) { return g_find_input_test ? g_find_pointer : __real_GetMousePosition(); }
Vector2 __real_GetMouseWheelMoveV(void);
Vector2 __wrap_GetMouseWheelMoveV(void)
{
    return g_find_input_test ? g_find_wheel : __real_GetMouseWheelMoveV();
}

static bool g_clay_frame_test;
/* Do not let an unrelated persistence worker consume the UI allocation fault. */
static __thread bool g_fail_frame_allocation;
static int g_clay_failed_allocations;
static int g_clay_layout_calls;
static int g_clay_after_layout_calls;
static int g_clay_after_render_calls;
static bool g_clay_sentinel_presented;

void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size)
{
    if (g_fail_frame_allocation)
    {
        g_fail_frame_allocation = false;
        g_clay_failed_allocations++;
        return NULL;
    }
    return __real_malloc(size);
}

int __real_GetScreenWidth(void);
int __wrap_GetScreenWidth(void) { return g_clay_frame_test ? 1100 : __real_GetScreenWidth(); }
int __real_GetScreenHeight(void);
int __wrap_GetScreenHeight(void) { return g_clay_frame_test ? 800 : __real_GetScreenHeight(); }
void __real_SetMouseCursor(int cursor);
void __wrap_SetMouseCursor(int cursor) { if (!g_clay_frame_test) __real_SetMouseCursor(cursor); }
void __real_BeginDrawing(void);
void __wrap_BeginDrawing(void) { if (!g_clay_frame_test) __real_BeginDrawing(); }
void __real_ClearBackground(Color color);
void __wrap_ClearBackground(Color color) { if (!g_clay_frame_test) __real_ClearBackground(color); }
void __real_EndDrawing(void);
void __wrap_EndDrawing(void) { if (!g_clay_frame_test) __real_EndDrawing(); }
#endif

static int g_presented_frames;

void Clay_Raylib_Render(Clay_RenderCommandArray renderCommands, Font *fonts)
{
    (void)renderCommands;
    (void)fonts;
    g_presented_frames++;
#ifdef PICO_CLAY_FRAME_FAULT_TESTS
    if (g_clay_frame_test)
    {
        /* Consume the rebuilt arena's commands, rather than only counting draws. */
        uint32_t sentinel = Clay_GetElementIdWithIndex(CLAY_STRING("RecoveryItem"), 199).id;
        for (int32_t i = 0; i < renderCommands.length; i++)
        {
            if (renderCommands.internalArray[i].id == sentinel)
            {
                g_clay_sentinel_presented = true;
            }
        }
    }
#endif
}

_Static_assert((PicoWorkspaceId)0 == 0, "zero is an invalid workspace id");
_Static_assert((PicoAgentId)0 == 0, "zero is an invalid agent id");

/* Existing lifecycle scenarios assert the final generation. Wait for compilation
 * while keeping the actual production entry points nonblocking. */
static void WaitPluginLoad(PicoHost *host)
{
    PICO_TEST_WAIT_LOOP("asynchronous completion")
    {
        PicoPlugins_Load(host);
        if (!host->plugin_compile) return;
    }
}
static void WaitPluginPoll(PicoHost *host)
{
    /* A due poll schedules detection; the next poll adopts its result. */
    host->plugin_last_poll = -1;
    PICO_TEST_WAIT_LOOP("asynchronous completion")
    {
        PicoPlugins_Poll(host);
        if (!host->plugin_scan_pending && !host->plugin_compile) return;
    }
}
static bool WaitHostReload(PicoHost *host)
{
    PICO_TEST_WAIT_LOOP("asynchronous completion")
    {
        bool ok = PicoPlugins_ReloadHost(host);
        if (!host->plugin_compile) return ok;
    }
    return false;
}
static bool WaitWorkspaceReload(PicoWorkspace *workspace)
{
    PICO_TEST_WAIT_LOOP("asynchronous completion")
    {
        bool ok = PicoWorkspace_Reload(workspace);
        if (!workspace->host->plugin_compile) return ok;
        PicoPlugins_Poll(workspace->host);
    }
    return false;
}
static void WaitPluginsReload(PicoHost *host)
{
    PicoPlugins_Reload(host);
    WaitPluginPoll(host);
}


static int g_failed;
static int g_persist_ready_fd = -1;
static int g_persist_continue_fd = -1;
static atomic_int g_catalog_scan_calls;
static atomic_int g_catalog_scan_done_calls;
static atomic_int g_catalog_snapshot_calls;
static atomic_int g_catalog_snapshot_done_calls;
static int g_catalog_ready_fd = -1;
static int g_catalog_continue_fd = -1;
static bool g_sidebar_poll_due;
static int g_replay_ready_fd = -1;
static int g_replay_continue_fd = -1;
static int g_replay_adopted;

static bool WaitCatalogWorkDone(PicoHost *host, const atomic_int *completed, int target_done)
{
    PICO_TEST_WAIT_LOOP("catalog worker completion and main-thread adoption")
    {
        pico_host_pump(host);
        if (*completed >= target_done && !host->tasks) return true;
    }
}

static bool WaitCatalogScanDone(PicoHost *host, int target_done)
{
    return WaitCatalogWorkDone(host, &g_catalog_scan_done_calls, target_done);
}

static bool WaitCatalogSnapshotDone(PicoHost *host, int target_done)
{
    return WaitCatalogWorkDone(host, &g_catalog_snapshot_done_calls, target_done);
}

static bool TransferTestByte(int fd, bool write_byte)
{
    PicoTest_Wait(__func__, write_byte ? "release gate" : "worker ready gate");
    char byte = 'x';
    ssize_t result;
    do
    {
        result = write_byte ? write(fd, &byte, 1) : read(fd, &byte, 1);
    } while (result < 0 && errno == EINTR);
    return result == 1;
}

bool PicoSession_TestHook(const char *stage)
{
    if (stage && strcmp(stage, "async_replay_after_adopt") == 0)
    {
        g_replay_adopted++;
    }
    if (stage && strcmp(stage, "async_replay_before_read") == 0 &&
        g_replay_ready_fd >= 0 && g_replay_continue_fd >= 0)
    {
        int ready = g_replay_ready_fd;
        int resume = g_replay_continue_fd;
        g_replay_ready_fd = g_replay_continue_fd = -1;
        return !TransferTestByte(ready, true) || !TransferTestByte(resume, false);
    }
    if (stage && strcmp(stage, "catalog_scan") == 0)
    {
        g_catalog_scan_calls++;
        if (g_catalog_ready_fd >= 0 && g_catalog_continue_fd >= 0)
        {
            int ready = g_catalog_ready_fd, resume = g_catalog_continue_fd;
            g_catalog_ready_fd = g_catalog_continue_fd = -1;
            if (!TransferTestByte(ready, true) || !TransferTestByte(resume, false)) return true;
        }
    }
    if (stage && strcmp(stage, "catalog_scan_done") == 0)
    {
        g_catalog_scan_done_calls++;
    }
    if (stage && strcmp(stage, "catalog_snapshot") == 0) g_catalog_snapshot_calls++;
    if (stage && strcmp(stage, "catalog_snapshot_done") == 0) g_catalog_snapshot_done_calls++;
    if (stage && strcmp(stage, "sidebar_poll_due") == 0)
    {
        bool due = g_sidebar_poll_due;
        g_sidebar_poll_due = false;
        return due;
    }
    if (stage && strcmp(stage, "catalog_before_upsert") == 0 &&
        g_persist_ready_fd >= 0 && g_persist_continue_fd >= 0)
    {
        int ready_fd = g_persist_ready_fd;
        int continue_fd = g_persist_continue_fd;
        g_persist_ready_fd = -1;
        g_persist_continue_fd = -1;
        return !TransferTestByte(ready_fd, true) || !TransferTestByte(continue_fd, false);
    }
    return false;
}

static void Fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    g_failed = 1;
}

static void ShellTestSidebar(PicoHost *host, void *state)
{
    (void)host;
    (void)state;
    CLAY(CLAY_ID("ShellTestSidebarRoot"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .childGap = 6,
                     .sizing = {.width = CLAY_SIZING_PERCENT(1), .height = CLAY_SIZING_GROW(0)}}})
    {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_PERCENT(1),
                                            .height = CLAY_SIZING_FIXED(25)}}})
        {
        }
        CLAY(CLAY_ID("ShellTestSidebarScroll"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .sizing = {.width = CLAY_SIZING_PERCENT(1), .height = CLAY_SIZING_GROW(0)}},
              .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
        {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_PERCENT(1),
                                                .height = CLAY_SIZING_FIXED(1800)}}})
            {
            }
        }
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(1),
                                            .height = CLAY_SIZING_FIXED(1400)}}})
        {
        }
    }
}

static void ShellTestChat(PicoHost *host, void *state)
{
    (void)host;
    (void)state;
    CLAY(CLAY_ID("ChatRow"),
         {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}})
    {
        CLAY(CLAY_ID("ChatScroll"),
             {.layout = {.layoutDirection = CLAY_TOP_TO_BOTTOM,
                         .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}},
              .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
        {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                                .height = CLAY_SIZING_FIXED(4803)}}})
            {
            }
        }
    }
    /* Host and workspace views are extension points. Oversized content must not
     * be allowed to enlarge the viewport panes that contain it. */
    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(1),
                                        .height = CLAY_SIZING_FIXED(1400)}}})
    {
    }
}

typedef struct ShellTestState {
    float composer_height;
} ShellTestState;

static void ShellTestComposer(PicoHost *host, void *state)
{
    ShellTestState *test = (ShellTestState *)state;
    (void)host;
    CLAY(CLAY_ID("ComposerAlign"),
         {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                .height = CLAY_SIZING_FIXED(test->composer_height)}},
          .transition = {.handler = Clay_EaseOut,
                         .duration = 0.18f,
                         .properties = CLAY_TRANSITION_PROPERTY_DIMENSIONS}})
    {
        CLAY(CLAY_ID("Composer"), {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0),
                                                           .height = CLAY_SIZING_PERCENT(1)}}}) {}
    }
}

static int g_shell_workspace_view_calls;

static void ShellTestWorkspaceView(PicoWorkspace *workspace, PicoAgentId selected_agent_id, void *state)
{
    (void)workspace;
    (void)selected_agent_id;
    (void)state;
    g_shell_workspace_view_calls++;
}

static void ShellTestFooter(PicoHost *host, void *state)
{
    (void)host;
    (void)state;
    CLAY(CLAY_ID("Footer"),
         {.layout = {.padding = {0, 0, 8, 8},
                     .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}}})
    {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(16),
                                            .height = CLAY_SIZING_FIXED(16)}}})
        {
        }
    }
}

static void ShellTestAddView(PicoHost *host, PicoUiSlot slot, PicoHostViewFn render, void *state)
{
    host->views[slot][0].host_render = render;
    host->views[slot][0].state = state;
    host->view_count[slot] = 1;
}

static Clay_Dimensions ShellMeasureText(Clay_StringSlice text, Clay_TextElementConfig *config,
                                        void *user_data)
{
    (void)user_data;
    return (Clay_Dimensions){.width = (float)text.length * (float)config->fontSize * 0.6f,
                             .height = (float)config->fontSize};
}

static bool ShellBoxStable(Clay_BoundingBox expected, Clay_BoundingBox actual)
{
    const float tolerance = 0.001f;
    return fabsf(expected.x - actual.x) <= tolerance &&
           fabsf(expected.y - actual.y) <= tolerance &&
           fabsf(expected.width - actual.width) <= tolerance &&
           fabsf(expected.height - actual.height) <= tolerance;
}

static bool ShellVerticallyContains(Clay_BoundingBox outer, Clay_BoundingBox inner)
{
    const float tolerance = 0.001f;
    return inner.y >= outer.y - tolerance &&
           inner.y + inner.height <= outer.y + outer.height + tolerance;
}

static Clay_ElementId MainTraceRowId(int message_index, int trace_index, const char *label);
static float TraceRowHeight(Clay_ElementId id, bool *found);

typedef struct ChatStabilitySnapshot {
    Clay_BoundingBox root;
    Clay_BoundingBox body;
    Clay_BoundingBox sidebar;
    Clay_BoundingBox right;
    Clay_BoundingBox main;
    Clay_BoundingBox chat;
    Clay_BoundingBox composer;
    Clay_BoundingBox footer;
    Clay_BoundingBox bottom;
    Clay_BoundingBox stabilization;
    float content_height;
    bool stabilization_found;
    float container_height;
    float scroll_y;
} ChatStabilitySnapshot;

static bool CaptureChatStabilitySnapshot(bool with_sidebar, ChatStabilitySnapshot *out)
{
    Clay_ElementData root = Clay_GetElementData(CLAY_ID("Root"));
    Clay_ElementData body = Clay_GetElementData(CLAY_ID("Body"));
    Clay_ElementData sidebar = Clay_GetElementData(CLAY_ID("Sidebar"));
    Clay_ElementData right = Clay_GetElementData(CLAY_ID("RightColumn"));
    Clay_ElementData main = Clay_GetElementData(CLAY_ID("MainColumn"));
    Clay_ElementData chat = Clay_GetElementData(CLAY_ID("ChatScroll"));
    Clay_ElementData composer = Clay_GetElementData(CLAY_ID("ComposerAlign"));
    Clay_ElementData footer = Clay_GetElementData(CLAY_ID("Footer"));
    Clay_ElementData bottom = Clay_GetElementData(CLAY_ID("ChatBottomSpacer"));
    Clay_ElementData stabilization = Clay_GetElementData(CLAY_ID("ChatStabilizationSpacer"));
    Clay_ScrollContainerData scroll =
        Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));

    if (!root.found || !body.found || !right.found || !main.found || !chat.found ||
        !composer.found || !footer.found || !bottom.found ||
        (with_sidebar && !sidebar.found) || (!with_sidebar && sidebar.found) ||
        !scroll.found || !scroll.scrollPosition)
    {
        return false;
    }
    out->root = root.boundingBox;
    out->body = body.boundingBox;
    out->sidebar = sidebar.boundingBox;
    out->right = right.boundingBox;
    out->main = main.boundingBox;
    out->chat = chat.boundingBox;
    out->composer = composer.boundingBox;
    out->footer = footer.boundingBox;
    out->bottom = bottom.boundingBox;
    out->stabilization = stabilization.boundingBox;
    out->stabilization_found = stabilization.found;
    out->content_height = scroll.contentDimensions.height;
    out->container_height = scroll.scrollContainerDimensions.height;
    out->scroll_y = scroll.scrollPosition->y;
    return true;
}

static bool ChatStabilitySnapshotStable(const ChatStabilitySnapshot *a,
                                        const ChatStabilitySnapshot *b,
                                        bool with_sidebar)
{
    return ShellBoxStable(a->root, b->root) && ShellBoxStable(a->body, b->body) &&
           (!with_sidebar || ShellBoxStable(a->sidebar, b->sidebar)) &&
           ShellBoxStable(a->right, b->right) && ShellBoxStable(a->main, b->main) &&
           ShellBoxStable(a->chat, b->chat) && ShellBoxStable(a->composer, b->composer) &&
           ShellBoxStable(a->footer, b->footer) && ShellBoxStable(a->bottom, b->bottom) &&
           a->stabilization_found == b->stabilization_found &&
           (!a->stabilization_found || ShellBoxStable(a->stabilization, b->stabilization)) &&
           fabsf(a->content_height - b->content_height) <= 0.001f &&
           fabsf(a->container_height - b->container_height) <= 0.001f &&
           fabsf(a->scroll_y - b->scroll_y) <= 0.001f;
}

static void LayoutChatStabilityFrame(PicoHost *host, const Clay_Dimensions viewport)
{
    Clay_SetLayoutDimensions(viewport);
    Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0.0f);
    (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
    PicoChat_HarvestVirtualHeights(host);
}

static int CheckChatViewportWrapping(PicoHost *host, PicoAgent *agent)
{
    const Clay_Dimensions viewport = {1100, 800};
    const Clay_Dimensions narrow_viewport = {600, 800};
    /* This paragraph wraps even in the wider pane. Check rendered geometry,
     * without a Raylib window or a warm-up frame after allocation changes. */
    PicoHost_ClearMessages(host, agent->id);
    PicoAgent_AddMessage(host, agent, PICO_ROLE_USER,
                         "A long paragraph should use the space provided by the current chat pane. "
                         "Resizing the viewport must change where its words wrap, and opening the "
                         "sidebar must leave less room for those words even when the window size "
                         "does not change. This paragraph is deliberately long enough to occupy "
                         "multiple lines in each of the layouts exercised here. "
                         "The same content should become taller in a narrower pane rather than "
                         "keeping a wrap width from the previous frame or from the window backend.");
    host->view_count[PICO_SLOT_SIDEBAR] = 0;
    LayoutChatStabilityFrame(host, viewport);
    Clay_ElementData wide = Clay_GetElementData(CLAY_IDI("MsgMain", 0));
    LayoutChatStabilityFrame(host, narrow_viewport);
    Clay_ElementData narrow = Clay_GetElementData(CLAY_IDI("MsgMain", 0));
    if (!wide.found || !narrow.found || narrow.boundingBox.height <= wide.boundingBox.height)
    {
        Fail("chat text must wrap to the current Clay viewport on resize");
        return 1;
    }
    ShellTestAddView(host, PICO_SLOT_SIDEBAR, ShellTestSidebar, NULL);
    LayoutChatStabilityFrame(host, narrow_viewport);
    Clay_ElementData sidebar_message = Clay_GetElementData(CLAY_IDI("MsgMain", 0));
    if (!sidebar_message.found || sidebar_message.boundingBox.height <= narrow.boundingBox.height)
    {
        Fail("opening the sidebar must rewrap chat text in the same frame");
        return 1;
    }
    host->view_count[PICO_SLOT_SIDEBAR] = 0;
    LayoutChatStabilityFrame(host, viewport);
    Clay_ElementData restored = Clay_GetElementData(CLAY_IDI("MsgMain", 0));
    if (!restored.found || !ShellBoxStable(wide.boundingBox, restored.boundingBox))
    {
        Fail("restoring the viewport must restore chat message geometry");
        return 1;
    }
    host->preferences.chat_width = 40;
    LayoutChatStabilityFrame(host, viewport);
    Clay_ElementData capped = Clay_GetElementData(CLAY_IDI("MsgMain", 0));
    LayoutChatStabilityFrame(host, (Clay_Dimensions){1500, 800});
    Clay_ElementData capped_wider = Clay_GetElementData(CLAY_IDI("MsgMain", 0));
    if (!capped.found || !capped_wider.found ||
        capped.boundingBox.width >= wide.boundingBox.width ||
        fabsf(capped.boundingBox.width - capped_wider.boundingBox.width) > 0.001f ||
        fabsf(capped.boundingBox.height - capped_wider.boundingBox.height) > 0.001f)
    {
        Fail("configured chat width must cap wrapping as the viewport grows");
        return 1;
    }
    return 0;
}

/* Exercise the real chat rather than a shell stand-in. A group of completed
 * tool rows gives us a deterministic, product-level shrink: the visible rows
 * become one finished-trace header. The assertions intentionally compare the
 * resulting relationship between the two spacers and the scroll state rather
 * than the configured spacer heights. */
static int RunChatRetainedSpacerCase(bool with_sidebar)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-chat-stabilize-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-chat-stabilize-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions opt;
    PicoAgent *agent = NULL;
    ShellTestState state = {.composer_height = 44.0f};
    Clay_Arena arena;
    ChatStabilitySnapshot open = {0};
    ChatStabilitySnapshot collapsed = {0};
    ChatStabilitySnapshot ended = {0};
    ChatStabilitySnapshot grown = {0};
    ChatStabilitySnapshot reset = {0};
    ChatStabilitySnapshot second_open = {0};
    ChatStabilitySnapshot second_collapsed = {0};
    ChatStabilitySnapshot away = {0};
    ChatStabilitySnapshot away_final = {0};
    Clay_BoundingBox away_anchor = {0};
    Clay_BoundingBox away_anchor_final = {0};
    int trace_message;
    bool f_tool = false;
    bool f_group = false;
    int rc = 1;

    if (!memory || !mkdtemp(dir))
    {
        free(memory);
        Fail("chat stabilization setup");
        return 1;
    }
    if (!mkdtemp(cfg))
    {
        free(memory);
        rmdir(dir);
        Fail("chat stabilization config setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(cfg);
        rmdir(dir);
        Fail("chat stabilization host init");
        return 1;
    }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    if (!with_sidebar)
    {
        host->view_count[PICO_SLOT_SIDEBAR] = 0;
    }
    else if (host->view_count[PICO_SLOT_SIDEBAR] <= 0)
    {
        Fail("chat stabilization sidebar was not registered");
        goto done;
    }
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK)
    {
        Fail("chat stabilization open workspace");
        goto done;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id)))
    {
        Fail("chat stabilization create agent");
        goto done;
    }

    /* Keep enough history to make every change an overflowing-chat case. */
    for (int i = 0; i < 28; i++)
    {
        PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "retained spacer history message");
    }
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    trace_message = agent->message_count - 1;
    for (int i = 0; i < 8; i++)
    {
        char call_id[32];
        snprintf(call_id, sizeof(call_id), "stabilize-%d", i);
        PicoAgent_AddToolCallWithId(host, agent, call_id, "read", "{}");
        PicoAgent_SetToolOutputByCallId(agent, call_id, "ok", false);
        agent->messages[trace_message].trace[i].tool_done_t0 = 0.0;
    }
    agent->messages[trace_message].trace_group_expanded = true;
    agent->state = PICO_AGENT_TOOL_WAIT;

    arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        Clay_SetCurrentContext(previous);
        Fail("chat stabilization Clay initialization");
        goto done_host;
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);

    host->chat_follow_bottom = true;
    PicoChat_ResetBottomSpace(host);
    for (int frame = 0; frame < 5; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    {
        Clay_ScrollContainerData scroll =
            Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
        if (!scroll.found || !scroll.scrollPosition)
        {
            Fail("chat stabilization could not establish the initial bottom");
            goto done;
        }
    }
    for (int frame = 0; frame < 4; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    if (!CaptureChatStabilitySnapshot(with_sidebar, &open))
    {
        Fail("chat stabilization could not capture the open layout");
        goto done;
    }
    if (TraceRowHeight(MainTraceRowId(trace_message, 0, "ToolRow"), &f_tool) <= 0.0f || !f_tool)
    {
        Fail("chat stabilization did not render the open tool rows");
        goto done;
    }
    {
        float overflow = open.content_height - open.container_height;
        if (overflow <= 0.0f || fabsf(open.scroll_y + overflow) > 0.5f)
        {
            Fail("chat stabilization open layout was not at the overflowing bottom");
            goto done;
        }
    }

    /* A direct completion/grouping transition shrinks the real transcript. */
    for (int i = 0; i < 8; i++)
    {
        char call_id[32];
        snprintf(call_id, sizeof(call_id), "stabilize-%d", i);
        PicoAgent_SetToolOutputByCallId(agent, call_id, "ok", false);
    }
    agent->messages[trace_message].trace_group_expanded = false;
    for (int i = 0; i < agent->messages[trace_message].trace_count; i++)
    {
        agent->messages[trace_message].trace[i].tool_done_t0 = 0.0;
    }
    for (int frame = 0; frame < 10; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
        if (!CaptureChatStabilitySnapshot(with_sidebar, &collapsed))
        {
            Fail("chat stabilization could not capture the collapsed layout");
            goto done;
        }
        if (fabsf(collapsed.scroll_y - open.scroll_y) > 0.5f)
        {
            Fail("grouping must preserve the scroll offset on every presented frame");
            goto done;
        }
    }
    (void)TraceRowHeight(MainTraceRowId(trace_message, 0, "ToolRow"), &f_tool);
    (void)TraceRowHeight(MainTraceRowId(trace_message, 0, "TraceGroupRow"), &f_group);
    if (!f_group || f_tool || collapsed.stabilization.height <= open.stabilization.height + 0.5f ||
        fabsf(collapsed.content_height - open.content_height) > 0.5f ||
        fabsf(collapsed.scroll_y - open.scroll_y) > 0.5f)
    {
        Fail("bottom-follow shrink must retain the effective extent and offset");
        goto done;
    }
    if (fabsf(collapsed.bottom.height - open.bottom.height) > 0.001f)
    {
        Fail("transcript shrink must not replace the real bottom clearance spacer");
        goto done;
    }

    /* Once retained, repeated direct layout passes must not accumulate space or
     * feed a Clay remainder back into any shell pane. */
    {
        ChatStabilitySnapshot stable = collapsed;
        for (int frame = 0; frame < 60; frame++)
        {
            LayoutChatStabilityFrame(host, viewport);
            ChatStabilitySnapshot now;
            if (!CaptureChatStabilitySnapshot(with_sidebar, &now) ||
                !ChatStabilitySnapshotStable(&stable, &now, with_sidebar))
            {
                Fail(with_sidebar ? "retained chat geometry accumulated with sidebar"
                                  : "retained chat geometry accumulated without sidebar");
                goto done;
            }
        }
    }

    /* Repeated equivalent expand/group cycles must not ratchet the extent. */
    for (int cycle = 0; cycle < 5; cycle++)
    {
        agent->messages[trace_message].trace_group_expanded = true;
        for (int frame = 0; frame < 3; frame++)
        {
            LayoutChatStabilityFrame(host, viewport);
        }
        agent->messages[trace_message].trace_group_expanded = false;
        for (int frame = 0; frame < 3; frame++)
        {
            LayoutChatStabilityFrame(host, viewport);
        }
        ChatStabilitySnapshot now;
        if (!CaptureChatStabilitySnapshot(with_sidebar, &now) ||
            !ChatStabilitySnapshotStable(&collapsed, &now, with_sidebar))
        {
            Fail("repeated grouping must not accumulate artificial scroll extent");
            goto done;
        }
    }

    /* Completing a turn is not an explicit request to jump to bottom. */
    agent->state = PICO_AGENT_IDLE;
    pico_run_hooks(host, PICO_HOOK_ON_TURN_END, agent->id);
    for (int frame = 0; frame < 6; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    if (!CaptureChatStabilitySnapshot(with_sidebar, &ended) ||
        ended.stabilization.height <= 0.5f ||
        !ChatStabilitySnapshotStable(&collapsed, &ended, with_sidebar))
    {
        Fail("turn end must not reset retained bottom space");
        goto done;
    }

    /* A small message growth consumes retained space rather than adding a
     * second independent spacer or changing the effective bottom. */
    PicoAgent_AddMessage(host, agent, PICO_ROLE_USER, "one small growth message");
    for (int frame = 0; frame < 8; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    Clay_ElementData small_message = Clay_GetElementData(CLAY_IDI("MsgMain", agent->message_count - 1));
    Clay_ElementData small_gap = Clay_GetElementData(CLAY_IDI("TranscriptGapMain", agent->message_count - 2));
    float small_growth = small_message.boundingBox.height + small_gap.boundingBox.height;
    if (!small_message.found || !small_gap.found || small_growth <= 0.0f ||
        small_growth >= ended.stabilization.height)
    {
        Fail("small-growth case requires a rendered message that fits inside retained space");
        goto done;
    }
    if (!CaptureChatStabilitySnapshot(with_sidebar, &grown) ||
        !grown.stabilization_found ||
        fabsf(ended.stabilization.height - grown.stabilization.height - small_growth) > 0.5f ||
        fabsf(grown.content_height - ended.content_height) > 0.5f ||
        fabsf(grown.scroll_y - ended.scroll_y) > 0.5f)
    {
        Fail("chat growth must consume retained bottom space before growing extent");
        goto done;
    }

    /* An actual user group collapse must reset retention, not just a direct
     * call to the reset helper. Reopen it, then release on the group header. */
    agent->messages[trace_message].trace_group_expanded = true;
    for (int frame = 0; frame < 3; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    {
        Clay_ElementData group = Clay_GetElementData(MainTraceRowId(trace_message, 0, "TraceGroupRow"));
        if (!group.found)
        {
            Fail("manual collapse requires a visible group header");
            goto done;
        }
        Clay_SetPointerState((Clay_Vector2){group.boundingBox.x + 8.0f,
                                           group.boundingBox.y + group.boundingBox.height / 2.0f}, false);
        host->chat_sel.mouse_selecting = true;
        host->chat_sel.pressed_group = true;
        host->chat_sel.tool_msg = trace_message;
        PicoChat_HandlePointer(host, NULL, NULL);
        if (agent->messages[trace_message].trace_group_expanded)
        {
            Fail("pointer release must collapse the group");
            goto done;
        }
        Clay_SetPointerState((Clay_Vector2){0}, false);
    }
    for (int frame = 0; frame < 6; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    if (!CaptureChatStabilitySnapshot(with_sidebar, &reset))
    {
        Fail("explicit bottom request could not capture the reset layout");
        goto done;
    }
    {
        float reset_overflow = reset.content_height - reset.container_height;
        if (reset.stabilization.height > 0.5f ||
            reset.content_height >= grown.content_height - 0.5f ||
            reset.bottom.height <= 0.0f || reset_overflow <= 0.0f ||
            fabsf(reset.scroll_y + reset_overflow) > 0.5f)
        {
            Fail("explicit bottom request must clear retention but keep bottom clearance");
            goto done;
        }
    }

    /* Recreate a retained shrink, then leave the bottom. Space that is below a
     * top-scrolled viewport is safe to reclaim; it must not pin the user back
     * to bottom or move the visible top message on later passes. */
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    trace_message = agent->message_count - 1;
    for (int i = 0; i < 6; i++)
    {
        char call_id[32];
        snprintf(call_id, sizeof(call_id), "away-%d", i);
        PicoAgent_AddToolCallWithId(host, agent, call_id, "read", "{}");
        PicoAgent_SetToolOutputByCallId(agent, call_id, "ok", false);
        agent->messages[trace_message].trace[i].tool_done_t0 = 0.0;
    }
    agent->messages[trace_message].trace_group_expanded = true;
    agent->state = PICO_AGENT_TOOL_WAIT;
    for (int frame = 0; frame < 6; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    if (!CaptureChatStabilitySnapshot(with_sidebar, &second_open) ||
        second_open.scroll_y >= reset.scroll_y - 0.5f ||
        second_open.stabilization.height > 0.5f)
    {
        Fail("new output beyond retained space must resume downward following");
        goto done;
    }
    for (int i = 0; i < 6; i++)
    {
        char call_id[32];
        snprintf(call_id, sizeof(call_id), "away-%d", i);
        PicoAgent_SetToolOutputByCallId(agent, call_id, "ok", false);
    }
    agent->messages[trace_message].trace_group_expanded = false;
    for (int i = 0; i < agent->messages[trace_message].trace_count; i++)
    {
        agent->messages[trace_message].trace[i].tool_done_t0 = 0.0;
    }
    for (int frame = 0; frame < 10; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    if (!CaptureChatStabilitySnapshot(with_sidebar, &second_collapsed) ||
        second_collapsed.stabilization.height <= 0.5f)
    {
        Fail("second chat shrink did not retain bottom space");
        goto done;
    }
    {
        Clay_ScrollContainerData scroll =
            Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
        if (!scroll.found || !scroll.scrollPosition)
        {
            Fail("chat stabilization missing scroll state when leaving bottom");
            goto done;
        }
        host->chat_follow_bottom = false;
        scroll.scrollPosition->y = 0.0f;
    }
    LayoutChatStabilityFrame(host, viewport);
    {
        Clay_ElementData anchor = Clay_GetElementData(CLAY_IDI("MsgMain", 0));
        if (!anchor.found)
        {
            Fail("scrolling away must keep the visible transcript anchor mounted");
            goto done;
        }
        away_anchor = anchor.boundingBox;
    }
    if (!CaptureChatStabilitySnapshot(with_sidebar, &away))
    {
        Fail("chat stabilization could not capture the scrolled-away layout");
        goto done;
    }
    for (int frame = 0; frame < 20; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    away_anchor_final = Clay_GetElementData(CLAY_IDI("MsgMain", 0)).boundingBox;
    if (!CaptureChatStabilitySnapshot(with_sidebar, &away_final) ||
        away_final.stabilization.height >= second_collapsed.stabilization.height - 0.5f ||
        away_final.scroll_y > 0.5f ||
        !ShellBoxStable(away_anchor, away_anchor_final))
    {
        Fail("scrolling away must reclaim only safe retained space");
        goto done;
    }

    /* Recreate retention and change actual width allocation without changing
     * window dimensions. Geometry changes must not inherit old pixel space. */
    host->chat_follow_bottom = true;
    agent->messages[trace_message].trace_group_expanded = true;
    for (int frame = 0; frame < 3; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    agent->messages[trace_message].trace_group_expanded = false;
    for (int frame = 0; frame < 3; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    if (with_sidebar)
    {
        host->view_count[PICO_SLOT_SIDEBAR] = 0;
    }
    else
    {
        ShellTestAddView(host, PICO_SLOT_SIDEBAR, ShellTestSidebar, NULL);
    }
    for (int frame = 0; frame < 4; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    ChatStabilitySnapshot rebased;
    if (!CaptureChatStabilitySnapshot(!with_sidebar, &rebased) ||
        rebased.stabilization.height > 0.5f)
    {
        Fail("changed chat width must rebase retained space");
        goto done;
    }

    /* Recreate retention for growth that exceeds it. */
    agent->messages[trace_message].trace_group_expanded = true;
    for (int frame = 0; frame < 3; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    agent->messages[trace_message].trace_group_expanded = false;
    for (int frame = 0; frame < 3; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    /* Growth larger than the retained amount must consume all of it and add
     * only the excess. Measure the rendered bubble and gap, not fixture text
     * length or configured spacer sizes. */
    ChatStabilitySnapshot before_excess = {0};
    if (!CaptureChatStabilitySnapshot(!with_sidebar, &before_excess) ||
        before_excess.stabilization.height <= 0.5f)
    {
        Fail("excess-growth case requires retained space");
        goto done;
    }
    PicoAgent_AddMessage(host, agent, PICO_ROLE_USER,
                         "line\n\nline\n\nline\n\nline\n\nline\n\nline\n\n"
                         "line\n\nline\n\nline\n\nline\n\nline\n\nline\n\n"
                         "line\n\nline\n\nline\n\nline");
    for (int frame = 0; frame < 8; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    ChatStabilitySnapshot after_excess = {0};
    Clay_ElementData added = Clay_GetElementData(CLAY_IDI("MsgMain", agent->message_count - 1));
    Clay_ElementData gap = Clay_GetElementData(CLAY_IDI("TranscriptGapMain", agent->message_count - 2));
    float growth = added.boundingBox.height + gap.boundingBox.height;
    float excess = growth - before_excess.stabilization.height;
    if (!added.found || !gap.found || excess <= 0.5f ||
        !CaptureChatStabilitySnapshot(!with_sidebar, &after_excess) ||
        after_excess.stabilization.height > 0.5f ||
        fabsf(after_excess.content_height - before_excess.content_height - excess) > 0.5f ||
        fabsf(after_excess.scroll_y - before_excess.scroll_y + excess) > 0.5f)
    {
        Fail("growth beyond retained space must expand extent and follow only the excess");
        goto done;
    }

    /* Re-establish retained space before testing transcript replacement. */
    for (int phase = 0; phase < 2; phase++)
    {
        agent->messages[trace_message].trace_group_expanded = phase == 0;
        for (int frame = 0; frame < 3; frame++)
        {
            LayoutChatStabilityFrame(host, viewport);
        }
    }
    if (!CaptureChatStabilitySnapshot(!with_sidebar, &rebased) ||
        rebased.stabilization.height <= 0.5f)
    {
        Fail("transcript replacement case requires retained space");
        goto done;
    }

    /* Clear and repopulate in one frame: agent/session identity is unchanged. */
    PicoHost_ClearMessages(host, agent_id);
    PicoAgent_AddMessage(host, agent, PICO_ROLE_USER, "replacement transcript");
    for (int frame = 0; frame < 4; frame++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    if (!CaptureChatStabilitySnapshot(!with_sidebar, &rebased) ||
        rebased.stabilization.height > 0.5f || fabsf(rebased.scroll_y) > 0.5f)
    {
        Fail("replaced transcript must not inherit old scroll extent");
        goto done;
    }

    if (!with_sidebar && CheckChatViewportWrapping(host, agent) != 0)
    {
        goto done;
    }

    /* Replacing all messages without changing the count must discard heights
     * for offscreen rows too, not just for the visible last message. */
    host->chat_follow_bottom = false;
    PicoHost_ClearMessages(host, agent_id);
    for (int i = 0; i < 100; i++)
        PicoAgent_AddMessage(host, agent, PICO_ROLE_USER, "short");
    for (int frame = 0; frame < 8; frame++) LayoutChatStabilityFrame(host, viewport);
    Clay_ScrollContainerData old_scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    float old_height = old_scroll.contentDimensions.height;
    PicoHost_ClearMessages(host, agent_id);
    for (int i = 0; i < 100; i++)
        PicoAgent_AddMessage(host, agent, PICO_ROLE_USER,
                             "longer paragraph\n\nlonger paragraph\n\nlonger paragraph\n\nlonger paragraph");
    for (int frame = 0; frame < 8; frame++) LayoutChatStabilityFrame(host, viewport);
    Clay_ScrollContainerData new_scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    if (!old_scroll.found || !new_scroll.found ||
        new_scroll.contentDimensions.height <= old_height * 1.5f)
    {
        Fail("equal-count replacement kept stale offscreen transcript heights");
        goto done;
    }

    rc = g_failed ? 1 : 0;

done:
    Clay_SetCurrentContext(previous);
done_host:
    if (host && pico_host_free(host) != PICO_HOST_SHUTDOWN_CLEAN)
    {
        Fail("chat stabilization host shutdown retained pending work");
        rc = 1;
    }
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    return rc;
}

static int RunShellStabilityCase(bool with_sidebar)
{
    const Clay_Dimensions viewport = {1714, 1392};
    const int frames = 400;
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost host;
    PicoWorkspace workspace;
    PicoAgent agent;
    ShellTestState state = {.composer_height = 44.0f};
    Clay_BoundingBox expected_root = {0};
    Clay_BoundingBox expected_body = {0};
    Clay_BoundingBox expected_right = {0};
    Clay_BoundingBox expected_sidebar = {0};
    Clay_BoundingBox expected_main = {0};
    Clay_BoundingBox expected_chat = {0};
    Clay_BoundingBox expected_composer = {0};
    Clay_BoundingBox expected_footer = {0};
    Clay_Vector2 expected_scroll = {0};
    Clay_SetCurrentContext(NULL);
    if (!Pico_InitClay(viewport))
    {
        Clay_SetCurrentContext(previous);
        Fail("shell stability Clay initialization");
        return 1;
    }

    memset(&host, 0, sizeof(host));
    memset(&workspace, 0, sizeof(workspace));
    memset(&agent, 0, sizeof(agent));
    workspace.host = &host;
    workspace.id = 1;
    workspace.state = PICO_WORKSPACE_OPEN;
    workspace.agents[0] = &agent;
    workspace.count = 1;
    agent.id = 1;
    agent.workspace = &workspace;
    host.workspaces[0] = &workspace;
    host.workspace_count = 1;
    host.selected_agent_id = agent.id;
    if (with_sidebar)
    {
        ShellTestAddView(&host, PICO_SLOT_SIDEBAR, ShellTestSidebar, NULL);
    }
    ShellTestAddView(&host, PICO_SLOT_MAIN, ShellTestChat, NULL);
    ShellTestAddView(&host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    ShellTestAddView(&host, PICO_SLOT_FOOTER, ShellTestFooter, NULL);

    for (int frame = 0; frame < frames; frame++)
    {
        if (frame == 1)
        {
            state.composer_height = 56.003f;
        }
        if (frame == 60 || frame == 220) PicoChatFind_Open(&host);
        if (frame == 160 || frame == 320) PicoChatFind_Close(&host);
        if (frame == frames / 2)
        {
            Pico_RememberClayScroll();
            Pico_HandleClayErrors((Clay_ErrorData){
                .errorType = CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED,
                .errorText = CLAY_STRING("shell capacity recovery"),
            });
            if (!Pico_ReinitClay(NULL, false))
            {
                Fail("shell stability arena replacement");
                break;
            }
        }
        Clay_SetLayoutDimensions(viewport);
        Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0.0f);
        (void)PicoHost_LayoutShell(&host, viewport.height, 1.0f / 60.0f);
        if (Pico_RestoreClayScroll())
        {
            (void)PicoHost_LayoutShell(&host, viewport.height, 0.0f);
        }
        Clay_ScrollContainerData scroll =
            Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
        if (!scroll.found || !scroll.scrollPosition)
        {
            Fail("production shell did not create ChatScroll");
            break;
        }
        if (PicoScrollbar_PinToBottom(scroll.scrollContainerDimensions.height,
                                      scroll.contentDimensions.height,
                                      &scroll.scrollPosition->y))
        {
            (void)PicoHost_LayoutShell(&host, viewport.height, 0.0f);
            scroll = Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
            if (!scroll.found || !scroll.scrollPosition)
            {
                Fail("ChatScroll disappeared after bottom-follow relayout");
                break;
            }
        }
        float overflow = scroll.contentDimensions.height - scroll.scrollContainerDimensions.height;
        float bottom = overflow > 0.0f ? -overflow : 0.0f;
        if (overflow <= 0.0f ||
            (frame >= 30 && fabsf(scroll.scrollPosition->y - bottom) > 0.01f))
        {
            Fail("ChatScroll was not pinned to its overflowing content bottom");
            break;
        }

        Clay_ElementData find = Clay_GetElementData(CLAY_ID("ChatFind"));
        if (find.found != host.find.open || (find.found &&
            (find.boundingBox.x < 0 || find.boundingBox.y < 0 ||
             find.boundingBox.x + find.boundingBox.width > viewport.width)))
        {
            Fail("floating chat find must remain within the viewport without resizing panes");
            break;
        }
        Clay_ElementData root = Clay_GetElementData(CLAY_ID("Root"));
        Clay_ElementData body = Clay_GetElementData(CLAY_ID("Body"));
        Clay_ElementData right = Clay_GetElementData(CLAY_ID("RightColumn"));
        Clay_ElementData sidebar = Clay_GetElementData(CLAY_ID("Sidebar"));
        Clay_ElementData main = Clay_GetElementData(CLAY_ID("MainColumn"));
        Clay_ElementData chat = Clay_GetElementData(CLAY_ID("ChatScroll"));
        Clay_ElementData composer = Clay_GetElementData(CLAY_ID("ComposerAlign"));
        Clay_ElementData footer = Clay_GetElementData(CLAY_ID("Footer"));
        if (!root.found || !body.found || !right.found || !main.found || !chat.found ||
            !composer.found || !footer.found || (with_sidebar && !sidebar.found))
        {
            Fail("production shell panes were not laid out");
            break;
        }
        if (fabsf(root.boundingBox.y) > 0.001f ||
            fabsf(root.boundingBox.height - viewport.height) > 0.001f ||
            !ShellVerticallyContains(root.boundingBox, body.boundingBox) ||
            !ShellVerticallyContains(body.boundingBox, right.boundingBox) ||
            (with_sidebar && !ShellVerticallyContains(body.boundingBox, sidebar.boundingBox)))
        {
            Fail(with_sidebar ? "oversized content escaped viewport panes with sidebar"
                              : "oversized content escaped viewport panes without sidebar");
            break;
        }
        if (frame == 30)
        {
            expected_root = root.boundingBox;
            expected_body = body.boundingBox;
            expected_right = right.boundingBox;
            expected_sidebar = sidebar.boundingBox;
            expected_main = main.boundingBox;
            expected_chat = chat.boundingBox;
            expected_composer = composer.boundingBox;
            expected_footer = footer.boundingBox;
            expected_scroll = *scroll.scrollPosition;
        }
        else if (frame > 30 &&
                 (!ShellBoxStable(expected_root, root.boundingBox) ||
                  !ShellBoxStable(expected_body, body.boundingBox) ||
                  !ShellBoxStable(expected_right, right.boundingBox) ||
                  (with_sidebar && !ShellBoxStable(expected_sidebar, sidebar.boundingBox)) ||
                  !ShellBoxStable(expected_main, main.boundingBox) ||
                  !ShellBoxStable(expected_chat, chat.boundingBox) ||
                  !ShellBoxStable(expected_composer, composer.boundingBox) ||
                  !ShellBoxStable(expected_footer, footer.boundingBox) ||
                  fabsf(expected_scroll.y - scroll.scrollPosition->y) > 0.001f))
        {
            Fail(with_sidebar ? "bottom-follow shell geometry drifted with sidebar"
                              : "bottom-follow shell geometry drifted without sidebar");
            break;
        }
    }

    PicoChatFind_Reset(&host);
    Pico_FreeClay();
    Clay_SetCurrentContext(previous);
    return g_failed ? 1 : 0;
}

static int RunWorkspaceLessShellCase(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-empty-layout-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-empty-layout-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoWorkspace *workspace;
    ShellTestState state = {.composer_height = 44.0f};
    Clay_BoundingBox expected_root = {0};
    Clay_BoundingBox expected_body = {0};
    Clay_BoundingBox expected_sidebar = {0};
    Clay_BoundingBox expected_right = {0};
    Clay_BoundingBox expected_main = {0};
    Clay_BoundingBox expected_chat = {0};
    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("workspace-less shell setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        Fail("workspace-less shell host initialization");
        return 1;
    }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        pico_host_free(host);
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        Clay_SetCurrentContext(previous);
        Fail("workspace-less shell Clay initialization");
        return 1;
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    host->view_count[PICO_SLOT_SIDEBAR] = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    host->view_count[PICO_SLOT_FOOTER] = 0;
    ShellTestAddView(host, PICO_SLOT_SIDEBAR, ShellTestSidebar, NULL);
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    ShellTestAddView(host, PICO_SLOT_FOOTER, ShellTestFooter, NULL);

    for (int frame = 0; frame < 120; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        Clay_SetPointerState((Clay_Vector2){550, 400}, false);
        (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        Clay_ElementData root = Clay_GetElementData(CLAY_ID("Root"));
        Clay_ElementData body = Clay_GetElementData(CLAY_ID("Body"));
        Clay_ElementData sidebar = Clay_GetElementData(CLAY_ID("Sidebar"));
        Clay_ElementData right = Clay_GetElementData(CLAY_ID("RightColumn"));
        Clay_ElementData main = Clay_GetElementData(CLAY_ID("MainColumn"));
        Clay_ElementData chat = Clay_GetElementData(CLAY_ID("ChatScroll"));
        if (!root.found || !body.found || !sidebar.found || !right.found || !main.found || !chat.found ||
            !Clay_GetElementData(CLAY_ID("NoWorkspaceCard")).found ||
            !Clay_GetElementData(CLAY_IDI("EmptyCard", 0)).found ||
            !Clay_GetElementData(CLAY_IDI("EmptyCard", 1)).found ||
            !Clay_GetElementData(CLAY_IDI("EmptyCard", 2)).found)
        {
            Fail("workspace-less shell must render sidebar and landing cards");
            break;
        }
        if (Clay_GetElementData(CLAY_ID("ComposerAlign")).found ||
            Clay_GetElementData(CLAY_ID("Footer")).found || host->hovered_clickable ||
            pico_ui_modal_count(host) != 0)
        {
            Fail("workspace-less landing card must be non-actionable and hide agent slots");
            break;
        }
        if (frame == 0)
        {
            expected_root = root.boundingBox;
            expected_body = body.boundingBox;
            expected_sidebar = sidebar.boundingBox;
            expected_right = right.boundingBox;
            expected_main = main.boundingBox;
            expected_chat = chat.boundingBox;
        }
        else if (!ShellBoxStable(expected_root, root.boundingBox) ||
                 !ShellBoxStable(expected_body, body.boundingBox) ||
                 !ShellBoxStable(expected_sidebar, sidebar.boundingBox) ||
                 !ShellBoxStable(expected_right, right.boundingBox) ||
                 !ShellBoxStable(expected_main, main.boundingBox) ||
                 !ShellBoxStable(expected_chat, chat.boundingBox))
        {
            Fail("workspace-less shell geometry drifted");
            break;
        }
    }

    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK ||
        !(workspace = PicoHost_FindWorkspace(host, workspace_id)) ||
        workspace->view_count[PICO_SLOT_MAIN] >= PICO_MAX_SLOT_VIEWS)
    {
        Fail("open workspace without an agent for shell test");
    }
    else
    {
        int index = workspace->view_count[PICO_SLOT_MAIN]++;
        workspace->views[PICO_SLOT_MAIN][index].workspace_render = ShellTestWorkspaceView;
        workspace->views[PICO_SLOT_MAIN][index].workspace = workspace;
        g_shell_workspace_view_calls = 0;
        Clay_SetLayoutDimensions(viewport);
        (void)PicoHost_LayoutShell(host, viewport.height, 0.0f);
        if (Clay_GetElementData(CLAY_ID("NoWorkspaceCard")).found)
        {
            Fail("open workspace without a selected agent must not show open-workspace instruction");
        }
        if (g_shell_workspace_view_calls != 0)
        {
            Fail("workspace views must not render without a selected agent");
        }
    }

    Clay_SetCurrentContext(previous);
    pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(dir);
    return g_failed ? 1 : 0;
}

/* Finds a rendered text run by exact contents. The landing page renders no
 * other text with these labels. */
static Clay_RenderCommand *FindCardText(Clay_RenderCommandArray *commands, const char *text)
{
    size_t length = strlen(text);
    for (int i = 0; i < commands->length; i++)
    {
        Clay_RenderCommand *command = Clay_RenderCommandArray_Get(commands, i);
        if (!command || command->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT)
        {
            continue;
        }
        Clay_StringSlice contents = command->renderData.text.stringContents;
        if (contents.length == (int32_t)length && memcmp(contents.chars, text, length) == 0)
        {
            return command;
        }
    }
    return NULL;
}

/* Finds a rendered text run made of a strict prefix of `label` plus an
 * ellipsis, i.e. a label trimmed to its column. */
static Clay_RenderCommand *FindTrimmedCardText(Clay_RenderCommandArray *commands,
                                               const char *label)
{
    static const char ellipsis[] = "\xE2\x80\xA6";
    size_t label_length = strlen(label);
    for (int i = 0; i < commands->length; i++)
    {
        Clay_RenderCommand *command = Clay_RenderCommandArray_Get(commands, i);
        if (!command || command->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT)
        {
            continue;
        }
        Clay_StringSlice contents = command->renderData.text.stringContents;
        int prefix = contents.length - 3;
        if (prefix > 0 && (size_t)prefix < label_length &&
            memcmp(contents.chars + prefix, ellipsis, 3) == 0 &&
            memcmp(contents.chars, label, (size_t)prefix) == 0)
        {
            return command;
        }
    }
    return NULL;
}

/* Find the active clipping region containing a rendered label. */
static Clay_BoundingBox CardTextClip(Clay_RenderCommandArray *commands, Clay_RenderCommand *text)
{
    int closed = 0;
    bool found_text = false;
    for (int i = commands->length - 1; i >= 0; i--)
    {
        Clay_RenderCommand *command = Clay_RenderCommandArray_Get(commands, i);
        if (!found_text)
        {
            found_text = command == text;
            continue;
        }
        if (command->commandType == CLAY_RENDER_COMMAND_TYPE_SCISSOR_END)
        {
            closed++;
        }
        else if (command->commandType == CLAY_RENDER_COMMAND_TYPE_SCISSOR_START)
        {
            if (closed == 0)
            {
                return command->boundingBox;
            }
            closed--;
        }
    }
    return (Clay_BoundingBox){0};
}

static int AssertDesktopEmptyCardTrim(Clay_RenderCommandArray *commands, const char *long_name)
{
    Clay_ElementData card0 = Clay_GetElementData(CLAY_IDI("EmptyCard", 0));
    Clay_ElementData card1 = Clay_GetElementData(CLAY_IDI("EmptyCard", 1));
    Clay_ElementData card2 = Clay_GetElementData(CLAY_IDI("EmptyCard", 2));
    if (!card0.found || !card1.found || !card2.found)
    {
        Fail("landing cards were not laid out");
        return 1;
    }
    if (fabsf(card0.boundingBox.y - card1.boundingBox.y) > 0.5f ||
        fabsf(card1.boundingBox.y - card2.boundingBox.y) > 0.5f ||
        card1.boundingBox.x + 0.5f < card0.boundingBox.x + card0.boundingBox.width ||
        card2.boundingBox.x + 0.5f < card1.boundingBox.x + card1.boundingBox.width)
    {
        Fail("landing cards were not laid out in a desktop row");
        return 1;
    }

    Clay_RenderCommand *alpha = FindCardText(commands, "alpha");
    Clay_RenderCommand *beta = FindCardText(commands, "beta");
    Clay_RenderCommand *delta = FindCardText(commands, "delta");
    Clay_RenderCommand *trimmed = FindTrimmedCardText(commands, long_name);
    if (!alpha || !beta || !delta)
    {
        Fail("landing card items were not rendered");
        return 1;
    }
    Clay_BoundingBox left = CardTextClip(commands, alpha);
    Clay_BoundingBox right = CardTextClip(commands, delta);
    if (left.width <= 0.0f || right.width <= 0.0f ||
        fabsf(left.width - right.width) > 0.5f ||
        right.x < left.x + left.width || left.x < card0.boundingBox.x ||
        right.x + right.width > card0.boundingBox.x + card0.boundingBox.width + 0.5f)
    {
        Fail("landing card columns must be equal, non-overlapping, and inside the card");
        return 1;
    }
    /* Column-major: the first three items stack in the left column, the
     * rest continue at the top of the right column. */
    if (fabsf(alpha->boundingBox.x - left.x) > 0.01f ||
        fabsf(beta->boundingBox.x - left.x) > 0.01f ||
        beta->boundingBox.y <= alpha->boundingBox.y ||
        fabsf(delta->boundingBox.x - right.x) > 0.01f ||
        fabsf(delta->boundingBox.y - alpha->boundingBox.y) > 0.01f)
    {
        Fail("landing card items were not split into two equal columns");
        return 1;
    }
    if (FindCardText(commands, long_name))
    {
        Fail("overwide landing card label was not trimmed");
        return 1;
    }
    if (!trimmed || fabsf(trimmed->boundingBox.x - right.x) > 0.01f ||
        trimmed->boundingBox.x + trimmed->boundingBox.width > right.x + right.width + 0.5f)
    {
        Fail("trimmed landing card label escaped its column");
        return 1;
    }
    return 0;
}

static int TestEmptyCardsTwoColumnTrim(void)
{
    Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-card-columns-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-card-columns-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoWorkspace *workspace;
    ShellTestState state = {.composer_height = 44.0f};
    static const char long_name[] =
        "tool_with_a_very_long_name_that_cannot_possibly_fit_inside_a_narrow_card_column";
    static const char *names[] = {"alpha", "beta", "gamma", "delta", long_name};
    int rc = 1;

    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("empty card column test setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        rmdir(cfg);
        Fail("empty card column host initialization");
        return 1;
    }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_SIDEBAR] = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    host->view_count[PICO_SLOT_FOOTER] = 0;
    ShellTestAddView(host, PICO_SLOT_SIDEBAR, ShellTestSidebar, NULL);
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    ShellTestAddView(host, PICO_SLOT_FOOTER, ShellTestFooter, NULL);
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK ||
        !(workspace = PicoHost_FindWorkspace(host, workspace_id)))
    {
        pico_host_free(host);
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        rmdir(cfg);
        Fail("empty card column open workspace");
        return 1;
    }
    /* Deterministic card content: four short labels and one label that is far
     * wider than a column. */
    for (int i = 0; i < 5; i++)
    {
        workspace->tools[i].name = names[i];
    }
    workspace->tool_count = 5;

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        pico_host_free(host);
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        rmdir(cfg);
        Clay_SetCurrentContext(previous);
        Fail("empty card column Clay initialization");
        return 1;
    }
    /* Measure with the production font metrics so layout and label trimming
     * agree on widths. */
    Clay_SetMeasureTextFunction(Pico_MeasureTextUtf8, NULL);
    Clay_SetPointerState((Clay_Vector2){0, 0}, false);

    Clay_SetLayoutDimensions(viewport);
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
    if (AssertDesktopEmptyCardTrim(&commands, long_name) != 0)
    {
        goto done;
    }

    /* First frame after a desktop resize must trim to the new column, not the
     * previous viewport's width. */
    viewport.width = 900;
    Clay_SetLayoutDimensions(viewport);
    commands = PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
    if (AssertDesktopEmptyCardTrim(&commands, long_name) != 0)
    {
        goto done;
    }
    rc = g_failed ? 1 : 0;

done:
    Clay_SetCurrentContext(previous);
    pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(dir);
    rmdir(cfg);
    return rc;
}

static int TestEmptyCardsOverflowScroll(void)
{
    Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-card-overflow-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-card-overflow-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoWorkspace *workspace;
    ShellTestState state = {.composer_height = 44.0f};
    static char names[40][16];
    int rc = 1;

    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("empty card overflow test setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        rmdir(cfg);
        Fail("empty card overflow host initialization");
        return 1;
    }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_SIDEBAR] = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    host->view_count[PICO_SLOT_FOOTER] = 0;
    ShellTestAddView(host, PICO_SLOT_SIDEBAR, ShellTestSidebar, NULL);
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    ShellTestAddView(host, PICO_SLOT_FOOTER, ShellTestFooter, NULL);
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK ||
        !(workspace = PicoHost_FindWorkspace(host, workspace_id)))
    {
        pico_host_free(host);
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        rmdir(cfg);
        Fail("empty card overflow open workspace");
        return 1;
    }
    for (int i = 0; i < 40; i++)
    {
        snprintf(names[i], sizeof(names[i]), "tool_%02d", i);
        workspace->tools[i].name = names[i];
    }
    workspace->tool_count = 40;

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        pico_host_free(host);
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        rmdir(cfg);
        Clay_SetCurrentContext(previous);
        Fail("empty card overflow Clay initialization");
        return 1;
    }
    Clay_SetMeasureTextFunction(Pico_MeasureTextUtf8, NULL);
    Clay_SetPointerState((Clay_Vector2){0, 0}, false);

    Clay_SetLayoutDimensions(viewport);
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
    commands = PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);

    Clay_ElementData card0 = Clay_GetElementData(CLAY_IDI("EmptyCard", 0));
    Clay_ElementData card1 = Clay_GetElementData(CLAY_IDI("EmptyCard", 1));
    Clay_ElementData card2 = Clay_GetElementData(CLAY_IDI("EmptyCard", 2));
    Clay_ScrollContainerData tools_scroll =
        Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("EmptyCardScroll0")));
    Clay_ScrollContainerData context_scroll =
        Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("EmptyCardScroll1")));
    if (!card0.found || !card1.found || !card2.found)
    {
        Fail("overflow landing cards were not laid out");
        goto done;
    }
    if (fabsf(card0.boundingBox.height - card1.boundingBox.height) > 0.5f ||
        fabsf(card1.boundingBox.height - card2.boundingBox.height) > 0.5f)
    {
        Fail("landing cards must keep equal height under the cap");
        goto done;
    }
    if (!tools_scroll.found || !tools_scroll.scrollPosition ||
        tools_scroll.contentDimensions.height <= tools_scroll.scrollContainerDimensions.height + 0.5f)
    {
        Fail("overfull landing card items must scroll inside the card");
        goto done;
    }
    if (!context_scroll.found ||
        context_scroll.contentDimensions.height > context_scroll.scrollContainerDimensions.height + 0.5f)
    {
        Fail("short landing card must not scroll");
        goto done;
    }
    if (!Clay_GetElementData(CLAY_ID("EmptyCardScrollTrack0")).found)
    {
        Fail("overfull landing card must show a scrollbar");
        goto done;
    }
    if (Clay_GetElementData(CLAY_ID("EmptyCardScrollTrack1")).found)
    {
        Fail("short landing card must not show a scrollbar");
        goto done;
    }

    Clay_RenderCommand *title = FindCardText(&commands, "Tools");
    Clay_RenderCommand *item = FindCardText(&commands, "tool_00");
    if (!title || !item)
    {
        Fail("overflow landing card title or item was not rendered");
        goto done;
    }
    float title_y = title->boundingBox.y;
    float item_y = item->boundingBox.y;
    tools_scroll.scrollPosition->y = -40.0f;
    commands = PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
    title = FindCardText(&commands, "Tools");
    item = FindCardText(&commands, "tool_00");
    if (!title || !item)
    {
        Fail("scrolled landing card title or item was not rendered");
        goto done;
    }
    if (fabsf(title->boundingBox.y - title_y) > 0.5f)
    {
        Fail("landing card title must stay pinned while items scroll");
        goto done;
    }
    if (item->boundingBox.y >= item_y - 0.5f)
    {
        Fail("landing card items must move when the card scrolls");
        goto done;
    }
    rc = g_failed ? 1 : 0;

done:
    Clay_SetCurrentContext(previous);
    pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(dir);
    rmdir(cfg);
    return rc;
}


/* Persistence assertions depend on completion, not storage latency. Leave
 * hang detection to CTest's overall timeout; production UI/shutdown callers
 * still use their own bounded deadlines. A NULL deadline waits on persist_cv. */
static bool DrainSessionForAssertion(PicoHost *host, PicoAgent *agent)
{
    if (!host || !agent)
    {
        return false;
    }
    PicoTest_Wait(__func__, "session writes completed");
    return PicoSession_DrainPersistBefore(host, agent, NULL);
}

static int TestFastSelectionPersistence(void)
{
    char dir[] = "/tmp/pico-fast-session-XXXXXX";
    char cfg[] = "/tmp/pico-fast-session-cfg-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId ws_id = 0;
    PicoAgentId id = 0;
    int rc = 1;
    const char *phase = "Fast persistence host initialization";
    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("Fast persistence fixture");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
        goto done;
    WaitPluginLoad(host);
    phase = "Fast persistence workspace open";
    if (pico_workspace_open(host, dir, &ws_id) != PICO_OK)
        goto done;
    pico_auth_set_env_key(host, "openai", "test-api-key");
    pico_auth_set_active(host, "openai", PICO_AUTH_API_KEY);
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, ws_id);
    phase = "Fast persistence model fixture allocation";
    PicoModel *models = realloc(ws->models, 2 * sizeof(PicoModel));
    if (!models)
        goto done;
    ws->models = models;
    ws->model_count = 2;
    models[0].supports_fast = true;
    snprintf(models[0].provider, sizeof(models[0].provider), "openai");
    models[0].base_url[0] = '\0';
    models[1] = models[0];
    snprintf(models[1].id, sizeof(models[1].id), "second-fast-model");
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NEW};
    phase = "Fast persistence writer creation";
    if (pico_main_agent_create(host, ws_id, &options, &id) != PICO_OK)
        goto done;
    PicoAgent *writer = PicoHost_FindAgent(host, id);
    phase = "Fast persistence enabling Fast";
    if (!PicoSettings_SetFast(writer, true)) goto done;
    phase = "Fast persistence queueing seed message";
    if (PicoSession_LogUser(host, writer, "seed", "seed", NULL) != PICO_SESSION_WRITE_OK) goto done;
    phase = "Fast persistence queueing usage";
    if (PicoSession_LogUsage(host, writer, 10, 0, true, "default") != PICO_SESSION_WRITE_OK) goto done;
    phase = "Fast persistence draining initial settings and usage";
    if (!DrainSessionForAssertion(host, writer)) goto done;
    phase = "Fast persistence resumed agent creation";
    options.session_start = PICO_SESSION_NONE;
    if (pico_main_agent_create(host, ws_id, &options, &id) != PICO_OK)
        goto done;
    PicoAgent *resumed = PicoHost_FindAgent(host, id);
    if (PicoSession_Replay(host, resumed, writer->session_path, false) != 0 || !resumed->fast ||
        strcmp(resumed->last_service_tier, "default") != 0)
    {
        Fail("resume must distinguish selected Fast from actually served standard tier");
        goto done;
    }
    PicoSession_Reset(host, resumed);
    if (resumed->fast || resumed->last_service_tier[0])
    {
        Fail("new conversation must reset Fast and last served tier");
        goto done;
    }
    if (!PicoSettings_SetModel(writer, models[1].id) || !writer->fast)
    {
        Fail("switching to another supported model must retain Fast");
        goto done;
    }
    models[1].supports_fast = false;
    PicoSettings_ReconcileIdleAgent(writer);
    if (writer->fast)
    {
        Fail("removing model capability must clear Fast");
        goto done;
    }
    phase = "Fast persistence draining capability-removal setting";
    if (!DrainSessionForAssertion(host, writer))
        goto done;
    /* Restoring capability must not resurrect the old fast:true session event. */
    models[1].supports_fast = true;
    if (PicoSession_Replay(host, resumed, writer->session_path, false) != 0 || resumed->fast)
    {
        Fail("capability-removal Fast clear must survive session resume");
        goto done;
    }
    phase = "Fast persistence re-enabling Fast";
    if (!PicoSettings_SetFast(writer, true))
        goto done;
    models[0].supports_fast = false;
    if (!PicoSettings_SetModel(writer, models[0].id) || writer->fast)
    {
        Fail("switching to an unsupported model must clear Fast");
        goto done;
    }
    /* These final settings changes queue session writes. Let the test's
     * persistence work finish before the host's bounded shutdown deadline. */
    if (!DrainSessionForAssertion(host, writer) || !DrainSessionForAssertion(host, resumed))
    {
        Fail("Fast persistence final writes did not drain");
        goto done;
    }
    rc = 0;
done:
    if (rc && !g_failed) Fail(phase);
    if (host && pico_host_free(host) != PICO_HOST_SHUTDOWN_CLEAN)
    {
        Fail("Fast persistence host shutdown retained pending work");
        rc = 1;
    }
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    return rc;
}

static int RunFastFooterCase(bool with_sidebar, bool cold_history)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-fast-footer-XXXXXX";
    char cfg[] = "/tmp/pico-fast-footer-cfg-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    ShellTestState state = {.composer_height = 44.0f};
    int rc = 1;
    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("Fast footer setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("Fast footer host init");
        goto done;
    }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_SIDEBAR] = 0;
    if (with_sidebar)
        ShellTestAddView(host, PICO_SLOT_SIDEBAR, ShellTestSidebar, NULL);
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK)
    {
        Fail("Fast footer workspace");
        goto done;
    }
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .select = true,
                                     .session_start = PICO_SESSION_NONE};
    if (pico_main_agent_create(host, workspace_id, &options, &agent_id) != PICO_OK)
    {
        Fail("Fast footer agent");
        goto done;
    }
    PicoAgent *agent = PicoHost_FindAgent(host, agent_id);
    for (int i = 0; i < (cold_history ? 320 : 40); i++)
        PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "Fast footer bottom-follow conversation content");
    PicoModel *model = PicoSettings_ActiveModel(agent);
    if (!model)
    {
        Fail("Fast footer model");
        goto done;
    }
    pico_auth_set_env_key(host, "openai", "test-api-key");
    pico_auth_set_active(host, "openai", PICO_AUTH_API_KEY);
    snprintf(model->provider, sizeof(model->provider), "openai");
    model->base_url[0] = '\0';
    model->supports_fast = true;
    model->effort_count = 1;
    snprintf(model->effort[0], sizeof(model->effort[0]), "high");
    PicoSettings_SyncAgent(agent);
    if (agent->fast || !PicoSettings_FastAvailable(agent))
    {
        Fail("Fast defaults off and requires explicit capability");
        goto done;
    }
    const PicoCommand *fast_command = NULL;
    PicoWorkspace *ws = agent->workspace;
    for (int i = 0; i < ws->command_count; i++)
        if (strcmp(ws->commands[i].name, "fast") == 0)
            fast_command = &ws->commands[i];
    if (!fast_command)
    {
        Fail("/fast was not registered");
        goto done;
    }
    fast_command->workspace_run(ws, agent_id, "on", fast_command->state);
    if (!agent->fast || strcmp(agent->effort, "high") != 0)
    {
        Fail("/fast must enable Fast without changing effort");
        goto done;
    }
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        Fail("Fast footer Clay initialization");
        goto done;
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);
    agent->has_tokens_per_second = true;
    agent->tokens_per_second = 42.0;
    const char *panes[] = {"Root", "Body", "RightColumn", "MainColumn", "ChatScroll",
                           "ComposerAlign", "Footer", "FooterEffort", "FooterFastIcon", "FooterTps", "Sidebar"};
    Clay_BoundingBox expected[11] = {0};
    int pane_count = with_sidebar ? 11 : 10;
    for (int frame = 0; frame < 120; frame++)
    {
        if (frame == 40) agent->tokens_per_second = 7.0;
        if (frame == 80) agent->tokens_per_second = 12345.0;
        Clay_SetLayoutDimensions(viewport);
        Clay_SetPointerState((Clay_Vector2){0, 0}, false);
        Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0.0f);
        (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
        if (!scroll.found || !scroll.scrollPosition ||
            scroll.contentDimensions.height <= scroll.scrollContainerDimensions.height)
        {
            Fail("Fast footer test must exercise overflowing bottom-follow chat");
            goto done;
        }
        PicoScrollbar_PinToBottom(scroll.scrollContainerDimensions.height,
                                   scroll.contentDimensions.height, &scroll.scrollPosition->y);
        (void)PicoHost_LayoutShell(host, viewport.height, 0.0f);
        PicoChat_HarvestVirtualHeights(host);
        for (int i = 0; i < pane_count; i++)
        {
            Clay_String name = {.chars = panes[i], .length = (int32_t)strlen(panes[i])};
            Clay_ElementData box = Clay_GetElementData(Clay_GetElementId(name));
            bool tps_text_changed = strcmp(panes[i], "FooterTps") == 0 && (frame == 40 || frame == 80);
            if (!box.found || (frame > (cold_history ? 30 : 2) && !tps_text_changed &&
                               !ShellBoxStable(expected[i], box.boundingBox)))
            {
                Fail("batched transcript measurement changed bottom-follow shell bounds");
                goto done;
            }
            expected[i] = box.boundingBox;
        }
    }
#ifdef PICO_CLAY_FRAME_FAULT_TESTS
    g_find_input_test = true;
    /* Open the production effort menu and click its Fast row. */
    Clay_ElementData chip = Clay_GetElementData(CLAY_ID("FooterEffort"));
    g_find_pointer = (Vector2){chip.boundingBox.x + chip.boundingBox.width / 2,
                               chip.boundingBox.y + chip.boundingBox.height / 2};
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
    g_find_press = true;
    pico_run_hooks(host, PICO_HOOK_AFTER_LAYOUT, 0);
    g_find_press = false;
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, viewport.height, 0.0f);
    Clay_ElementData row = Clay_GetElementData(CLAY_IDI("FooterMenuItem", model->effort_count));
    if (!row.found || !FindCardText(&commands, "Fast mode") || !FindCardText(&commands, "On"))
    {
        Fail("reasoning dropdown must include the Fast toggle state");
        goto done;
    }
    g_find_pointer = (Vector2){row.boundingBox.x + row.boundingBox.width / 2,
                               row.boundingBox.y + row.boundingBox.height / 2};
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
    g_find_press = true;
    pico_run_hooks(host, PICO_HOOK_AFTER_LAYOUT, 0);
    g_find_press = false;
    (void)PicoHost_LayoutShell(host, viewport.height, 0.0f);
    if (agent->fast || Clay_GetElementData(CLAY_ID("FooterFastIcon")).found ||
        strcmp(agent->effort, "high") != 0)
    {
        Fail("Fast dropdown toggle must turn off the icon without changing effort");
        goto done;
    }
    g_find_input_test = false;
#endif
    model->effort_count = 0;
    PicoSettings_SyncAgent(agent);
    fast_command->workspace_run(ws, agent_id, "on", fast_command->state);
    (void)PicoHost_LayoutShell(host, viewport.height, 0.0f);
    if (!agent->fast || Clay_GetElementData(CLAY_ID("FooterEffort")).found)
    {
        Fail("models without effort use /fast without introducing a dropdown");
        goto done;
    }
    model->supports_fast = false;
    fast_command->workspace_run(ws, agent_id, "off", fast_command->state);
    fast_command->workspace_run(ws, agent_id, "on", fast_command->state);
    if (agent->fast)
    {
        Fail("/fast on must reject models without explicit capability");
        goto done;
    }
    model->supports_fast = true;
    snprintf(model->provider, sizeof(model->provider), "xai");
    pico_auth_set_env_key(host, "xai", NULL);
    pico_auth_set_active(host, "xai", PICO_AUTH_API_KEY);
    if (PicoSettings_FastAvailable(agent))
    {
        Fail("Fast requires credentials for the selected route");
        goto done;
    }
    pico_auth_set_env_key(host, "xai", "test-api-key");
    bool xai_key = PicoSettings_FastAvailable(agent);
    pico_auth_set_env_key(host, "xai", NULL);
    /* A refreshable OAuth session must enable Fast without an API key, even
     * when the provider needs to refresh before sending the priority request. */
    pico_auth_set_oauth(host, "xai", "", "test-refresh", NULL, 0);
    pico_auth_set_active(host, "xai", PICO_AUTH_OAUTH);
    if (!xai_key || !PicoSettings_FastAvailable(agent) || !PicoSettings_SetFast(agent, true) ||
        !agent->fast)
    {
        Fail("xAI API-key and refreshable OAuth routes must both permit Fast");
        goto done;
    }
    rc = 0;
done:
#ifdef PICO_CLAY_FRAME_FAULT_TESTS
    g_find_input_test = false;
    g_find_press = false;
#endif
    Clay_SetCurrentContext(previous);
    if (host) pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    return rc;
}

static int RunQuestionPanelShellCase(bool with_sidebar);

static bool NoticeTextHasColor(Clay_RenderCommandArray *commands, const char *text,
                               PicoNoticeSeverity severity)
{
    Clay_RenderCommand *command = FindCardText(commands, text);
    if (!command) return false;
    Clay_Color color = command->renderData.text.textColor;
    if (severity == PICO_NOTICE_ERROR) return color.r > color.g && color.r > color.b;
    if (severity == PICO_NOTICE_WARNING) return color.r > color.b && color.g > color.b;
    return true;
}

static int RunNoticeTranscriptCase(bool with_sidebar)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-notice-layout-XXXXXX";
    char cfg[] = "/tmp/pico-notice-cfg-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    ShellTestState state = {.composer_height = 44.0f};
    ChatStabilitySnapshot expected = {0};
    int rc = 1;
    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("notice layout setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host) goto done;
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_SIDEBAR] = 0;
    if (with_sidebar) ShellTestAddView(host, PICO_SLOT_SIDEBAR, ShellTestSidebar, NULL);
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .select = true,
                                     .session_start = PICO_SESSION_NONE};
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK ||
        pico_main_agent_create(host, workspace_id, &options, &agent_id) != PICO_OK) goto done;
    PicoAgent *agent = PicoHost_FindAgent(host, agent_id);
    for (int i = 0; i < 40; i++)
        PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "notice layout history");
    PicoHost_AddNotice(host, agent_id, PICO_NOTICE_ERROR,
                       "error-prose [error-url](https://chatgpt.com/backend-api/codex/responses)\n\n```\nerror-code\n```");
    PicoHost_AddNotice(host, agent_id, PICO_NOTICE_WARNING, "warning-prose");
    PicoHost_AddNotice(host, agent_id, PICO_NOTICE_INFO, "info-prose");
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT,
                         "HTTP 504 from [normal-url](https://chatgpt.com/backend-api/codex/responses)");
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    int live_index = agent->message_count - 1;
    PicoHost_AddNotice(host, agent_id, PICO_NOTICE_INFO, "trailing-notice");
    agent->state = PICO_AGENT_LLM_WAIT;
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0})) goto done;
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);
    host->chat_follow_bottom = true;
    PicoChat_ResetBottomSpace(host);
    for (int frame = 0; frame < 120; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0.0f);
        Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
        if (!scroll.found || !scroll.scrollPosition ||
            scroll.contentDimensions.height <= scroll.scrollContainerDimensions.height)
        {
            Fail("notice test must exercise overflowing bottom-follow chat");
            goto done;
        }
        PicoScrollbar_PinToBottom(scroll.scrollContainerDimensions.height,
                                   scroll.contentDimensions.height, &scroll.scrollPosition->y);
        commands = PicoHost_LayoutShell(host, viewport.height, 0.0f);
        PicoChat_HarvestVirtualHeights(host);
        ChatStabilitySnapshot current;
        if (!CaptureChatStabilitySnapshot(with_sidebar, &current)) goto done;
        if (frame == 30) expected = current;
        if (frame > 30 && !ChatStabilitySnapshotStable(&expected, &current, with_sidebar))
        {
            Fail("notice blocks changed stable bottom-follow geometry");
            goto done;
        }
        if (frame == 119)
        {
            Clay_RenderCommand *ordinary = FindCardText(&commands, "normal-url");
            if (!NoticeTextHasColor(&commands, "Error", PICO_NOTICE_ERROR) ||
                !NoticeTextHasColor(&commands, "error-prose", PICO_NOTICE_ERROR) ||
                !NoticeTextHasColor(&commands, "error-url", PICO_NOTICE_ERROR) ||
                !NoticeTextHasColor(&commands, "error-code", PICO_NOTICE_ERROR) ||
                !NoticeTextHasColor(&commands, "Warning", PICO_NOTICE_WARNING) ||
                !NoticeTextHasColor(&commands, "warning-prose", PICO_NOTICE_WARNING) ||
                !FindCardText(&commands, "Info") || !ordinary ||
                ordinary->renderData.text.textColor.b <= ordinary->renderData.text.textColor.r ||
                !Clay_GetElementData(MainTraceRowId(live_index, 0, "ThinkSynthRow")).found)
            {
                Fail("notice labels/body/link severity or ordinary chat/live assistant styling is incorrect");
                goto done;
            }
        }
    }
    rc = 0;
done:
    Clay_SetCurrentContext(previous);
    if (host) pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    if (rc && !g_failed) Fail("notice transcript layout");
    return rc;
}

static int TestBottomFollowShellGeometryStable(void)
{
    if (RunShellStabilityCase(false) != 0)
    {
        return 1;
    }
    if (RunShellStabilityCase(true) != 0)
    {
        return 1;
    }
    if (RunChatRetainedSpacerCase(false) != 0)
    {
        return 1;
    }
    if (RunChatRetainedSpacerCase(true) != 0)
    {
        return 1;
    }
    if (RunQuestionPanelShellCase(false) != 0 || RunQuestionPanelShellCase(true) != 0)
        return 1;
    if (RunFastFooterCase(false, false) != 0 || RunFastFooterCase(true, false) != 0 ||
        RunFastFooterCase(false, true) != 0 || RunFastFooterCase(true, true) != 0)
        return 1;
    if (RunNoticeTranscriptCase(false) != 0 || RunNoticeTranscriptCase(true) != 0) return 1;
    return RunWorkspaceLessShellCase();
}

static int TestChatBottomFollowClearsComposer(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-chat-spacer-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-chat-spacer-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions opt;
    PicoAgent *agent;
    ShellTestState state = {.composer_height = 44.0f};
    Clay_Arena arena;
    int last_index;
    int frame;

    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("chat bottom spacer setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(cfg);
        rmdir(dir);
        Fail("chat bottom spacer host init");
        return 1;
    }
    WaitPluginLoad(host);
    /* Headless runs have no real fonts; keep chat on the unclamped width path
     * and stub the composer, which measures text with raylib directly. */
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK)
    {
        Fail("chat bottom spacer open workspace");
        pico_host_free(host);
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(cfg);
        rmdir(dir);
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id)))
    {
        Fail("chat bottom spacer create agent");
        pico_host_free(host);
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(cfg);
        rmdir(dir);
        return 1;
    }
    for (int i = 0; i < 320; i++)
    {
        PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "chat bottom spacer clearance test message");
    }
    last_index = agent->message_count - 1;

    arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        pico_host_free(host);
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(cfg);
        rmdir(dir);
        Clay_SetCurrentContext(previous);
        Fail("chat bottom spacer Clay initialization");
        return 1;
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);

    for (frame = 0; frame < 30; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0.0f);
        (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        Clay_ScrollContainerData scroll =
            Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
        if (!scroll.found || !scroll.scrollPosition)
        {
            Fail("chat bottom spacer missing ChatScroll");
            break;
        }
        if (PicoScrollbar_PinToBottom(scroll.scrollContainerDimensions.height,
                                      scroll.contentDimensions.height, &scroll.scrollPosition->y))
        {
            (void)PicoHost_LayoutShell(host, viewport.height, 0.0f);
        }
        PicoChat_HarvestVirtualHeights(host);
    }

    {
        Clay_ScrollContainerData scroll =
            Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
        Clay_ElementData last = Clay_GetElementData(CLAY_IDI("MsgMain", last_index));
        Clay_ElementData composer = Clay_GetElementData(CLAY_ID("ComposerAlign"));
        Clay_ElementData spacer = Clay_GetElementData(CLAY_ID("ChatBottomSpacer"));
        if (!scroll.found || !scroll.scrollPosition || !last.found || !composer.found || !spacer.found)
        {
            Fail("chat bottom spacer could not find transcript, composer, spacer, or scroll state");
        }
        else
        {
            float overflow = scroll.contentDimensions.height - scroll.scrollContainerDimensions.height;
            float gap = composer.boundingBox.y - (last.boundingBox.y + last.boundingBox.height);
            if (overflow <= 0.0f || fabsf(scroll.scrollPosition->y + overflow) > 0.5f)
            {
                Fail("chat bottom spacer test needs an overflowing transcript pinned to the bottom");
            }
            /* The todo pill and attachment strip float above the composer; the
             * bottom spacer must keep the last message clear of them. Whatever
             * height the spacer is configured to, pinning to the bottom must
             * turn it into clearance between the message and the composer. */
            else if (spacer.boundingBox.height <= 0.0f)
            {
                Fail("chat must keep a positive-height bottom spacer below the last message");
            }
            else if (gap + 0.5f < spacer.boundingBox.height)
            {
                Fail("last message must clear the composer overlays when pinned to the bottom");
            }
            else
            {
                Clay_ElementData viewport_box = Clay_GetElementData(CLAY_ID("ChatScroll"));
                if (last.boundingBox.y + last.boundingBox.height < viewport_box.boundingBox.y ||
                    last.boundingBox.y + last.boundingBox.height >
                        viewport_box.boundingBox.y + viewport_box.boundingBox.height)
                    Fail("cold measurements must not leave the last message outside the bottom viewport");
            }
        }
    }

    Clay_SetCurrentContext(previous);
    pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    return g_failed ? 1 : 0;
}

static Clay_ElementId MainTraceRowId(int message_index, int trace_index, const char *label)
{
    Clay_ElementId message = CLAY_IDI("MsgMain", message_index);
    Clay_String key = {.length = (int32_t)strlen(label), .chars = label};
    return Clay__HashStringWithOffset(key, (uint32_t)trace_index, message.id);
}

static float TraceRowHeight(Clay_ElementId id, bool *found)
{
    Clay_ElementData el = Clay_GetElementData(id);
    *found = el.found;
    return el.found ? el.boundingBox.height : -1.0f;
}

/* A running tool row, a live think row, the synthetic Thinking… row, and the
 * finished-trace group header replace each other as tools complete and the
 * agent starts thinking. The chat pins to the bottom, so if these rows do not
 * share one height every transition shifts the transcript by the difference. */
/* Render through the actual shell/chat virtualizer, then apply the same bounded
 * reveal correction as the host frame. No graphics context is needed. */
static void FindLayoutFrame(PicoHost *host, Clay_Dimensions viewport)
{
    Clay_SetLayoutDimensions(viewport);
    Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0);
    (void)PicoHost_LayoutShell(host, viewport.height, 0);
    PicoChat_HarvestVirtualHeights(host);
    if (PicoChatFind_Reveal(host))
    {
        (void)PicoHost_LayoutShell(host, viewport.height, 0);
        PicoChat_RecordVirtualScroll();
    }
}

typedef struct FindTestRange {
    bool found;
    Clay_BoundingBox box;
    Clay_ElementId horizontal;
} FindTestRange;

static void FindCaptureRange(Clay_BoundingBox box, Clay_ElementId horizontal, bool temporary, void *user)
{
    (void)temporary;
    FindTestRange *range = user;
    if (!range->found) { range->found = true; range->box = box; range->horizontal = horizontal; }
}

static int TestChatFindTranscript(void)
{
    const Clay_Dimensions viewport = {1000, 640};
    char dir[] = "/tmp/pico-find-ws-XXXXXX", cfg[] = "/tmp/pico-find-cfg-XXXXXX";
    Clay_Context *previous = Clay_GetCurrentContext();
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id;
    PicoAgentId agent_id;
    PicoAgent *agent;
    ShellTestState shell = {.composer_height = 44};
    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg)) { Fail("find test setup"); free(memory); return 1; }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host) { Fail("find host initialization"); goto done; }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_SIDEBAR] = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &shell);
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE, .select = true};
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK ||
        pico_main_agent_create(host, workspace_id, &options, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id))) { Fail("find agent setup"); goto done; }
    char code[512] = "```\n";
    memset(code + 4, 'x', 200);
    strcpy(code + 204, " marker\n```\n");
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, code);
    for (int i = 1; i < 60; i++)
        PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT,
                            i == 30 || i == 59 ? "marker" : "Ordinary transcript paragraph.\n\nAnother block.");
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0})) { Fail("find Clay initialization"); goto done; }
    Clay_SetMeasureTextFunction(Pico_MeasureTextUtf8, NULL);
    RichText_SetMeasureFunction(Pico_MeasureTextUtf8, NULL);
    Clay_SetPointerState((Clay_Vector2){0}, false);
    for (int i = 0; i < 4; i++) FindLayoutFrame(host, viewport);
    PicoChatFind_Open(host);
    PicoChatFind_SetQuery(host, "marker");
    FindLayoutFrame(host, viewport);
    PicoChatSearch *search = &host->find.search;
    if (search->count != 3 || search->active < 0 || search->matches[search->active].message != 59)
    { Fail("query must count offscreen text and select nearest bottom-view hit"); goto done; }
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    float y = scroll.scrollPosition->y;
    PicoChatFind_SetQuery(host, "MARKER");
    FindLayoutFrame(host, viewport);
    if (fabsf(scroll.scrollPosition->y - y) > 0.01f)
    { Fail("editing query must not move an already-visible match"); goto done; }
    for (int i = 0; i < 40; i++) PicoAgent_AppendAssistant(host, agent, "\n\nLive content added below the visible hit.");
    for (int i = 0; i < 4; i++) FindLayoutFrame(host, viewport);
    if (fabsf(scroll.scrollPosition->y - y) > 0.01f || search->matches[search->active].message != 59)
    { Fail("finding a visible bottom-follow hit must stop subsequent stream growth from moving the view"); goto done; }
    PicoChatFind_Navigate(host, 1); /* Wrap to unmounted first message. */
    for (int i = 0; i < 3; i++) FindLayoutFrame(host, viewport);
    PicoChatMatch match = search->matches[search->active];
    FindTestRange range = {0};
    PicoChatSel_VisitRange(match.message, match.from, match.to, FindCaptureRange, &range);
    Clay_ElementData chat = Clay_GetElementData(CLAY_ID("ChatScroll"));
    Clay_ElementData horizontal = Clay_GetElementData(range.horizontal);
    if (match.message != 0 || !range.found || !ShellVerticallyContains(chat.boundingBox, range.box) ||
        !horizontal.found || range.box.x < horizontal.boundingBox.x - 0.1f ||
        range.box.x + range.box.width > horizontal.boundingBox.x + horizontal.boundingBox.width + 0.1f)
    { Fail("navigation must mount offscreen text and reveal its vertical and horizontal range"); goto done; }
    y = scroll.scrollPosition->y;
    PicoAgent_AppendAssistant(host, agent, " marker");
    FindLayoutFrame(host, viewport);
    if (search->count != 4 || search->matches[search->active].message != 0 ||
        fabsf(scroll.scrollPosition->y - y) > 0.01f)
    { Fail("streaming must update offscreen results without stealing the viewport"); goto done; }

#ifdef PICO_CLAY_FRAME_FAULT_TESTS
    /* Drive the real handlers with a keyboard snapshot, in host frame order. */
    g_find_input_test = true;
    PicoComposer_SetText(host, "draft");
    g_find_key = KEY_F; g_find_ctrl = true;
    PicoChatFind_HandleInput(host);
    g_find_key = 0; g_find_ctrl = false; g_find_character = 'm';
    PicoChatFind_HandleInput(host);
    PicoComposer_HandleInput(host);
    if (strcmp(host->find.field.text, "m") != 0 || strcmp(host->composer.text, "draft") != 0)
    { Fail("Ctrl+F must select the query and route subsequent typing away from the composer"); goto done; }
    g_find_key = KEY_ENTER;
    PicoChatFind_HandleInput(host);
    PicoComposer_HandleInput(host);
    if (agent->message_count != 60 || strcmp(host->composer.text, "draft") != 0)
    { Fail("Enter in find must navigate rather than submit the composer"); goto done; }
    g_find_key = KEY_ESCAPE;
    PicoChatFind_HandleInput(host);
    if (host->find.open || PicoHost_AgentEscapeEnabled(host, false, false, false, false))
    { Fail("Escape closing find must remain claimed against agent cancellation"); goto done; }
    g_find_key = 0;
    PicoChatFind_HandleInput(host);
    if (!PicoHost_AgentEscapeEnabled(host, false, false, false, false))
    { Fail("closing find must not permanently disable agent Escape"); goto done; }
    PicoChatFind_Open(host);
    PicoChatFind_SetQuery(host, "marker");
    FindLayoutFrame(host, viewport);
    /* Existing modals own Ctrl+F and leave the find editor untouched. */
    pico_ui_modal_push(host, "find-input-test");
    g_find_key = KEY_F; g_find_ctrl = true; g_find_character = 'z';
    PicoChatFind_HandleInput(host);
    if (strcmp(host->find.field.text, "marker") != 0 || !g_find_character)
    { Fail("find must not consume text belonging to a modal"); goto done; }
    pico_ui_modal_pop(host, "find-input-test");
    g_find_key = 0; g_find_ctrl = false; g_find_character = 0;
    Clay_ElementData composer = Clay_GetElementData(CLAY_ID("Composer"));
    /* The shell fixture wraps the real composer-sized pane with this ID. */
    if (!composer.found) { Fail("find focus test needs composer bounds"); goto done; }
    g_find_pointer = (Vector2){composer.boundingBox.x + 10, composer.boundingBox.y + 10};
    g_find_press = true;
    PicoChatFind_HandleInput(host);
    g_find_press = false; g_find_character = '!';
    PicoChatFind_HandleInput(host);
    PicoComposer_HandleInput(host);
    if (!host->find.open || strcmp(host->composer.text, "draft!") != 0 || strcmp(host->find.field.text, "marker") != 0)
    { Fail("clicking the composer must return typing there without closing find"); goto done; }
    g_find_input_test = false;
    y = scroll.scrollPosition->y;
#endif

    /* A completed tool group is initially hidden. Expanded output respects the
     * renderer's truncation, including when the tool message is offscreen. */
    PicoAgent_AddToolCallWithId(host, agent, "find-tool", "sh", "echo output");
    char output[8192] = "shown-find-token\n";
    for (int i = 0; i < 300; i++) strcat(output, "filler\n");
    strcat(output, "truncated-find-token");
    PicoAgent_SetLastToolOutput(agent, output, false);
    PicoMessage *last = &agent->messages[agent->message_count - 1];
    last->trace[0].tool_done_t0 = 0;
    PicoChatFind_SetQuery(host, "shown-find-token");
    FindLayoutFrame(host, viewport);
    if (search->count != 0) { Fail("collapsed tool body must not be searched"); goto done; }
    last->trace_group_expanded = last->trace[0].expanded = true;
    FindLayoutFrame(host, viewport);
    if (search->count != 1 || fabsf(scroll.scrollPosition->y - y) > 0.01f)
    { Fail("expanding offscreen tool output updates results without scrolling"); goto done; }
    PicoChatFind_SetQuery(host, "truncated-find-token");
    FindLayoutFrame(host, viewport);
    if (search->count != 0) { Fail("truncated-away output must not be searched"); goto done; }
    PicoChatFind_Close(host);
    PicoChatFind_Open(host);
    if (strcmp(host->find.field.text, "truncated-find-token") != 0)
    { Fail("closing find must remember the query"); goto done; }
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    PicoMessage *thinking = &agent->messages[agent->message_count - 1];
    thinking->trace = calloc(1, sizeof(*thinking->trace));
    thinking->trace_count = 1;
    char long_thought[2048];
    memset(long_thought, 'w', 200);
    strcpy(long_thought + 200, " suffix-to-reveal");
    thinking->trace[0].text = strdup(long_thought);
    thinking->trace[0].think_parts = calloc(1, sizeof(char *));
    thinking->trace[0].think_parts[0] = strdup(long_thought);
    thinking->trace[0].think_part_count = 1;
    thinking->trace_group_expanded = true;
    PicoChatFind_SetQuery(host, "suffix-to-reveal");
    for (int i = 0; i < 3; i++) FindLayoutFrame(host, viewport);
    if (!search->count) { Fail("long thinking label suffix should remain revealable"); goto done; }
    match = search->matches[search->active];
    range = (FindTestRange){0};
    PicoChatSel_VisitRange(match.message, match.from, match.to, FindCaptureRange, &range);
    Clay_ScrollContainerData label_scroll = Clay_GetScrollContainerData(range.horizontal);
    if (!label_scroll.found || !label_scroll.scrollPosition || label_scroll.scrollPosition->x >= 0)
    { Fail("search should reveal the clipped thinking label suffix"); goto done; }
    PicoChatFind_Close(host);
    if (label_scroll.scrollPosition->x != 0)
    { Fail("closing find must restore the non-user-scrollable thinking label"); goto done; }
    /* A result near the end of a very long expanded thinking body must still
     * have reveal geometry after wrapping. */
    free(thinking->trace[0].think_parts[0]);
    free(thinking->trace[0].think_parts);
    thinking->trace[0].think_parts = NULL;
    thinking->trace[0].think_part_count = 0;
    free(thinking->trace[0].text);
    char *body = malloc(60032);
    if (!body) { Fail("long thought fixture allocation"); goto done; }
    for (int i = 0; i < 10000; i++) memcpy(body + i * 6, "words ", 6);
    strcpy(body + 60000, "deep-thought-target");
    thinking->trace[0].text = body;
    thinking->trace[0].expanded = true;
    PicoChatFind_Open(host);
    PicoChatFind_SetQuery(host, "deep-thought-target");
    for (int i = 0; i < 3; i++) FindLayoutFrame(host, viewport);
    if (search->count != 1) { Fail("full expanded thought must be searchable"); goto done; }
    match = search->matches[search->active];
    range = (FindTestRange){0};
    PicoChatSel_VisitRange(match.message, match.from, match.to, FindCaptureRange, &range);
    chat = Clay_GetElementData(CLAY_ID("ChatScroll"));
    if (!range.found || !ShellVerticallyContains(chat.boundingBox, range.box))
    { Fail("a match near the end of a long expanded thought must be revealable"); goto done; }
    PicoAgent_ClearMessages(agent);
    if (host->find.open || host->find.field.length || host->find.search.count)
    { Fail("session reset must clear search and pending navigation"); goto done; }
    char separated[1202];
    memset(separated, 'x', 600);
    separated[600] = ' ';
    memset(separated + 601, 'y', 600);
    separated[1201] = 0;
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, separated);
    PicoChatFind_Open(host);
    PicoChatFind_SetQuery(host, " ");
    for (int i = 0; i < 3; i++) FindLayoutFrame(host, viewport);
    if (search->count != 1) { Fail("soft wrapping must retain a literal source space"); goto done; }
    match = search->matches[search->active];
    range = (FindTestRange){0};
    PicoChatSel_VisitRange(match.message, match.from, match.to, FindCaptureRange, &range);
    chat = Clay_GetElementData(CLAY_ID("ChatScroll"));
    if (!range.found || !ShellVerticallyContains(chat.boundingBox, range.box))
    { Fail("a whitespace-only hit at a soft wrap must be reachable"); goto done; }
    /* A mounted row much taller than the viewport must retain its logical
     * text and scroll range without exhausting the ordinary Clay arena. */
    PicoAgent_ClearMessages(agent);
    host->preferences.chat_width = 20;
    const size_t huge_len = 256 * 1024;
    char *huge = malloc(huge_len + 40);
    if (!huge) { Fail("large visible row allocation"); goto done; }
    memset(huge, 'a', huge_len);
    strcpy(huge + huge_len, " deep-window-needle");
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, huge);
    free(huge);
    int32_t ordinary_capacity = Clay_GetMaxElementCount();
    PicoChatFind_Open(host);
    PicoChatFind_SetQuery(host, "deep-window-needle");
    for (int i = 0; i < 5; i++) FindLayoutFrame(host, viewport);
    if (search->count != 1 || search->active < 0) goto done;
    match = search->matches[search->active];
    range = (FindTestRange){0};
    PicoChatSel_VisitRange(match.message, match.from, match.to, FindCaptureRange, &range);
    chat = Clay_GetElementData(CLAY_ID("ChatScroll"));
    scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    if (Clay_GetMaxElementCount() != ordinary_capacity || search->count != 1 ||
        !range.found || !ShellVerticallyContains(chat.boundingBox, range.box) ||
        !scroll.found || scroll.contentDimensions.height <= viewport.height * 10.0f)
    { Fail("giant visible paragraph must keep full scroll/search with bounded Clay elements"); goto done; }

    PicoAgent_ClearMessages(agent);
    FILE *block = tmpfile();
    if (!block) { Fail("large code fixture allocation"); goto done; }
    fputs("```text\n", block);
    fputs("short\n", block);
    for (int i = 0; i < 10500; i++)
    {
        if (i == 5000)
        {
            for (int j = 0; j < 300; j++) fputc('W', block);
            fputs(" middle-window-needle\n", block);
        }
        else if (i == 10490)
        {
            for (int j = 0; j < 300; j++) fputc('W', block);
            fputs(" last-window-needle\n", block);
        }
        else fputs("short\n", block);
    }
    fputs("```\n", block);
    long code_len = ftell(block);
    if (code_len <= 0 || fseek(block, 0, SEEK_SET) != 0) { fclose(block); goto done; }
    char *code_source = malloc((size_t)code_len + 1);
    if (!code_source || fread(code_source, 1, (size_t)code_len, block) != (size_t)code_len)
    { free(code_source); fclose(block); goto done; }
    fclose(block);
    code_source[code_len] = '\0';
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, code_source);
    free(code_source);
    host->chat_follow_bottom = true;
    for (int i = 0; i < 3; i++) FindLayoutFrame(host, viewport);
    PicoChatFind_Open(host);
    PicoChatFind_SetQuery(host, "middle-window-needle");
    for (int i = 0; i < 5; i++) FindLayoutFrame(host, viewport);
    if (search->count != 1 || Clay_GetMaxElementCount() != ordinary_capacity)
    { Fail("middle of giant code block must remain searchable at ordinary Clay capacity"); goto done; }
    match = search->matches[search->active];
    range = (FindTestRange){0};
    PicoChatSel_VisitRange(match.message, match.from, match.to, FindCaptureRange, &range);
    Clay_ScrollContainerData mid_horizontal = Clay_GetScrollContainerData(range.horizontal);
    if (!range.found || !mid_horizontal.found || !mid_horizontal.scrollPosition ||
        mid_horizontal.scrollPosition->x >= 0)
    { Fail("search must horizontally reveal a wide late code line before the width scan reaches it"); goto done; }
    PicoChatFind_SetQuery(host, "last-window-needle");
    for (int i = 0; i < 5; i++) FindLayoutFrame(host, viewport);
    if (search->count != 1 || Clay_GetMaxElementCount() != ordinary_capacity)
    { Fail("end of giant code block must be reachable at ordinary Clay capacity"); goto done; }
    match = search->matches[search->active];
    range = (FindTestRange){0};
    PicoChatSel_VisitRange(match.message, match.from, match.to, FindCaptureRange, &range);
    chat = Clay_GetElementData(CLAY_ID("ChatScroll"));
    Clay_ScrollContainerData horizontal_window = Clay_GetScrollContainerData(range.horizontal);
    if (!range.found || !ShellVerticallyContains(chat.boundingBox, range.box) ||
        !horizontal_window.found ||
        horizontal_window.contentDimensions.width <= horizontal_window.scrollContainerDimensions.width)
    { Fail("giant code block must keep full horizontal extent while vertically windowed"); goto done; }

    PicoChatFind_Open(host);
    PicoChatFind_SetQuery(host, "remember only this conversation");
    PicoAgentId other;
    if (pico_main_agent_create(host, workspace_id, &options, &other) != PICO_OK || host->find.open || host->find.field.length)
    { Fail("switching conversation must close and clear find"); goto done; }

done:
#ifdef PICO_CLAY_FRAME_FAULT_TESTS
    g_find_input_test = false;
    g_find_key = g_find_character = 0;
    g_find_ctrl = g_find_press = false;
#endif
    Clay_SetCurrentContext(previous);
    if (host) pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(dir); rmdir(cfg);
    return g_failed ? 1 : 0;
}


static int TestExpandedStreamingThinkStaysInsideChat(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-think-stream-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-think-stream-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions opt;
    PicoAgent *agent = NULL;
    ShellTestState state = {.composer_height = 44.0f};
    PicoMessage *msg;
    char *body = NULL;
    size_t body_len = 0;
    size_t body_cap = 0;
    char tail[32];
    int word = 0;
    int rc = 1;
    int frame;

    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("streaming think setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(cfg);
        rmdir(dir);
        Fail("streaming think host init");
        return 1;
    }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_SIDEBAR] = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK)
    {
        Fail("streaming think open workspace");
        goto done;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id)))
    {
        Fail("streaming think create agent");
        goto done;
    }
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    msg = &agent->messages[agent->message_count - 1];
    msg->trace = calloc(1, sizeof(PicoTraceLine));
    if (!msg->trace)
    {
        Fail("streaming think trace allocation");
        goto done;
    }
    msg->trace_count = 1;
    msg->trace[0].expanded = true;
    agent->state = PICO_AGENT_LLM_WAIT;
    host->chat_follow_bottom = true;

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){Pico_HandleClayErrors, 0}))
    {
        Fail("streaming think Clay initialization");
        goto done;
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);
    Pico_ClearClayReinit();

    body_cap = 8192;
    body = malloc(body_cap);
    if (!body)
    {
        Fail("streaming think body allocation");
        goto done;
    }
    body[0] = '\0';
    for (frame = 0; frame < 30; frame++)
    {
        int i;
        for (i = 0; i < 180; i++)
        {
            char word_buf[16];
            int n = snprintf(word_buf, sizeof(word_buf), "w%05d ", word++);
            if (body_len + (size_t)n + sizeof(tail) + 1 > body_cap)
            {
                size_t cap = body_cap * 2;
                char *next = realloc(body, cap);
                if (!next)
                {
                    Fail("streaming think body grow");
                    goto done;
                }
                body = next;
                body_cap = cap;
            }
            memcpy(body + body_len, word_buf, (size_t)n);
            body_len += (size_t)n;
        }
        snprintf(tail, sizeof(tail), "tail-%02d", frame);
        memcpy(body + body_len, tail, strlen(tail) + 1);
        free(msg->trace[0].text);
        msg->trace[0].text = strdup(body);
        if (!msg->trace[0].text)
        {
            Fail("streaming think text dup");
            goto done;
        }
        Clay_SetLayoutDimensions(viewport);
        Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        PicoChat_HarvestVirtualHeights(host);
        {
            Clay_ScrollContainerData data =
                Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
            if (data.found && data.scrollPosition)
            {
                PicoScrollbar_PinToBottom(data.scrollContainerDimensions.height,
                                          data.contentDimensions.height,
                                          &data.scrollPosition->y);
            }
        }
        commands = PicoHost_LayoutShell(host, viewport.height, 0.0f);
        PicoChat_HarvestVirtualHeights(host);
        if (Pico_NeedsClayReinit())
        {
            Fail("streaming expanded thinking overflowed Clay capacity");
            goto done;
        }
        Clay_ElementData chat = Clay_GetElementData(CLAY_ID("ChatScroll"));
        if (!chat.found)
        {
            Fail("streaming think layout missing ChatScroll");
            goto done;
        }
        {
            float right = chat.boundingBox.x + chat.boundingBox.width;
            int32_t c;
            bool saw_tail = false;
            for (c = 0; c < commands.length; c++)
            {
                Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(&commands, c);
                Clay_StringSlice text;
                Clay_BoundingBox box;
                if (!cmd || cmd->commandType != CLAY_RENDER_COMMAND_TYPE_TEXT)
                {
                    continue;
                }
                box = cmd->boundingBox;
                if (box.y + box.height >= chat.boundingBox.y &&
                    box.y <= chat.boundingBox.y + chat.boundingBox.height &&
                    box.x + box.width > right + 1.0f)
                {
                    Fail("expanded streaming thinking must stay inside the chat column");
                    goto done;
                }
                text = cmd->renderData.text.stringContents;
                if (!saw_tail && text.chars && text.length >= (int32_t)strlen(tail))
                {
                    int32_t o;
                    for (o = 0; o + (int32_t)strlen(tail) <= text.length; o++)
                    {
                        if (memcmp(text.chars + o, tail, strlen(tail)) == 0)
                        {
                            saw_tail = true;
                            break;
                        }
                    }
                }
            }
            if (!saw_tail)
            {
                Fail("latest streamed thinking token must remain visible");
                goto done;
            }
        }
    }
    rc = 0;
done:
    free(body);
    if (host)
    {
        pico_host_free(host);
    }
    Clay_SetCurrentContext(previous);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    return rc;
}

/* Expanded live thinking grows the transcript while following the bottom.
 * When that body folds into a collapsed group, the scroll extent must shrink
 * with it so the new bottom stays in view instead of a blank retained spacer. */
static int TestExpandedThinkFoldResizesChat(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-think-fold-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-think-fold-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions opt;
    PicoAgent *agent = NULL;
    ShellTestState state = {.composer_height = 44.0f};
    PicoMessage *msg;
    ChatStabilitySnapshot live = {0};
    ChatStabilitySnapshot folded = {0};
    char *body = NULL;
    size_t body_cap = 4096;
    size_t body_len = 0;
    int last_index;
    int rc = 1;
    int i;

    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("think fold setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(cfg);
        rmdir(dir);
        Fail("think fold host init");
        return 1;
    }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_SIDEBAR] = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK)
    {
        Fail("think fold open workspace");
        goto done;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id)))
    {
        Fail("think fold create agent");
        goto done;
    }
    for (i = 0; i < 28; i++)
    {
        PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "think fold history message");
    }
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    last_index = agent->message_count - 1;
    msg = &agent->messages[last_index];
    msg->trace = calloc(1, sizeof(PicoTraceLine));
    if (!msg->trace)
    {
        Fail("think fold trace allocation");
        goto done;
    }
    msg->trace_count = 1;
    body = malloc(body_cap);
    if (!body)
    {
        Fail("think fold body allocation");
        goto done;
    }
    body[0] = '\0';
    for (i = 0; i < 1600; i++)
    {
        char word[16];
        int n = snprintf(word, sizeof(word), "w%05d ", i);
        if (body_len + (size_t)n + 1 > body_cap)
        {
            size_t cap = body_cap * 2;
            char *next = realloc(body, cap);
            if (!next)
            {
                Fail("think fold body grow");
                goto done;
            }
            body = next;
            body_cap = cap;
        }
        memcpy(body + body_len, word, (size_t)n);
        body_len += (size_t)n;
        body[body_len] = '\0';
    }
    msg->trace[0].text = strdup(body);
    if (!msg->trace[0].text)
    {
        Fail("think fold text dup");
        goto done;
    }
    msg->trace[0].expanded = true;
    msg->trace_group_expanded = false;
    agent->state = PICO_AGENT_LLM_WAIT;

    {
        Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
        if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
        {
            Fail("think fold Clay initialization");
            goto done;
        }
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);

    host->chat_follow_bottom = true;
    PicoChat_ResetBottomSpace(host);
    for (i = 0; i < 8; i++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    if (!CaptureChatStabilitySnapshot(false, &live))
    {
        Fail("think fold could not capture the live layout");
        goto done;
    }
    {
        Clay_ElementData think_row = Clay_GetElementData(MainTraceRowId(last_index, 0, "ThinkRow"));
        Clay_ElementData last = Clay_GetElementData(CLAY_IDI("MsgMain", last_index));
        float overflow = live.content_height - live.container_height;
        if (!think_row.found || !last.found || last.boundingBox.height <= viewport.height ||
            overflow <= 0.0f || fabsf(live.scroll_y + overflow) > 0.5f)
        {
            Fail("expanded thinking must overflow the chat while following the bottom");
            goto done;
        }
    }

    agent->state = PICO_AGENT_IDLE;
    pico_run_hooks(host, PICO_HOOK_ON_TURN_END, agent->id);
    LayoutChatStabilityFrame(host, viewport);
    if (!CaptureChatStabilitySnapshot(false, &folded))
    {
        Fail("think fold could not capture the folded layout");
        goto done;
    }
    {
        Clay_ElementData group = Clay_GetElementData(MainTraceRowId(last_index, 0, "TraceGroupRow"));
        Clay_ElementData think_row = Clay_GetElementData(MainTraceRowId(last_index, 0, "ThinkRow"));
        Clay_ElementData chat = Clay_GetElementData(CLAY_ID("ChatScroll"));
        Clay_ElementData last = Clay_GetElementData(CLAY_IDI("MsgMain", last_index));
        float overflow = folded.content_height - folded.container_height;
        if (!group.found || think_row.found)
        {
            Fail("finished thinking must fold into the trace group header");
            goto done;
        }
        if (!chat.found || !last.found ||
            !ShellVerticallyContains(chat.boundingBox, last.boundingBox) ||
            folded.stabilization.height > 0.5f ||
            folded.content_height >= live.content_height - 0.5f ||
            (overflow > 0.0f && fabsf(folded.scroll_y + overflow) > 0.5f))
        {
            Fail("folding expanded thinking must resize the chat onto the new bottom");
            goto done;
        }
    }

    /* A replacement live think on the same presented-frame boundary must not
     * hide the fold: the previous body disappeared even though a think body
     * is still visible. */
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    last_index = agent->message_count - 1;
    msg = &agent->messages[last_index];
    msg->trace = calloc(1, sizeof(PicoTraceLine));
    if (!msg->trace)
    {
        Fail("think fold replacement trace allocation");
        goto done;
    }
    msg->trace_count = 1;
    msg->trace[0].text = strdup(body);
    if (!msg->trace[0].text)
    {
        Fail("think fold replacement text dup");
        goto done;
    }
    msg->trace[0].expanded = true;
    msg->trace_group_expanded = false;
    agent->state = PICO_AGENT_LLM_WAIT;
    PicoChat_ResetBottomSpace(host);
    for (i = 0; i < 8; i++)
    {
        LayoutChatStabilityFrame(host, viewport);
    }
    if (!CaptureChatStabilitySnapshot(false, &live))
    {
        Fail("think fold could not capture the replacement live layout");
        goto done;
    }
    {
        Clay_ElementData think_row = Clay_GetElementData(MainTraceRowId(last_index, 0, "ThinkRow"));
        float overflow = live.content_height - live.container_height;
        if (!think_row.found || live.content_height <= folded.content_height + 0.5f ||
            overflow <= 0.0f || fabsf(live.scroll_y + overflow) > 0.5f)
        {
            Fail("replacement expanded thinking must overflow the chat again");
            goto done;
        }
    }
    {
        PicoTraceLine *lines = realloc(msg->trace, 2 * sizeof(*lines));
        if (!lines)
        {
            Fail("think fold replacement grow");
            goto done;
        }
        msg->trace = lines;
        memset(&msg->trace[1], 0, sizeof(msg->trace[1]));
        msg->trace[1].text = strdup("next think");
        if (!msg->trace[1].text)
        {
            Fail("think fold replacement next think");
            goto done;
        }
        msg->trace[1].expanded = true;
        msg->trace_count = 2;
    }
    LayoutChatStabilityFrame(host, viewport);
    if (!CaptureChatStabilitySnapshot(false, &folded))
    {
        Fail("think fold could not capture the replacement folded layout");
        goto done;
    }
    {
        Clay_ElementData group = Clay_GetElementData(MainTraceRowId(last_index, 0, "TraceGroupRow"));
        Clay_ElementData folded_row = Clay_GetElementData(MainTraceRowId(last_index, 0, "ThinkRow"));
        Clay_ElementData live_row = Clay_GetElementData(MainTraceRowId(last_index, 1, "ThinkRow"));
        Clay_ElementData chat = Clay_GetElementData(CLAY_ID("ChatScroll"));
        Clay_ElementData last = Clay_GetElementData(CLAY_IDI("MsgMain", last_index));
        float overflow = folded.content_height - folded.container_height;
        if (!group.found || folded_row.found || !live_row.found)
        {
            Fail("a new live think must not keep the previous body outside the group");
            goto done;
        }
        if (!chat.found || !last.found ||
            !ShellVerticallyContains(chat.boundingBox, last.boundingBox) ||
            folded.stabilization.height > 0.5f ||
            folded.content_height >= live.content_height - 0.5f ||
            (overflow > 0.0f && fabsf(folded.scroll_y + overflow) > 0.5f))
        {
            Fail("replacing expanded thinking must resize the chat onto the new bottom");
            goto done;
        }
    }
    rc = 0;

done:
    free(body);
    if (host)
    {
        pico_host_free(host);
    }
    Clay_SetCurrentContext(previous);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    return rc;
}

static int TestChatTraceRowsShareHeight(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-trace-rows-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-trace-rows-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions opt;
    PicoAgent *agent;
    ShellTestState state = {.composer_height = 44.0f};
    Clay_Arena arena;
    bool f_tool, f_group, f_think, f_think_group, f_synth;
    float tool_h, group_h, think_h, think_group_h, synth_h;
    int rc = 1;

    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("trace row height setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(cfg);
        rmdir(dir);
        Fail("trace row height host init");
        return 1;
    }
    WaitPluginLoad(host);
    /* Headless runs have no real fonts; keep chat on the unclamped width path
     * and stub the composer, which measures text with raylib directly. */
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK)
    {
        Fail("trace row height open workspace");
        goto done_host;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id)))
    {
        Fail("trace row height create agent");
        goto done_host;
    }

    /* msg 0: finished tool call, collapsed into the group header row. */
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    PicoAgent_AddToolCallWithId(host, agent, "done-1", "read", "");
    PicoAgent_SetLastToolOutput(agent, "ok", false);
    agent->messages[0].trace[0].tool_done_t0 = 0.0;
    /* msg 1 (last): running tool call, rendered as an open tool row. */
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    PicoAgent_AddToolCallWithId(host, agent, "run-1", "read", "");
    agent->state = PICO_AGENT_TOOL_WAIT;

    arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        Clay_SetCurrentContext(previous);
        Fail("trace row height Clay initialization");
        goto done_host;
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);

    for (int frame = 0; frame < 3; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        PicoChat_HarvestVirtualHeights(host);
    }
    tool_h = TraceRowHeight(MainTraceRowId(1, 0, "ToolRow"), &f_tool);
    group_h = TraceRowHeight(MainTraceRowId(0, 0, "TraceGroupRow"), &f_group);

    /* The tool completes and the agent starts thinking: the open tool row is
     * replaced by the group header plus a live think row. */
    PicoAgent_SetToolOutputByCallId(agent, "run-1", "ok", false);
    agent->messages[1].trace[0].tool_done_t0 = 0.0;
    PicoAgent_AppendThink(host, agent, "reasoning", 0);
    agent->state = PICO_AGENT_LLM_WAIT;
    for (int frame = 0; frame < 3; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        PicoChat_HarvestVirtualHeights(host);
    }
    think_h = TraceRowHeight(MainTraceRowId(1, 1, "ThinkRow"), &f_think);
    think_group_h = TraceRowHeight(MainTraceRowId(1, 0, "TraceGroupRow"), &f_think_group);

    /* Before any thinking body streams, the synthetic Thinking… row stands in. */
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    for (int frame = 0; frame < 3; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        PicoChat_HarvestVirtualHeights(host);
    }
    synth_h = TraceRowHeight(MainTraceRowId(2, 0, "ThinkSynthRow"), &f_synth);

    if (!f_tool || !f_group || !f_think || !f_think_group || !f_synth)
    {
        Fail("trace row height scenario did not render every row kind");
        goto done;
    }
    if (fabsf(tool_h - group_h) > 0.001f || fabsf(tool_h - think_h) > 0.001f ||
        fabsf(tool_h - think_group_h) > 0.001f || fabsf(tool_h - synth_h) > 0.001f)
    {
        fprintf(stderr, "trace row heights: tool %.3f group %.3f think %.3f think-group %.3f synth %.3f\n",
                tool_h, group_h, think_h, think_group_h, synth_h);
        Fail("tool, think, and group header rows must share one height");
        goto done;
    }
    rc = g_failed ? 1 : 0;

done:
    Clay_SetCurrentContext(previous);
done_host:
    pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    return rc;
}

/* Fast tool completions must keep the individual row on screen before the
 * collapsed group replaces it; otherwise the row only flashes. */
static int TestChatCompletedToolRowDwells(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-tool-dwell-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-tool-dwell-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions opt;
    PicoAgent *agent;
    ShellTestState state = {.composer_height = 44.0f};
    Clay_Arena arena;
    bool f_tool, f_group;
    int rc = 1;

    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("completed tool dwell setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(cfg);
        rmdir(dir);
        Fail("completed tool dwell host init");
        return 1;
    }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK)
    {
        Fail("completed tool dwell open workspace");
        goto done_host;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id)))
    {
        Fail("completed tool dwell create agent");
        goto done_host;
    }

    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    PicoAgent_AddToolCallWithId(host, agent, "done-1", "read", "");
    PicoAgent_SetLastToolOutput(agent, "ok", false);

    arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        Clay_SetCurrentContext(previous);
        Fail("completed tool dwell Clay initialization");
        goto done_host;
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);

    for (int frame = 0; frame < 3; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        PicoChat_HarvestVirtualHeights(host);
    }
    (void)TraceRowHeight(MainTraceRowId(0, 0, "ToolRow"), &f_tool);
    (void)TraceRowHeight(MainTraceRowId(0, 0, "TraceGroupRow"), &f_group);
    if (!f_tool || f_group)
    {
        Fail("a just-completed tool row stays visible instead of grouping");
        goto done;
    }

    agent->messages[0].trace[0].tool_done_t0 = 0.0;
    for (int frame = 0; frame < 3; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        PicoChat_HarvestVirtualHeights(host);
    }
    (void)TraceRowHeight(MainTraceRowId(0, 0, "ToolRow"), &f_tool);
    (void)TraceRowHeight(MainTraceRowId(0, 0, "TraceGroupRow"), &f_group);
    if (f_tool || !f_group)
    {
        Fail("a completed tool row joins the group after the dwell");
        goto done;
    }
    /* Measure two dwelling rows, then move them outside the mounted range.
     * Expiry must shrink their spacer without scrolling back to the rows. */
    PicoAgent_AddToolCallWithId(host, agent, "done-2", "read", "");
    PicoAgent_SetLastToolOutput(agent, "ok", false);
    agent->messages[0].trace[0].tool_done_t0 = pico_trace_now();
    for (int i = 0; i < 100; i++)
    {
        PicoAgent_AddMessage(host, agent, PICO_ROLE_USER, "Later message");
    }
    host->chat_follow_bottom = false;
    for (int frame = 0; frame < 4; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        PicoChat_HarvestVirtualHeights(host);
        Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
        if (!scroll.found || !scroll.scrollPosition)
        {
            Fail("offscreen dwell requires a scroll container");
            goto done;
        }
        scroll.scrollPosition->y = scroll.scrollContainerDimensions.height -
                                   scroll.contentDimensions.height;
    }
    (void)TraceRowHeight(MainTraceRowId(0, 0, "ToolRow"), &f_tool);
    Clay_ScrollContainerData before = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    float dwelling_height = before.contentDimensions.height;
    if (f_tool)
    {
        Fail("dwelling rows must be offscreen for the expiry regression");
        goto done;
    }
    for (int t = 0; t < agent->messages[0].trace_count; t++)
    {
        agent->messages[0].trace[t].tool_done_t0 = pico_trace_now() - 60.0;
    }
    for (int frame = 0; frame < 3; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        PicoChat_HarvestVirtualHeights(host);
    }
    Clay_ScrollContainerData after = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    if (!after.found || after.contentDimensions.height >= dwelling_height - 1.0f)
    {
        Fail("offscreen tool expiry must shrink the transcript scroll extent");
        goto done;
    }

    /* Exercise the real replay path, whose output setters also serve live tools. */
    char replay_path[4096];
    snprintf(replay_path, sizeof(replay_path), "%s/replay.jsonl", dir);
    FILE *replay = fopen(replay_path, "w");
    if (!replay)
    {
        Fail("create tool dwell replay fixture");
        goto done;
    }
    fputs("{\"type\":\"session\",\"version\":4,\"id\":\"dwell-replay\",\"kind\":\"normal\"}\n"
          "{\"type\":\"tool_call\",\"message_group\":0,\"call_id\":\"restored\",\"name\":\"read\",\"arguments\":\"{}\"}\n"
          "{\"type\":\"tool_result\",\"call_id\":\"restored\",\"name\":\"read\",\"output\":\"ok\",\"is_error\":false}\n", replay);
    fclose(replay);
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id)) ||
        PicoSession_Replay(host, agent, replay_path, false) != 0)
    {
        unlink(replay_path);
        Fail("replay completed tool into a selected agent");
        goto done;
    }
    unlink(replay_path);
    Clay_ScrollContainerData restored_scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    if (restored_scroll.found && restored_scroll.scrollPosition)
    {
        restored_scroll.scrollPosition->y = 0.0f;
    }
    for (int frame = 0; frame < 3; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        PicoChat_HarvestVirtualHeights(host);
    }
    (void)TraceRowHeight(MainTraceRowId(0, 0, "ToolRow"), &f_tool);
    (void)TraceRowHeight(MainTraceRowId(0, 0, "TraceGroupRow"), &f_group);
    if (f_tool || !f_group)
    {
        Fail("replayed tools must group immediately without a completion dwell");
        goto done;
    }
    rc = g_failed ? 1 : 0;

done:
    Clay_SetCurrentContext(previous);
done_host:
    pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    return rc;
}

/* The status dot on an open tool row must sit at the vertical center of the
 * first text line. For a one-line row that is the row center; when args wrap
 * and the row grows taller the dot must keep the same offset from the row top
 * instead of drifting to the middle of the row. */
static int TestChatToolStatusDotCentered(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-dot-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-dot-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions opt;
    PicoAgent *agent;
    ShellTestState state = {.composer_height = 44.0f};
    Clay_Arena arena;
    Clay_ElementData row, dot;
    float single_row_h, dot_off, center_delta;
    int rc = 1;

    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("status dot setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(cfg);
        rmdir(dir);
        Fail("status dot host init");
        return 1;
    }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK)
    {
        Fail("status dot open workspace");
        goto done_host;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id)))
    {
        Fail("status dot create agent");
        goto done_host;
    }

    /* A running tool call renders as an open tool row; empty args keep the
     * row exactly one text line tall. */
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    PicoAgent_AddToolCallWithId(host, agent, "run-1", "sh", "");
    agent->state = PICO_AGENT_TOOL_WAIT;

    arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        Clay_SetCurrentContext(previous);
        Fail("status dot Clay initialization");
        goto done_host;
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);

    for (int frame = 0; frame < 3; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        PicoChat_HarvestVirtualHeights(host);
    }
    row = Clay_GetElementData(MainTraceRowId(0, 0, "ToolRow"));
    dot = Clay_GetElementData(MainTraceRowId(0, 0, "ToolStatus"));
    if (!row.found || !dot.found)
    {
        Fail("status dot scenario did not render the tool row");
        goto done;
    }
    single_row_h = row.boundingBox.height;
    dot_off = dot.boundingBox.y - row.boundingBox.y;
    center_delta = fabsf((dot.boundingBox.y + dot.boundingBox.height * 0.5f) -
                         (row.boundingBox.y + row.boundingBox.height * 0.5f));
    if (center_delta > 0.001f)
    {
        fprintf(stderr, "status dot center offset: %.3f\n", center_delta);
        Fail("status dot must be vertically centered on a one-line tool row");
        goto done;
    }

    /* Wrapped args make the row taller; the dot must stay on the first line. */
    free(agent->messages[0].trace[0].tool_args);
    agent->messages[0].trace[0].tool_args = strdup(
        "build the project with the debug preset and run every workspace test "
        "and then run the entire suite again after the fix to be sure nothing "
        "else regressed while the row layout changed");
    if (!agent->messages[0].trace[0].tool_args)
    {
        Fail("status dot args allocation");
        goto done;
    }
    for (int frame = 0; frame < 3; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
        PicoChat_HarvestVirtualHeights(host);
    }
    row = Clay_GetElementData(MainTraceRowId(0, 0, "ToolRow"));
    dot = Clay_GetElementData(MainTraceRowId(0, 0, "ToolStatus"));
    if (!row.found || !dot.found || row.boundingBox.height <= single_row_h + 1.0f)
    {
        Fail("status dot wrapped-args scenario did not grow the row");
        goto done;
    }
    if (fabsf(dot.boundingBox.y - row.boundingBox.y - dot_off) > 0.001f)
    {
        fprintf(stderr, "status dot offset: wrapped %.3f single-line %.3f\n",
                dot.boundingBox.y - row.boundingBox.y, dot_off);
        Fail("status dot must stay on the first line when tool args wrap");
        goto done;
    }
    rc = g_failed ? 1 : 0;

done:
    Clay_SetCurrentContext(previous);
done_host:
    pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    return rc;
}

static int TestCanonicalOpenAndDuplicate(void)
{
    char dir[] = "/tmp/pico-ws-XXXXXX";
    char alias[4096];
    PicoHost *host = NULL;
    PicoWorkspaceId first = 0;
    PicoWorkspaceId again = 0;
    PicoWorkspaceId linked = 0;
    PicoWorkspaceInfo info;

    if (!mkdtemp(dir))
    {
        Fail("mkdtemp");
        return 1;
    }
    snprintf(alias, sizeof(alias), "%s-alias", dir);
    if (symlink(dir, alias) != 0)
    {
        Fail("symlink");
        rmdir(dir);
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("pico_host_init");
        unlink(alias);
        rmdir(dir);
        return 1;
    }
    if (pico_workspace_open(host, dir, &first) != PICO_OK || first == 0)
    {
        Fail("open canonical directory");
        pico_host_free(host);
        unlink(alias);
        rmdir(dir);
        return 1;
    }
    if (pico_workspace_open(host, dir, &again) != PICO_ALREADY_OPEN || again != first)
    {
        Fail("duplicate open should return PICO_ALREADY_OPEN with the same id");
        pico_host_free(host);
        unlink(alias);
        rmdir(dir);
        return 1;
    }
    if (pico_workspace_open(host, alias, &linked) != PICO_ALREADY_OPEN || linked != first)
    {
        Fail("symlink alias should return PICO_ALREADY_OPEN with the same id");
        pico_host_free(host);
        unlink(alias);
        rmdir(dir);
        return 1;
    }
    if (pico_workspace_count(host) != 1 || !pico_workspace_info(host, 0, &info) || info.id != first)
    {
        Fail("workspace info");
        pico_host_free(host);
        unlink(alias);
        rmdir(dir);
        return 1;
    }
    if (info.main_agent_count != 0 || info.total_agent_count != 0)
    {
        Fail("opening a workspace must not create a main agent");
        pico_host_free(host);
        unlink(alias);
        rmdir(dir);
        return 1;
    }
    {
        char other[] = "/tmp/pico-ws-XXXXXX";
        PicoWorkspaceId second = 0;
        if (!mkdtemp(other))
        {
            Fail("mkdtemp second");
            pico_host_free(host);
            unlink(alias);
            rmdir(dir);
            return 1;
        }
        if (pico_workspace_open(host, other, &second) != PICO_OK || second == 0 || second == first ||
            pico_workspace_count(host) != 2)
        {
            Fail("a second live workspace should succeed");
            pico_host_free(host);
            unlink(alias);
            rmdir(other);
            rmdir(dir);
            return 1;
        }

        /* Fill the remaining workspace slots, then verify overflow rejection. */
        char extra_dirs[PICO_MAX_WORKSPACES][64];
        int extra_count = PICO_MAX_WORKSPACES - pico_workspace_count(host);
        PicoWorkspaceId extra_ids[PICO_MAX_WORKSPACES];
        for (int i = 0; i < extra_count; i++)
        {
            snprintf(extra_dirs[i], sizeof(extra_dirs[i]), "/tmp/pico-ws-ext-%d-XXXXXX", i);
            if (!mkdtemp(extra_dirs[i]))
            {
                Fail("mkdtemp extra");
                return 1;
            }
            if (pico_workspace_open(host, extra_dirs[i], &extra_ids[i]) != PICO_OK)
            {
                Fail("open extra workspace up to limit");
            }
        }
        if (pico_workspace_count(host) != PICO_MAX_WORKSPACES)
        {
            Fail("workspace count should reach PICO_MAX_WORKSPACES");
        }

        /* Opening past the live-workspace cap closes the least-recently-active
         * idle workspace (the first-opened one, never touched since) and the
         * open succeeds; the live count stays at the cap. */
        char overflow[] = "/tmp/pico-ws-overflow-XXXXXX";
        PicoWorkspaceId overflow_id = 0;
        if (mkdtemp(overflow))
        {
            bool oldest_gone = true;
            PicoWorkspaceInfo probe;
            if (pico_workspace_open(host, overflow, &overflow_id) != PICO_OK || overflow_id == 0)
            {
                Fail("opening a workspace beyond capacity should evict an idle workspace");
            }
            else if (pico_workspace_count(host) != PICO_MAX_WORKSPACES)
            {
                Fail("eviction should keep the live workspace count at the cap");
            }
            for (int i = 0; i < pico_workspace_count(host); i++)
            {
                if (pico_workspace_info(host, i, &probe) && probe.id == first)
                {
                    oldest_gone = false;
                }
            }
            if (!oldest_gone)
            {
                Fail("eviction should close the least-recently-active idle workspace");
            }
            rmdir(overflow);
        }

        for (int i = 0; i < extra_count; i++)
        {
            rmdir(extra_dirs[i]);
        }
        rmdir(other);
    }
    pico_host_free(host);
    unlink(alias);
    rmdir(dir);
    return 0;
}

static void DummyView(PicoHost *host, void *state)
{
    (void)host;
    (void)state;
}

static int TestSortedViewRegistrationAssignsStateAndRollsBack(void)
{
    PicoHost host;
    char state_old;
    char state_new;

    memset(&host, 0, sizeof(host));
    PicoHost_BeginRegistration(&host, PICO_REG_HOST, NULL);
    pico_host_add_view(&host, PICO_SLOT_SIDEBAR, 10, DummyView);
    PicoHost_PublishRegistration(&host, &state_old);
    PicoHost_BeginRegistration(&host, PICO_REG_HOST, NULL);
    pico_host_add_view(&host, PICO_SLOT_SIDEBAR, 0, DummyView);
    PicoHost_PublishRegistration(&host, &state_new);
    if (host.view_count[PICO_SLOT_SIDEBAR] != 2 || host.views[PICO_SLOT_SIDEBAR][0].z != 0 ||
        host.views[PICO_SLOT_SIDEBAR][0].state != &state_new || host.views[PICO_SLOT_SIDEBAR][1].z != 10 ||
        host.views[PICO_SLOT_SIDEBAR][1].state != &state_old)
    {
        Fail("lower-z view should receive the new state without stealing the old callback");
        return 1;
    }

    PicoHost_BeginRegistration(&host, PICO_REG_HOST, NULL);
    pico_host_add_view(&host, PICO_SLOT_SIDEBAR, -5, DummyView);
    PicoHost_DiscardRegistration(&host);
    if (host.view_count[PICO_SLOT_SIDEBAR] != 2 || host.views[PICO_SLOT_SIDEBAR][0].state != &state_new ||
        host.views[PICO_SLOT_SIDEBAR][1].state != &state_old)
    {
        Fail("failed init must not truncate an older view");
        return 1;
    }
    return 0;
}

static char *DupStr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (out)
    {
        memcpy(out, s, n);
    }
    return out;
}

static int TestSubmitSettersTakeOwnership(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    pico_host_set_agent_input(&host, DupStr("one"));
    pico_host_set_agent_input(&host, DupStr("two"));
    pico_host_set_agent_parts(&host, DupStr("[]"));
    pico_host_request_submit_cancel(&host);
    if (!host.submit_cancel || !host.agent_input || strcmp(host.agent_input, "two") != 0 || !host.agent_parts ||
        strcmp(host.agent_parts, "[]") != 0)
    {
        Fail("submit setters should own replacements and record cancel");
        free(host.agent_input);
        free(host.agent_parts);
        return 1;
    }
    pico_host_set_agent_input(&host, NULL);
    pico_host_set_agent_parts(&host, NULL);
    return 0;
}

static int TestSidebarDragBehavior(void)
{
    const float midpoints[] = {10.0f, 40.0f, 70.0f};
    PicoHost host;
    memset(&host, 0, sizeof(host));
    if (PicoSidebar_DragMoved(20.0f, 20.0f, 20.0f, 20.0f) ||
        !PicoSidebar_DragMoved(20.0f, 20.0f, 80.0f, 80.0f))
    {
        Fail("workspace drag should require pointer movement");
        return 1;
    }
    if (PicoSidebar_DragTarget(midpoints, 3, 1, 40.0f) != 1 ||
        PicoSidebar_DragTarget(midpoints, 3, 1, 69.0f) != 1 ||
        PicoSidebar_DragTarget(midpoints, 3, 1, 71.0f) != 2 ||
        PicoSidebar_DragTarget(midpoints, 3, 1, 9.0f) != 0 ||
        PicoSidebar_DragTarget(midpoints, 3, 0, 39.0f) != 0 ||
        PicoSidebar_DropTarget(midpoints, 3, 1, 71.0f, false) != -1)
    {
        Fail("workspace drag should cross an adjacent row midpoint before reordering");
        return 1;
    }
    if (!PicoHost_AgentEscapeEnabled(&host, false, false, false, false))
    {
        Fail("agent Escape should be enabled without a competing UI interaction");
        return 1;
    }
    host.ui_drag_active = true;
    if (PicoHost_AgentEscapeEnabled(&host, false, false, false, false))
    {
        Fail("workspace drag should own Escape instead of cancelling the active agent");
        return 1;
    }
    return 0;
}

static int TestWorkspaceBuiltinsRegisterThroughWorkspaceInit(void)
{
    PicoHost host;
    PicoWorkspace *workspace;
    PicoExt shell;
    PicoExt subagent;
    int i;
    bool has_sh = false;
    bool has_subagent = false;

    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    workspace = PicoHost_PrimaryWorkspace(&host);
    shell = pico_ext_shell();
    subagent = pico_ext_subagent();
    if (shell.host_init || !shell.workspace_init || subagent.host_init || !subagent.workspace_init || !workspace)
    {
        Fail("shell and subagent must initialize as workspace instances");
        return 1;
    }
    PicoHost_BeginRegistration(&host, PICO_REG_WORKSPACE, workspace);
    if (shell.workspace_init(workspace, NULL) != 0)
    {
        Fail("shell workspace builtin init");
        return 1;
    }
    PicoHost_PublishRegistration(&host, NULL);

    PicoHost_BeginRegistration(&host, PICO_REG_WORKSPACE, workspace);
    if (subagent.workspace_init(workspace, NULL) != 0)
    {
        Fail("subagent workspace builtin init");
        return 1;
    }
    PicoHost_PublishRegistration(&host, NULL);

    for (i = 0; i < workspace->tool_count; i++)
    {
        if (workspace->tools[i].name && strcmp(workspace->tools[i].name, "sh") == 0)
        {
            has_sh = true;
        }
        if (workspace->tools[i].name && strcmp(workspace->tools[i].name, "subagent") == 0)
        {
            has_subagent = true;
        }
    }
    if (!has_sh || !has_subagent)
    {
        Fail("workspace init must register sh and subagent tools");
        PicoWorkspace_RegistrationClear(host.workspaces[0]);
        free(host.workspaces[0]);
        return 1;
    }
    PicoWorkspace_RegistrationClear(host.workspaces[0]);
    free(host.workspaces[0]);
    return 0;
}

static void RmRf(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *ent;
    if (!d)
    {
        unlink(path);
        return;
    }
    while ((ent = readdir(d)) != NULL)
    {
        char child[4096];
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        {
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        RmRf(child);
    }
    closedir(d);
    rmdir(path);
}

static int MkdirParents(const char *path)
{
    char buf[4096];
    char *p;
    snprintf(buf, sizeof(buf), "%s", path);
    for (p = buf + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = 0;
            if (mkdir(buf, 0700) != 0 && errno != EEXIST)
            {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST)
    {
        return -1;
    }
    return 0;
}

static int WriteFile(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        return -1;
    }
    if (fputs(text, f) < 0)
    {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static void ReadFileStr(const char *path, char *out, size_t cap)
{
    FILE *f;
    size_t n;
    if (!out || cap == 0)
    {
        return;
    }
    out[0] = '\0';
    f = fopen(path, "rb");
    if (!f)
    {
        return;
    }
    n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
}

static const char *kLifecycleExt =
    "#include \"pico/plugin.h\"\n"
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "static FILE *Life(void)\n"
    "{\n"
    "    const char *path = getenv(\"PICO_TEST_LIFE\");\n"
    "    return path ? fopen(path, \"a\") : NULL;\n"
    "}\n"
    "static void HostView(PicoHost *host, void *state)\n"
    "{\n"
    "    (void)host;\n"
    "    (void)state;\n"
    "}\n"
    "static int HostInit(PicoHost *host, void **state_out)\n"
    "{\n"
    "    *state_out = malloc(1);\n"
    "    pico_host_add_view(host, PICO_SLOT_SIDEBAR, 99, HostView);\n"
    "    return 0;\n"
    "}\n"
    "static void HostShutdown(PicoHost *host, void *state)\n"
    "{\n"
    "    FILE *f = Life();\n"
    "    (void)host;\n"
    "    if (f)\n"
    "    {\n"
    "        fputc('H', f);\n"
    "        fclose(f);\n"
    "    }\n"
    "    free(state);\n"
    "}\n"
    "static int WorkspaceInit(PicoWorkspace *workspace, void **state_out)\n"
    "{\n"
    "    (void)workspace;\n"
    "    if (getenv(\"PICO_TEST_FAIL_WORKSPACE\"))\n"
    "    {\n"
    "        return -1;\n"
    "    }\n"
    "    *state_out = malloc(1);\n"
    "    return 0;\n"
    "}\n"
    "static void WorkspaceShutdown(PicoWorkspace *workspace, void *state)\n"
    "{\n"
    "    FILE *f = Life();\n"
    "    if (f)\n"
    "    {\n"
    "        fputc(workspace && pico_workspace_host(workspace) ? 'Y' : 'N', f);\n"
    "        fclose(f);\n"
    "    }\n"
    "    free(state);\n"
    "}\n"
    "PicoExt pico_ext(void)\n"
    "{\n"
    "    return (PicoExt){\n"
    "        .abi = PICO_EXT_ABI,\n"
    "        .name = \"lifecycle\",\n"
    "        .host_init = HostInit,\n"
    "        .host_shutdown = HostShutdown,\n"
    "        .workspace_init = WorkspaceInit,\n"
    "        .workspace_shutdown = WorkspaceShutdown,\n"
    "    };\n"
    "}\n";

static int StartLifecycleHost(PicoHost **host_out, char *cfg, char *cache, char *ws, char *life, int fail_workspace)
{
    char ext_dir[320];
    char src[336];

    snprintf(cfg, 256, "/tmp/pico-cfg-XXXXXX");
    snprintf(cache, 256, "/tmp/pico-cache-XXXXXX");
    snprintf(ws, 256, "/tmp/pico-ws-XXXXXX");
    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(ws))
    {
        Fail("mkdtemp lifecycle");
        return -1;
    }
    snprintf(life, 512, "%s/life", cache);
    snprintf(ext_dir, sizeof(ext_dir), "%s/pico/extensions", cfg);
    snprintf(src, sizeof(src), "%s/lifecycle.c", ext_dir);
    if (MkdirParents(ext_dir) != 0 || WriteFile(src, kLifecycleExt) != 0)
    {
        Fail("write lifecycle extension");
        return -1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    setenv("PICO_TEST_LIFE", life, 1);
    if (fail_workspace)
    {
        setenv("PICO_TEST_FAIL_WORKSPACE", "1", 1);
    }
    else
    {
        unsetenv("PICO_TEST_FAIL_WORKSPACE");
    }
    if (pico_host_init(host_out, NULL, false) != PICO_OK || !*host_out)
    {
        Fail("pico_host_init lifecycle");
        return -1;
    }
    PicoWorkspaceId id = 0;
    if (pico_workspace_open(*host_out, ws, &id) != PICO_OK)
    {
        Fail("open lifecycle workspace");
        pico_host_free(*host_out);
        *host_out = NULL;
        return -1;
    }
    WaitPluginLoad(*host_out);
    return 0;
}

static void FinishLifecycleHost(PicoHost *host, char *cfg, char *cache, char *ws)
{
    if (host)
    {
        pico_host_free(host);
    }
    unsetenv("PICO_TEST_FAIL_WORKSPACE");
    unsetenv("PICO_TEST_LIFE");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(ws);
}

static int SidebarHasLifecycleView(const PicoHost *host)
{
    int i;
    if (!host)
    {
        return 0;
    }
    for (i = 0; i < host->view_count[PICO_SLOT_SIDEBAR]; i++)
    {
        if (host->views[PICO_SLOT_SIDEBAR][i].z == 99 && host->views[PICO_SLOT_SIDEBAR][i].host_render &&
            host->views[PICO_SLOT_SIDEBAR][i].state)
        {
            return 1;
        }
    }
    return 0;
}

static int TestFailedWorkspaceInitKeepsHostSlot(void)
{
    char cfg[256];
    char cache[256];
    char ws[256];
    char life[512];
    char log[8];
    PicoHost *host = NULL;

    cfg[0] = cache[0] = ws[0] = '\0';
    if (StartLifecycleHost(&host, cfg, cache, ws, life, 1) != 0)
    {
        FinishLifecycleHost(host, cfg, cache, ws);
        return 1;
    }
    ReadFileStr(life, log, sizeof(log));
    if (log[0] != '\0' || !SidebarHasLifecycleView(host))
    {
        Fail("failed workspace init must keep the published host instance");
        if (host && host->status_warn)
        {
            fprintf(stderr, "status_warn:\n%s\n", host->status_warn);
        }
        FinishLifecycleHost(host, cfg, cache, ws);
        return 1;
    }
    PicoPlugins_Shutdown(host);
    ReadFileStr(life, log, sizeof(log));
    FinishLifecycleHost(host, cfg, cache, ws);
    if (strcmp(log, "H") != 0)
    {
        Fail("host shutdown should run once when the process tears down the kept host slot");
        return 1;
    }
    return 0;
}

static int TestWorkspaceShutdownSeesOwningWorkspace(void)
{
    char cfg[256];
    char cache[256];
    char ws[256];
    char life[512];
    char log[8];
    PicoHost *host = NULL;

    cfg[0] = cache[0] = ws[0] = '\0';
    if (StartLifecycleHost(&host, cfg, cache, ws, life, 0) != 0)
    {
        FinishLifecycleHost(host, cfg, cache, ws);
        return 1;
    }
    pico_host_free(host);
    host = NULL;
    ReadFileStr(life, log, sizeof(log));
    FinishLifecycleHost(NULL, cfg, cache, ws);
    if (log[0] != 'Y')
    {
        Fail("workspace shutdown must receive the owning workspace");
        return 1;
    }
    return 0;
}

static int TestWorkspaceChangeSeesOwningWorkspace(void)
{
    char cfg[256];
    char cache[256];
    char ws[256];
    char ws2[256];
    char life[512];
    char log[16];
    PicoHost *host = NULL;

    cfg[0] = cache[0] = ws[0] = ws2[0] = '\0';
    if (StartLifecycleHost(&host, cfg, cache, ws, life, 0) != 0)
    {
        FinishLifecycleHost(host, cfg, cache, ws);
        return 1;
    }
    snprintf(ws2, sizeof(ws2), "/tmp/pico-ws2-XXXXXX");
    if (!mkdtemp(ws2))
    {
        Fail("mkdtemp ws2");
        FinishLifecycleHost(host, cfg, cache, ws);
        return 1;
    }
    if (!PicoHost_ChangeWorkspace(host, PicoHost_PrimaryWorkspace(host), ws2))
    {
        Fail("request change workspace");
        FinishLifecycleHost(host, cfg, cache, ws);
        RmRf(ws2);
        return 1;
    }
    pico_host_pump(host);
    ReadFileStr(life, log, sizeof(log));
    if (log[0] != '\0')
    {
        Fail("cd must not shut down the previous workspace");
        FinishLifecycleHost(host, cfg, cache, ws);
        RmRf(ws2);
        return 1;
    }
    if (pico_workspace_count(host) != 2)
    {
        Fail("cd must leave both workspaces open");
        FinishLifecycleHost(host, cfg, cache, ws);
        RmRf(ws2);
        return 1;
    }
    pico_host_free(host);
    host = NULL;
    ReadFileStr(life, log, sizeof(log));
    FinishLifecycleHost(NULL, cfg, cache, ws);
    RmRf(ws2);
    if (strcmp(log, "YYH") != 0)
    {
        fprintf(stderr, "actual log: %s\n", log);
        Fail("workspace shutdown must run cleanly for both workspaces without use-after-free");
        return 1;
    }
    return 0;
}

static int TestCdOpensSelectsAndReusesWorkspace(void)
{
    PicoHost *host = NULL;
    char dirA[] = "/tmp/pico-cd-A-XXXXXX";
    char dirB[] = "/tmp/pico-cd-B-XXXXXX";
    PicoWorkspaceId idA = 0;
    PicoWorkspaceId idB = 0;
    PicoWorkspaceId reused = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId agentA = 0;
    PicoAgent *selected;
    PicoWorkspace *wsA;
    PicoWorkspace *wsB;

    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp cd open/select");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init cd open/select");
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK)
    {
        Fail("open A for cd");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, idA, &opt, &agentA) != PICO_OK)
    {
        Fail("create main agent A for cd");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    host->chat_sel.msg = 9;
    host->chat_follow_bottom = false;
    host->hovered_tool = true;
    if (!PicoHost_ChangeWorkspace(host, PicoHost_FindWorkspace(host, idA), dirB))
    {
        Fail("cd to B");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    wsA = PicoHost_FindWorkspace(host, idA);
    selected = PicoHost_SelectedAgent(host);
    wsB = selected ? selected->workspace : NULL;
    if (!wsA || !wsB || wsA == wsB || pico_workspace_count(host) != 2 ||
        wsA->state != PICO_WORKSPACE_OPEN || !PicoWorkspace_AcceptsNewWork(wsA) ||
        !PicoHost_FindAgent(host, agentA) || selected->id == agentA)
    {
        Fail("cd must open B, select a main agent there, and leave A running");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (host->chat_sel.msg != -1 || !host->chat_follow_bottom || host->hovered_tool)
    {
        Fail("cd onto a newly created agent must reset transcript UI state");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    idB = wsB->id;
    if (!PicoHost_ChangeWorkspace(host, wsB, dirA) || pico_workspace_count(host) != 2 ||
        pico_workspace_open(host, dirA, &reused) != PICO_ALREADY_OPEN || reused != idA ||
        PicoHost_SelectedWorkspace(host) != wsA || pico_agent_active(host) != agentA ||
        PicoHost_FindWorkspace(host, idB) != wsB)
    {
        Fail("cd back to A must reuse the open workspace");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}


static int TestBusySlashCommandsOpenBackgroundWithoutJobs(void)
{
    PicoHost *host = NULL;
    char dir[] = "/tmp/pico-bg-busy-XXXXXX";
    PicoWorkspaceId ws_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions options = {
        .kind = PICO_AGENT_MAIN,
        .session_start = PICO_SESSION_NONE,
        .select = true,
    };
    PicoAgent *agent;
    PicoWorkspace *workspace;
    int original_messages;
    int rc = 1;

    if (!mkdtemp(dir))
    {
        Fail("mkdtemp busy background command");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host ||
        pico_workspace_open(host, dir, &ws_id) != PICO_OK ||
        pico_main_agent_create(host, ws_id, &options, &agent_id) != PICO_OK)
    {
        Fail("start busy background command host");
        goto done;
    }
    workspace = PicoHost_FindWorkspace(host, ws_id);
    agent = PicoHost_FindAgent(host, agent_id);
    if (!workspace || !agent || PicoBgTable_RunningCount(PicoWorkspace_Background(workspace), agent_id) != 0)
    {
        Fail("busy background command requires an empty process list");
        goto done;
    }
    original_messages = pico_agent_message_count(host, agent_id);
    agent->state = PICO_AGENT_LLM_WAIT;

    PicoComposer_SetText(host, " /BACKGROUND ");
    PicoHost_Submit(host);
    if (!pico_ui_modal_has(host, "background-list") || host->composer.length != 0 ||
        agent->state != PICO_AGENT_LLM_WAIT || pico_agent_message_count(host, agent_id) != original_messages)
    {
        Fail("/background must open an empty modal without interrupting a streaming turn");
        goto done;
    }
    pico_ui_modal_pop(host, "background-list");

    PicoComposer_SetText(host, "/help");
    PicoHost_Submit(host);
    if (host->composer.length != 0 || pico_agent_message_count(host, agent_id) <= original_messages ||
        agent->state != PICO_AGENT_LLM_WAIT)
    {
        Fail("opted-in informational command must run during streaming");
        goto done;
    }
    original_messages = pico_agent_message_count(host, agent_id);

    PicoComposer_SetText(host, "/new");
    PicoHost_Submit(host);
    if (pico_agent_active(host) != agent_id || host->composer.length != 4 ||
        strcmp(host->composer.text, "/new") != 0 || agent->state != PICO_AGENT_LLM_WAIT ||
        pico_agent_message_count(host, agent_id) != original_messages)
    {
        Fail("unmarked commands must retain the draft and not interrupt streaming");
        goto done;
    }
    rc = 0;
done:
    if (host)
    {
        agent = PicoHost_FindAgent(host, agent_id);
        if (agent && agent->state == PICO_AGENT_LLM_WAIT)
        {
            agent->state = PICO_AGENT_IDLE;
        }
        pico_host_free(host);
    }
    rmdir(dir);
    return rc;
}

static int TestBackgroundJobsSurviveWorkspaceReload(void)
{
    PicoHost *host = NULL;
    char dir[] = "/tmp/pico-bg-rl-XXXXXX";
    PicoWorkspaceId ws_id = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId agent_id = 0;
    PicoWorkspace *ws;
    PicoBgTable *table;
    char *error = NULL;
    char *json;
    char *list;
    int i;
    bool found_tool = false;
    const PicoRegistrationGeneration *reg;

    if (!mkdtemp(dir))
    {
        Fail("mkdtemp background reload");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init background reload");
        rmdir(dir);
        return 1;
    }
    if (pico_workspace_open(host, dir, &ws_id) != PICO_OK)
    {
        Fail("open workspace background reload");
        pico_host_free(host);
        rmdir(dir);
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, ws_id, &opt, &agent_id) != PICO_OK)
    {
        Fail("create agent background reload");
        pico_host_free(host);
        rmdir(dir);
        return 1;
    }
    ws = PicoHost_FindWorkspace(host, ws_id);
    table = PicoWorkspace_Background(ws);
    if (!ws || !table)
    {
        Fail("background table missing before reload");
        pico_host_free(host);
        rmdir(dir);
        return 1;
    }
    json = PicoBgTable_Spawn(table, agent_id, dir, "sleep", "while :; do sleep 3600; done", &error);
    if (!json)
    {
        free(error);
        Fail("spawn before reload");
        pico_host_free(host);
        rmdir(dir);
        return 1;
    }
    free(json);
    PicoHost_RequestReload(host);
    for (i = 0; i < 32; i++)
    {
        pico_host_pump(host);
        if (ws->state == PICO_WORKSPACE_OPEN && PicoWorkspace_AcceptsNewWork(ws))
        {
            break;
        }
    }
    if (ws->state != PICO_WORKSPACE_OPEN || PicoWorkspace_Background(ws) != table)
    {
        Fail("background table must survive workspace reload");
        pico_host_free(host);
        rmdir(dir);
        return 1;
    }
    list = PicoBgTable_ListJson(table, agent_id);
    if (!list || !strstr(list, "running") || PicoBgTable_RunningCount(table, agent_id) < 1)
    {
        free(list);
        Fail("background job did not stay running across reload");
        pico_host_free(host);
        rmdir(dir);
        return 1;
    }
    free(list);
    reg = PicoWorkspace_RegistrationActiveConst(ws);
    for (i = 0; reg && i < reg->tool_count; i++)
    {
        if (reg->tools[i].name && strcmp(reg->tools[i].name, "run_background") == 0)
        {
            found_tool = true;
            break;
        }
    }
    if (!found_tool)
    {
        Fail("run_background must re-register after reload");
        pico_host_free(host);
        rmdir(dir);
        return 1;
    }
    pico_host_free(host);
    rmdir(dir);
    return 0;
}

static int TestReloadTargetsSelectedWorkspace(void)
{
    PicoHost *host = NULL;
    char dirA[] = "/tmp/pico-rl-A-XXXXXX";
    char dirB[] = "/tmp/pico-rl-B-XXXXXX";
    PicoWorkspaceId idA = 0;
    PicoWorkspaceId idB = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId agentA = 0;
    PicoAgentId agentB = 0;
    PicoWorkspace *wsA;
    PicoWorkspace *wsB;

    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp reload selected");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init reload selected");
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK || pico_workspace_open(host, dirB, &idB) != PICO_OK)
    {
        Fail("open workspaces for reload selected");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, idA, &opt, &agentA) != PICO_OK)
    {
        Fail("create agent A for reload selected");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    opt.select = false;
    if (pico_main_agent_create(host, idB, &opt, &agentB) != PICO_OK)
    {
        Fail("create agent B for reload selected");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (!pico_agent_select(host, agentA))
    {
        Fail("select agent A for reload");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    PicoHost_RequestReload(host);
    wsA = PicoHost_FindWorkspace(host, idA);
    wsB = PicoHost_FindWorkspace(host, idB);
    if (!wsA || !wsB || wsA->state != PICO_WORKSPACE_RELOADING || PicoWorkspace_AcceptsNewWork(wsA) ||
        wsB->state != PICO_WORKSPACE_OPEN || !PicoWorkspace_AcceptsNewWork(wsB) || !host->reload_queued)
    {
        Fail("reload must target the selected workspace without pausing others");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    pico_host_pump(host);
    if (host->reload_queued || wsA->state != PICO_WORKSPACE_OPEN || !PicoWorkspace_AcceptsNewWork(wsA) ||
        wsB->state != PICO_WORKSPACE_OPEN || !PicoWorkspace_AcceptsNewWork(wsB))
    {
        Fail("selected workspace reload must not block the other workspace");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestSkillSubmissionPreservesFileMentions(void)
{
    char root[] = "/tmp/pico-skill-submit-XXXXXX";
    char skill_dir[1024], skill_path[1200], file_path[1024];
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    int failed = 1;
    if (!mkdtemp(root))
    {
        Fail("mkdtemp skill submission");
        return 1;
    }
    snprintf(skill_dir, sizeof(skill_dir), "%s/.pico/skills/review", root);
    snprintf(skill_path, sizeof(skill_path), "%s/SKILL.md", skill_dir);
    snprintf(file_path, sizeof(file_path), "%s/README.md", root);
    if (MkdirParents(skill_dir) != 0 ||
        WriteFile(skill_path, "---\nname: review\ndescription: Review files.\n---\n"
                             "Check the implementation carefully.\n") != 0 ||
        WriteFile(file_path, "File content for review.\n") != 0 ||
        pico_host_init(&host, NULL, true) != PICO_OK || !host ||
        pico_workspace_open(host, root, &workspace_id) != PICO_OK)
    {
        Fail("setup skill submission");
        goto done;
    }
    PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN,
                                  .session_start = PICO_SESSION_NONE, .select = true};
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK)
    {
        Fail("create skill submission agent");
        goto done;
    }
    PicoComposer_SetText(host, "/skill review review @README.md");
    /* Exercise the actual command and file hooks without starting a provider. */
    pico_run_hooks(host, PICO_HOOK_BEFORE_SUBMIT, agent_id);
    if (host->submit_cancel || !host->agent_input ||
        !strstr(host->agent_input, "Check the implementation carefully.") ||
        !strstr(host->agent_input, "File content for review.") ||
        strcmp(host->composer.text, "/skill review review @README.md") != 0)
    {
        Fail("skill submission must keep skill instructions, mentioned file, and display text");
        goto done;
    }
    failed = 0;
done:
    if (host) pico_host_free(host);
    unlink(skill_path);
    unlink(file_path);
    rmdir(skill_dir);
    snprintf(skill_dir, sizeof(skill_dir), "%s/.pico/skills", root);
    rmdir(skill_dir);
    snprintf(skill_dir, sizeof(skill_dir), "%s/.pico", root);
    rmdir(skill_dir);
    rmdir(root);
    return failed;
}

static PicoAgentId g_cd_other_agent;

static void SelectOtherBeforeCd(PicoWorkspace *workspace, const PicoHookEvent *event, void *state)
{
    PicoHost *host = workspace ? workspace->host : NULL;
    (void)event;
    (void)state;
    if (host && g_cd_other_agent)
    {
        pico_agent_select(host, g_cd_other_agent);
    }
}

static bool PrependWorkspaceSubmitHook(PicoWorkspace *ws, PicoWorkspaceHookFn fn)
{
    PicoRegistrationGeneration *reg = ws ? ws->active_registration : NULL;
    if (!reg || reg->hook_count >= PICO_MAX_HOOKS)
    {
        return false;
    }
    memmove(&reg->hooks[1], &reg->hooks[0], (size_t)reg->hook_count * sizeof(reg->hooks[0]));
    memset(&reg->hooks[0], 0, sizeof(reg->hooks[0]));
    reg->hooks[0].hook = PICO_HOOK_BEFORE_SUBMIT;
    reg->hooks[0].workspace_fn = fn;
    reg->hooks[0].workspace = ws;
    reg->hook_count++;
    return true;
}

static int TestHostReloadIgnoresWorkspaceLocalCompileFailure(void)
{
    char cfg[256];
    char cache[256];
    char dirA[] = "/tmp/pico-hrl-A-XXXXXX";
    char dirB[] = "/tmp/pico-hrl-B-XXXXXX";
    char ext_dir[1024];
    char src[2048];
    PicoHost *host = NULL;
    PicoWorkspaceId idA = 0;
    PicoWorkspaceId idB = 0;

    snprintf(cfg, sizeof(cfg), "/tmp/pico-cfg-XXXXXX");
    snprintf(cache, sizeof(cache), "/tmp/pico-cache-XXXXXX");
    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp host reload isolation");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    if (pico_host_init(&host, NULL, false) != PICO_OK || !host)
    {
        Fail("host init host reload isolation");
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        RmRf(cfg);
        RmRf(cache);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK || pico_workspace_open(host, dirB, &idB) != PICO_OK)
    {
        Fail("open workspaces host reload isolation");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        RmRf(cfg);
        RmRf(cache);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    snprintf(ext_dir, sizeof(ext_dir), "%s/.pico/extensions", dirB);
    snprintf(src, sizeof(src), "%s/broken_ws.c", ext_dir);
    if (MkdirParents(ext_dir) != 0 || WriteFile(src, "this is not valid C {\n") != 0)
    {
        Fail("write broken workspace-local extension");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        RmRf(cfg);
        RmRf(cache);
        RmRf(dirA);
        RmRf(dirB);
        return 1;
    }
    if (!WaitHostReload(host))
    {
        Fail("host reload must succeed when another workspace's local extension fails to compile");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        RmRf(cfg);
        RmRf(cache);
        RmRf(dirA);
        RmRf(dirB);
        return 1;
    }
    if (host->status_warn && strstr(host->status_warn, "broken_ws.c"))
    {
        Fail("host reload must not compile workspace-local sources");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        RmRf(cfg);
        RmRf(cache);
        RmRf(dirA);
        RmRf(dirB);
        return 1;
    }
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(dirA);
    RmRf(dirB);
    return 0;
}

static int TestCdResolvesAgainstCommandWorkspace(void)
{
    PicoHost *host = NULL;
    char dirA[] = "/tmp/pico-cdrel-A-XXXXXX";
    char dirB[] = "/tmp/pico-cdrel-B-XXXXXX";
    char childA[4096];
    char childB[4096];
    PicoWorkspaceId idA = 0;
    PicoWorkspaceId idB = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId agentA = 0;
    PicoAgentId agentB = 0;
    PicoWorkspace *wsA;
    PicoAgent *selected;
    const char *selected_path;

    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp cd relative");
        return 1;
    }
    snprintf(childA, sizeof(childA), "%s/child", dirA);
    snprintf(childB, sizeof(childB), "%s/child", dirB);
    if (mkdir(childA, 0700) != 0 || mkdir(childB, 0700) != 0)
    {
        Fail("mkdir cd relative children");
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init cd relative");
        rmdir(childA);
        rmdir(childB);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK || pico_workspace_open(host, dirB, &idB) != PICO_OK)
    {
        Fail("open workspaces cd relative");
        pico_host_free(host);
        rmdir(childA);
        rmdir(childB);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, idA, &opt, &agentA) != PICO_OK)
    {
        Fail("create agent A cd relative");
        pico_host_free(host);
        rmdir(childA);
        rmdir(childB);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    opt.select = false;
    if (pico_main_agent_create(host, idB, &opt, &agentB) != PICO_OK || !pico_agent_select(host, agentA))
    {
        Fail("create agent B cd relative");
        pico_host_free(host);
        rmdir(childA);
        rmdir(childB);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    wsA = PicoHost_FindWorkspace(host, idA);
    g_cd_other_agent = agentB;
    if (!wsA || !PrependWorkspaceSubmitHook(wsA, SelectOtherBeforeCd))
    {
        Fail("prepend cd submit hook");
        pico_host_free(host);
        rmdir(childA);
        rmdir(childB);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    PicoComposer_SetText(host, "/cd child");
    PicoHost_Submit(host);
    selected = PicoHost_SelectedAgent(host);
    selected_path = PicoWorkspace_Path(selected ? selected->workspace : NULL);
    if (!selected || strcmp(selected_path, childA) != 0)
    {
        Fail("/cd relative path must resolve against the command workspace after a selection change");
        pico_host_free(host);
        rmdir(childA);
        rmdir(childB);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    pico_host_free(host);
    rmdir(childA);
    rmdir(childB);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestCdRejectsClosingWorkspace(void)
{
    PicoHost *host = NULL;
    char dirA[] = "/tmp/pico-cdcls-A-XXXXXX";
    char dirB[] = "/tmp/pico-cdcls-B-XXXXXX";
    PicoWorkspaceId idA = 0;
    PicoWorkspaceId idB = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId agentA = 0;
    PicoAgentId agentB = 0;
    PicoWorkspace *wsB;
    PicoAgentId selected_before;

    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp cd closing");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init cd closing");
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK || pico_workspace_open(host, dirB, &idB) != PICO_OK)
    {
        Fail("open workspaces cd closing");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, idA, &opt, &agentA) != PICO_OK)
    {
        Fail("create agent A cd closing");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    opt.select = false;
    if (pico_main_agent_create(host, idB, &opt, &agentB) != PICO_OK)
    {
        Fail("create agent B cd closing");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    wsB = PicoHost_FindWorkspace(host, idB);
    if (!wsB || pico_workspace_request_close(host, idB) != PICO_OK || wsB->state != PICO_WORKSPACE_CLOSING)
    {
        Fail("close B for cd closing");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    selected_before = pico_agent_active(host);
    if (PicoHost_ChangeWorkspace(host, PicoHost_FindWorkspace(host, idA), dirB) ||
        pico_agent_active(host) != selected_before || pico_workspace_count(host) != 2)
    {
        Fail("cd must not select an agent in a closing workspace");
        pico_host_free(host);
        rmdir(dirA);
        rmdir(dirB);
        return 1;
    }
    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestModelChangeDoesNotMutateWorkspaceDefault(void)
{
    PicoWorkspace ws;
    PicoAgent agent;
    PicoModel models[2];
    memset(&ws, 0, sizeof(ws));
    memset(&agent, 0, sizeof(agent));
    memset(models, 0, sizeof(models));

    snprintf(models[0].id, sizeof(models[0].id), "original-default-model");
    snprintf(models[1].id, sizeof(models[1].id), "custom-agent-model");
    ws.models = models;
    ws.model_count = 2;
    snprintf(ws.settings.default_model, sizeof(ws.settings.default_model), "original-default-model");
    agent.workspace = &ws;

    PicoSettings_InitAgent(&agent);
    if (strcmp(agent.model_name, "original-default-model") != 0)
    {
        Fail("agent should initialize with workspace default model");
        return 1;
    }

    PicoSettings_SetModel(&agent, "custom-agent-model");
    if (strcmp(agent.model_name, "custom-agent-model") != 0)
    {
        Fail("agent model should update to custom model");
        return 1;
    }
    if (strcmp(ws.settings.default_model, "original-default-model") != 0)
    {
        Fail("PicoSettings_SetModel must not mutate workspace default_model");
        return 1;
    }
    return 0;
}

static int TestWorkspacePluginIsolation(void)
{
    char ws1_dir[] = "/tmp/pico-ws-iso1-XXXXXX";
    char ws2_dir[] = "/tmp/pico-ws-iso2-XXXXXX";
    if (!mkdtemp(ws1_dir) || !mkdtemp(ws2_dir))
    {
        Fail("mkdtemp ws iso");
        return 1;
    }
    PicoHost *host1 = NULL;
    PicoHost *host2 = NULL;
    PicoWorkspaceId id1 = 0;
    PicoWorkspaceId id2 = 0;
    if (pico_host_init(&host1, NULL, true) != PICO_OK || !host1 ||
        pico_host_init(&host2, NULL, true) != PICO_OK || !host2)
    {
        Fail("host_init iso");
        if (host1) pico_host_free(host1);
        if (host2) pico_host_free(host2);
        rmdir(ws1_dir);
        rmdir(ws2_dir);
        return 1;
    }
    if (pico_workspace_open(host1, ws1_dir, &id1) != PICO_OK ||
        pico_workspace_open(host2, ws2_dir, &id2) != PICO_OK)
    {
        Fail("workspace_open iso");
        pico_host_free(host1);
        pico_host_free(host2);
        rmdir(ws1_dir);
        rmdir(ws2_dir);
        return 1;
    }
    WaitPluginLoad(host1);
    WaitPluginLoad(host2);
    PicoWorkspace *ws1 = PicoHost_PrimaryWorkspace(host1);
    PicoWorkspace *ws2 = PicoHost_PrimaryWorkspace(host2);
    void *files1 = PicoPlugins_WorkspaceState(ws1, "files");
    void *diff1 = PicoPlugins_WorkspaceState(ws1, "diff");
    void *todo1 = PicoPlugins_WorkspaceState(ws1, "todos");
    void *files2 = PicoPlugins_WorkspaceState(ws2, "files");
    void *diff2 = PicoPlugins_WorkspaceState(ws2, "diff");
    void *todo2 = PicoPlugins_WorkspaceState(ws2, "todos");
    if (!files1 || !diff1 || !todo1 || !files2 || !diff2 || !todo2)
    {
        Fail("workspace plugins must be initialized on both workspaces");
        pico_host_free(host1);
        pico_host_free(host2);
        rmdir(ws1_dir);
        rmdir(ws2_dir);
        return 1;
    }
    if (files1 == files2 || diff1 == diff2 || todo1 == todo2)
    {
        Fail("workspace plugin states must be isolated per-workspace instance");
        pico_host_free(host1);
        pico_host_free(host2);
        rmdir(ws1_dir);
        rmdir(ws2_dir);
        return 1;
    }

    pico_host_free(host1);
    pico_host_free(host2);
    rmdir(ws1_dir);
    rmdir(ws2_dir);
    return 0;
}

static int TestHostPluginIsolation(void)
{
    char cfg1[] = "/tmp/pico-cfg1-XXXXXX";
    char cfg2[] = "/tmp/pico-cfg2-XXXXXX";
    char cache1[] = "/tmp/pico-cache1-XXXXXX";
    char cache2[] = "/tmp/pico-cache2-XXXXXX";
    if (!mkdtemp(cfg1) || !mkdtemp(cfg2) || !mkdtemp(cache1) || !mkdtemp(cache2))
    {
        Fail("mkdtemp host iso");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg1, 1);
    setenv("XDG_CACHE_HOME", cache1, 1);
    PicoHost *host1 = NULL;
    if (pico_host_init(&host1, NULL, true) != PICO_OK || !host1)
    {
        Fail("host1 init");
        return 1;
    }
    WaitPluginLoad(host1);

    setenv("XDG_CONFIG_HOME", cfg2, 1);
    setenv("XDG_CACHE_HOME", cache2, 1);
    PicoHost *host2 = NULL;
    if (pico_host_init(&host2, NULL, true) != PICO_OK || !host2)
    {
        Fail("host2 init");
        pico_host_free(host1);
        return 1;
    }
    WaitPluginLoad(host2);

    void *comp1 = PicoPlugins_HostState(host1, "composer");
    void *comp2 = PicoPlugins_HostState(host2, "composer");
    void *chat1 = PicoPlugins_HostState(host1, "chat");
    void *chat2 = PicoPlugins_HostState(host2, "chat");
    if (!comp1 || !comp2 || !chat1 || !chat2 || comp1 == comp2 || chat1 == chat2)
    {
        Fail("host plugins must have distinct per-host instances without global fallback");
        pico_host_free(host1);
        pico_host_free(host2);
        return 1;
    }
    PicoSettingsUi_Open(host1);
    if (!PicoSettingsUi_IsOpen(host1) || PicoSettingsUi_IsOpen(host2) ||
        !pico_ui_modal_is_top(host1, "settings") || pico_ui_modal_claimed(host2))
    {
        Fail("settings modal must route open state to the requested host");
        pico_host_free(host1);
        pico_host_free(host2);
        return 1;
    }
    PicoSettingsUi_Close(host1);
    if (pico_ui_modal_claimed(host1))
    {
        Fail("settings modal must close on the requested host");
        pico_host_free(host1);
        pico_host_free(host2);
        return 1;
    }

    pico_host_free(host1);
    pico_host_free(host2);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg1);
    RmRf(cfg2);
    RmRf(cache1);
    RmRf(cache2);
    return 0;
}

static int TestSettingsModalWheelOverField(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    char cfg[] = "/tmp/pico-cfg-settings-wheel-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    char config_dir[512];
    char settings_path[512];
    char json[4096];
    size_t n = 0;
    int i;
    int rc = 1;
    Clay_Arena arena;
    Clay_ElementData field;
    Clay_ScrollContainerData scroll;
    float before_y;

    if (!memory || !mkdtemp(cfg))
    {
        free(memory);
        Fail("settings field wheel test setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    snprintf(config_dir, sizeof(config_dir), "%s/pico", cfg);
    snprintf(settings_path, sizeof(settings_path), "%s/pico/settings.json", cfg);
    Pico_MkdirP(config_dir);
    n += (size_t)snprintf(json + n, sizeof(json) - n,
                          "{\n  \"model\": \"model-00\",\n  \"models\": [\n");
    for (i = 0; i < 24; i++)
    {
        n += (size_t)snprintf(json + n, sizeof(json) - n,
                              "    {\"id\": \"model-%02d\", \"name\": \"Model %02d\", \"provider\": \"openai\"}%s\n",
                              i, i, i == 23 ? "" : ",");
    }
    n += (size_t)snprintf(json + n, sizeof(json) - n, "  ]\n}\n");
    if (n >= sizeof(json) || WriteFile(settings_path, json) != 0)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        RmRf(cfg);
        Fail("write settings field wheel fixture");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        RmRf(cfg);
        Fail("settings field wheel host initialization");
        return 1;
    }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;

    arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        pico_host_free(host);
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        RmRf(cfg);
        Clay_SetCurrentContext(previous);
        Fail("settings field wheel Clay initialization");
        return 1;
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);
#ifdef PICO_CLAY_FRAME_FAULT_TESTS
    g_clay_frame_test = true;
#endif
    PicoSettingsUi_Open(host);
    if (!PicoSettingsUi_IsOpen(host) || !pico_ui_modal_is_top(host, "settings"))
    {
        Fail("settings modal must open");
        goto done;
    }

    Clay_SetLayoutDimensions(viewport);
    Clay_SetPointerState((Clay_Vector2){0, 0}, false);
    (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);

    field = Clay_GetElementData(CLAY_ID("SettingsContextLimit"));
    scroll = Clay_GetScrollContainerData(CLAY_ID("SettingsModalScroll"));
    if (!field.found || !scroll.found || !scroll.scrollPosition)
    {
        Fail("settings modal field and scroller were not laid out");
        goto done;
    }
    if (scroll.contentDimensions.height <= scroll.scrollContainerDimensions.height + 0.5f)
    {
        Fail("settings modal must overflow so a wheel can move it");
        goto done;
    }

    Clay_SetPointerState((Clay_Vector2){field.boundingBox.x + field.boundingBox.width / 2.0f,
                                        field.boundingBox.y + field.boundingBox.height / 2.0f},
                         false);
    (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
    scroll = Clay_GetScrollContainerData(CLAY_ID("SettingsModalScroll"));
    if (!scroll.found || !scroll.scrollPosition)
    {
        Fail("settings scroller disappeared after hover layout");
        goto done;
    }
    before_y = scroll.scrollPosition->y;
    if (!PicoSettingsUi_ScrollHovered(host, -3.0f) ||
        scroll.scrollPosition->y >= before_y - 0.5f)
    {
        Fail("hovering a settings text field must still scroll the modal");
        goto done;
    }
    rc = 0;

done:
#ifdef PICO_CLAY_FRAME_FAULT_TESTS
    g_clay_frame_test = false;
#endif
    Clay_SetCurrentContext(previous);
    pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    RmRf(cfg);
    return rc;
}

static int TestHostSettingsPersistence(void)
{
    static const char legacy_settings[] =
        "{\n  \"font_scale\": 2.0,\n  \"chat_width\": 100,\n"
        "  \"disabled_host_extensions\": [\"footer\"]\n}\n";
    char cfg[] = "/tmp/pico-pref-cfg-XXXXXX";
    char cache[] = "/tmp/pico-pref-cache-XXXXXX";
    char ws_dir[] = "/tmp/pico-pref-ws-XXXXXX";
    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(ws_dir))
    {
        Fail("mkdtemp pref");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);

    char config_dir[512];
    char settings_path[512];
    char legacy_path[512];
    snprintf(config_dir, sizeof(config_dir), "%s/pico", cfg);
    snprintf(settings_path, sizeof(settings_path), "%s/pico/settings.json", cfg);
    snprintf(legacy_path, sizeof(legacy_path), "%s/pico/host_preferences.json", cfg);
    Pico_MkdirP(config_dir);
    if (WriteFile(settings_path, "{\n  \"model\": \"keep-me\",\n  \"chat_width\": 110,\n  \"font_scale\": 1.25\n}\n") != 0 ||
        WriteFile(legacy_path, legacy_settings) != 0 || chmod(settings_path, 0640) != 0)
    {
        Fail("write unified settings fixtures");
        return 1;
    }
    unsetenv("PICO_FONT_SCALE");
    unsetenv("PICO_CHAT_WIDTH");

    PicoHost *host = NULL;
    PicoWorkspaceId id = 0;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("pref host init");
        return 1;
    }
    if (host->preferences.font_scale != 1.25 || host->preferences.chat_width != 110 ||
        host->preferences.disabled_host_extension_count != 0)
    {
        Fail("host_preferences.json must not be read");
        pico_host_free(host);
        return 1;
    }
    if (pico_workspace_open(host, ws_dir, &id) != PICO_OK)
    {
        Fail("pref ws open");
        pico_host_free(host);
        return 1;
    }
    WaitPluginLoad(host);

    int ext_count = PicoPlugins_Count(host);
    int target_idx = -1;
    for (int i = 0; i < ext_count; i++)
    {
        PicoExtInfo info;
        if (PicoPlugins_Get(host, i, &info) && info.name && strcmp(info.name, "footer") == 0)
        {
            target_idx = i;
            break;
        }
    }
    if (target_idx < 0)
    {
        Fail("find footer extension");
        pico_host_free(host);
        return 1;
    }

    if (!PicoPlugins_SetEnabled(host, target_idx, false))
    {
        Fail("PicoPlugins_SetEnabled to false");
        pico_host_free(host);
        return 1;
    }

    char settings_content[8192];
    ReadFileStr(settings_path, settings_content, sizeof(settings_content));
    struct stat settings_stat;
    if (!strstr(settings_content, "disabled_host_extensions") || !strstr(settings_content, "footer") ||
        !strstr(settings_content, "keep-me") || stat(settings_path, &settings_stat) != 0 ||
        (settings_stat.st_mode & 0777) != 0640)
    {
        Fail("host plugin persistence must preserve unified settings content and mode");
        pico_host_free(host);
        return 1;
    }

    char legacy_content[1024];
    ReadFileStr(legacy_path, legacy_content, sizeof(legacy_content));
    if (strcmp(legacy_content, legacy_settings) != 0)
    {
        Fail("host_preferences.json must not be written");
        pico_host_free(host);
        return 1;
    }

    char ws_settings_path[512];
    snprintf(ws_settings_path, sizeof(ws_settings_path), "%s/.pico/settings.json", ws_dir);
    if (access(ws_settings_path, F_OK) == 0)
    {
        char content[1024];
        ReadFileStr(ws_settings_path, content, sizeof(content));
        if (strstr(content, "disabled_extensions") || strstr(content, "footer"))
        {
            Fail("disabling host plugin must NOT write disabled_extensions to workspace settings.json");
            pico_host_free(host);
            return 1;
        }
    }

    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(ws_dir);
    return 0;
}

static void DummyHostView(PicoHost *h, void *s) { (void)h; (void)s; }
static void DummyWsView(PicoWorkspace *w, PicoAgentId a, void *s) { (void)w; (void)a; (void)s; }
static void DummyHostHook(PicoHost *h, const PicoHookEvent *e, void *s) { (void)h; (void)e; (void)s; }
static void DummyWsHook(PicoWorkspace *w, const PicoHookEvent *e, void *s) { (void)w; (void)e; (void)s; }
static void DummyTool(PicoAgentContext *c, const char *a, PicoToolResult *o, void *s) { (void)c; (void)a; (void)o; (void)s; }
static void DummyHostCmd(PicoHost *h, PicoAgentId a, const char *args, void *s) { (void)h; (void)a; (void)args; (void)s; }
static void DummyWsCmd(PicoWorkspace *w, PicoAgentId a, const char *args, void *s) { (void)w; (void)a; (void)args; (void)s; }
static int DummyHostQuery(PicoHost *h, const char *p, PicoCompleteItem *o, int m, void *s) { (void)h; (void)p; (void)o; (void)m; (void)s; return 0; }
static int DummyWsQuery(PicoWorkspace *w, const char *p, PicoCompleteItem *o, int m, void *s) { (void)w; (void)p; (void)o; (void)m; (void)s; return 0; }
static void DummyAuthLogin(PicoHost *h, PicoAgentId a, const char *args, void *s) { (void)h; (void)a; (void)args; (void)s; }

static int TestScopeEnforcement(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&host);

    /* 1. In Host Init scope */
    PicoHost_BeginRegistration(&host, PICO_REG_HOST, NULL);

    /* Workspace registrations must be rejected during host init */
    if (pico_add_tool(ws, "invalid_tool", "desc", "{}", DummyTool, NULL, PICO_TOOL_SEQUENTIAL))
    {
        Fail("pico_add_tool must be rejected during host init");
        free(host.workspaces[0]);
        return 1;
    }
    pico_workspace_add_view(ws, PICO_SLOT_SIDEBAR, 0, DummyWsView);
    pico_workspace_add_empty_view(ws, PICO_EMPTY_ABOVE, 0, DummyWsView);
    pico_workspace_add_command(ws, "invalid_cmd", "help", DummyWsCmd);
    pico_workspace_add_completer(ws, '#', false, DummyWsQuery, NULL);
    pico_workspace_add_hook(ws, PICO_HOOK_BEFORE_SUBMIT, DummyWsHook);
    pico_add_tool_before_hook(ws, NULL);
    pico_add_tool_after_hook(ws, NULL);
    pico_add_llm_hook(ws, NULL);
    pico_add_context_hook(ws, NULL);
    pico_add_tool_row_hook(ws, NULL);

    if (ws->tool_count > 0 || ws->view_count[PICO_SLOT_SIDEBAR] > 0 || ws->empty_view_count > 0 ||
        ws->command_count > 0 || ws->completer_count > 0 || ws->hook_count > 0 ||
        ws->tool_before_hook_count > 0 || ws->tool_after_hook_count > 0 || ws->llm_hook_count > 0 ||
        ws->context_hook_count > 0 || ws->tool_row_hook_count > 0 ||
        host.staging.ws_tool_count > 0)
    {
        Fail("workspace registrations during host init must not mutate workspace or staging state");
        free(host.workspaces[0]);
        return 1;
    }
    if (!host.status_warn)
    {
        Fail("workspace registrations during host init must generate warnings");
        free(host.workspaces[0]);
        return 1;
    }
    PicoHost_DiscardRegistration(&host);
    free(host.status_warn);
    host.status_warn = NULL;

    /* 2. In Workspace Init scope */
    PicoHost_BeginRegistration(&host, PICO_REG_WORKSPACE, ws);

    /* Host registrations must be rejected during workspace init */
    pico_host_add_view(&host, PICO_SLOT_SIDEBAR, 0, DummyHostView);
    pico_host_add_command(&host, "invalid_hcmd", "help", DummyHostCmd);
    pico_host_add_completer(&host, '#', false, DummyHostQuery, NULL);
    pico_add_auth(&host, &(PicoAuth){.provider = "test", .login = DummyAuthLogin});
    pico_host_add_hook(&host, PICO_HOOK_AFTER_LAYOUT, DummyHostHook);

    /* Workspace cannot register AFTER_LAYOUT or AFTER_RENDER */
    pico_workspace_add_hook(ws, PICO_HOOK_AFTER_LAYOUT, DummyWsHook);
    pico_workspace_add_hook(ws, PICO_HOOK_AFTER_RENDER, DummyWsHook);

    if (host.view_count[PICO_SLOT_SIDEBAR] > 0 || host.command_count > 0 || host.completer_count > 0 ||
        host.auth_count > 0 || host.hook_count > 0 || host.staging.host_view_count[PICO_SLOT_SIDEBAR] > 0)
    {
        Fail("host registrations during workspace init must not mutate host state");
        free(host.workspaces[0]);
        return 1;
    }
    if (!host.status_warn)
    {
        Fail("host registrations during workspace init must generate warnings");
        free(host.workspaces[0]);
        return 1;
    }
    PicoHost_DiscardRegistration(&host);
    free(host.status_warn);
    host.status_warn = NULL;

    /* 3. Outside of any init (PICO_REG_NONE) */
    pico_host_add_command(&host, "unscoped_hcmd", "help", DummyHostCmd);
    pico_workspace_add_command(ws, "unscoped_wcmd", "help", DummyWsCmd);
    if (host.command_count > 0 || ws->command_count > 0)
    {
        Fail("registrations outside init must not mutate host or workspace");
        free(host.workspaces[0]);
        return 1;
    }

    free(host.workspaces[0]);
    return 0;
}

typedef struct RollbackState {
    bool freed;
} RollbackState;

static int FailingWorkspaceInit(PicoWorkspace *ws, void **state_out)
{
    RollbackState *s = (RollbackState *)calloc(1, sizeof(RollbackState));
    *state_out = s;
    pico_add_tool(ws, "rollback_tool", "desc", "{}", DummyTool, NULL, PICO_TOOL_SEQUENTIAL);
    pico_workspace_add_command(ws, "rollback_cmd", "help", DummyWsCmd);
    return -1;
}

static void RollbackWorkspaceShutdown(PicoWorkspace *ws, void *state)
{
    (void)ws;
    RollbackState *s = (RollbackState *)state;
    if (s)
    {
        s->freed = true;
        free(s);
    }
}

static int TestStagingRollbackOnFailedInit(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&host);

    int init_tools = ws->tool_count;
    int init_cmds = ws->command_count;

    PicoExt ext = {
        .abi = PICO_EXT_ABI,
        .name = "failing_ext",
        .workspace_init = FailingWorkspaceInit,
        .workspace_shutdown = RollbackWorkspaceShutdown,
    };

    void *state = NULL;
    PicoHost_BeginRegistration(&host, PICO_REG_WORKSPACE, ws);
    int rc = ext.workspace_init(ws, &state);
    if (rc != 0)
    {
        PicoHost_DiscardRegistration(&host);
        if (state && ext.workspace_shutdown)
        {
            ext.workspace_shutdown(ws, state);
        }
    }
    else
    {
        PicoHost_PublishRegistration(&host, state);
    }

    if (ws->tool_count != init_tools || ws->command_count != init_cmds)
    {
        Fail("staged registrations must be rolled back on init failure");
        free(host.workspaces[0]);
        return 1;
    }

    free(host.workspaces[0]);
    return 0;
}

typedef struct ReloadOwnerState {
    PicoWorkspace *workspace;
} ReloadOwnerState;

static int g_host_old_shutdowns;
static int g_host_candidate_shutdowns;
static int g_workspace_reload_inits;
static int g_workspace_reload_shutdowns;
static bool g_workspace_reload_command_ok;
static bool g_workspace_reload_staging_isolated;
static PicoWorkspace *g_expected_reload_workspace;
static PicoRegistrationGeneration *g_expected_old_registration;

static void ReloadHostCommand(PicoHost *host, PicoAgentId agent_id, const char *args, void *state)
{
    (void)host;
    (void)agent_id;
    (void)args;
    (void)state;
}

static void OldHostShutdown(PicoHost *host, void *state)
{
    (void)host;
    (void)state;
    g_host_old_shutdowns++;
}

static int CandidateHostInit(PicoHost *host, void **state_out)
{
    int *state = (int *)malloc(sizeof(*state));
    if (!state)
    {
        return -1;
    }
    *state = 42;
    *state_out = state;
    pico_host_add_command(host, "candidate_host_command", "candidate", ReloadHostCommand);
    return 0;
}

static void CandidateHostShutdown(PicoHost *host, void *state)
{
    (void)host;
    g_host_candidate_shutdowns++;
    free(state);
}

static int FailingHostReloadInit(PicoHost *host, void **state_out)
{
    (void)host;
    (void)state_out;
    return -1;
}

static int TestFailedHostReloadPreservesLiveInstances(void)
{
    PicoHost host;
    PicoModuleGeneration old_module;
    PicoModuleGeneration candidates[2];
    int old_state = 7;

    memset(&host, 0, sizeof(host));
    memset(&old_module, 0, sizeof(old_module));
    memset(candidates, 0, sizeof(candidates));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *workspace = PicoHost_PrimaryWorkspace(&host);

    old_module.ext.name = "old_host_extension";
    old_module.ext.host_shutdown = OldHostShutdown;
    old_module.ref_count = 1; /* active instance */
    snprintf(host.host_plugins[0].name, sizeof(host.host_plugins[0].name), "%s",
             old_module.ext.name);
    host.host_plugins[0].state = &old_state;
    host.host_plugins[0].module = &old_module;
    host.host_plugins[0].initialized = true;
    host.host_plugin_count = 1;
    host.commands[0] = (PicoCommand){
        .name = "old_host_command",
        .host_run = ReloadHostCommand,
        .state = &old_state,
    };
    host.command_count = 1;

    /* Host-only reload must not clear workspace registrations either. */
    workspace->commands[0] = (PicoCommand){.name = "workspace_sentinel"};
    workspace->command_count = 1;

    candidates[0].ext.name = "candidate_host_extension";
    candidates[0].ext.host_init = CandidateHostInit;
    candidates[0].ext.host_shutdown = CandidateHostShutdown;
    candidates[0].desired = true;
    candidates[0].ref_count = 1; /* module store */
    candidates[1].ext.name = "failing_host_extension";
    candidates[1].ext.host_init = FailingHostReloadInit;
    candidates[1].desired = true;
    candidates[1].ref_count = 1; /* module store */
    host.modules = candidates;
    host.module_count = 2;
    host.module_capacity = 2;

    g_host_old_shutdowns = 0;
    g_host_candidate_shutdowns = 0;
    bool reloaded = PicoHostExtensions_Reload(&host);
    bool preserved = !reloaded && g_host_old_shutdowns == 0 &&
                     g_host_candidate_shutdowns == 1 && host.host_plugin_count == 1 &&
                     host.host_plugins[0].initialized &&
                     host.host_plugins[0].module == &old_module &&
                     host.host_plugins[0].state == &old_state && host.command_count == 1 &&
                     host.commands[0].state == &old_state && workspace->command_count == 1 &&
                     strcmp(workspace->commands[0].name, "workspace_sentinel") == 0;

    PicoHostExtensions_Shutdown(&host);
    for (int i = 0; i < 2; i++)
    {
        candidates[i].desired = false;
        PicoModule_Release(&candidates[i]);
    }
    host.workspaces[0] = NULL;
    host.workspace_count = 0;
    PicoWorkspace_Free(workspace);

    if (!preserved)
    {
        Fail("failed host reload must shut down only staged instances and restore live state");
        return 1;
    }
    return 0;
}

static void ReloadWorkspaceCommand(PicoWorkspace *workspace, PicoAgentId agent_id,
                                   const char *args, void *state)
{
    (void)agent_id;
    (void)args;
    ReloadOwnerState *owner = (ReloadOwnerState *)state;
    g_workspace_reload_command_ok = workspace == g_expected_reload_workspace && owner &&
                                    owner->workspace == g_expected_reload_workspace;
}

static int LiveWorkspaceReloadInit(PicoWorkspace *workspace, void **state_out)
{
    if (workspace != g_expected_reload_workspace)
    {
        return -1;
    }
    ReloadOwnerState *state = (ReloadOwnerState *)calloc(1, sizeof(*state));
    if (!state)
    {
        return -1;
    }
    state->workspace = workspace;
    *state_out = state;
    g_workspace_reload_inits++;
    bool old_visible = workspace->command_count == 1 &&
                       workspace->commands[0].name &&
                       strcmp(workspace->commands[0].name, "old_workspace_command") == 0 &&
                       workspace->active_registration == g_expected_old_registration;
    pico_workspace_add_command(workspace, "live_reload_command", "live owner",
                               ReloadWorkspaceCommand);
    g_workspace_reload_staging_isolated = old_visible && workspace->command_count == 1 &&
                                          workspace->active_registration ==
                                              g_expected_old_registration;
    return 0;
}

static void LiveWorkspaceReloadShutdown(PicoWorkspace *workspace, void *state)
{
    ReloadOwnerState *owner = (ReloadOwnerState *)state;
    if (workspace == g_expected_reload_workspace && owner && owner->workspace == workspace)
    {
        g_workspace_reload_shutdowns++;
    }
    free(owner);
}

static int TestWorkspaceReloadUsesLiveOwnerAndSettings(void)
{
    PicoHost host;
    PicoModuleGeneration module;

    memset(&host, 0, sizeof(host));
    memset(&module, 0, sizeof(module));
    PicoHost_SetPath(&host, ".");
    host.safe_mode = true;
    PicoWorkspace *workspace = PicoHost_PrimaryWorkspace(&host);
    g_expected_reload_workspace = workspace;
    g_workspace_reload_inits = 0;
    g_workspace_reload_shutdowns = 0;
    g_workspace_reload_command_ok = false;
    g_workspace_reload_staging_isolated = false;

    workspace->commands[0] = (PicoCommand){.name = "old_workspace_command"};
    workspace->command_count = 1;
    if (!PicoWorkspace_PublishRegistrationGeneration(workspace))
    {
        Fail("publish old workspace registration for reload staging test");
        host.workspaces[0] = NULL;
        host.workspace_count = 0;
        PicoWorkspace_Free(workspace);
        return 1;
    }
    g_expected_old_registration = workspace->active_registration;

    module.ext.name = "live_reload_extension";
    module.ext.workspace_init = LiveWorkspaceReloadInit;
    module.ext.workspace_shutdown = LiveWorkspaceReloadShutdown;
    module.desired = true;
    module.ref_count = 1; /* module store */
    host.modules = &module;
    host.module_count = 1;
    host.module_capacity = 1;

    bool first = WaitWorkspaceReload(workspace);
    if (first && workspace->command_count == 1)
    {
        workspace->commands[0].workspace_run(workspace, 0, "",
                                              workspace->commands[0].state);
    }
    bool live_owner = first && g_workspace_reload_inits == 1 &&
                      g_workspace_reload_command_ok && g_workspace_reload_staging_isolated &&
                      workspace->command_count == 1 &&
                      workspace->commands[0].workspace == workspace;

    snprintf(workspace->settings.disabled_extensions[0],
             sizeof(workspace->settings.disabled_extensions[0]), "%s", module.ext.name);
    workspace->settings.disabled_extension_count = 1;
    bool second = WaitWorkspaceReload(workspace);
    bool disabled = second && g_workspace_reload_inits == 1 &&
                    g_workspace_reload_shutdowns == 1 && workspace->command_count == 0 &&
                    workspace->workspace_plugin_count == 1 &&
                    !workspace->workspace_plugins[0].initialized;

    PicoWorkspaceExtensions_Shutdown(workspace);
    module.desired = false;
    PicoModule_Release(&module);
    host.workspaces[0] = NULL;
    host.workspace_count = 0;
    PicoWorkspace_Free(workspace);
    g_expected_reload_workspace = NULL;
    g_expected_old_registration = NULL;

    if (!live_owner || !disabled)
    {
        Fail("workspace reload must initialize against the live owner and honor its disable settings");
        return 1;
    }
    return 0;
}

static int TestNestedWorkspaceExtensionOwnership(void)
{
    PicoHost host;
    PicoWorkspace outer;
    PicoWorkspace inner;
    memset(&host, 0, sizeof(host));
    memset(&outer, 0, sizeof(outer));
    memset(&inner, 0, sizeof(inner));
    outer.host = &host;
    inner.host = &host;
    snprintf(outer.path, sizeof(outer.path), "/tmp/project");
    snprintf(inner.path, sizeof(inner.path),
             "/tmp/project/.pico/extensions/nested-workspace");
    host.workspaces[0] = &outer;
    host.workspaces[1] = &inner;
    host.workspace_count = 2;
    const char *source =
        "/tmp/project/.pico/extensions/nested-workspace/.pico/extensions/local.c";
    if (PicoHost_SourceWorkspace(&host, source) != &inner)
    {
        Fail("nested workspace-local sources must belong to the most specific workspace");
        return 1;
    }
    return 0;
}

static const char *kWorkspaceLocalPollExtV1 =
    "#include \"pico/plugin.h\"\n"
    "#include <stdlib.h>\n"
    "static int Init(PicoWorkspace *workspace, void **state_out)\n"
    "{\n"
    "    (void)workspace;\n"
    "    *state_out = malloc(1);\n"
    "    return *state_out ? 0 : -1;\n"
    "}\n"
    "static void Shutdown(PicoWorkspace *workspace, void *state)\n"
    "{\n"
    "    (void)workspace;\n"
    "    free(state);\n"
    "}\n"
    "PicoExt pico_ext(void)\n"
    "{\n"
    "    return (PicoExt){\n"
    "        .abi = PICO_EXT_ABI,\n"
    "        .name = \"local_poll\",\n"
    "        .description = \"version one\",\n"
    "        .workspace_init = Init,\n"
    "        .workspace_shutdown = Shutdown,\n"
    "    };\n"
    "}\n";

static const char *kWorkspaceLocalPollExtV2 =
    "#include \"pico/plugin.h\"\n"
    "#include <stdlib.h>\n"
    "static int Init(PicoWorkspace *workspace, void **state_out)\n"
    "{\n"
    "    (void)workspace;\n"
    "    *state_out = malloc(1);\n"
    "    return *state_out ? 0 : -1;\n"
    "}\n"
    "static void Shutdown(PicoWorkspace *workspace, void *state)\n"
    "{\n"
    "    (void)workspace;\n"
    "    free(state);\n"
    "}\n"
    "PicoExt pico_ext(void)\n"
    "{\n"
    "    return (PicoExt){\n"
    "        .abi = PICO_EXT_ABI,\n"
    "        .name = \"local_poll\",\n"
    "        .description = \"version two\",\n"
    "        .workspace_init = Init,\n"
    "        .workspace_shutdown = Shutdown,\n"
    "    };\n"
    "}\n";

static uint64_t WorkspaceSourceGeneration(const PicoWorkspace *workspace,
                                          const char *source)
{
    if (!workspace || !source)
    {
        return 0;
    }
    for (int i = 0; i < workspace->workspace_plugin_count; i++)
    {
        const PicoPluginSlot *slot = &workspace->workspace_plugins[i];
        if (slot->source && strcmp(slot->source, source) == 0 && slot->initialized)
        {
            return slot->active_generation;
        }
    }
    return 0;
}

/* A dependency manifest can be on a slow filesystem. A pending read must not
 * block an ordinary automatic poll, and closing the host must quiesce the
 * scanner once the read completes. */
static int TestPluginSourceScanDoesNotBlockUi(void)
{
    char cfg[] = "/tmp/pico-scan-cfg-XXXXXX";
    char cache[] = "/tmp/pico-scan-cache-XXXXXX";
    char ext_dir[4096], src[8192], header[8192], cache_dir[4096];
    char manifest[8192] = {0}, saved[8192] = {0};
    PicoHost *host = NULL;
    int failed = 1;
    if (!mkdtemp(cfg) || !mkdtemp(cache)) return 1;
    snprintf(ext_dir, sizeof(ext_dir), "%s/pico/extensions", cfg);
    snprintf(cache_dir, sizeof(cache_dir), "%s/pico/ext", cache);
    snprintf(src, sizeof(src), "%s/probe.c", ext_dir);
    snprintf(header, sizeof(header), "%s/value.h", ext_dir);
    const char *code =
        "#include \"pico/plugin.h\"\n#include \"value.h\"\n"
        "static int Init(PicoHost *h, void **s) { (void)h; int *v=malloc(sizeof(*v)); if(!v)return -1; *v=VALUE; *s=v; return 0; }\n"
        "static void Stop(PicoHost *h, void *s) { (void)h; free(s); }\n"
        "PicoExt pico_ext(void) { return (PicoExt){.abi=PICO_EXT_ABI,.name=\"scan_probe\",.host_init=Init,.host_shutdown=Stop}; }\n";
    if (MkdirParents(ext_dir) || WriteFile(src, code) ||
        WriteFile(header, "#define VALUE 10\n")) goto done;
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    if (pico_host_init(&host, NULL, false) != PICO_OK) goto done;
    WaitPluginLoad(host);
    int *value = PicoPlugins_HostState(host, "scan_probe");
    if (!value || *value != 10) goto done;
    DIR *dir = opendir(cache_dir);
    if (!dir) goto done;
    struct dirent *entry;
    while ((entry = readdir(dir)))
    {
        size_t n = strlen(entry->d_name);
        if (n > 5 && strcmp(entry->d_name + n - 5, ".deps") == 0)
        {
            snprintf(manifest, sizeof(manifest), "%s/%s", cache_dir, entry->d_name);
            break;
        }
    }
    closedir(dir);
    if (!manifest[0]) goto done;
    snprintf(saved, sizeof(saved), "%s.saved", manifest);
    if (rename(manifest, saved) || mkfifo(manifest, 0600)) goto done;

    host->plugin_last_poll = -1;
    PicoPlugins_Poll(host);
    if (!host->plugin_scan_pending) goto done;
    /* Opening the writer proves the scanner has opened the FIFO. Keep it
     * open so a UI poll must return while the dependency read is still held. */
    PicoTest_Wait(__func__, "scanner opened dependency FIFO");
    int writer = open(manifest, O_WRONLY);
    if (writer < 0) goto done;
    PicoPlugins_Poll(host);
    bool responsive = PicoPlugins_HostState(host, "scan_probe") == value;
    close(writer);
    if (!responsive || rename(saved, manifest)) goto done;
    WaitPluginPoll(host);
    value = PicoPlugins_HostState(host, "scan_probe");
    if (!value || *value != 10) goto done;
    if (WriteFile(header, "#define VALUE 20\n")) goto done;
    host->plugin_last_poll = -1;
    WaitPluginPoll(host);
    value = PicoPlugins_HostState(host, "scan_probe");
    if (!value || *value != 20) goto done;
    failed = 0;
done:
    if (failed) Fail("source detection must not block UI on a slow dependency read");
    if (saved[0] && manifest[0]) rename(saved, manifest);
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg); RmRf(cache);
    return failed;
}

static int TestBlockedSourceScanRetainsShutdown(void)
{
    pid_t child = fork();
    if (child < 0) { Fail("source scanner shutdown fork"); return 1; }
    if (child == 0)
    {
        char cfg[] = "/tmp/pico-scan-shutdown-cfg-XXXXXX";
        char cache[] = "/tmp/pico-scan-shutdown-cache-XXXXXX";
        char ext_dir[4096], src[8192], cache_dir[4096], manifest[8192] = {0};
        PicoHost *host = NULL;
        if (!mkdtemp(cfg) || !mkdtemp(cache)) _exit(2);
        snprintf(ext_dir, sizeof(ext_dir), "%s/pico/extensions", cfg);
        snprintf(cache_dir, sizeof(cache_dir), "%s/pico/ext", cache);
        snprintf(src, sizeof(src), "%s/probe.c", ext_dir);
        if (MkdirParents(ext_dir) ||
            WriteFile(src, "#include \"pico/plugin.h\"\nPicoExt pico_ext(void) { return (PicoExt){.abi=PICO_EXT_ABI,.name=\"shutdown_scan_probe\"}; }\n"))
            _exit(3);
        setenv("XDG_CONFIG_HOME", cfg, 1);
        setenv("XDG_CACHE_HOME", cache, 1);
        if (pico_host_init(&host, NULL, false) != PICO_OK) _exit(4);
        WaitPluginLoad(host);
        bool loaded = false;
        for (int i = 0; i < host->module_count; i++)
            if (host->modules[i].desired && strcmp(host->modules[i].source, src) == 0)
                loaded = true;
        if (!loaded) _exit(5);
        DIR *dir = opendir(cache_dir);
        if (!dir) _exit(6);
        struct dirent *entry;
        while ((entry = readdir(dir)))
        {
            size_t n = strlen(entry->d_name);
            if (n > 5 && strcmp(entry->d_name + n - 5, ".deps") == 0)
            {
                snprintf(manifest, sizeof(manifest), "%s/%s", cache_dir, entry->d_name);
                break;
            }
        }
        closedir(dir);
        if (!manifest[0] || unlink(manifest) || mkfifo(manifest, 0600)) _exit(7);
        host->plugin_last_poll = -1;
        PicoPlugins_Poll(host);
        /* Keep the writer open, so the scanner has opened the FIFO but cannot
         * reach EOF. Unlike a sleep, this guarantees it stays blocked. */
        PicoTest_Wait(__func__, "scanner opened dependency FIFO");
        int writer = open(manifest, O_WRONLY);
        if (writer < 0) _exit(8);
        PicoHostShutdownResult result = PicoHost_Shutdown(host);

        close(writer);
        RmRf(cfg); RmRf(cache);
        _exit(result == PICO_HOST_SHUTDOWN_RETAINED ? 0 : 10);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        Fail("blocked source scan must use retained bounded shutdown");
        return 1;
    }
    return 0;
}

static int TestHeaderReloadIsAsynchronous(void)
{
    char cfg[] = "/tmp/pico-header-cfg-XXXXXX";
    char cache[] = "/tmp/pico-header-cache-XXXXXX";
    char ws[] = "/tmp/pico-header-ws-XXXXXX";
    char ext_dir[4096], source[8192], header[8192], compiler[4096];
    char ready_path[4096], release_path[4096], compiler_script[16384];
    PicoHost *host = NULL;
    int failed = 1;
    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(ws)) return 1;
    snprintf(ext_dir, sizeof(ext_dir), "%s/pico/extensions", cfg);
    snprintf(source, sizeof(source), "%s/probe.c", ext_dir);
    snprintf(header, sizeof(header), "%s/value header.h", ext_dir);
    snprintf(compiler, sizeof(compiler), "%s/compiler", cfg);
    snprintf(ready_path, sizeof(ready_path), "%s/compiler-ready", cfg);
    snprintf(release_path, sizeof(release_path), "%s/compiler-release", cfg);
    snprintf(compiler_script, sizeof(compiler_script),
             "#!/bin/sh\nprintf x > '%s'\nread token < '%s'\nexec cc \"$@\"\n",
             ready_path, release_path);
    if (mkfifo(ready_path, 0600) || mkfifo(release_path, 0600)) goto done;
    const char *code =
        "#include \"pico/plugin.h\"\n#include \"value header.h\"\n"
        "static int Init(PicoHost *h, void **s) { (void)h; int *v=malloc(sizeof(*v)); if(!v)return -1; *v=VALUE; *s=v; return 0; }\n"
        "static void Stop(PicoHost *h, void *s) { (void)h; free(s); }\n"
        "PicoExt pico_ext(void) { return (PicoExt){.abi=PICO_EXT_ABI,.name=\"header_probe\",.host_init=Init,.host_shutdown=Stop}; }\n";
    if (MkdirParents(ext_dir) || WriteFile(source, code) || WriteFile(header, "#define VALUE 10\n")) goto done;
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    PicoWorkspaceId id;
    if (pico_host_init(&host, NULL, false) != PICO_OK || pico_workspace_open(host, ws, &id) != PICO_OK) goto done;
    WaitPluginLoad(host);
    int *value = PicoPlugins_HostState(host, "header_probe");
    if (!value || *value != 10) goto done;
    PicoRegistrationGeneration *registration = PicoHost_FindWorkspace(host, id)->active_registration;
    struct stat st;
    if (stat(header, &st)) goto done;
    if (WriteFile(header, "#define VALUE 20\n") ||
        WriteFile(compiler, compiler_script) || chmod(compiler, 0700)) goto done;
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    if (utimensat(AT_FDCWD, header, times, 0)) goto done;
    setenv("PICO_CC", compiler, 1);
    bool immediate = PicoPlugins_ReloadHost(host);
    if (immediate ||
        PicoPlugins_HostState(host, "header_probe") != value) goto done;
    PicoTest_Wait(__func__, "compiler reached its release gate");
    int ready_fd = open(ready_path, O_RDONLY);
    if (ready_fd < 0) goto done;
    bool compiler_ready = TransferTestByte(ready_fd, false);
    close(ready_fd);
    if (!compiler_ready || PicoPlugins_ReloadHost(host)) goto done;
    int release_fd = open(release_path, O_WRONLY);
    if (release_fd < 0) goto done;
    bool released = write(release_fd, "x\n", 2) == 2;
    close(release_fd);
    if (!released) goto done;
    PICO_TEST_WAIT_LOOP("replacement extension generation")
    {
        pico_host_pump(host);
        value = PicoPlugins_HostState(host, "header_probe");
        if (value && *value == 20) break;
    }
    if (!value || *value != 20 || PicoHost_FindWorkspace(host, id)->active_registration != registration) goto done;
    /* A compiler that never produces output must still expire, keeping the
     * previous generation active. Wait for the timeout outcome, not a guessed
     * upper bound on the configurable production duration. */
    if (WriteFile(compiler, "#!/bin/sh\nwhile :; do sleep 3600; done\n") || WriteFile(header, "#define VALUE 25\n")) goto done;
    PicoPlugins_ReloadHost(host);
    PICO_TEST_WAIT(!host->status_warn || !strstr(host->status_warn, "compiler timed out"))
    {
        pico_host_pump(host);
    }
    value = PicoPlugins_HostState(host, "header_probe");
    if (!value || *value != 20 || !host->status_warn || !strstr(host->status_warn, "compiler timed out")) goto done;
    if (WriteFile(compiler, "#!/bin/sh\nwhile :; do sleep 3600; done\n")) goto done;
    /* Removing a queued source must cancel its build, without blocking future builds. */
    if (WriteFile(header, "#define VALUE 30\n")) goto done;
    PicoPlugins_ReloadHost(host);
    unlink(source);
    PICO_TEST_WAIT(host->plugin_compile)
    {
        pico_host_pump(host);
    }
    if (host->plugin_compile) goto done;
    failed = 0;
done:
    if (failed) Fail("header dependency reload must be nonblocking, scoped, and cancellable");
    pico_host_free(host);
    unsetenv("PICO_CC");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg); RmRf(cache); RmRf(ws);
    return failed;
}

static uint64_t HostSourceGeneration(const PicoHost *host, const char *source)
{
    if (!host || !source)
    {
        return 0;
    }
    for (int i = 0; i < host->host_plugin_count; i++)
    {
        const PicoPluginSlot *slot = &host->host_plugins[i];
        if (slot->source && strcmp(slot->source, source) == 0 && slot->initialized)
        {
            return slot->active_generation;
        }
    }
    return 0;
}

static bool RemoveCachedSharedObjects(const char *cache_root)
{
    char directory[4096];
    DIR *dir;
    struct dirent *entry;
    if ((size_t)snprintf(directory, sizeof(directory), "%s/pico/ext", cache_root) >=
        sizeof(directory))
    {
        return false;
    }
    dir = opendir(directory);
    if (!dir)
    {
        return false;
    }
    while ((entry = readdir(dir)) != NULL)
    {
        size_t length = strlen(entry->d_name);
        if (length < 3 || strcmp(entry->d_name + length - 3, ".so") != 0)
        {
            continue;
        }
        char path[8192];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >=
                sizeof(path) ||
            unlink(path) != 0)
        {
            closedir(dir);
            return false;
        }
    }
    closedir(dir);
    return true;
}

static int TestSdkDependencyManifestsDoNotCrossReloadHosts(void)
{
    static const char *extension =
        "#include \"pico/plugin.h\"\n"
        "#include <stdlib.h>\n"
        "static int Init(PicoHost *host, void **state_out)\n"
        "{\n"
        "    (void)host;\n"
        "    int *state = malloc(sizeof(*state));\n"
        "    if (!state) return -1;\n"
        "    *state = SDK_MARKER;\n"
        "    *state_out = state;\n"
        "    return 0;\n"
        "}\n"
        "static void Shutdown(PicoHost *host, void *state)\n"
        "{\n"
        "    (void)host;\n"
        "    free(state);\n"
        "}\n"
        "PicoExt pico_ext(void)\n"
        "{\n"
        "    return (PicoExt){.abi=PICO_EXT_ABI, .name=\"manifest_probe\",\n"
        "                     .host_init=Init, .host_shutdown=Shutdown};\n"
        "}\n";
    static const char *sdk_header_format =
        "#ifndef PICO_PLUGIN_H\n"
        "#define PICO_PLUGIN_H\n"
        "#define PICO_EXT_ABI %d\n"
        "#define SDK_MARKER %d\n"
        "typedef struct PicoHost PicoHost;\n"
        "typedef struct PicoWorkspace PicoWorkspace;\n"
        "typedef int (*PicoHostExtInitFn)(PicoHost *, void **);\n"
        "typedef void (*PicoHostExtShutdownFn)(PicoHost *, void *);\n"
        "typedef void (*PicoHostExtFrameFn)(PicoHost *, void *, float);\n"
        "typedef int (*PicoWorkspaceExtInitFn)(PicoWorkspace *, void **);\n"
        "typedef void (*PicoWorkspaceExtShutdownFn)(PicoWorkspace *, void *);\n"
        "typedef void (*PicoWorkspaceExtFrameFn)(PicoWorkspace *, void *, float);\n"
        "typedef struct PicoExt {\n"
        " int abi; const char *name; const char *description;\n"
        " PicoHostExtInitFn host_init; PicoHostExtShutdownFn host_shutdown;\n"
        " PicoHostExtFrameFn host_on_frame; PicoWorkspaceExtInitFn workspace_init;\n"
        " PicoWorkspaceExtShutdownFn workspace_shutdown;\n"
        " PicoWorkspaceExtFrameFn workspace_on_frame;\n"
        "} PicoExt;\n"
        "#endif\n";
    char cfg[] = "/tmp/pico-manifest-cfg-XXXXXX";
    char cache[] = "/tmp/pico-manifest-cache-XXXXXX";
    char sdk_a[] = "/tmp/pico-manifest-sdk-a-XXXXXX";
    char sdk_b[] = "/tmp/pico-manifest-sdk-b-XXXXXX";
    char ext_dir[4096];
    char source[8192];
    char include_a[8192];
    char include_b[8192];
    char header_a[16384];
    char header_b[16384];
    PicoHost *host_a = NULL;
    PicoHost *host_b = NULL;
    int failed = 1;

    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(sdk_a) || !mkdtemp(sdk_b))
    {
        Fail("mkdtemp SDK dependency manifest isolation");
        return 1;
    }
    snprintf(ext_dir, sizeof(ext_dir), "%s/pico/extensions", cfg);
    snprintf(source, sizeof(source), "%s/manifest_probe.c", ext_dir);
    snprintf(include_a, sizeof(include_a), "%s/sdk/include/pico", sdk_a);
    snprintf(include_b, sizeof(include_b), "%s/sdk/include/pico", sdk_b);
    snprintf(header_a, sizeof(header_a), "%s/plugin.h", include_a);
    snprintf(header_b, sizeof(header_b), "%s/plugin.h", include_b);
    char sdk_contents_a[4096];
    char sdk_contents_b[4096];
    if ((size_t)snprintf(sdk_contents_a, sizeof(sdk_contents_a), sdk_header_format,
                         PICO_EXT_ABI, 1) >= sizeof(sdk_contents_a) ||
        (size_t)snprintf(sdk_contents_b, sizeof(sdk_contents_b), sdk_header_format,
                         PICO_EXT_ABI, 2) >= sizeof(sdk_contents_b) ||
        MkdirParents(ext_dir) != 0 || MkdirParents(include_a) != 0 ||
        MkdirParents(include_b) != 0 || WriteFile(source, extension) != 0 ||
        WriteFile(header_a, sdk_contents_a) != 0 ||
        WriteFile(header_b, sdk_contents_b) != 0)
    {
        Fail("write SDK dependency manifest isolation fixture");
        goto done;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    setenv("PICO_DATA_DIR", sdk_a, 1);
    if (pico_host_init(&host_a, NULL, false) != PICO_OK || !host_a)
    {
        Fail("open first SDK dependency manifest host");
        goto done;
    }
    WaitPluginLoad(host_a);
    uint64_t generation = HostSourceGeneration(host_a, source);
    int *state_a = PicoPlugins_HostState(host_a, "manifest_probe");
    if (!generation || !state_a || *state_a != 1 || !RemoveCachedSharedObjects(cache))
    {
        Fail("load first SDK dependency manifest extension");
        goto done;
    }

    setenv("PICO_DATA_DIR", sdk_b, 1);
    if (pico_host_init(&host_b, NULL, false) != PICO_OK || !host_b)
    {
        Fail("open second SDK dependency manifest host");
        goto done;
    }
    WaitPluginLoad(host_b);
    int *state_b = PicoPlugins_HostState(host_b, "manifest_probe");
    if (!HostSourceGeneration(host_b, source) || !state_b || *state_b != 2)
    {
        Fail("second SDK root must compile its own extension generation");
        goto done;
    }

    setenv("PICO_DATA_DIR", sdk_a, 1);
    Pico_PathsInit(NULL);
    host_a->plugin_last_poll = -1.0;
    WaitPluginPoll(host_a);
    state_a = PicoPlugins_HostState(host_a, "manifest_probe");
    if (HostSourceGeneration(host_a, source) != generation || !state_a || *state_a != 1)
    {
        Fail("another SDK root's dependency manifest must not reload this host");
        goto done;
    }
    failed = 0;

done:
    pico_host_free(host_b);
    pico_host_free(host_a);
    unsetenv("PICO_DATA_DIR");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(sdk_a);
    RmRf(sdk_b);
    return failed;
}

static int TestWorkspaceLocalPollingReloadsOnlyOwner(void)
{
    char cfg[] = "/tmp/pico-local-poll-cfg-XXXXXX";
    char cache[] = "/tmp/pico-local-poll-cache-XXXXXX";
    char dir_a[] = "/tmp/pico-local-poll-a-XXXXXX";
    char dir_b[] = "/tmp/pico-local-poll-b-XXXXXX";
    char ext_dir[4096];
    char source[8192];
    PicoHost *host = NULL;
    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(dir_a) || !mkdtemp(dir_b))
    {
        Fail("mkdtemp local polling isolation");
        return 1;
    }
    snprintf(ext_dir, sizeof(ext_dir), "%s/.pico/extensions", dir_a);
    snprintf(source, sizeof(source), "%s/local_poll.c", ext_dir);
    if (MkdirParents(ext_dir) != 0 || WriteFile(source, kWorkspaceLocalPollExtV1) != 0)
    {
        Fail("write initial workspace-local polling extension");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    PicoWorkspaceId id_a = 0;
    PicoWorkspaceId id_b = 0;
    if (pico_host_init(&host, NULL, false) != PICO_OK || !host ||
        pico_workspace_open(host, dir_a, &id_a) != PICO_OK ||
        pico_workspace_open(host, dir_b, &id_b) != PICO_OK)
    {
        Fail("open workspaces for local polling isolation");
        goto fail;
    }
    WaitPluginPoll(host);
    PicoWorkspace *workspace_a = PicoHost_FindWorkspace(host, id_a);
    PicoWorkspace *workspace_b = PicoHost_FindWorkspace(host, id_b);
    PicoRegistrationGeneration *old_a = workspace_a->active_registration;
    PicoRegistrationGeneration *old_b = workspace_b->active_registration;
    PicoModuleGeneration *old_host_module = host->host_plugin_count > 0
                                                ? host->host_plugins[0].module
                                                : NULL;
    uint64_t old_local_generation = WorkspaceSourceGeneration(workspace_a, source);
    if (!old_a || !old_b || !old_host_module || !old_local_generation ||
        WriteFile(source, kWorkspaceLocalPollExtV2) != 0)
    {
        Fail("prepare workspace-local polling change");
        goto fail;
    }

    host->plugin_last_poll = -1.0;
    WaitPluginPoll(host);
    uint64_t new_local_generation = WorkspaceSourceGeneration(workspace_a, source);
    bool isolated = new_local_generation > old_local_generation &&
                    workspace_a->active_registration != old_a &&
                    workspace_b->active_registration == old_b &&
                    workspace_b->state == PICO_WORKSPACE_OPEN &&
                    PicoWorkspace_AcceptsNewWork(workspace_b) &&
                    host->host_plugins[0].module == old_host_module;
    if (!isolated)
    {
        Fail("workspace-local source polling must reload only its owning workspace");
        goto fail;
    }

    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(dir_a);
    RmRf(dir_b);
    return 0;

fail:
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(dir_a);
    RmRf(dir_b);
    return 1;
}

static const char *kQuarantineHostExt =
    "#include \"pico/plugin.h\"\n"
    "static int HostInit(PicoHost *host, void **state_out)\n"
    "{\n"
    "    static int state;\n"
    "    (void)host;\n"
    "    *state_out = &state;\n"
    "    return 0;\n"
    "}\n"
    "PicoExt pico_ext(void)\n"
    "{\n"
    "    return (PicoExt){\n"
    "        .abi = PICO_EXT_ABI,\n"
    "        .name = \"quarantine_host\",\n"
    "        .host_init = HostInit,\n"
    "    };\n"
    "}\n";

static const char *kQuarantineOtherHostExt =
    "#include \"pico/plugin.h\"\n"
    "static int HostInit(PicoHost *host, void **state_out)\n"
    "{\n"
    "    static int state;\n"
    "    (void)host;\n"
    "    *state_out = &state;\n"
    "    return 0;\n"
    "}\n"
    "PicoExt pico_ext(void)\n"
    "{\n"
    "    return (PicoExt){\n"
    "        .abi = PICO_EXT_ABI,\n"
    "        .name = \"quarantine_other_host\",\n"
    "        .host_init = HostInit,\n"
    "    };\n"
    "}\n";

static const char *kQuarantineWorkspaceExt =
    "#include \"pico/plugin.h\"\n"
    "static int WorkspaceInit(PicoWorkspace *workspace, void **state_out)\n"
    "{\n"
    "    static int state;\n"
    "    (void)workspace;\n"
    "    *state_out = &state;\n"
    "    return 0;\n"
    "}\n"
    "PicoExt pico_ext(void)\n"
    "{\n"
    "    return (PicoExt){\n"
    "        .abi = PICO_EXT_ABI,\n"
    "        .name = \"quarantine_ws\",\n"
    "        .workspace_init = WorkspaceInit,\n"
    "    };\n"
    "}\n";

static int TestHostCompileFailureQuarantinesUnchangedPoll(void)
{
    char cfg[] = "/tmp/pico-qhost-cfg-XXXXXX";
    char cache[] = "/tmp/pico-qhost-cache-XXXXXX";
    char ws[] = "/tmp/pico-qhost-ws-XXXXXX";
    char ext_dir[4096];
    char source[8192];
    char other_source[8192];
    PicoHost *host = NULL;
    PicoWorkspaceId id = 0;
    void *working_state = NULL;
    char *warned = NULL;

    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(ws))
    {
        Fail("mkdtemp host compile quarantine");
        return 1;
    }
    snprintf(ext_dir, sizeof(ext_dir), "%s/pico/extensions", cfg);
    snprintf(source, sizeof(source), "%s/quarantine_host.c", ext_dir);
    snprintf(other_source, sizeof(other_source), "%s/quarantine_other_host.c", ext_dir);
    if (MkdirParents(ext_dir) != 0 || WriteFile(source, kQuarantineHostExt) != 0)
    {
        Fail("write host compile quarantine extension");
        RmRf(cfg);
        RmRf(cache);
        RmRf(ws);
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    if (pico_host_init(&host, NULL, false) != PICO_OK || !host ||
        pico_workspace_open(host, ws, &id) != PICO_OK)
    {
        Fail("open host compile quarantine");
        goto fail;
    }
    WaitPluginLoad(host);
    working_state = PicoPlugins_HostState(host, "quarantine_host");
    if (!working_state)
    {
        Fail("host compile quarantine extension must start active");
        goto fail;
    }
    if (WriteFile(source, "this is not valid C {\n") != 0)
    {
        Fail("overwrite host compile quarantine extension");
        goto fail;
    }
    host->plugin_last_poll = -1.0;
    WaitPluginPoll(host);
    if (!host->status_warn || !strstr(host->status_warn, "quarantine_host.c"))
    {
        Fail("host compile failure must warn");
        goto fail;
    }
    if (PicoPlugins_HostState(host, "quarantine_host") != working_state)
    {
        Fail("host compile failure must preserve the working generation");
        goto fail;
    }
    warned = strdup(host->status_warn);
    if (!warned)
    {
        Fail("strdup host compile quarantine warning");
        goto fail;
    }
    host->plugin_last_poll = -1.0;
    WaitPluginPoll(host);
    if (!host->status_warn || strcmp(host->status_warn, warned) != 0)
    {
        Fail("unchanged host compile failure must not retry on poll");
        goto fail;
    }
    if (WriteFile(other_source, kQuarantineOtherHostExt) != 0)
    {
        Fail("write second host extension beside quarantined failure");
        goto fail;
    }
    host->plugin_last_poll = -1.0;
    WaitPluginPoll(host);
    if (!host->status_warn || strcmp(host->status_warn, warned) != 0 ||
        !PicoPlugins_HostState(host, "quarantine_other_host"))
    {
        Fail("another host source change must skip the quarantined failure");
        goto fail;
    }
    if (WaitHostReload(host) || !host->status_warn ||
        strcmp(host->status_warn, warned) == 0)
    {
        Fail("explicit host reload must retry a quarantined compile failure");
        goto fail;
    }
    free(warned);
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(ws);
    return 0;

fail:
    free(warned);
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(ws);
    return 1;
}

static int TestWorkspaceCompileFailureQuarantinesUnchangedPoll(void)
{
    char cfg[] = "/tmp/pico-qws-cfg-XXXXXX";
    char cache[] = "/tmp/pico-qws-cache-XXXXXX";
    char ws[] = "/tmp/pico-qws-ws-XXXXXX";
    char ext_dir[4096];
    char source[8192];
    char global_ext_dir[4096];
    char global_source[8192];
    PicoHost *host = NULL;
    PicoWorkspaceId id = 0;
    PicoWorkspace *workspace;
    void *working_state = NULL;
    char *warned = NULL;

    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(ws))
    {
        Fail("mkdtemp workspace compile quarantine");
        return 1;
    }
    snprintf(ext_dir, sizeof(ext_dir), "%s/.pico/extensions", ws);
    snprintf(source, sizeof(source), "%s/quarantine_ws.c", ext_dir);
    snprintf(global_ext_dir, sizeof(global_ext_dir), "%s/pico/extensions", cfg);
    snprintf(global_source, sizeof(global_source), "%s/quarantine_other_host.c", global_ext_dir);
    if (MkdirParents(ext_dir) != 0 || WriteFile(source, kQuarantineWorkspaceExt) != 0)
    {
        Fail("write initial workspace compile quarantine extension");
        RmRf(cfg);
        RmRf(cache);
        RmRf(ws);
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    if (pico_host_init(&host, NULL, false) != PICO_OK || !host ||
        pico_workspace_open(host, ws, &id) != PICO_OK)
    {
        Fail("open workspace compile quarantine");
        goto fail;
    }
    WaitPluginPoll(host);
    workspace = PicoHost_FindWorkspace(host, id);
    working_state = PicoPlugins_WorkspaceState(workspace, "quarantine_ws");
    PicoRegistrationGeneration *working_registration = workspace ? workspace->active_registration : NULL;
    if (!workspace || !working_state || WriteFile(source, "this is not valid C {\n") != 0)
    {
        Fail("activate and overwrite workspace compile quarantine extension");
        goto fail;
    }
    host->plugin_last_poll = -1.0;
    WaitPluginPoll(host);
    if (!host->status_warn || !strstr(host->status_warn, "quarantine_ws.c"))
    {
        Fail("workspace compile failure must warn");
        goto fail;
    }
    if (workspace->active_registration != working_registration)
    {
        Fail("workspace compile failure must preserve the working generation");
        goto fail;
    }
    warned = strdup(host->status_warn);
    if (!warned)
    {
        Fail("strdup workspace compile quarantine warning");
        goto fail;
    }
    host->plugin_last_poll = -1.0;
    WaitPluginPoll(host);
    if (!host->status_warn || strcmp(host->status_warn, warned) != 0)
    {
        Fail("unchanged workspace compile failure must not retry on poll");
        goto fail;
    }
    if (MkdirParents(global_ext_dir) != 0 ||
        WriteFile(global_source, kQuarantineOtherHostExt) != 0)
    {
        Fail("write global extension beside workspace quarantine");
        goto fail;
    }
    host->plugin_last_poll = -1.0;
    WaitPluginPoll(host);
    if (!host->status_warn || strcmp(host->status_warn, warned) != 0 ||
        !PicoPlugins_HostState(host, "quarantine_other_host"))
    {
        Fail("global rollout must skip unchanged workspace compile failure");
        goto fail;
    }
    (void)WaitWorkspaceReload(workspace);
    if (!host->status_warn || strcmp(host->status_warn, warned) == 0)
    {
        Fail("explicit workspace reload must retry a quarantined compile failure");
        goto fail;
    }
    free(warned);
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(ws);
    return 0;

fail:
    free(warned);
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(ws);
    return 1;
}

static const char *kWorkspaceLocalWithHostExt =
    "#include \"pico/plugin.h\"\n"
    "#include <stdlib.h>\n"
    "static int HostInit(PicoHost *host, void **state_out)\n"
    "{\n"
    "    (void)host;\n"
    "    (void)state_out;\n"
    "    return 0;\n"
    "}\n"
    "PicoExt pico_ext(void)\n"
    "{\n"
    "    return (PicoExt){\n"
    "        .abi = PICO_EXT_ABI,\n"
    "        .name = \"ws_local_bad\",\n"
    "        .host_init = HostInit,\n"
    "    };\n"
    "}\n";

static int TestWorkspaceLocalExtensionWithHostCallbacksRejected(void)
{
    char cfg[256];
    char cache[256];
    char ws[256];
    char ws_ext_dir[1024];
    char src[2048];
    PicoHost *host = NULL;
    PicoWorkspaceId id = 0;

    snprintf(cfg, sizeof(cfg), "/tmp/pico-cfg-XXXXXX");
    snprintf(cache, sizeof(cache), "/tmp/pico-cache-XXXXXX");
    snprintf(ws, sizeof(ws), "/tmp/pico-ws-XXXXXX");
    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(ws))
    {
        Fail("mkdtemp ws_local");
        return 1;
    }
    snprintf(ws_ext_dir, sizeof(ws_ext_dir), "%s/.pico/extensions", ws);
    snprintf(src, sizeof(src), "%s/bad.c", ws_ext_dir);
    if (MkdirParents(ws_ext_dir) != 0 || WriteFile(src, kWorkspaceLocalWithHostExt) != 0)
    {
        Fail("write ws_local extension");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    if (pico_host_init(&host, NULL, false) != PICO_OK || !host)
    {
        Fail("pico_host_init ws_local");
        return 1;
    }
    if (pico_workspace_open(host, ws, &id) != PICO_OK)
    {
        Fail("open ws_local workspace");
        pico_host_free(host);
        return 1;
    }
    WaitPluginLoad(host);

    if (!host->status_warn || !strstr(host->status_warn, "workspace-local extension cannot have host callbacks"))
    {
        Fail("workspace-local extension with host callbacks must be rejected with warning");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        RmRf(cfg);
        RmRf(cache);
        RmRf(ws);
        return 1;
    }

    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    RmRf(ws);
    return 0;
}

static int TestReloadInitRollbackPreservesActiveState(void)
{
    char cfg[256];
    char cache[256];
    char ws[256];
    char life[512];
    char log[32];
    PicoHost *host = NULL;

    cfg[0] = cache[0] = ws[0] = '\0';
    if (StartLifecycleHost(&host, cfg, cache, ws, life, 0) != 0)
    {
        FinishLifecycleHost(host, cfg, cache, ws);
        return 1;
    }
    void *host_state = PicoPlugins_HostState(host, "lifecycle");
    void *workspace_state = PicoPlugins_WorkspaceState(PicoHost_PrimaryWorkspace(host), "lifecycle");
    char source[512];
    snprintf(source, sizeof(source), "%s/pico/extensions/lifecycle.c", cfg);
    bool preserved = host_state && workspace_state;
    if (preserved && WriteFile(source, "this is not valid C\n") != 0)
    {
        preserved = false;
    }
    WaitPluginsReload(host);
    preserved = preserved && PicoPlugins_HostState(host, "lifecycle") == host_state &&
                PicoPlugins_WorkspaceState(PicoHost_PrimaryWorkspace(host), "lifecycle") == workspace_state;
    if (WriteFile(source, kLifecycleExt) != 0)
    {
        preserved = false;
    }
    setenv("PICO_TEST_FAIL_WORKSPACE", "1", 1);
    WaitPluginsReload(host);
    ReadFileStr(life, log, sizeof(log));
    preserved = preserved &&
                PicoPlugins_WorkspaceState(PicoHost_PrimaryWorkspace(host), "lifecycle") == workspace_state &&
                PicoPlugins_HostState(host, "lifecycle") != NULL;
    pico_host_free(host);
    host = NULL;
    FinishLifecycleHost(NULL, cfg, cache, ws);
    if (!preserved)
    {
        Fail("failed reload must preserve the previous initialized extension state");
        return 1;
    }
    return 0;
}

static int TestGenerationRolloutAndDlcloseOnRelease(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&host);

    PicoModuleGeneration mod_n;
    memset(&mod_n, 0, sizeof(mod_n));
    mod_n.ext.name = "test_gen_mod";
    mod_n.generation = 1;
    mod_n.desired = true;
    mod_n.ref_count = 1; /* module store */

    ws->workspace_plugin_count = 1;
    snprintf(ws->workspace_plugins[0].name, sizeof(ws->workspace_plugins[0].name), "%s", mod_n.ext.name);
    ws->workspace_plugins[0].module = &mod_n;
    ws->workspace_plugins[0].initialized = true;

    /* Publish registration generation N */
    if (!PicoWorkspace_PublishRegistrationGeneration(ws))
    {
        Fail("publish registration generation N failed");
        free(host.workspaces[0]);
        return 1;
    }

    PicoRegistrationGeneration *gen_n = PicoWorkspace_RegistrationActive(ws);
    if (!gen_n || mod_n.ref_count != 2) /* store + snapshot */
    {
        Fail("gen N should be active and ref_count should be 2");
        free(host.workspaces[0]);
        return 1;
    }

    /* Simulate a running turn worker retaining gen_n */
    PicoWorkspace_RegistrationRetain(gen_n);
    if (gen_n->ref_count != 2)
    {
        Fail("gen_n ref_count should be 2 (workspace active + turn worker)");
        free(host.workspaces[0]);
        return 1;
    }

    /* Now rollout generation N+1 */
    PicoModuleGeneration mod_n1;
    memset(&mod_n1, 0, sizeof(mod_n1));
    mod_n1.ext.name = "test_gen_mod";
    mod_n1.generation = 2;
    mod_n1.desired = true;
    mod_n1.ref_count = 1; /* module store */

    ws->workspace_plugins[0].module = &mod_n1;
    if (!PicoWorkspace_PublishRegistrationGeneration(ws))
    {
        Fail("publish registration generation N+1 failed");
        free(host.workspaces[0]);
        return 1;
    }

    PicoRegistrationGeneration *gen_n1 = PicoWorkspace_RegistrationActive(ws);
    if (!gen_n1 || gen_n1 == gen_n || mod_n1.ref_count != 2)
    {
        Fail("gen N+1 should be active with ref_count 2");
        free(host.workspaces[0]);
        return 1;
    }

    /* Old generation mod_n is no longer desired in store */
    mod_n.desired = false;
    PicoModule_Release(&mod_n); /* release store ref */

    /* mod_n is still referenced by turn worker's gen_n */
    if (mod_n.ref_count != 1)
    {
        Fail("mod_n should still have ref_count 1 from retained gen_n snapshot");
        free(host.workspaces[0]);
        return 1;
    }

    /* Now turn worker completes and releases gen_n */
    PicoWorkspace_RegistrationRelease(gen_n);

    /* mod_n should now have ref_count 0 */
    if (mod_n.ref_count != 0)
    {
        Fail("mod_n should have ref_count 0 after gen_n released");
        free(host.workspaces[0]);
        return 1;
    }

    PicoWorkspace_RegistrationClear(ws);
    free(host.workspaces[0]);
    return 0;
}

static int TestReloadReusesReleasedModuleSlots(void)
{
    char cfg[] = "/tmp/pico-slot-cfg-XXXXXX";
    char cache[] = "/tmp/pico-slot-cache-XXXXXX";
    PicoHost *host = NULL;
    if (!mkdtemp(cfg) || !mkdtemp(cache))
    {
        Fail("mkdtemp module slot reuse");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        RmRf(cfg);
        RmRf(cache);
        Fail("host init module slot reuse");
        return 1;
    }
    WaitPluginLoad(host);
    int high_water = host->module_count;
    bool reloaded = high_water > 0 && WaitHostReload(host);

    /* Force a failed transaction after it reuses one released hole. The
     * candidate must be removed without disturbing occupied old generations. */
    int rollback_count = host->module_count;
    int kept_hole = -1;
    for (int i = 0; reloaded && i < rollback_count; i++)
    {
        PicoModuleGeneration *module = &host->modules[i];
        if (module->generation == 0)
        {
            if (kept_hole < 0)
            {
                kept_hole = i;
            }
            else
            {
                snprintf(module->source, sizeof(module->source), "/tmp/module-slot-sentinel");
                module->generation = 1;
                module->desired = true;
                module->ref_count = 1;
            }
        }
    }
    host->module_capacity = rollback_count;
    bool failed_cleanly = kept_hole >= 0 && !WaitHostReload(host) &&
                          host->module_count == rollback_count &&
                          host->modules[kept_hole].generation == 0;
    for (int i = 0; i < rollback_count; i++)
    {
        PicoModuleGeneration *module = &host->modules[i];
        if (strcmp(module->source, "/tmp/module-slot-sentinel") == 0)
        {
            failed_cleanly = failed_cleanly && module->desired && module->ref_count == 1;
            module->desired = false;
            PicoModule_Release(module);
        }
    }
    host->module_capacity = PICO_MAX_MODULE_GENERATIONS;
    reloaded = reloaded && failed_cleanly;
    for (int i = 0; i < 32 && reloaded; i++)
    {
        reloaded = WaitHostReload(host);
        if (host->module_count > high_water * 2)
        {
            reloaded = false;
        }
    }
    int final_count = host->module_count;
    uint64_t final_generation = host->next_module_generation;
    bool reused = reloaded && final_count <= high_water * 2 &&
                  final_generation > (uint64_t)final_count;
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    RmRf(cfg);
    RmRf(cache);
    if (!reused)
    {
        Fail("repeated host reloads must reuse released module-generation slots");
        return 1;
    }
    return 0;
}

static int TestScopedExtensionListingRecords(void)
{
    char cfg[256];
    char cache[256];
    char ws1[256];
    char life[512];
    PicoHost *host = NULL;
    if (StartLifecycleHost(&host, cfg, cache, ws1, life, 0) != 0 || !host)
    {
        Fail("lifecycle host init failed");
        return 1;
    }

    char ws2[256];
    snprintf(ws2, sizeof(ws2), "/tmp/pico_test_ws2_%ld", (long)time(NULL));
    mkdir(ws2, 0755);
    PicoWorkspaceId ws2_id = 0;
    (void)pico_workspace_open(host, ws2, &ws2_id);

    int count = PicoPlugins_Count(host);
    if (count <= 0)
    {
        Fail("plugin count should be positive");
        FinishLifecycleHost(host, cfg, cache, ws1);
        rmdir(ws2);
        return 1;
    }

    bool found_host = false;
    bool found_ws = false;
    for (int i = 0; i < count; i++)
    {
        PicoExtInfo info;
        if (!PicoPlugins_Get(host, i, &info))
        {
            Fail("PicoPlugins_Get failed for valid index");
            FinishLifecycleHost(host, cfg, cache, ws1);
            rmdir(ws2);
            return 1;
        }
        if (info.scope == PICO_EXTENSION_HOST)
        {
            found_host = true;
            if (info.workspace_id != 0)
            {
                Fail("host-scoped plugin record must have workspace_id == 0");
                FinishLifecycleHost(host, cfg, cache, ws1);
                rmdir(ws2);
                return 1;
            }
        }
        else if (info.scope == PICO_EXTENSION_WORKSPACE)
        {
            found_ws = true;
            if (info.workspace_id == 0)
            {
                Fail("workspace-scoped plugin record must have non-zero workspace_id");
                FinishLifecycleHost(host, cfg, cache, ws1);
                rmdir(ws2);
                return 1;
            }
        }
    }

    if (!found_host || !found_ws)
    {
        Fail("scoped listing must report both host and workspace records");
        FinishLifecycleHost(host, cfg, cache, ws1);
        rmdir(ws2);
        return 1;
    }

    FinishLifecycleHost(host, cfg, cache, ws1);
    rmdir(ws2);
    return 0;
}

static int FailingWsInitDummy(PicoWorkspace *ws, void **state_out)
{
    (void)ws;
    (void)state_out;
    return -1;
}

static int SuccessfulHostInitDummy(PicoHost *host, void **state_out)
{
    (void)host;
    static int s_host_state = 42;
    *state_out = &s_host_state;
    return 0;
}

static int TestDualScopeIndependentPublicationRollback(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&host);

    PicoModuleGeneration mod;
    memset(&mod, 0, sizeof(mod));
    mod.ext.name = "dual_scope_ext";
    mod.generation = 1;
    mod.desired = true;
    mod.builtin = true;
    mod.ext.host_init = SuccessfulHostInitDummy;
    mod.ext.workspace_init = FailingWsInitDummy;

    bool host_ok = PicoHostExtensions_Activate(&host, &mod);
    bool ws_ok = PicoWorkspaceExtensions_Activate(ws, &mod);

    if (!host_ok || host.host_plugin_count != 1 || !host.host_plugins[0].initialized ||
        PicoHostExtensions_State(&host, "dual_scope_ext") == NULL)
    {
        Fail("host activation must succeed independently of workspace activation");
        free(host.workspaces[0]);
        return 1;
    }
    if (ws_ok || PicoWorkspaceExtensions_State(ws, "dual_scope_ext") != NULL)
    {
        Fail("workspace activation must fail and stay inactive without crashing");
        free(host.workspaces[0]);
        return 1;
    }

    /* Host instance is alive and working */
    if (host.host_plugin_count != 1 || !host.host_plugins[0].initialized)
    {
        Fail("host plugin slot must remain initialized");
        free(host.workspaces[0]);
        return 1;
    }

    PicoHostExtensions_Shutdown(&host);
    PicoWorkspaceExtensions_Shutdown(ws);
    free(host.workspaces[0]);
    return 0;
}

static int g_retained_host_frames;
static int g_retained_workspace_frames;

static void RetainedHostFrame(PicoHost *host, void *state, float dt)
{
    (void)host;
    (void)state;
    (void)dt;
    g_retained_host_frames++;
}

static void RetainedWorkspaceFrame(PicoWorkspace *workspace, void *state, float dt)
{
    (void)workspace;
    (void)state;
    (void)dt;
    g_retained_workspace_frames++;
}

static int TestRetainedActiveGenerationsReceiveFrameCallbacks(void)
{
    PicoHost host;
    PicoModuleGeneration module;
    memset(&host, 0, sizeof(host));
    memset(&module, 0, sizeof(module));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *workspace = PicoHost_PrimaryWorkspace(&host);

    module.ext.name = "retained_frame_extension";
    module.ext.host_on_frame = RetainedHostFrame;
    module.ext.workspace_on_frame = RetainedWorkspaceFrame;
    module.desired = false;
    host.host_plugins[0] = (PicoPluginSlot){
        .name = "retained_frame_extension",
        .module = &module,
        .initialized = true,
    };
    host.host_plugin_count = 1;
    workspace->workspace_plugins[0] = (PicoPluginSlot){
        .name = "retained_frame_extension",
        .module = &module,
        .initialized = true,
    };
    workspace->workspace_plugin_count = 1;

    g_retained_host_frames = 0;
    g_retained_workspace_frames = 0;
    pico_host_pump(&host);

    host.host_plugin_count = 0;
    workspace->workspace_plugin_count = 0;
    host.workspaces[0] = NULL;
    host.workspace_count = 0;
    PicoWorkspace_Free(workspace);
    if (g_retained_host_frames != 1 || g_retained_workspace_frames != 1)
    {
        Fail("one host pump must dispatch each active retained frame callback exactly once");
        return 1;
    }
    return 0;
}

static int g_duplicate_host_inits;
static int g_duplicate_workspace_inits;

static int DuplicateHostInit(PicoHost *host, void **state_out)
{
    (void)host;
    int *state = (int *)malloc(sizeof(*state));
    if (!state)
    {
        return -1;
    }
    *state = ++g_duplicate_host_inits;
    *state_out = state;
    return 0;
}

static void DuplicateHostShutdown(PicoHost *host, void *state)
{
    (void)host;
    free(state);
}

static int DuplicateWorkspaceInit(PicoWorkspace *workspace, void **state_out)
{
    (void)workspace;
    int *state = (int *)malloc(sizeof(*state));
    if (!state)
    {
        return -1;
    }
    *state = ++g_duplicate_workspace_inits;
    *state_out = state;
    return 0;
}

static void DuplicateWorkspaceShutdown(PicoWorkspace *workspace, void *state)
{
    (void)workspace;
    free(state);
}

static int TestExtensionSlotsUseSourceIdentity(void)
{
    PicoHost host;
    PicoModuleGeneration modules[2];
    memset(&host, 0, sizeof(host));
    memset(modules, 0, sizeof(modules));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *workspace = PicoHost_PrimaryWorkspace(&host);
    host.modules = modules;
    host.module_count = 2;
    host.module_capacity = 2;

    for (int i = 0; i < 2; i++)
    {
        snprintf(modules[i].source, sizeof(modules[i].source), "/tmp/duplicate-%d.c", i);
        modules[i].ext.name = "duplicate_descriptor_name";
        modules[i].ext.host_init = DuplicateHostInit;
        modules[i].ext.host_shutdown = DuplicateHostShutdown;
        modules[i].ext.workspace_init = DuplicateWorkspaceInit;
        modules[i].ext.workspace_shutdown = DuplicateWorkspaceShutdown;
        modules[i].generation = (uint64_t)(i + 1);
        modules[i].desired = true;
        modules[i].ref_count = 1;
    }
    g_duplicate_host_inits = 0;
    g_duplicate_workspace_inits = 0;
    bool activated = PicoHostExtensions_Activate(&host, &modules[0]) &&
                     PicoHostExtensions_Activate(&host, &modules[1]) &&
                     PicoWorkspaceExtensions_Activate(workspace, &modules[0]) &&
                     PicoWorkspaceExtensions_Activate(workspace, &modules[1]);
    bool distinct = activated && g_duplicate_host_inits == 2 &&
                    g_duplicate_workspace_inits == 2 && host.host_plugin_count == 2 &&
                    workspace->workspace_plugin_count == 2 && PicoPlugins_Count(&host) == 4;
    for (int i = 0; distinct && i < PicoPlugins_Count(&host); i++)
    {
        PicoExtInfo info;
        if (!PicoPlugins_Get(&host, i, &info) || info.active_generation == 0 ||
            !info.source || (strcmp(info.source, modules[0].source) != 0 &&
                             strcmp(info.source, modules[1].source) != 0))
        {
            distinct = false;
        }
    }

    PicoHostExtensions_Shutdown(&host);
    PicoWorkspaceExtensions_Shutdown(workspace);
    for (int i = 0; i < 2; i++)
    {
        modules[i].desired = false;
        PicoModule_Release(&modules[i]);
    }
    host.workspaces[0] = NULL;
    host.workspace_count = 0;
    PicoWorkspace_Free(workspace);
    if (!distinct)
    {
        Fail("extension instance slots and listing must use source identity, not descriptor name");
        return 1;
    }
    return 0;
}

static int StatelessWsInitDummy(PicoWorkspace *ws, void **state_out)
{
    (void)ws;
    (void)state_out;
    return 0;
}

static int TestStatelessExtensionRollbackDoesNotLeakModule(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&host);

    /* Setup 2 candidate modules: mod1 is stateless (no shutdown callback), mod2 fails init */
    PicoModuleGeneration mod1;
    memset(&mod1, 0, sizeof(mod1));
    mod1.ext.name = "stateless_ext";
    mod1.generation = 1;
    mod1.desired = true;
    mod1.ext.workspace_init = StatelessWsInitDummy;
    mod1.ext.workspace_shutdown = NULL;
    mod1.ref_count = 1;

    PicoModuleGeneration mod2;
    memset(&mod2, 0, sizeof(mod2));
    mod2.ext.name = "failing_ext";
    mod2.generation = 1;
    mod2.desired = true;
    mod2.ext.workspace_init = FailingWsInitDummy;
    mod2.ref_count = 1;

    host.modules = (PicoModuleGeneration *)calloc(2, sizeof(PicoModuleGeneration));
    host.modules[0] = mod1;
    host.modules[1] = mod2;
    host.module_count = 2;
    host.module_capacity = 2;
    ws->state = PICO_WORKSPACE_RELOADING;
    PicoWorkspace_SetAcceptingWork(ws, false);

    bool reload_ok = WaitWorkspaceReload(ws);
    if (reload_ok)
    {
        Fail("workspace reload must fail when one module fails init");
        free(host.modules);
        free(host.workspaces[0]);
        return 1;
    }

    /* mod1 was activated during staging, but on rollback must be released! */
    if (host.modules[0].ref_count != 1)
    {
        Fail("stateless extension module must be released on staging rollback even without shutdown callback");
        free(host.modules);
        free(host.workspaces[0]);
        return 1;
    }
    if (ws->state != PICO_WORKSPACE_OPEN || !PicoWorkspace_AcceptsNewWork(ws))
    {
        Fail("failed workspace reload must roll back to OPEN and restore work acceptance");
        free(host.modules);
        free(host.workspaces[0]);
        return 1;
    }

    free(host.modules);
    free(host.workspaces[0]);
    return 0;
}

static int TestBusyReloadQueuesAndRejectsNewWork(void)
{
    PicoHost host;
    memset(&host, 0, sizeof(host));
    PicoHost_SetPath(&host, ".");
    PicoWorkspace *ws = PicoHost_PrimaryWorkspace(&host);

    /* Simulate workspace being busy */
    ws->accepting_work = true;
    ws->count = 1;
    ws->agents[0] = (PicoAgent *)calloc(1, sizeof(PicoAgent));
    ws->agents[0]->workspace = ws;
    ws->agents[0]->state = PICO_AGENT_TOOL_WAIT;

    bool ok = WaitWorkspaceReload(ws);
    if (ok)
    {
        Fail("reload must not proceed while workspace is busy");
        free(ws->agents[0]);
        free(host.workspaces[0]);
        return 1;
    }

    if (!ws->reload_queued)
    {
        Fail("busy workspace must set reload_queued = true");
        free(ws->agents[0]);
        free(host.workspaces[0]);
        return 1;
    }

    if (ws->state != PICO_WORKSPACE_RELOADING || ws->accepting_work)
    {
        Fail("busy reload must enter RELOADING and reject new work until rollout");
        free(ws->agents[0]);
        free(host.workspaces[0]);
        return 1;
    }

    free(ws->agents[0]);
    free(host.workspaces[0]);
    return 0;
}

static int TestMultiWorkspaceInstructionsIsolation(void)
{
    char dirA[] = "/tmp/pico-ws-instA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-instB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp instructions test");
        return 1;
    }

    char fileA[4096];
    char fileB[4096];
    snprintf(fileA, sizeof(fileA), "%s/AGENTS.md", dirA);
    snprintf(fileB, sizeof(fileB), "%s/AGENTS.md", dirB);
    FILE *fA = fopen(fileA, "wb");
    if (fA) { fputs("INSTRUCTION_ALPHA", fA); fclose(fA); }
    FILE *fB = fopen(fileB, "wb");
    if (fB) { fputs("INSTRUCTION_BETA", fB); fclose(fB); }

    char picoA[4096];
    char picoB[4096];
    snprintf(picoA, sizeof(picoA), "%s/.pico", dirA);
    snprintf(picoB, sizeof(picoB), "%s/.pico", dirB);
    mkdir(picoA, 0755);
    mkdir(picoB, 0755);

    char sysA[4096];
    char sysB[4096];
    snprintf(sysA, sizeof(sysA), "%s/.pico/SYSTEM.md", dirA);
    snprintf(sysB, sizeof(sysB), "%s/.pico/SYSTEM.md", dirB);
    fA = fopen(sysA, "wb");
    if (fA) { fputs("SYSTEM_ALPHA", fA); fclose(fA); }
    fB = fopen(sysB, "wb");
    if (fB) { fputs("SYSTEM_BETA", fB); fclose(fB); }

    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init instructions");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK ||
        pico_workspace_open(host, dirB, &idB) != PICO_OK)
    {
        Fail("open workspaces for instructions");
        pico_host_free(host);
        return 1;
    }

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId agA = 0, agB = 0;
    if (pico_main_agent_create(host, idA, &opt, &agA) != PICO_OK ||
        pico_main_agent_create(host, idB, &opt, &agB) != PICO_OK)
    {
        Fail("create agents for instructions");
        pico_host_free(host);
        return 1;
    }

    PicoAgent *agentA = PicoHost_FindAgent(host, agA);
    PicoAgent *agentB = PicoHost_FindAgent(host, agB);
    char *instA = PicoAgent_BuildInstructions(host, agentA);
    char *instB = PicoAgent_BuildInstructions(host, agentB);

    if (!instA || strstr(instA, "INSTRUCTION_ALPHA") == NULL || strstr(instA, "SYSTEM_ALPHA") == NULL ||
        strstr(instA, "INSTRUCTION_BETA") != NULL || strstr(instA, "SYSTEM_BETA") != NULL)
    {
        Fail("agent A instructions should only contain workspace A instructions");
    }
    if (!instB || strstr(instB, "INSTRUCTION_BETA") == NULL || strstr(instB, "SYSTEM_BETA") == NULL ||
        strstr(instB, "INSTRUCTION_ALPHA") != NULL || strstr(instB, "SYSTEM_ALPHA") != NULL)
    {
        Fail("agent B instructions should only contain workspace B instructions");
    }

    free(instA);
    free(instB);
    pico_host_free(host);

    unlink(fileA);
    unlink(fileB);
    unlink(sysA);
    unlink(sysB);
    rmdir(picoA);
    rmdir(picoB);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static void RunToolA(PicoAgentContext *ctx, const char *args, PicoToolResult *out, void *state)
{
    (void)ctx; (void)args; (void)state;
    out->output = strdup("TOOL_OUTPUT_ALPHA");
    out->is_error = false;
}

static void RunToolB(PicoAgentContext *ctx, const char *args, PicoToolResult *out, void *state)
{
    (void)ctx; (void)args; (void)state;
    out->output = strdup("TOOL_OUTPUT_BETA");
    out->is_error = false;
}

static int TestMultiWorkspaceToolNameIsolation(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init tool isolation");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-toolA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-toolB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp tool isolation");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);
    PicoWorkspace *wsB = PicoHost_FindWorkspace(host, idB);

    PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, wsA);
    pico_add_tool(wsA, "custom_worker_tool", "desc A", "{}", RunToolA, NULL, PICO_TOOL_SEQUENTIAL);
    PicoHost_PublishRegistration(host, NULL);

    PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, wsB);
    pico_add_tool(wsB, "custom_worker_tool", "desc B", "{}", RunToolB, NULL, PICO_TOOL_SEQUENTIAL);
    PicoHost_PublishRegistration(host, NULL);

    int idxA = -1, idxB = -1;
    for (int i = 0; i < wsA->tool_count; i++)
    {
        if (wsA->tools[i].name && strcmp(wsA->tools[i].name, "custom_worker_tool") == 0)
        {
            idxA = i;
            break;
        }
    }
    for (int i = 0; i < wsB->tool_count; i++)
    {
        if (wsB->tools[i].name && strcmp(wsB->tools[i].name, "custom_worker_tool") == 0)
        {
            idxB = i;
            break;
        }
    }

    if (idxA < 0 || idxB < 0)
    {
        Fail("workspace tool registrations must register in both workspaces");
        pico_host_free(host);
        return 1;
    }

    PicoToolResult resA = {0}, resB = {0};
    wsA->tools[idxA].run(NULL, "{}", &resA, wsA->tools[idxA].state);
    wsB->tools[idxB].run(NULL, "{}", &resB, wsB->tools[idxB].state);

    if (!resA.output || strcmp(resA.output, "TOOL_OUTPUT_ALPHA") != 0 ||
        !resB.output || strcmp(resB.output, "TOOL_OUTPUT_BETA") != 0)
    {
        Fail("executing tool with same name in workspace A and B must execute distinct implementations");
    }

    free(resA.output);
    free(resB.output);
    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestMultiWorkspaceMailboxIsolation(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init mailbox isolation");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-mbA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-mbB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp mailbox isolation");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);
    PicoWorkspace *wsB = PicoHost_FindWorkspace(host, idB);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0, a2 = 0, b1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);
    pico_main_agent_create(host, idA, &opt, &a2);
    pico_main_agent_create(host, idB, &opt, &b1);

    PicoAgent *agentA1 = PicoHost_FindAgent(host, a1);
    PicoAgent *agentA2 = PicoHost_FindAgent(host, a2);
    PicoAgent *agentB1 = PicoHost_FindAgent(host, b1);

    PicoWorkspace_UiPost(wsA, "status_box", PICO_UI_POST_TEXT, a1, agentA1->runtime_generation, "A1_POST", 7);
    PicoWorkspace_UiPost(wsA, "status_box", PICO_UI_POST_TEXT, a2, agentA2->runtime_generation, "A2_POST", 7);
    PicoWorkspace_UiPost(wsB, "status_box", PICO_UI_POST_TEXT, b1, agentB1->runtime_generation, "B1_POST", 7);

    PicoWorkspace_PumpUiPosts(wsA);
    PicoWorkspace_PumpUiPosts(wsB);

    PicoUiPost p1 = {0}, p2 = {0}, p3 = {0};
    if (!pico_agent_ui_latest(host, a1, "status_box", &p1) || !p1.text || strcmp(p1.text, "A1_POST") != 0 ||
        !pico_agent_ui_latest(host, a2, "status_box", &p2) || !p2.text || strcmp(p2.text, "A2_POST") != 0 ||
        !pico_agent_ui_latest(host, b1, "status_box", &p3) || !p3.text || strcmp(p3.text, "B1_POST") != 0)
    {
        Fail("mailbox posts with the same name across agents and workspaces must remain completely isolated");
    }

    /* Stale/zero ID must not match or fall back to selection */
    PicoUiPost p_invalid = {0};
    if (pico_agent_ui_latest(host, 0, "status_box", &p_invalid) ||
        pico_agent_ui_latest(host, 9999, "status_box", &p_invalid))
    {
        Fail("lookup on zero or stale agent ID must return false");
    }

    pico_agent_ui_clear(host, a1, "status_box");
    PicoUiPost p1_cleared = {0};
    if (pico_agent_ui_latest(host, a1, "status_box", &p1_cleared))
    {
        Fail("clearing agent a1 mailbox should make it not found");
    }
    if (!pico_agent_ui_latest(host, a2, "status_box", &p2) || strcmp(p2.text, "A2_POST") != 0)
    {
        Fail("clearing a1 mailbox must not clear a2 mailbox");
    }
    if (!pico_agent_ui_latest(host, b1, "status_box", &p3) || strcmp(p3.text, "B1_POST") != 0)
    {
        Fail("clearing a1 mailbox must not clear b1 mailbox");
    }

    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

typedef enum MatrixProviderMode {
    MATRIX_PROVIDER_ASK = 0,
    MATRIX_PROVIDER_BLOCK,
    MATRIX_PROVIDER_STREAM,
    MATRIX_PROVIDER_COMPLETE,
    MATRIX_PROVIDER_TOOL,
} MatrixProviderMode;

static struct MatrixProviderState *g_matrix_states[PICO_MAX_WORKSPACES + 1];

#define MATRIX_RECORD_MAX 8

typedef struct MatrixProviderState {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    MatrixProviderMode mode;
    bool entered;
    bool release;
    bool exited;
    int calls;
    char *answer;
    const char *ask_request;
    bool tool_entered;
    bool tool_release;
    int recorded;
    char models_seen[MATRIX_RECORD_MAX][128];
    char efforts_seen[MATRIX_RECORD_MAX][PICO_EFFORT_LEN];
    char urls_seen[MATRIX_RECORD_MAX][512];
    bool fasts_seen[MATRIX_RECORD_MAX];
    bool clarification_fixture;
    bool delegation_fixture;
    PicoAgentId helper_id;
    bool block_helper;
    bool helper_entered;
    bool helper_permission;
    bool block_helper_tool;
    int helper_calls;
    char *inputs_seen[MATRIX_RECORD_MAX];
    char *instructions_seen[MATRIX_RECORD_MAX];
    char tools_seen[MATRIX_RECORD_MAX][128];
    char keys_seen[MATRIX_RECORD_MAX][80];
    char sessions_seen[MATRIX_RECORD_MAX][40];
} MatrixProviderState;

static void MatrixStateInit(MatrixProviderState *state, MatrixProviderMode mode)
{
    memset(state, 0, sizeof(*state));
    pthread_mutex_init(&state->mu, NULL);
    pthread_cond_init(&state->cv, NULL);
    state->mode = mode;
}

static void MatrixStateRelease(MatrixProviderState *state)
{
    pthread_mutex_lock(&state->mu);
    state->release = true;
    pthread_cond_broadcast(&state->cv);
    pthread_mutex_unlock(&state->mu);
}

static void MatrixStateDestroy(MatrixProviderState *state)
{
    free(state->answer);
    for (int i = 0; i < MATRIX_RECORD_MAX; i++)
    {
        free(state->inputs_seen[i]);
        free(state->instructions_seen[i]);
    }
    pthread_mutex_destroy(&state->mu);
    pthread_cond_destroy(&state->cv);
}

static bool MatrixStateFlag(MatrixProviderState *state, bool exited)
{
    pthread_mutex_lock(&state->mu);
    bool value = exited ? state->exited : state->entered;
    pthread_mutex_unlock(&state->mu);
    return value;
}

static int MatrixProvider(PicoAgentContext *ctx, const PicoLlmTurn *turn,
                          PicoLlmCancelFn cancel, PicoLlmDeltaFn on_delta,
                          void *user, PicoLlmResult *out, void *opaque)
{
    (void)ctx;
    (void)cancel;
    (void)user;
    MatrixProviderState *state = (MatrixProviderState *)opaque;
    PicoWorkspaceId workspace_id = pico_agent_context_workspace_id(ctx);
    if (!state && workspace_id <= PICO_MAX_WORKSPACES)
    {
        state = g_matrix_states[workspace_id];
    }
    if (!state)
    {
        return PICO_LLM_FAIL;
    }
    pthread_mutex_lock(&state->mu);
    int call = state->calls++;
    bool helper = state->clarification_fixture && pico_agent_context_id(ctx) == state->helper_id;
    int helper_call = helper ? ++state->helper_calls : 0;
    bool block_helper = helper && state->block_helper;
    bool helper_permission = helper && state->helper_permission;
    if (helper) state->helper_entered = true;
    state->entered = true;
    if (state->recorded < MATRIX_RECORD_MAX)
    {
        int slot = state->recorded++;
        snprintf(state->models_seen[slot], sizeof(state->models_seen[slot]), "%s",
                 turn && turn->model ? turn->model : "");
        snprintf(state->efforts_seen[slot], sizeof(state->efforts_seen[slot]), "%s",
                 turn && turn->effort ? turn->effort : "");
        state->fasts_seen[slot] = turn && turn->fast;
        snprintf(state->urls_seen[slot], sizeof(state->urls_seen[slot]), "%s", turn->base_url ? turn->base_url : "");
        if (state->clarification_fixture)
        {
            state->instructions_seen[slot] = DupStr(turn->instructions);
            JsonBuf input;
            JsonBuf_Init(&input);
            for (int i = 0; i < turn->input_count; i++) JsonBuf_Puts(&input, turn->input_json[i]);
            state->inputs_seen[slot] = JsonBuf_Steal(&input);
            snprintf(state->keys_seen[slot], sizeof(state->keys_seen[slot]), "%s", turn->cache_key);
            snprintf(state->sessions_seen[slot], sizeof(state->sessions_seen[slot]), "%s", pico_agent_context_session_id(ctx));
            for (int i = 0; i < turn->tool_count; i++)
            {
                if (i) strncat(state->tools_seen[slot], ",", sizeof(state->tools_seen[slot]) - strlen(state->tools_seen[slot]) - 1);
                strncat(state->tools_seen[slot], turn->tools[i].name, sizeof(state->tools_seen[slot]) - strlen(state->tools_seen[slot]) - 1);
            }
        }
    }
    pthread_cond_broadcast(&state->cv);
    MatrixProviderMode mode = state->mode;
    if (mode == MATRIX_PROVIDER_BLOCK)
    {
        while (!state->release)
        {
            pthread_cond_wait(&state->cv, &state->mu);
        }
    }
    pthread_mutex_unlock(&state->mu);

    if (block_helper)
    {
        while (!cancel(user)) usleep(1000);
        return PICO_LLM_CANCEL;
    }
    if (helper)
    {
        if (helper_permission && helper_call == 1)
            pico_llm_result_add_tool_call(out, "clarify-read", "sh", "{\"description\":\"Inspect repository context\",\"command\":\"printf clarification-inspection\"}", NULL);
        else pico_llm_result_add_text(out, turn->compact ? "Summary of clarification discussion." : "A grounded clarification explanation.");
        return PICO_LLM_OK;
    }
    if (mode == MATRIX_PROVIDER_STREAM)
    {
        for (;;)
        {
            pthread_mutex_lock(&state->mu);
            bool release = state->release;
            pthread_mutex_unlock(&state->mu);
            if (release)
            {
                break;
            }
            if (on_delta)
            {
                PicoLlmDelta d = {.kind = PICO_LLM_DELTA_TEXT, .text = "x", .len = 1, .call_index = -1};
                on_delta(user, &d);
            }
            usleep(100);
        }
    }
    if (state->delegation_fixture && call == 0)
    {
        pico_llm_result_add_tool_call(out, "clarify-delegate", "subagent",
                                    "{\"profile\":\"clarify\",\"task\":\"DELEGATION_ASSIGNMENT\"}", NULL);
    }
    else if (mode == MATRIX_PROVIDER_ASK && call == (state->delegation_fixture ? 1 : 0))
    {
        pico_llm_result_add_tool_call(out, "matrix-ask", "matrix_ask", "{}", NULL);
    }
    else if (mode == MATRIX_PROVIDER_TOOL && call == 0)
    {
        pico_llm_result_add_tool_call(out, "matrix-call-1", "matrix_block", "{}", NULL);
    }
    else
    {
        pico_llm_result_add_text(out, mode == MATRIX_PROVIDER_COMPLETE ? "complete" : "done");
    }

    pthread_mutex_lock(&state->mu);
    state->exited = true;
    pthread_cond_broadcast(&state->cv);
    pthread_mutex_unlock(&state->mu);
    return PICO_LLM_OK;
}

static void MatrixAskTool(PicoAgentContext *ctx, const char *args_json,
                          PicoToolResult *out, void *opaque)
{
    (void)args_json;
    MatrixProviderState *state = (MatrixProviderState *)opaque;
    PicoWorkspaceId workspace_id = pico_agent_context_workspace_id(ctx);
    if (!state && workspace_id <= PICO_MAX_WORKSPACES)
    {
        state = g_matrix_states[workspace_id];
    }
    if (!state)
    {
        return;
    }
    char *answer = NULL;
    int rc = pico_tool_ask(ctx, state->ask_request ? state->ask_request :
                           "{\"type\":\"confirm\",\"message\":\"matrix ask\"}", &answer);
    pthread_mutex_lock(&state->mu);
    if (rc == PICO_ASK_OK)
    {
        state->answer = answer;
        answer = NULL;
    }
    pthread_cond_broadcast(&state->cv);
    pthread_mutex_unlock(&state->mu);
    free(answer);
    if (out)
    {
        memset(out, 0, sizeof(*out));
        out->output = DupStr(rc == PICO_ASK_OK ? "answered" : "cancelled");
    }
}

static void MatrixBlockTool(PicoAgentContext *ctx, const char *args_json,
                            PicoToolResult *out, void *opaque)
{
    (void)args_json;
    MatrixProviderState *state = (MatrixProviderState *)opaque;
    PicoWorkspaceId workspace_id = pico_agent_context_workspace_id(ctx);
    if (!state && workspace_id <= PICO_MAX_WORKSPACES)
    {
        state = g_matrix_states[workspace_id];
    }
    if (out)
    {
        memset(out, 0, sizeof(*out));
    }
    if (!state)
    {
        return;
    }
    pthread_mutex_lock(&state->mu);
    state->tool_entered = true;
    pthread_cond_broadcast(&state->cv);
    while (!state->tool_release)
    {
        pthread_cond_wait(&state->cv, &state->mu);
    }
    pthread_mutex_unlock(&state->mu);
    if (out)
    {
        out->output = DupStr("tool done");
    }
}

static bool MatrixSupportsFast(PicoHost *host, const PicoModel *model, void *opaque)
{
    (void)host;
    (void)model;
    (void)opaque;
    return true;
}

static void ClarificationExecutionHook(PicoWorkspace *ws, PicoAgentId id, PicoLlmEvent *event, void *state)
{
    (void)ws; (void)id; (void)state;
    event->extra_instructions = DupStr("MAIN_EXECUTION_HOOK_MARKER");
}

static void ClarificationContextHook(PicoWorkspace *ws, PicoAgentId id, PicoContextEvent *event, void *state)
{
    (void)ws; (void)id; (void)state;
    event->extra_context = DupStr("MAIN_REQUEST_CONTEXT_MARKER");
}

static void ClarificationPermissionHook(PicoAgentContext *ctx, PicoToolEvent *event, void *state)
{
    (void)state;
    if (strcmp(event->name, "sh")) return;
    char *answer = NULL;
    int rc = pico_tool_ask(ctx, "{\"type\":\"confirm\",\"message\":\"Allow clarification inspection?\"}", &answer);
    if (rc != PICO_ASK_OK) { event->deny = true; event->result = DupStr("inspection not permitted"); }
    free(answer);
}

static void ClarificationBlockingHook(PicoAgentContext *ctx, PicoToolEvent *event, void *opaque)
{
    MatrixProviderState *state = opaque;
    if (!state || strcmp(event->name, "sh") || pico_agent_context_id(ctx) != state->helper_id) return;
    pthread_mutex_lock(&state->mu);
    if (state->block_helper_tool)
    {
        state->tool_entered = true;
        pthread_cond_broadcast(&state->cv);
        while (!state->tool_release) pthread_cond_wait(&state->cv, &state->mu);
    }
    pthread_mutex_unlock(&state->mu);
}

static bool ConfigureMatrixWorkspace(PicoHost *host, PicoWorkspace *workspace,
                                     MatrixProviderState *state, bool add_ask_tool)
{
    if (workspace->id <= PICO_MAX_WORKSPACES)
    {
        g_matrix_states[workspace->id] = state;
    }
    workspace->models = (PicoModel *)calloc(1, sizeof(*workspace->models));
    if (!workspace->models)
    {
        return false;
    }
    workspace->model_count = 1;
    snprintf(workspace->models[0].id, sizeof(workspace->models[0].id), "matrix-model");
    snprintf(workspace->models[0].name, sizeof(workspace->models[0].name), "matrix-model");
    snprintf(workspace->models[0].provider, sizeof(workspace->models[0].provider), "matrix");
    snprintf(workspace->settings.default_model, sizeof(workspace->settings.default_model),
             "matrix-model");
    PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, workspace);
    PicoProvider provider = {
        .name = "matrix", .stream = MatrixProvider, .map_context = true, .state = state,
    };
    if (state->mode == MATRIX_PROVIDER_TOOL)
    {
        provider.supports_fast = MatrixSupportsFast;
    }
    pico_add_provider(workspace, &provider);
    bool tool_ok = !add_ask_tool ||
                   pico_add_tool(workspace, "matrix_ask", "matrix ask", "{}",
                                 MatrixAskTool, NULL, PICO_TOOL_SEQUENTIAL);
    if (state->mode == MATRIX_PROVIDER_TOOL)
    {
        tool_ok = tool_ok && pico_add_tool(workspace, "matrix_block", "matrix block", "{}",
                                           MatrixBlockTool, NULL, PICO_TOOL_SEQUENTIAL);
    }
    if (state->clarification_fixture)
    {
        /* The compiled-in sh tool is already registered in real-host fixtures. */
        pico_add_llm_hook(workspace, ClarificationExecutionHook);
        pico_add_context_hook(workspace, ClarificationContextHook);
        if (state->helper_permission) pico_add_tool_before_hook(workspace, ClarificationPermissionHook);
        if (state->block_helper_tool) pico_add_tool_before_hook(workspace, ClarificationBlockingHook);
    }
    PicoHost_PublishRegistration(host, state);
    return tool_ok && pico_workspace_find_provider(workspace, "matrix") != NULL;
}

static bool PumpUntilIdle(PicoHost *host, PicoAgent *agent)
{
    PICO_TEST_WAIT_LOOP("asynchronous completion")
    {
        pico_host_pump(host);
        if (!PicoAgent_IsBusy(agent))
        {
            return true;
        }
    }
    return false;
}

/* Selecting an agent stamps its workspace as active; the live-workspace cap
 * then evicts the oldest workspace with no recent activity, not the one the
 * user most recently selected. */
static int TestWorkspaceOpenEvictsLeastRecentlyActiveIdle(void)
{
    PicoHost *host = NULL;
    char dirs[PICO_MAX_WORKSPACES][64] = {{0}};
    PicoWorkspaceId ids[PICO_MAX_WORKSPACES] = {0};
    PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    PicoAgentId rescued = 0;
    PicoAgentId untouched = 0;
    int i;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init lru eviction");
        return 1;
    }
    for (i = 0; i < PICO_MAX_WORKSPACES; i++)
    {
        snprintf(dirs[i], sizeof(dirs[i]), "/tmp/pico-ws-lru-%d-XXXXXX", i);
        if (!mkdtemp(dirs[i]) || pico_workspace_open(host, dirs[i], &ids[i]) != PICO_OK)
        {
            Fail("open workspaces for lru eviction");
            goto done;
        }
    }
    /* Selecting the first workspace's agent re-stamps it, making it more
     * recent than every never-touched workspace even though it opened first. */
    if (pico_main_agent_create(host, ids[0], &opt, &rescued) != PICO_OK ||
        pico_main_agent_create(host, ids[1], &opt, &untouched) != PICO_OK)
    {
        Fail("create agents for lru eviction");
        goto done;
    }
    if (!pico_agent_select(host, rescued))
    {
        Fail("select rescued agent for lru eviction");
        goto done;
    }
    {
        char extra[] = "/tmp/pico-ws-lru-extra-XXXXXX";
        PicoWorkspaceId extra_id = 0;
        if (mkdtemp(extra))
        {
            bool rescued_live = false;
            bool victim_gone = true;
            PicoWorkspaceInfo probe;
            if (pico_workspace_open(host, extra, &extra_id) != PICO_OK || extra_id == 0)
            {
                Fail("opening past the workspace cap should evict an idle workspace");
            }
            else if (pico_workspace_count(host) != PICO_MAX_WORKSPACES)
            {
                Fail("eviction should keep the live workspace count at the cap");
            }
            for (i = 0; i < pico_workspace_count(host); i++)
            {
                if (!pico_workspace_info(host, i, &probe))
                {
                    continue;
                }
                if (probe.id == ids[0])
                {
                    rescued_live = true;
                }
                if (probe.id == ids[1])
                {
                    victim_gone = false;
                }
            }
            if (!rescued_live || !victim_gone)
            {
                Fail("eviction should target the never-touched workspace, not the recently selected one");
            }
            rmdir(extra);
        }
    }
done:
    pico_host_free(host);
    for (i = 0; i < PICO_MAX_WORKSPACES; i++)
    {
        if (dirs[i][0])
        {
            rmdir(dirs[i]);
        }
    }
    return g_failed ? 1 : 0;
}

/* The workspace cap still surfaces PICO_LIMIT when every live workspace is
 * either the selected one or has an agent mid-turn. */
static int TestWorkspaceOpenLimitWhenAllBusy(void)
{
    PicoHost *host = NULL;
    char dirs[PICO_MAX_WORKSPACES][64];
    PicoWorkspaceId ids[PICO_MAX_WORKSPACES];
    MatrixProviderState states[PICO_MAX_WORKSPACES] = {{0}};
    PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    PicoAgentId agents[PICO_MAX_WORKSPACES] = {0};
    int i;
    for (i = 0; i < PICO_MAX_WORKSPACES; i++)
    {
        snprintf(dirs[i], sizeof(dirs[i]), "/tmp/pico-ws-busy-%d-XXXXXX", i);
        if (!mkdtemp(dirs[i]))
        {
            Fail("mkdtemp busy cap");
            for (i = i - 1; i >= 0; i--)
            {
                rmdir(dirs[i]);
            }
            return 1;
        }
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init busy cap");
        goto done;
    }
    for (i = 0; i < PICO_MAX_WORKSPACES; i++)
    {
        PicoWorkspace *ws;
        if (pico_workspace_open(host, dirs[i], &ids[i]) != PICO_OK)
        {
            Fail("open workspace busy cap");
            goto done;
        }
        ws = PicoHost_FindWorkspace(host, ids[i]);
        MatrixStateInit(&states[i], MATRIX_PROVIDER_BLOCK);
        if (!ConfigureMatrixWorkspace(host, ws, &states[i], false))
        {
            Fail("configure matrix provider busy cap");
            goto done;
        }
        if (pico_main_agent_create(host, ids[i], &opt, &agents[i]) != PICO_OK)
        {
            Fail("create agent busy cap");
            goto done;
        }
    }
    if (!pico_agent_select(host, agents[0]))
    {
        Fail("select agent busy cap");
        goto done;
    }
    for (i = 1; i < PICO_MAX_WORKSPACES; i++)
    {
        PicoAgent_StartTurn(host, PicoHost_FindAgent(host, agents[i]), "busy");
        PICO_TEST_WAIT(!MatrixStateFlag(&states[i], false))
        {
            pico_host_pump(host);
        }
        if (!PicoAgent_IsBusy(PicoHost_FindAgent(host, agents[i])))
        {
            Fail("busy-cap fixture should keep agents mid-turn");
            goto done;
        }
    }
    {
        char extra[] = "/tmp/pico-ws-busy-extra-XXXXXX";
        PicoWorkspaceId extra_id = 0;
        if (mkdtemp(extra))
        {
            if (pico_workspace_open(host, extra, &extra_id) != PICO_LIMIT || extra_id != 0)
            {
                Fail("opening past the workspace cap should fail when nothing is evictable");
            }
            else if (pico_workspace_count(host) != PICO_MAX_WORKSPACES)
            {
                Fail("failed capacity open must not change the live workspace count");
            }
            rmdir(extra);
        }
    }
done:
    if (host)
    {
        for (i = 0; i < PICO_MAX_WORKSPACES; i++)
        {
            MatrixStateRelease(&states[i]);
        }
        for (i = 1; i < PICO_MAX_WORKSPACES; i++)
        {
            if (agents[i])
            {
                PumpUntilIdle(host, PicoHost_FindAgent(host, agents[i]));
            }
        }
        pico_host_free(host);
    }
    for (i = 0; i < PICO_MAX_WORKSPACES; i++)
    {
        MatrixStateDestroy(&states[i]);
        if (dirs[i][0]) rmdir(dirs[i]);
    }
    return g_failed ? 1 : 0;
}

/* At the host-wide agent cap, creating another main agent closes the
 * least-recently-active idle agent; the selected agent is never the victim. */
static int TestMainAgentCreateEvictsIdleAgent(void)
{
    PicoHost *host = NULL;
    char dirA[] = "/tmp/pico-agent-evict-A-XXXXXX";
    char dirB[] = "/tmp/pico-agent-evict-B-XXXXXX";
    char dirC[] = "/tmp/pico-agent-evict-C-XXXXXX";
    PicoWorkspaceId idA = 0, idB = 0, idC = 0;
    PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    PicoAgentId ids[PICO_MAX_TOTAL_AGENTS];
    int i;
    if (!mkdtemp(dirA) || !mkdtemp(dirB) || !mkdtemp(dirC))
    {
        Fail("mkdtemp agent eviction");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init agent eviction");
        goto done;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK ||
        pico_workspace_open(host, dirB, &idB) != PICO_OK ||
        pico_workspace_open(host, dirC, &idC) != PICO_OK)
    {
        Fail("open workspaces agent eviction");
        goto done;
    }
    for (i = 0; i < PICO_MAX_TOTAL_AGENTS; i++)
    {
        if (pico_main_agent_create(host, i < PICO_MAX_AGENTS ? idA : idB, &opt, &ids[i]) != PICO_OK)
        {
            Fail("fill host agent budget for eviction");
            goto done;
        }
    }
    /* The first published agent is auto-selected and therefore protected even
     * though it is the oldest; the second-oldest is the eviction victim. The
     * overflow agent is created in an empty workspace, so the host-wide budget,
     * not a per-workspace cap, is what triggers the eviction. */
    {
        PicoAgentId created = 0;
        if (pico_main_agent_create(host, idC, &opt, &created) != PICO_OK || created == 0)
        {
            Fail("creating past the agent cap should evict an idle agent");
            goto done;
        }
        if (pico_agent_count(host) != PICO_MAX_TOTAL_AGENTS)
        {
            Fail("agent eviction should keep the live count at the cap");
            goto done;
        }
        if (PicoHost_FindAgent(host, ids[0]) == NULL)
        {
            Fail("agent eviction must never close the selected agent");
            goto done;
        }
        if (PicoHost_FindAgent(host, ids[1]) != NULL)
        {
            Fail("agent eviction should close the least-recently-active idle agent");
            goto done;
        }
    }
done:
    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    rmdir(dirC);
    return g_failed ? 1 : 0;
}

/* At the per-workspace agent cap, eviction only closes idle agents inside that
 * workspace; an older idle agent elsewhere survives. */
static int TestMainAgentCreateEvictsPerWorkspaceCap(void)
{
    PicoHost *host = NULL;
    char dirA[] = "/tmp/pico-agent-ws-evict-A-XXXXXX";
    char dirB[] = "/tmp/pico-agent-ws-evict-B-XXXXXX";
    PicoWorkspaceId idA = 0, idB = 0;
    PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    PicoAgentId older_elsewhere = 0;
    PicoAgentId ids[PICO_MAX_AGENTS];
    int i;
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp per-workspace eviction");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init per-workspace eviction");
        goto done;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK ||
        pico_workspace_open(host, dirB, &idB) != PICO_OK)
    {
        Fail("open workspaces per-workspace eviction");
        goto done;
    }
    /* The oldest agent overall lives in the other workspace; it must survive. */
    if (pico_main_agent_create(host, idB, &opt, &older_elsewhere) != PICO_OK)
    {
        Fail("create older agent elsewhere");
        goto done;
    }
    for (i = 0; i < PICO_MAX_AGENTS; i++)
    {
        if (pico_main_agent_create(host, idA, &opt, &ids[i]) != PICO_OK)
        {
            Fail("fill workspace agent budget for eviction");
            goto done;
        }
    }
    {
        PicoAgentId created = 0;
        PicoWorkspaceInfo probe;
        int ws_a_count = -1;
        if (pico_main_agent_create(host, idA, &opt, &created) != PICO_OK || created == 0)
        {
            Fail("creating past the workspace agent cap should evict an idle agent");
            goto done;
        }
        for (i = 0; i < pico_workspace_count(host); i++)
        {
            if (pico_workspace_info(host, i, &probe) && probe.id == idA)
            {
                ws_a_count = probe.total_agent_count;
            }
        }
        if (ws_a_count != PICO_MAX_AGENTS)
        {
            Fail("workspace agent eviction should keep the workspace at its cap");
            goto done;
        }
        /* The selected agent lives in the other workspace, so the oldest agent
         * in the capped workspace is the least-recently-active victim. */
        if (PicoHost_FindAgent(host, ids[0]) != NULL)
        {
            Fail("workspace agent eviction should close a victim in that workspace");
            goto done;
        }
        if (PicoHost_FindAgent(host, older_elsewhere) == NULL)
        {
            Fail("workspace agent eviction must not close agents in other workspaces");
            goto done;
        }
    }
done:
    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return g_failed ? 1 : 0;
}

static int TestCdRollsBackNewWorkspaceOnAgentLimit(void)
{
    const int workspace_count = (PICO_MAX_TOTAL_AGENTS + PICO_MAX_AGENTS - 1) / PICO_MAX_AGENTS;
    /* This scenario needs room to open a workspace after filling the agent
     * budget with busy agents. Otherwise workspace capacity rejects /cd
     * before agent creation. */
    if (workspace_count >= PICO_MAX_WORKSPACES)
    {
        return 0;
    }
    PicoHost *host = NULL;
    char root[] = "/tmp/pico-cdlim-XXXXXX";
    char directories[PICO_MAX_WORKSPACES][128] = {{0}};
    int created = 0;
    PicoWorkspaceId first = 0;
    MatrixProviderState states[PICO_MAX_WORKSPACES] = {{0}};
    PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    if (!mkdtemp(root))
    {
        Fail("mkdtemp cd rollback");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init cd rollback");
        rmdir(root);
        return 1;
    }
    int total = 0;
    for (int w = 0; w <= workspace_count; w++)
    {
        snprintf(directories[w], sizeof(directories[w]), "%s/workspace-%d", root, w);
        if (mkdir(directories[w], 0700) != 0)
        {
            Fail("mkdir cd rollback workspace");
            goto done;
        }
        created++;
        if (w == workspace_count)
        {
            break;
        }
        PicoWorkspaceId workspace = 0;
        if (pico_workspace_open(host, directories[w], &workspace) != PICO_OK)
        {
            Fail("open workspace cd rollback");
            goto done;
        }
        if (w == 0)
        {
            first = workspace;
        }
        /* Every agent must be mid-turn: idle agents would be evicted to make
         * room and the creation failure under test would not happen. */
        MatrixStateInit(&states[w], MATRIX_PROVIDER_BLOCK);
        if (!ConfigureMatrixWorkspace(host, PicoHost_FindWorkspace(host, workspace),
                                      &states[w], false))
        {
            Fail("configure matrix provider cd rollback");
            goto done;
        }
        for (int i = 0; i < PICO_MAX_AGENTS && total < PICO_MAX_TOTAL_AGENTS; i++, total++)
        {
            PicoAgentId id = 0;
            if (pico_main_agent_create(host, workspace, &opt, &id) != PICO_OK)
            {
                Fail("fill host agent budget for cd rollback");
                goto done;
            }
            PicoAgent *busy_agent = PicoHost_FindAgent(host, id);
            PicoAgent_StartTurn(host, busy_agent, "busy");
            if (!PicoAgent_IsBusy(busy_agent))
            {
                Fail("cd rollback fixture should keep agents mid-turn");
                goto done;
            }
        }
    }
    for (int w = 0; w < workspace_count; w++)
    {
        PICO_TEST_WAIT(!MatrixStateFlag(&states[w], false))
        {
            pico_host_pump(host);
        }
    }
    int count_before = pico_workspace_count(host);
    if (PicoHost_ChangeWorkspace(host, PicoHost_FindWorkspace(host, first), directories[workspace_count]) ||
        pico_workspace_count(host) != count_before)
    {
        Fail("cd must roll back a newly opened workspace when agent creation fails");
    }

done:
    if (host)
    {
        for (int w = 0; w < workspace_count; w++)
        {
            MatrixStateRelease(&states[w]);
        }
        pico_host_free(host);
    }
    for (int w = 0; w < workspace_count; w++)
    {
        MatrixStateDestroy(&states[w]);
    }
    for (int i = 0; i < created; i++)
    {
        rmdir(directories[i]);
    }
    rmdir(root);
    return g_failed ? 1 : 0;
}


/* The agent cap still surfaces PICO_LIMIT when every candidate in scope is
 * mid-turn. */
static int TestMainAgentCreateLimitWhenAllBusy(void)
{
    PicoHost *host = NULL;
    char dirA[] = "/tmp/pico-agent-busy-A-XXXXXX";
    PicoWorkspaceId idA = 0;
    MatrixProviderState state = {0};
    PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    PicoAgentId ids[PICO_MAX_AGENTS] = {0};
    int i;
    if (!mkdtemp(dirA))
    {
        Fail("mkdtemp busy agent cap");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init busy agent cap");
        goto done;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK)
    {
        Fail("open workspace busy agent cap");
        goto done;
    }
    MatrixStateInit(&state, MATRIX_PROVIDER_BLOCK);
    if (!ConfigureMatrixWorkspace(host, PicoHost_FindWorkspace(host, idA), &state, false))
    {
        Fail("configure matrix provider busy agent cap");
        goto done;
    }
    for (i = 0; i < PICO_MAX_AGENTS; i++)
    {
        if (pico_main_agent_create(host, idA, &opt, &ids[i]) != PICO_OK)
        {
            Fail("fill workspace agent budget busy cap");
            goto done;
        }
        PicoAgent_StartTurn(host, PicoHost_FindAgent(host, ids[i]), "busy");
    }
    PICO_TEST_WAIT(!MatrixStateFlag(&state, false))
    {
        pico_host_pump(host);
    }
    for (i = 0; i < PICO_MAX_AGENTS; i++)
    {
        if (!PicoAgent_IsBusy(PicoHost_FindAgent(host, ids[i])))
        {
            Fail("busy agent-cap fixture should keep agents mid-turn");
            goto done;
        }
    }
    {
        PicoAgentId created = 0;
        if (pico_main_agent_create(host, idA, &opt, &created) != PICO_LIMIT || created != 0)
        {
            Fail("creating past the workspace agent cap should fail when nothing is evictable");
            goto done;
        }
    }
done:
    if (host)
    {
        MatrixStateRelease(&state);
        for (i = 0; i < PICO_MAX_AGENTS; i++)
        {
            if (ids[i])
            {
                PumpUntilIdle(host, PicoHost_FindAgent(host, ids[i]));
            }
        }
        pico_host_free(host);
    }
    MatrixStateDestroy(&state);
    rmdir(dirA);
    return g_failed ? 1 : 0;
}

/* A session-reset hook that creates an agent at the cap must fail rather than
 * evict the agent whose hook dispatch is still on the stack. */
static PicoResult g_reset_hook_create_result;
static PicoAgentId g_reset_hook_created;

static void SessionResetCreatingHook(PicoWorkspace *workspace, const PicoHookEvent *event,
                                     void *state)
{
    PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    (void)state;
    if (!workspace || !workspace->host || !event || !event->agent_id)
    {
        return;
    }
    g_reset_hook_create_result = pico_main_agent_create(workspace->host, workspace->id, &opt,
                                                        &g_reset_hook_created);
}

static int TestSessionResetHookCreateProtectsDispatchingAgent(void)
{
    PicoHost *host = NULL;
    char dirA[] = "/tmp/pico-hook-evict-A-XXXXXX";
    PicoWorkspaceId idA = 0;
    PicoWorkspace *wsA;
    MatrixProviderState state = {0};
    PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    PicoAgentId fillers[PICO_MAX_AGENTS] = {0};
    PicoAgentId created = 0;
    int i;
    if (!mkdtemp(dirA))
    {
        Fail("mkdtemp hook agent eviction");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init hook agent eviction");
        goto done;
    }
    if (pico_workspace_open(host, dirA, &idA) != PICO_OK)
    {
        Fail("open workspace hook agent eviction");
        goto done;
    }
    wsA = PicoHost_FindWorkspace(host, idA);
    PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, wsA);
    pico_workspace_add_hook(wsA, PICO_HOOK_ON_SESSION_RESET, SessionResetCreatingHook);
    PicoHost_PublishRegistration(host, NULL);
    /* Every filler except the selected first is mid-turn, so the newly
     * published agent is the only evictable candidate when its own
     * session-reset hook tries to create another agent at the cap. */
    MatrixStateInit(&state, MATRIX_PROVIDER_BLOCK);
    if (!ConfigureMatrixWorkspace(host, wsA, &state, false))
    {
        Fail("configure matrix provider hook agent eviction");
        goto done;
    }
    for (i = 0; i < PICO_MAX_AGENTS - 1; i++)
    {
        PicoAgent *filler_agent;
        if (pico_main_agent_create(host, idA, &opt, &fillers[i]) != PICO_OK)
        {
            Fail("fill workspace for hook agent eviction");
            goto done;
        }
        if (i == 0)
        {
            continue;
        }
        filler_agent = PicoHost_FindAgent(host, fillers[i]);
        PicoAgent_StartTurn(host, filler_agent, "busy");
        if (!PicoAgent_IsBusy(filler_agent))
        {
            Fail("hook agent eviction fixture should keep agents mid-turn");
            goto done;
        }
    }
    PICO_TEST_WAIT(!MatrixStateFlag(&state, false))
    {
        pico_host_pump(host);
    }
    g_reset_hook_create_result = PICO_INVALID;
    g_reset_hook_created = 0;
    if (pico_main_agent_create(host, idA, &opt, &created) != PICO_OK || created == 0)
    {
        Fail("create agent whose hook creates another agent");
        goto done;
    }
    /* The dispatching agent survives; the hook's create refused to evict it
     * and failed with PICO_LIMIT instead. */
    if (PicoHost_FindAgent(host, created) == NULL)
    {
        Fail("a hook-triggered create must not evict the agent whose hook is running");
        goto done;
    }
    if (g_reset_hook_create_result != PICO_LIMIT || g_reset_hook_created != 0)
    {
        Fail("the hook create must fail rather than evict the dispatching agent");
        goto done;
    }
    for (i = 0; i < PICO_MAX_AGENTS - 1; i++)
    {
        if (PicoHost_FindAgent(host, fillers[i]) == NULL)
        {
            Fail("a hook-triggered create must not close fixture agents");
            goto done;
        }
    }
done:
    if (host)
    {
        MatrixStateRelease(&state);
        for (i = 1; i < PICO_MAX_AGENTS - 1; i++)
        {
            if (fillers[i])
            {
                PumpUntilIdle(host, PicoHost_FindAgent(host, fillers[i]));
            }
        }
        pico_host_free(host);
    }
    MatrixStateDestroy(&state);
    rmdir(dirA);
    return g_failed ? 1 : 0;
}

/* A hook that opens a workspace at the workspace cap must not close the
 * workspace whose hook dispatch is on the stack; after the dispatch pops, that
 * same idle workspace becomes evictable again. */
static PicoResult g_reset_hook_open_result;
static char g_reset_hook_open_path[4096];
static PicoWorkspaceId g_reset_hook_open_id;

static void SessionResetOpeningHook(PicoWorkspace *workspace, const PicoHookEvent *event,
                                    void *state)
{
    (void)state;
    if (!workspace || !workspace->host || !event || !event->agent_id)
    {
        return;
    }
    g_reset_hook_open_result = pico_workspace_open(workspace->host, g_reset_hook_open_path,
                                                   &g_reset_hook_open_id);
}

static int TestSessionResetHookOpenProtectsDispatchingWorkspace(void)
{
    PicoHost *host = NULL;
    char dirs[PICO_MAX_WORKSPACES][64] = {{0}};
    char extra[] = "/tmp/pico-hook-ws-extra-XXXXXX";
    MatrixProviderState states[PICO_MAX_WORKSPACES] = {{0}};
    PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    PicoAgentId agents[PICO_MAX_WORKSPACES] = {0};
    PicoWorkspaceId ids[PICO_MAX_WORKSPACES] = {0};
    int i;
    for (i = 0; i < PICO_MAX_WORKSPACES; i++)
    {
        snprintf(dirs[i], sizeof(dirs[i]), "/tmp/pico-hook-ws-%d-XXXXXX", i);
        if (!mkdtemp(dirs[i]))
        {
            Fail("mkdtemp hook workspace eviction");
            goto done;
        }
    }
    if (!mkdtemp(extra))
    {
        Fail("mkdtemp hook workspace extra");
        goto done;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init hook workspace eviction");
        goto done;
    }
    for (i = 0; i < PICO_MAX_WORKSPACES; i++)
    {
        if (pico_workspace_open(host, dirs[i], &ids[i]) != PICO_OK)
        {
            Fail("open workspace hook workspace eviction");
            goto done;
        }
    }
    /* Workspace 0 is the idle dispatching workspace (oldest, so it is the LRU
     * victim). Workspace 1 holds the selected agent. The rest stay busy. */
    {
        PicoWorkspace *ws0 = PicoHost_FindWorkspace(host, ids[0]);
        PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, ws0);
        pico_workspace_add_hook(ws0, PICO_HOOK_ON_SESSION_RESET, SessionResetOpeningHook);
        PicoHost_PublishRegistration(host, NULL);
    }
    for (i = 1; i < PICO_MAX_WORKSPACES; i++)
    {
        PicoWorkspace *ws = PicoHost_FindWorkspace(host, ids[i]);
        MatrixStateInit(&states[i], MATRIX_PROVIDER_BLOCK);
        if (!ConfigureMatrixWorkspace(host, ws, &states[i], false))
        {
            Fail("configure matrix provider hook workspace eviction");
            goto done;
        }
        if (pico_main_agent_create(host, ids[i], &opt, &agents[i]) != PICO_OK)
        {
            Fail("create agent hook workspace eviction");
            goto done;
        }
        if (i == 1)
        {
            if (!pico_agent_select(host, agents[1]))
            {
                Fail("select agent hook workspace eviction");
                goto done;
            }
        }
        else
        {
            PicoAgent_StartTurn(host, PicoHost_FindAgent(host, agents[i]), "busy");
        }
    }
    for (i = 2; i < PICO_MAX_WORKSPACES; i++)
    {
        PICO_TEST_WAIT(!MatrixStateFlag(&states[i], false))
        {
            pico_host_pump(host);
        }
    }
    snprintf(g_reset_hook_open_path, sizeof(g_reset_hook_open_path), "%s", extra);
    g_reset_hook_open_result = PICO_INVALID;
    g_reset_hook_open_id = 0;
    if (pico_main_agent_create(host, ids[0], &opt, &agents[0]) != PICO_OK)
    {
        Fail("create agent whose hook opens a workspace");
        goto done;
    }
    {
        PicoWorkspace *ws0 = PicoHost_FindWorkspace(host, ids[0]);
        if (g_reset_hook_open_result != PICO_LIMIT || g_reset_hook_open_id != 0)
        {
            Fail("a hook-triggered open must fail rather than close the dispatching workspace");
            goto done;
        }
        if (!ws0 || ws0->state != PICO_WORKSPACE_OPEN || pico_workspace_count(host) != PICO_MAX_WORKSPACES)
        {
            Fail("the dispatching workspace must survive a hook-triggered capacity open");
            goto done;
        }
    }
    /* Outside any dispatch the hold is gone: the same idle workspace is now
     * the LRU victim and the open succeeds. */
    {
        PicoWorkspaceId extra_id = 0;
        bool ws0_gone = true;
        PicoWorkspaceInfo probe;
        if (pico_workspace_open(host, extra, &extra_id) != PICO_OK || extra_id == 0)
        {
            Fail("open after the hook should evict the now-unheld idle workspace");
            goto done;
        }
        for (i = 0; i < pico_workspace_count(host); i++)
        {
            if (pico_workspace_info(host, i, &probe) && probe.id == ids[0])
            {
                ws0_gone = false;
            }
        }
        if (!ws0_gone || pico_workspace_count(host) != PICO_MAX_WORKSPACES)
        {
            Fail("the previously held workspace should be evictable after its dispatch popped");
            goto done;
        }
    }
done:
    if (host)
    {
        for (i = 1; i < PICO_MAX_WORKSPACES; i++)
        {
            MatrixStateRelease(&states[i]);
        }
        for (i = 2; i < PICO_MAX_WORKSPACES; i++)
        {
            if (agents[i])
            {
                PumpUntilIdle(host, PicoHost_FindAgent(host, agents[i]));
            }
        }
        pico_host_free(host);
    }
    for (i = 0; i < PICO_MAX_WORKSPACES; i++)
    {
        if (i >= 1)
        {
            MatrixStateDestroy(&states[i]);
        }
        rmdir(dirs[i]);
    }
    rmdir(extra);
    return g_failed ? 1 : 0;
}




static bool ConfigureMatrixTwoModels(PicoWorkspace *workspace)
{
    PicoModel *models = realloc(workspace->models, 2 * sizeof(*workspace->models));
    if (!models)
    {
        return false;
    }
    memset(&models[1], 0, sizeof(models[1]));
    workspace->models = models;
    workspace->model_count = 2;
    snprintf(models[0].id, sizeof(models[0].id), "matrix-a");
    snprintf(models[0].name, sizeof(models[0].name), "matrix-a");
    models[0].supports_fast = true;
    snprintf(models[0].effort[0], sizeof(models[0].effort[0]), "low");
    snprintf(models[0].effort[1], sizeof(models[0].effort[1]), "high");
    models[0].effort_count = 2;
    snprintf(models[0].default_effort, sizeof(models[0].default_effort), "low");
    snprintf(models[1].id, sizeof(models[1].id), "matrix-b");
    snprintf(models[1].name, sizeof(models[1].name), "matrix-b");
    snprintf(models[1].provider, sizeof(models[1].provider), "matrix");
    snprintf(models[1].effort[0], sizeof(models[1].effort[0]), "medium");
    models[1].effort_count = 1;
    snprintf(models[1].default_effort, sizeof(models[1].default_effort), "medium");
    snprintf(workspace->settings.default_model, sizeof(workspace->settings.default_model),
             "matrix-a");
    return true;
}

static bool MatrixWaitToolEntered(PicoHost *host, MatrixProviderState *state)
{
    PICO_TEST_WAIT_LOOP("asynchronous completion")
    {
        pico_host_pump(host);
        pthread_mutex_lock(&state->mu);
        bool entered = state->tool_entered;
        pthread_mutex_unlock(&state->mu);
        if (entered)
        {
            return true;
        }
    }
    return false;
}

static void MatrixReleaseTool(MatrixProviderState *state)
{
    pthread_mutex_lock(&state->mu);
    state->tool_release = true;
    pthread_cond_broadcast(&state->cv);
    pthread_mutex_unlock(&state->mu);
}

static bool MatrixRecordedIs(const MatrixProviderState *state, int slot, const char *model,
                             const char *effort)
{
    return state->recorded > slot && strcmp(state->models_seen[slot], model) == 0 &&
           strcmp(state->efforts_seen[slot], effort) == 0;
}

/* Switching the model mid-turn must not reroute the running turn: every
 * request of the turn (tool follow-ups included) keeps the model and effort
 * pinned at turn start; the new selection applies from the next turn. */
static int TestTurnKeepsPinnedModelAndEffort(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init pinned turn");
        return 1;
    }
    char dir[] = "/tmp/pico-ws-pinturn-XXXXXX";
    if (!mkdtemp(dir))
    {
        Fail("mkdtemp pinned turn");
        pico_host_free(host);
        return 1;
    }
    PicoWorkspaceId id = 0;
    pico_workspace_open(host, dir, &id);
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, id);
    MatrixProviderState state;
    MatrixStateInit(&state, MATRIX_PROVIDER_TOOL);
    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId agent_id = 0;
    bool ok = ws && ConfigureMatrixWorkspace(host, ws, &state, false) &&
              ConfigureMatrixTwoModels(ws) &&
              pico_main_agent_create(host, id, &opt, &agent_id) == PICO_OK;
    PicoAgent *agent = PicoHost_FindAgent(host, agent_id);
    if (ok && agent)
    {
        ok = PicoSettings_SetEffort(agent, "high");
    }
    if (ok)
    {
        PicoAgent_StartTurn(host, agent, "first");
        ok = MatrixWaitToolEntered(host, &state);
    }
    bool first_pinned = ok && MatrixRecordedIs(&state, 0, "matrix-a", "high");

    /* Mid-turn switch: selection changes immediately... */
    bool switched = first_pinned && PicoSettings_SetModel(agent, "matrix-b") &&
                    strcmp(agent->model, "matrix-b") == 0;

    MatrixReleaseTool(&state);
    bool completed = switched && PumpUntilIdle(host, agent);
    /* ...but the running turn's follow-up keeps the pinned model and effort. */
    bool turn_pinned = completed && agent->error == NULL &&
                       MatrixRecordedIs(&state, 1, "matrix-a", "high");

    bool next_ok = turn_pinned;
    if (next_ok)
    {
        PicoAgent_StartTurn(host, agent, "next");
        next_ok = PumpUntilIdle(host, agent);
    }
    bool next_applied = next_ok && MatrixRecordedIs(&state, 2, "matrix-b", "medium");

    pico_host_free(host);
    MatrixStateDestroy(&state);
    rmdir(dir);
    if (!turn_pinned)
    {
        Fail("busy turn must keep the model and effort pinned at turn start");
        return 1;
    }
    if (!next_applied)
    {
        Fail("selected model and effort must apply from the next turn");
        return 1;
    }
    return 0;
}

/* With Fast on, switching to a non-Fast model mid-turn must not fail the
 * running turn: the turn keeps its pinned Fast-capable model; the selection
 * (Fast cleared) applies from the next turn. */
static int TestFastSurvivesMidTurnModelSwitch(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init fast pin");
        return 1;
    }
    char dir[] = "/tmp/pico-ws-fastpin-XXXXXX";
    if (!mkdtemp(dir))
    {
        Fail("mkdtemp fast pin");
        pico_host_free(host);
        return 1;
    }
    PicoWorkspaceId id = 0;
    pico_workspace_open(host, dir, &id);
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, id);
    MatrixProviderState state;
    MatrixStateInit(&state, MATRIX_PROVIDER_TOOL);
    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId agent_id = 0;
    bool ok = ws && ConfigureMatrixWorkspace(host, ws, &state, false) &&
              ConfigureMatrixTwoModels(ws) &&
              pico_main_agent_create(host, id, &opt, &agent_id) == PICO_OK;
    PicoAgent *agent = PicoHost_FindAgent(host, agent_id);
    if (ok && agent)
    {
        ok = PicoSettings_SetFast(agent, true);
    }
    if (ok)
    {
        PicoAgent_StartTurn(host, agent, "first");
        ok = MatrixWaitToolEntered(host, &state);
    }
    bool first_fast = ok && state.fasts_seen[0] && MatrixRecordedIs(&state, 0, "matrix-a", "low");

    bool switched = first_fast && PicoSettings_SetModel(agent, "matrix-b") && !agent->fast;

    MatrixReleaseTool(&state);
    bool completed = switched && PumpUntilIdle(host, agent);
    bool turn_fast = completed && agent->error == NULL && state.fasts_seen[1] &&
                     MatrixRecordedIs(&state, 1, "matrix-a", "low");

    bool next_ok = turn_fast;
    if (next_ok)
    {
        PicoAgent_StartTurn(host, agent, "next");
        next_ok = PumpUntilIdle(host, agent);
    }
    bool next_standard = next_ok && !state.fasts_seen[2] &&
                         MatrixRecordedIs(&state, 2, "matrix-b", "medium");

    pico_host_free(host);
    MatrixStateDestroy(&state);
    rmdir(dir);
    if (!turn_fast)
    {
        Fail("mid-turn switch to a non-Fast model must not fail the pinned turn");
        return 1;
    }
    if (!next_standard)
    {
        Fail("next turn must run the selected model without Fast");
        return 1;
    }
    return 0;
}

/* Real chat/composer/questionnaire views in the production shell. The tool
 * waits normally; sidebar/footer content is simplified for geometry. */
static int RunQuestionPanelShellCase(bool with_sidebar)
{
#ifndef PICO_CLAY_FRAME_FAULT_TESTS
    (void)with_sidebar;
    return 0;
#else
    Clay_Context *previous = Clay_GetCurrentContext();
    void *memory = malloc(Clay_MinMemorySize());
    PicoHost *host = NULL;
    char dir[] = "/tmp/pico-question-shell-XXXXXX";
    char cfg[] = "/tmp/pico-question-config-XXXXXX";
    MatrixProviderState state;
    MatrixStateInit(&state, MATRIX_PROVIDER_ASK);
    state.clarification_fixture = true;
    state.ask_request = "{\"type\":\"questionnaire\",\"ui\":\"custom\",\"questions\":["
        "{\"id\":\"target\",\"question\":\"Which target?\",\"kind\":\"select\","
        "\"options\":[\"one\",\"two\",\"three\",\"four\",\"five\",\"six\",\"seven\",\"eight\"]}]}";
    int result = 1;
    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg)) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    PicoWorkspaceId workspace_id;
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, workspace_id);
    if (!ConfigureMatrixWorkspace(host, ws, &state, true)) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
    PicoAgentId agent_id;
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE, .select = true};
    if (pico_main_agent_create(host, workspace_id, &options, &agent_id) != PICO_OK) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
    PicoAgent *agent = PicoHost_FindAgent(host, agent_id);
    for (int i = 0; i < 28; i++)
        PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "Earlier conversation to read while answering.");
    PicoAgent_StartTurn(host, agent, "ask");
    PicoToolAsk ask = {0};
    PICO_TEST_WAIT(!ask.id)
    {
        pico_host_pump(host);
        pico_tool_pending_ask(host, &ask);
    }
    if (!ask.id || PicoUi_ModalOpen(host)) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
    /* Registration refreshes rebuild the effective slot lists. */
    host->view_count[PICO_SLOT_SIDEBAR] = 0;
    host->view_count[PICO_SLOT_OVERLAY] = 0;
    if (with_sidebar) ShellTestAddView(host, PICO_SLOT_SIDEBAR, ShellTestSidebar, NULL);
    ShellTestAddView(host, PICO_SLOT_MAIN, PicoChat_Render, NULL);
    ShellTestAddView(host, PICO_SLOT_FOOTER, ShellTestFooter, NULL);
    strcpy(host->composer.text, "unsent draft");
    host->composer.length = host->composer.cursor = (int)strlen(host->composer.text);
    int message_count = agent->message_count;
    const Clay_Dimensions viewport = {1100, 800};
    if (!Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(Clay_MinMemorySize(), memory),
                         viewport, (Clay_ErrorHandler){0})) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);
    g_find_input_test = true;
    PicoExt ext = pico_ext_ask_user();
    void *ui = PicoPlugins_HostState(host, "ask-user");
    const char *panes[] = {"Root", "Body", "RightColumn", "MainColumn", "ChatScroll",
                           "ComposerAlign", "Composer", "Footer", "AskUserHeader", "AskUserToggle",
                           "AskUserHandle", "Sidebar"};
    const char *clarification_panes[] = {"Root", "Body", "RightColumn", "MainColumn", "ChatScroll",
        "ComposerAlign", "Composer", "Footer", "ClarificationControls", "ClarificationFocus", "ClarificationBack", "Sidebar"};
    Clay_BoundingBox expected[12];
    float expanded_height = 0;
    for (int phase = 0; phase < 5; phase++)
    {
        for (int frame = 0; frame < 100; frame++)
        {
            Clay_SetLayoutDimensions(viewport);
            Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0);
            PicoHost_LayoutShell(host, viewport.height, 0);
            Clay_ScrollContainerData chat = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
            if (!chat.found || !chat.scrollPosition) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
            if (PicoScrollbar_PinToBottom(chat.scrollContainerDimensions.height, chat.contentDimensions.height,
                                         &chat.scrollPosition->y)) PicoHost_LayoutShell(host, viewport.height, 0);
            if (fabsf(chat.scrollPosition->y + chat.contentDimensions.height - chat.scrollContainerDimensions.height) > .01f)
                { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
            for (int pane = 0; pane < (with_sidebar ? 12 : 11); pane++)
            {
                Clay_ElementData box = Clay_GetElementData(Clay_GetElementId((Clay_String){
                    .chars = phase == 3 ? clarification_panes[pane] : panes[pane],
                    .length = (int32_t)strlen(phase == 3 ? clarification_panes[pane] : panes[pane])}));
                if (!box.found) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
                if (frame == 0) expected[pane] = box.boundingBox;
                else if (!ShellBoxStable(expected[pane], box.boundingBox)) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
            }
            if (phase == 3)
            {
                if (!Clay_GetElementData(CLAY_ID("ComposerScroll")).found ||
                    Clay_GetElementData(CLAY_ID("AskUserHeader")).found ||
                    !ShellVerticallyContains(expected[3], expected[8]) ||
                    !ShellVerticallyContains(expected[8], expected[9]) ||
                    !ShellVerticallyContains(expected[3], expected[6]))
                { Fail("clarification controls and composer must remain bounded inside the shell"); goto done; }
                ext.host_on_frame(host, ui, 0);
                continue;
            }
            Clay_ElementData panel = Clay_GetElementData(CLAY_ID("Composer"));
            if (!ShellVerticallyContains(panel.boundingBox, expected[8]) ||
                !ShellVerticallyContains(expected[8], expected[9]) ||
                fabsf(expected[9].x + expected[9].width / 2 - panel.boundingBox.x - panel.boundingBox.width / 2) > .01f)
            { Fail("question handle must stay centered inside the bounded header"); goto done; }
            Clay_ElementData body = Clay_GetElementData(CLAY_ID("AskUserBody"));
            Clay_ElementData next = Clay_GetElementData(CLAY_ID("AskUserNext"));
            if (Clay_GetElementData(CLAY_ID("ComposerScroll")).found ||
                !ShellVerticallyContains(expected[3], panel.boundingBox) ||
                expected[4].y + expected[4].height > panel.boundingBox.y) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
            if (phase != 1)
            {
                if (!body.found || !next.found || !ShellVerticallyContains(panel.boundingBox, next.boundingBox)) { fprintf(stderr, "question navigation escaped the panel\n"); goto done; }
                if (phase == 0) expanded_height = panel.boundingBox.height;
                else if (phase == 2 && fabsf(panel.boundingBox.height - expanded_height) > .001f) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
            }
            else if (body.found || next.found || panel.boundingBox.height >= expanded_height) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
            /* Search is genuinely available without claiming the questionnaire's keys. */
            if (frame == 20) PicoChatFind_Open(host);
            if (frame == 40) PicoChatFind_Close(host);
            ext.host_on_frame(host, ui, 0);
        }
        if (phase == 2)
        {
            /* An Other answer must survive the real clarification/back actions. */
            Clay_ScrollContainerData body = Clay_GetScrollContainerData(CLAY_ID("AskUserBody"));
            if (body.found && body.scrollPosition)
                body.scrollPosition->y = -(body.contentDimensions.height - body.scrollContainerDimensions.height);
            PicoHost_LayoutShell(host, viewport.height, 0);
            Clay_BoundingBox other = Clay_GetElementData(CLAY_IDI("AskUserOption", 8)).boundingBox;
            g_find_pointer = (Vector2){other.x + other.width / 2, other.y + other.height / 2};
            g_find_press = true;
            Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, true);
            pico_run_hooks(host, PICO_HOOK_AFTER_LAYOUT, agent_id);
            g_find_press = false;
            PicoChatFind_HandleInput(host); /* Begin a new input frame after closing search. */
            g_find_character = 'Z';
            ext.host_on_frame(host, ui, 0);
            g_find_character = 0;
            PicoHost_LayoutShell(host, viewport.height, 0);
            /* Use the actual question action rather than changing UI state. */
            Clay_BoundingBox clarify = Clay_GetElementData(CLAY_ID("AskUserClarify")).boundingBox;
            g_find_pointer = (Vector2){clarify.x + clarify.width / 2, clarify.y + clarify.height / 2};
            g_find_press = true;
            Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, true);
            pico_run_hooks(host, PICO_HOOK_AFTER_LAYOUT, agent_id);
            g_find_press = false;
            Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
            PicoAgent *helper = PicoClarification_View(host);
            if (!helper) { Fail("question action must open clarification"); goto done; }
            for (int i = 0; i < 28; i++)
                PicoAgent_AddMessage(host, helper, PICO_ROLE_ASSISTANT, "Clarification explanation in a separate transcript.");
        }
        else if (phase == 3)
        {
            Clay_BoundingBox back = Clay_GetElementData(CLAY_ID("ClarificationBack")).boundingBox;
            g_find_pointer = (Vector2){back.x + back.width / 2, back.y + back.height / 2};
            g_find_press = true;
            Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, true);
            pico_run_hooks(host, PICO_HOOK_AFTER_LAYOUT, agent_id);
            g_find_press = false;
            Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
            if (PicoClarification_View(host) || strcmp(host->composer.text, "unsent draft"))
            { Fail("Back action must restore questionnaire and parent composer draft"); goto done; }
        }
        if (phase < 2)
        {
            Clay_BoundingBox toggle = Clay_GetElementData(CLAY_ID("AskUserToggle")).boundingBox;
            g_find_pointer = (Vector2){toggle.x + toggle.width / 2, toggle.y + toggle.height / 2};
            g_find_press = true;
            Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, true);
            pico_run_hooks(host, PICO_HOOK_AFTER_LAYOUT, agent_id);
            g_find_press = false;
            Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
        }
    }
    /* Reading history is independent of the panel's height changes. */
    host->chat_follow_bottom = false;
    Clay_ScrollContainerData history = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    history.scrollPosition->y = -200;
    PicoHost_LayoutShell(host, viewport.height, 0);
    float reading_y = Clay_GetElementData(CLAY_IDI("MsgMain", 0)).boundingBox.y;
    Clay_BoundingBox toggle = Clay_GetElementData(CLAY_ID("AskUserToggle")).boundingBox;
    g_find_pointer = (Vector2){toggle.x + toggle.width / 2, toggle.y + toggle.height / 2};
    g_find_press = true;
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, true);
    pico_run_hooks(host, PICO_HOOK_AFTER_LAYOUT, agent_id);
    g_find_press = false;
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
    PicoHost_LayoutShell(host, viewport.height, 0);
    if (fabsf(Clay_GetElementData(CLAY_IDI("MsgMain", 0)).boundingBox.y - reading_y) > .01f)
    { Fail("collapsing questions must not move the conversation being read"); goto done; }
    if (agent->message_count != message_count || strcmp(host->composer.text, "unsent draft")) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
    if (!pico_ui_modal_push(host, "question-test") || !PicoUi_ModalOpen(host) ||
        !pico_ui_modal_pop(host, "question-test")) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
    /* The real footer remains interactive; its menu then takes modal priority. */
    ShellTestAddView(host, PICO_SLOT_FOOTER, PicoFooter_Render, NULL);
    PicoHost_LayoutShell(host, viewport.height, 0);
    Clay_BoundingBox model = Clay_GetElementData(CLAY_ID("FooterModel")).boundingBox;
    g_find_pointer = (Vector2){model.x + model.width / 2, model.y + model.height / 2};
    g_find_press = true;
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, true);
    pico_run_hooks(host, PICO_HOOK_AFTER_LAYOUT, agent_id);
    g_find_press = false;
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
    if (!pico_ui_modal_is_top(host, "footer-menu"))
    { Fail("questionnaire must not block footer controls"); goto done; }
    g_find_key = KEY_ESCAPE;
    g_clay_frame_test = true;
    PicoHost_Frame(host);
    g_clay_frame_test = false;
    g_find_key = 0;
    if (pico_ui_modal_claimed(host) || PicoAgent_CancelRequested(agent) ||
        !pico_tool_pending_ask(host, &ask))
    { Fail("Escape closing a footer menu must not cancel the pending questionnaire"); goto done; }
    /* Expand again, then Submit the preserved Other answer explicitly. */
    toggle = Clay_GetElementData(CLAY_ID("AskUserToggle")).boundingBox;
    g_find_pointer = (Vector2){toggle.x + toggle.width / 2, toggle.y + toggle.height / 2};
    g_find_press = true;
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, true);
    pico_run_hooks(host, PICO_HOOK_AFTER_LAYOUT, agent_id);
    g_find_press = false;
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
    PicoHost_LayoutShell(host, viewport.height, 0);
    Clay_BoundingBox next = Clay_GetElementData(CLAY_ID("AskUserNext")).boundingBox;
    g_find_pointer = (Vector2){next.x + next.width / 2, next.y + next.height / 2};
    g_find_press = true;
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, true);
    pico_run_hooks(host, PICO_HOOK_AFTER_LAYOUT, agent_id);
    g_find_press = false;
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
    if (!PumpUntilIdle(host, agent)) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
    pthread_mutex_lock(&state.mu);
    bool preserved_answer = state.answer && strstr(state.answer, "Z");
    pthread_mutex_unlock(&state.mu);
    if (!preserved_answer) { Fail("Other answer draft must survive clarification and submit unchanged"); goto done; }
    PicoHost_LayoutShell(host, viewport.height, 0);
    if (!Clay_GetElementData(CLAY_ID("ComposerScroll")).found ||
        Clay_GetElementData(CLAY_ID("AskUserHeader")).found || strcmp(host->composer.text, "unsent draft")) { fprintf(stderr, "question shell failed at %d\n", __LINE__); goto done; }
    result = 0;
done:
    g_find_input_test = g_find_press = false;
    g_find_key = g_find_character = 0;
    if (host) pico_host_free(host);
    MatrixStateDestroy(&state);
    Clay_SetCurrentContext(previous);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    if (result) Fail(with_sidebar ? "question panel shell geometry/input with sidebar" :
                                   "question panel shell geometry/input without sidebar");
    return result;
#endif
}

/* Provider-facing isolation and lifetime contracts, including delegated asks. */
static int RunClarificationConversationCase(bool delegated, bool permission, bool cancel_stream, bool cancel_owner, bool block_tool)
{
    int result = 1;
    PicoHost *host = NULL;
    char dir[] = "/tmp/pico-clarification-XXXXXX";
    char cfg[] = "/tmp/pico-clarification-config-XXXXXX";
    MatrixProviderState state;
    MatrixStateInit(&state, MATRIX_PROVIDER_ASK);
    state.clarification_fixture = true;
    state.delegation_fixture = delegated;
    state.helper_permission = permission;
    state.block_helper_tool = block_tool;
    state.ask_request = "{\"type\":\"questionnaire\",\"ui\":\"custom\",\"questions\":["
        "{\"id\":\"first\",\"question\":\"What is optimistic concurrency?\",\"kind\":\"text\"},"
        "{\"id\":\"second\",\"question\":\"Which tradeoff?\",\"kind\":\"text\"}]}";
#define CLARIFY_CHECK(condition, message) do { if (!(condition)) { Fail(message); goto done; } } while (0)
    CLARIFY_CHECK(mkdtemp(dir) && mkdtemp(cfg), "clarification fixture directories");
    setenv("XDG_CONFIG_HOME", cfg, 1);
    CLARIFY_CHECK(pico_host_init(&host, NULL, true) == PICO_OK, "clarification host initialization");
    WaitPluginLoad(host);
    PicoWorkspaceId ws_id;
    CLARIFY_CHECK(pico_workspace_open(host, dir, &ws_id) == PICO_OK, "clarification workspace");
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, ws_id);
    CLARIFY_CHECK(ConfigureMatrixWorkspace(host, ws, &state, true) && ConfigureMatrixTwoModels(ws), "clarification model setup");
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE,
                                      .model = "matrix-a", .effort = "low", .select = true};
    PicoAgentId root_id;
    CLARIFY_CHECK(pico_main_agent_create(host, ws_id, &options, &root_id) == PICO_OK, "clarification root");
    PicoAgent *owner = PicoHost_FindAgent(host, root_id);
    if (delegated)
    {
        char profile_dir[4096], profile_path[4096];
        snprintf(profile_dir, sizeof(profile_dir), "%s/pico/subagents", cfg);
        Pico_MkdirP(profile_dir);
        snprintf(profile_path, sizeof(profile_path), "%s/pico/subagents/clarify.json", cfg);
        FILE *profile = fopen(profile_path, "wb");
        CLARIFY_CHECK(profile, "clarification delegation profile file");
        bool written = fputs("{\"purpose\":\"Ask about the assigned task\",\"tools\":[\"matrix_ask\"],\"max_parallel_tools\":2}", profile) >= 0;
        int closed = fclose(profile);
        CLARIFY_CHECK(written && closed == 0, "clarification delegation profile");
        PicoWorkspace_LoadProfiles(ws);
    }
    PicoAgent_PushHistoryUser(owner, "PARENT_EXECUTION_HISTORY_MUST_NOT_LEAK");
    CLARIFY_CHECK(pico_agent_submit(host, root_id, "ORIGINAL_TYPED_TASK", NULL) == PICO_OK,
                  "start original task");
    PicoToolAsk ask = {0};
    PICO_TEST_WAIT(!ask.id)
    {
        pico_host_pump(host);
        pico_tool_pending_ask(host, &ask);
    }
    if (delegated && ask.id) owner = PicoHost_FindAgent(host, ask.agent_id);
    CLARIFY_CHECK(owner && ask.id && ask.agent_id == owner->id &&
                  (!delegated || ask.agent_id != root_id), "surface correct question owner");
    int first_helper_slot = delegated ? 2 : 1;
    uint64_t original_ask = ask.id;
    int parent_messages = owner->message_count;
    strcpy(host->composer.text, "parked parent draft");
    host->composer.length = host->composer.cursor = (int)strlen(host->composer.text);
    strcpy(owner->model, "matrix-b");
    strcpy(owner->effort, "medium");
    if (!delegated && !permission && !cancel_stream)
    {
        PicoAgentId fillers[PICO_MAX_AGENTS];
        int count = 0;
        PicoAgentCreateOptions filler = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
        /* Fill this workspace to its per-workspace agent cap with idle agents.
         * Clarification helper creation must refuse to evict user agents and
         * fail at the cap without disturbing pending state. */
        while (pico_agent_count(host) < PICO_MAX_AGENTS &&
               pico_main_agent_create(host, ws_id, &filler, &fillers[count]) == PICO_OK) count++;
        CLARIFY_CHECK(count == PICO_MAX_AGENTS - 1 && pico_agent_count(host) == PICO_MAX_AGENTS &&
                      PicoClarification_Open(host, &ask, "first") == PICO_LIMIT &&
                      pico_agent_active(host) == root_id && !host->clarification_view_id &&
                      !strcmp(host->composer.text, "parked parent draft") && PicoAgent_PendingAsk(owner, &ask),
                      "capacity failure preserves the pending questionnaire and drafts");
        for (int i = 0; i < count; i++)
            CLARIFY_CHECK(pico_agent_close(host, fillers[i]) == PICO_OK, "release clarification test capacity");
    }
    CLARIFY_CHECK(PicoClarification_Open(host, &ask, "first") == PICO_OK, "open clarification");
    PicoAgent *helper = PicoClarification_View(host);
    CLARIFY_CHECK(helper && pico_agent_active(host) == root_id && !PicoUi_ModalOpen(host) &&
                  !PicoUi_QuestionnaireOpen(host), "clarification preserves selected session and opens composer");
    PicoAgentId helper_id = helper->id;
    state.helper_id = helper_id;
    CLARIFY_CHECK(!pico_agent_select(host, helper_id), "helper cannot become the selected user session");
    int captured_concurrency = helper->max_parallel_tools_override;
    ws->settings.max_parallel_tools = captured_concurrency == 1 ? 2 : 1;
    strcpy(ws->models[0].provider, "replaced-provider");
    strcpy(ws->models[0].base_url, "replaced-url");
    CLARIFY_CHECK(helper->persistence == PICO_SESSION_EPHEMERAL && !helper->session_path[0] &&
                  helper->session_id[0] && strcmp(helper->session_id, owner->session_id), "distinct ephemeral helper identity");
    PicoWorkspaceInfo info;
    CLARIFY_CHECK(pico_workspace_info(host, 0, &info) && info.main_agent_count == 1 &&
                  info.total_agent_count > info.main_agent_count, "helper is counted but not a main session");
    strcpy(host->composer.text = realloc(host->composer.text, 128), "EXPLAIN_FIRST");
    host->composer.capacity = 128;
    host->composer.length = host->composer.cursor = (int)strlen(host->composer.text);
    PicoHost_Submit(host);
    if (permission)
    {
        PicoToolAsk permit = {0};
        PICO_TEST_WAIT(!permit.id)
        {
            pico_host_pump(host);
            PicoToolAsk pending;
            if (pico_tool_pending_ask(host, &pending) && pending.agent_id == helper_id) permit = pending;
        }
        CLARIFY_CHECK(permit.id && permit.id != original_ask && PicoUi_ModalOpen(host), "helper permission ask is visible");
        CLARIFY_CHECK(PicoAgent_PendingAsk(owner, &ask) && ask.id == original_ask,
                      "permission does not replace original questionnaire");
        CLARIFY_CHECK(pico_tool_answer(host, permit.id, "{\"ok\":true}"), "answer helper permission");
    }
    if (block_tool)
    {
        bool entered = false;
        PICO_TEST_WAIT(!entered)
        {
            pico_host_pump(host);
            pthread_mutex_lock(&state.mu); entered = state.tool_entered; pthread_mutex_unlock(&state.mu);
        }
        CLARIFY_CHECK(entered, "blocked helper tool entered");
        PicoClarification_Stop(host);
        PicoClarification_Stop(host); /* Force-stop the non-cooperative tool worker. */
        CLARIFY_CHECK(!PicoAgent_IsBusy(helper) && PicoAgent_PendingAsk(owner, &ask) &&
                      !PicoAgent_CancelRequested(owner), "force-stop retires helper worker without canceling owner");
        CLARIFY_CHECK(pico_tool_answer(host, original_ask, "{\"answers\":[]}") &&
                      PumpUntilIdle(host, owner), "answer owner while retired helper tool drains");
        CLARIFY_CHECK(PicoHost_FindAgent(host, helper_id) && !host->clarification_view_id,
                      "helper remains retained, but hidden, until retired worker exits");
        pthread_mutex_lock(&state.mu); state.tool_release = true; pthread_cond_broadcast(&state.cv); pthread_mutex_unlock(&state.mu);
        PICO_TEST_WAIT(PicoHost_FindAgent(host, helper_id)) { pico_host_pump(host);  }
        CLARIFY_CHECK(!PicoHost_FindAgent(host, helper_id) && !strcmp(host->composer.text, "parked parent draft"),
                      "retired tool completion cannot reopen clarification or affect parent drafts");
        result = 0;
        goto done;
    }
    CLARIFY_CHECK(PumpUntilIdle(host, helper), "clarification explanation completes");
    pthread_mutex_lock(&state.mu);
    bool isolated = state.inputs_seen[first_helper_slot] && strstr(state.inputs_seen[first_helper_slot], "ORIGINAL_TYPED_TASK") &&
        strstr(state.inputs_seen[first_helper_slot], "optimistic concurrency") && strstr(state.inputs_seen[first_helper_slot], "EXPLAIN_FIRST") &&
        !strstr(state.inputs_seen[first_helper_slot], "PARENT_EXECUTION_HISTORY_MUST_NOT_LEAK") &&
        !strstr(state.inputs_seen[first_helper_slot], "MAIN_REQUEST_CONTEXT_MARKER") &&
        strstr(state.inputs_seen[0], "MAIN_REQUEST_CONTEXT_MARKER") &&
        (!delegated || strstr(state.inputs_seen[first_helper_slot], "DELEGATION_ASSIGNMENT")) &&
        !strstr(state.instructions_seen[first_helper_slot], "MAIN_EXECUTION_HOOK_MARKER") &&
        strstr(state.instructions_seen[0], "MAIN_EXECUTION_HOOK_MARKER") &&
        !strcmp(state.tools_seen[first_helper_slot], "sh") && !strcmp(state.models_seen[first_helper_slot], "matrix-a") &&
        !strcmp(state.efforts_seen[first_helper_slot], "low") && strcmp(state.keys_seen[0], state.keys_seen[first_helper_slot]);
    pthread_mutex_unlock(&state.mu);
    CLARIFY_CHECK(isolated && owner->message_count == parent_messages && PicoAgent_PendingAsk(owner, &ask),
                  "clean helper context, restricted tools, owner snapshot, and parent isolation");
    strcpy(host->composer.text, "unsubmitted helper draft");
    host->composer.length = host->composer.cursor = (int)strlen(host->composer.text);
    PicoClarification_Back(host);
    CLARIFY_CHECK(!PicoClarification_View(host) && PicoUi_QuestionnaireOpen(host) &&
                  !strcmp(host->composer.text, "parked parent draft"), "Back restores parent draft");
    CLARIFY_CHECK(PicoClarification_Open(host, &ask, "second") == PICO_OK &&
                  PicoClarification_View(host)->id == helper_id && !strcmp(host->composer.text, "unsubmitted helper draft"),
                  "questions share helper history and preserve clarification draft");
    PicoWorkspace_SetAcceptingWork(ws, false);
    PicoHost_Submit(host);
    CLARIFY_CHECK(!PicoAgent_IsBusy(helper) && !strcmp(host->composer.text, "unsubmitted helper draft"),
                  "reload gate preserves blocked clarification draft");
    PicoWorkspace_SetAcceptingWork(ws, true);
    strcpy(host->composer.text, "EXPLAIN_SECOND");
    host->composer.length = host->composer.cursor = (int)strlen(host->composer.text);
    if (cancel_stream)
    {
        pthread_mutex_lock(&state.mu); state.block_helper = true; state.helper_entered = false; pthread_mutex_unlock(&state.mu);
    }
    PicoHost_Submit(host);
    if (cancel_stream)
    {
        bool entered = false;
        PICO_TEST_WAIT(!entered)
        {
            pico_host_pump(host);
            pthread_mutex_lock(&state.mu); entered = state.helper_entered; pthread_mutex_unlock(&state.mu);
        }
        CLARIFY_CHECK(entered, "helper stream started");
        PicoClarification_Back(host);
        CLARIFY_CHECK(PicoAgent_IsBusy(helper), "Back leaves explanation running");
        CLARIFY_CHECK(PicoClarification_Open(host, &ask, "second") == PICO_OK, "return to running explanation");
        PicoClarification_Stop(host);
        CLARIFY_CHECK(PumpUntilIdle(host, helper) && PicoAgent_PendingAsk(owner, &ask) &&
                      !PicoAgent_CancelRequested(owner), "Stop cancels helper only");
    }
    else CLARIFY_CHECK(PumpUntilIdle(host, helper), "clarification follow-up completes");
    int followup_slot = first_helper_slot + (permission ? 2 : 1);
    pthread_mutex_lock(&state.mu);
    bool followup = state.inputs_seen[followup_slot] && strstr(state.inputs_seen[followup_slot], "EXPLAIN_FIRST") &&
                    strstr(state.inputs_seen[followup_slot], "EXPLAIN_SECOND") &&
                    strstr(state.inputs_seen[followup_slot], "second") &&
                    !strcmp(state.keys_seen[first_helper_slot], state.keys_seen[followup_slot]);
    pthread_mutex_unlock(&state.mu);
    CLARIFY_CHECK(followup && !strcmp(state.models_seen[first_helper_slot], state.models_seen[followup_slot]) &&
                  !strcmp(state.urls_seen[first_helper_slot], state.urls_seen[followup_slot]) &&
                  helper->max_parallel_tools_override == captured_concurrency,
                  "follow-ups retain helper history, identity, and configuration while changing question focus");
    if (!delegated && !permission && !cancel_stream)
    {
        PicoAgent_Compact(host, helper);
        CLARIFY_CHECK(PumpUntilIdle(host, helper), "clarification compaction completes");
        strcpy(host->composer.text, "EXPLAIN_AFTER_COMPACTION");
        host->composer.length = host->composer.cursor = (int)strlen(host->composer.text);
        PicoHost_Submit(host);
        CLARIFY_CHECK(PumpUntilIdle(host, helper), "clarification follow-up after compaction");
        pthread_mutex_lock(&state.mu);
        int compacted_followup = state.recorded - 1;
        bool restored_seed = state.inputs_seen[compacted_followup] &&
            strstr(state.inputs_seen[compacted_followup], "ORIGINAL_TYPED_TASK") &&
            strstr(state.inputs_seen[compacted_followup], "optimistic concurrency") &&
            strstr(state.inputs_seen[compacted_followup], "EXPLAIN_AFTER_COMPACTION") &&
            !strstr(state.inputs_seen[compacted_followup], "MAIN_REQUEST_CONTEXT_MARKER") &&
            !strstr(state.instructions_seen[compacted_followup], "MAIN_EXECUTION_HOOK_MARKER");
        pthread_mutex_unlock(&state.mu);
        CLARIFY_CHECK(restored_seed, "compaction restores questionnaire/task seed without execution hooks");
    }
    if (cancel_stream)
    {
        /* Submit/cancel the owner while a second helper turn is still running. */
        pthread_mutex_lock(&state.mu); state.helper_entered = false; pthread_mutex_unlock(&state.mu);
        strcpy(host->composer.text, "EXPLAIN_BEFORE_ASK_END");
        host->composer.length = host->composer.cursor = (int)strlen(host->composer.text);
        PicoHost_Submit(host);
        bool entered = false;
        PICO_TEST_WAIT(!entered)
        {
            pico_host_pump(host);
            pthread_mutex_lock(&state.mu); entered = state.helper_entered; pthread_mutex_unlock(&state.mu);
        }
        CLARIFY_CHECK(entered, "ask end while helper is running");
    }
    if (cancel_owner)
    {
        pico_agent_force_cancel(host, owner->id);
        CLARIFY_CHECK(PumpUntilIdle(host, owner), "force-cancelled owner drains");
        PICO_TEST_WAIT(PicoHost_FindAgent(host, helper_id)) { pico_host_pump(host);  }
        CLARIFY_CHECK(!PicoHost_FindAgent(host, helper_id) && !PicoAgent_PendingAsk(owner, &ask) &&
                      !host->clarification_view_id && !strcmp(host->composer.text, "parked parent draft"),
                      "owner force cancellation tears down helper without losing parent draft");
        result = 0;
        goto done;
    }
    CLARIFY_CHECK(pico_tool_answer(host, original_ask, "{\"answers\":[{\"id\":\"first\",\"answer\":\"EXPLICIT_ANSWER\"}]}"),
                  "submit explicit questionnaire answer");
    CLARIFY_CHECK(PumpUntilIdle(host, PicoHost_FindAgent(host, root_id)), "parent continues after explicit answer");
    PICO_TEST_WAIT(PicoHost_FindAgent(host, helper_id)) { pico_host_pump(host);  }
    CLARIFY_CHECK(!PicoHost_FindAgent(host, helper_id) && !host->clarification_view_id &&
                  !strcmp(host->composer.text, "parked parent draft"), "ask completion cleans up temporary helper");
    pthread_mutex_lock(&state.mu);
    int last = state.recorded - 1;
    bool parent_clean = last >= 0 && state.inputs_seen[last] &&
                        !strstr(state.inputs_seen[last], "EXPLAIN_FIRST") &&
                        !strstr(state.inputs_seen[last], "EXPLAIN_SECOND") &&
                        !strstr(state.inputs_seen[last], "EXPLAIN_BEFORE_ASK_END") &&
                        state.answer && strstr(state.answer, "EXPLICIT_ANSWER");
    pthread_mutex_unlock(&state.mu);
    CLARIFY_CHECK(parent_clean, "parent receives only explicit answers, never clarification transcript");
    result = 0;
done:
    pthread_mutex_lock(&state.mu); state.tool_release = true; pthread_cond_broadcast(&state.cv); pthread_mutex_unlock(&state.mu);
    if (host) pico_host_free(host);
    MatrixStateDestroy(&state);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg); rmdir(dir);
#undef CLARIFY_CHECK
    return result;
}

static int TestQuestionnaireClarification(void)
{
    return RunClarificationConversationCase(false, false, true, false, false) ||
           RunClarificationConversationCase(true, false, false, false, false) ||
           RunClarificationConversationCase(false, true, false, false, false) ||
           RunClarificationConversationCase(false, false, true, true, false) ||
           RunClarificationConversationCase(false, false, false, false, false) ||
           RunClarificationConversationCase(false, true, false, false, true);
}

static int TestMultiWorkspaceAskOrderingAndRouting(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init ask ordering");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-askA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-askB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp ask ordering");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);
    PicoWorkspace *wsB = PicoHost_FindWorkspace(host, idB);
    MatrixProviderState stateA, stateB;
    MatrixStateInit(&stateA, MATRIX_PROVIDER_ASK);
    MatrixStateInit(&stateB, MATRIX_PROVIDER_ASK);
    bool configured = ConfigureMatrixWorkspace(host, wsA, &stateA, true) &&
                      ConfigureMatrixWorkspace(host, wsB, &stateB, true);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0, b1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);
    pico_main_agent_create(host, idB, &opt, &b1);
    PicoAgent *agentA = PicoHost_FindAgent(host, a1);
    PicoAgent *agentB = PicoHost_FindAgent(host, b1);
    bool started = configured && agentA && agentB;
    if (started)
    {
        PicoAgent_StartTurn(host, agentA, "ask A");
        PicoAgent_StartTurn(host, agentB, "ask B");
        started = PicoAgent_IsBusy(agentA) && PicoAgent_IsBusy(agentB);
    }

    PicoToolAsk ask_a = {0}, ask_b = {0};
    PICO_TEST_WAIT(started && (ask_a.id == 0 || ask_b.id == 0))
    {
        pico_host_pump(host);
        if (ask_a.id == 0)
        {
            PicoAgent_PendingAsk(agentA, &ask_a);
        }
        if (ask_b.id == 0)
        {
            PicoAgent_PendingAsk(agentB, &ask_b);
        }
    }
    bool both_pending = ask_a.id != 0 && ask_b.id != 0;

    /* Only the open session's ask surfaces; the other session's stays hidden. */
    PicoToolAsk surfaced = {0};
    bool scoped = both_pending && host->selected_agent_id == a1 &&
                  pico_tool_pending_ask(host, &surfaced) && surfaced.id == ask_a.id;

    /* Opening the other session surfaces its ask instead. */
    bool switched = scoped && pico_agent_select(host, b1);
    PicoToolAsk after_switch = {0};
    bool follows_selection = switched && pico_tool_pending_ask(host, &after_switch) &&
                             after_switch.id == ask_b.id;

    bool answered_b = follows_selection &&
                      pico_tool_answer(host, ask_b.id, "{\"step\":2}") &&
                      !pico_tool_answer(host, ask_b.id, "{\"stale\":true}") &&
                      !pico_tool_answer(host, 0, "{}") &&
                      !pico_tool_answer(host, 9999, "{}");

    /* While b1 stays open, a1's still-pending ask never surfaces. */
    bool stays_hidden = answered_b;
    if (stays_hidden)
    {
        pico_host_pump(host);
        PicoToolAsk still_a = {0}, now = {0};
        stays_hidden = PicoAgent_PendingAsk(agentA, &still_a) &&
                       (!pico_tool_pending_ask(host, &now) || now.id != ask_a.id);
    }

    /* Reopening a1 surfaces its ask again; answering it completes both turns. */
    bool back_to_a = stays_hidden && pico_agent_select(host, a1);
    PicoToolAsk final_ask = {0};
    bool resurfaces = back_to_a && pico_tool_pending_ask(host, &final_ask) &&
                      final_ask.id == ask_a.id;
    bool answered_a = resurfaces && pico_tool_answer(host, ask_a.id, "{\"step\":1}") &&
                      !pico_tool_answer(host, ask_a.id, "{\"stale\":true}");
    bool completed = answered_a && PumpUntilIdle(host, agentA) &&
                     PumpUntilIdle(host, agentB);
    pthread_mutex_lock(&stateA.mu);
    bool answerA = stateA.answer && strcmp(stateA.answer, "{\"step\":1}") == 0;
    pthread_mutex_unlock(&stateA.mu);
    pthread_mutex_lock(&stateB.mu);
    bool answerB = stateB.answer && strcmp(stateB.answer, "{\"step\":2}") == 0;
    pthread_mutex_unlock(&stateB.mu);

    pico_host_free(host);
    MatrixStateDestroy(&stateA);
    MatrixStateDestroy(&stateB);
    rmdir(dirA);
    rmdir(dirB);
    if (!started || !completed || !answerA || !answerB)
    {
        Fail("asks must surface only for the open session and answers routed by ask ID");
        return 1;
    }
    return 0;
}

static int TestMultiWorkspaceReloadAndCloseIsolation(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init reload close isolation");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-rcA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-rcB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp reload close isolation");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);
    PicoWorkspace *wsB = PicoHost_FindWorkspace(host, idB);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0, b1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);
    pico_main_agent_create(host, idB, &opt, &b1);

    /* Request reload on A */
    if (pico_workspace_request_reload(host, idA) != PICO_OK || wsA->state != PICO_WORKSPACE_RELOADING)
    {
        Fail("workspace A should enter RELOADING");
    }
    if (PicoWorkspace_AcceptsNewWork(wsA))
    {
        Fail("workspace A in RELOADING should reject new work");
    }
    if (!PicoWorkspace_AcceptsNewWork(wsB) || wsB->state != PICO_WORKSPACE_OPEN)
    {
        Fail("workspace B should remain OPEN and accepting work while A is reloading");
    }

    /* Request close on A while reloading */
    if (pico_workspace_request_close(host, idA) != PICO_OK || wsA->state != PICO_WORKSPACE_CLOSING)
    {
        Fail("workspace A should enter CLOSING");
    }

    /* Pump host - A is quiescent so it should close and be removed */
    pico_host_pump(host);

    if (PicoHost_FindWorkspace(host, idA) != NULL || pico_workspace_count(host) != 1)
    {
        Fail("workspace A should be destroyed and removed after quiescence");
    }
    if (PicoHost_FindWorkspace(host, idB) == NULL || wsB->state != PICO_WORKSPACE_OPEN)
    {
        Fail("workspace B should continue running normally");
    }

    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestMultiWorkspaceStuckWorkerIsolation(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init stuck worker isolation");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-stuckA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-stuckB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp stuck worker isolation");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);
    PicoWorkspace *wsB = PicoHost_FindWorkspace(host, idB);
    MatrixProviderState stateA, stateB;
    MatrixStateInit(&stateA, MATRIX_PROVIDER_BLOCK);
    MatrixStateInit(&stateB, MATRIX_PROVIDER_COMPLETE);
    bool configured = ConfigureMatrixWorkspace(host, wsA, &stateA, false) &&
                      ConfigureMatrixWorkspace(host, wsB, &stateB, false);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0, b1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);
    pico_main_agent_create(host, idB, &opt, &b1);
    PicoAgent *agentA = PicoHost_FindAgent(host, a1);
    PicoAgent *agentB = PicoHost_FindAgent(host, b1);
    if (configured && agentA && agentB)
    {
        PicoAgent_StartTurn(host, agentA, "blocked A");
        PicoAgent_StartTurn(host, agentB, "complete B");
    }
    PICO_TEST_WAIT(!MatrixStateFlag(&stateA, false))
    {
        pico_host_pump(host);
    }
    pico_workspace_request_close(host, idA);
    bool b_completed = agentB && PumpUntilIdle(host, agentB);
    bool isolated_while_blocked = MatrixStateFlag(&stateA, false) &&
                                  PicoHost_FindWorkspace(host, idA) == wsA &&
                                  wsA->state == PICO_WORKSPACE_CLOSING &&
                                  PicoHost_FindWorkspace(host, idB) == wsB &&
                                  wsB->state == PICO_WORKSPACE_OPEN && b_completed;

    MatrixStateRelease(&stateA);
    PICO_TEST_WAIT(PicoHost_FindWorkspace(host, idA))
    {
        pico_host_pump(host);
    }
    bool closed_after_release = PicoHost_FindWorkspace(host, idA) == NULL &&
                                PicoHost_FindWorkspace(host, idB) == wsB;

    pico_host_free(host);
    MatrixStateDestroy(&stateA);
    MatrixStateDestroy(&stateB);
    rmdir(dirA);
    rmdir(dirB);
    if (!isolated_while_blocked || !closed_after_release)
    {
        Fail("a controlled stuck worker must hold only its closing workspace");
        return 1;
    }
    return 0;
}

static int TestMultiWorkspaceMainAgentDelegationDrain(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init delegation drain test");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-delgA-XXXXXX";
    if (!mkdtemp(dirA))
    {
        Fail("mkdtemp delegation drain");
        return 1;
    }

    PicoWorkspaceId idA = 0;
    pico_workspace_open(host, dirA, &idA);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId main1 = 0, main2 = 0;
    pico_main_agent_create(host, idA, &opt, &main1);
    pico_main_agent_create(host, idA, &opt, &main2);

    /* Create a child subagent descended from main1 */
    PicoAgentCreateOptions child_opt = {
        .kind = PICO_AGENT_SUBAGENT,
        .parent_id = main1,
        .session_start = PICO_SESSION_NONE,
    };
    PicoAgentId sub1 = 0;
    if (PicoWorkspace_CreateAgent(wsA, &child_opt, &sub1) != PICO_OK || sub1 == 0)
    {
        Fail("subagent creation under main1 should succeed");
    }

    if (wsA->count != 3)
    {
        Fail("workspace should have 3 agents (main1, main2, sub1)");
    }

    /* Close main1: child tree is cancelled and drained, sub1 and main1 destroyed, main2 survives */
    PicoResult res = pico_agent_close(host, main1);
    if (res != PICO_OK)
    {
        Fail("closing main1 should cancel/drain subagent and destroy main1");
    }

    if (PicoHost_FindAgent(host, main1) != NULL || PicoHost_FindAgent(host, sub1) != NULL)
    {
        Fail("main1 and sub1 should be destroyed");
    }
    if (PicoHost_FindAgent(host, main2) == NULL || wsA->count != 1)
    {
        Fail("main2 in workspace A should survive unharmed");
    }

    pico_host_free(host);
    rmdir(dirA);
    return 0;
}

static int TestMultiWorkspaceModelAndSettingsIsolation(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init model isolation test");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-modA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-modB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp model isolation");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);
    PicoWorkspace *wsB = PicoHost_FindWorkspace(host, idB);

    PicoModel modelsA[2];
    memset(modelsA, 0, sizeof(modelsA));
    snprintf(modelsA[0].id, sizeof(modelsA[0].id), "model-alpha");
    snprintf(modelsA[0].name, sizeof(modelsA[0].name), "model-alpha");
    snprintf(modelsA[0].default_effort, sizeof(modelsA[0].default_effort), "low");
    snprintf(modelsA[0].effort[0], sizeof(modelsA[0].effort[0]), "low");
    modelsA[0].effort_count = 1;
    snprintf(modelsA[1].id, sizeof(modelsA[1].id), "model-shared");
    snprintf(modelsA[1].name, sizeof(modelsA[1].name), "model-shared");
    wsA->models = modelsA;
    wsA->model_count = 2;

    PicoModel modelsB[2];
    memset(modelsB, 0, sizeof(modelsB));
    snprintf(modelsB[0].id, sizeof(modelsB[0].id), "model-beta");
    snprintf(modelsB[0].name, sizeof(modelsB[0].name), "model-beta");
    snprintf(modelsB[0].default_effort, sizeof(modelsB[0].default_effort), "high");
    snprintf(modelsB[0].effort[0], sizeof(modelsB[0].effort[0]), "high");
    modelsB[0].effort_count = 1;
    snprintf(modelsB[1].id, sizeof(modelsB[1].id), "model-shared");
    snprintf(modelsB[1].name, sizeof(modelsB[1].name), "model-shared");
    wsB->models = modelsB;
    wsB->model_count = 2;

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0, b1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);
    pico_main_agent_create(host, idB, &opt, &b1);

    PicoAgent *agentA = PicoHost_FindAgent(host, a1);
    PicoAgent *agentB = PicoHost_FindAgent(host, b1);

    PicoSettings_SetModel(agentA, "model-alpha");
    PicoSettings_SetModel(agentB, "model-beta");

    if (strcmp(agentA->model, "model-alpha") != 0 ||
        strcmp(agentB->model, "model-beta") != 0)
    {
        Fail("workspaces must maintain isolated agent model catalogs and assignments");
    }

    /* Model alpha in workspace A must not be visible or settable in workspace B */
    if (PicoSettings_SetModel(agentB, "model-alpha"))
    {
        Fail("workspace B should reject models that only exist in workspace A");
    }

    wsA->models = NULL;
    wsA->model_count = 0;
    wsB->models = NULL;
    wsB->model_count = 0;

    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestMultiWorkspaceFrameCallbacks(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init frame callbacks");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-fcA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-fcB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp frame callbacks");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);

    /* Pump host three times */
    pico_host_pump(host);
    pico_host_pump(host);
    pico_host_pump(host);

    pico_host_free(host);
    rmdir(dirA);
    rmdir(dirB);
    return 0;
}

static int TestMultiWorkspaceCloseLastMainAgentAndZeroAgents(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init zero agent test");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-zeroA-XXXXXX";
    if (!mkdtemp(dirA))
    {
        Fail("mkdtemp zero agent test");
        return 1;
    }

    PicoWorkspaceId idA = 0;
    pico_workspace_open(host, dirA, &idA);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);

    if (wsA->count != 1)
    {
        Fail("workspace should have 1 agent");
    }

    /* Closing the only/last main agent in workspace */
    if (pico_agent_close(host, a1) != PICO_OK)
    {
        Fail("pico_agent_close on last agent should succeed");
    }
    if (wsA->count != 0 || wsA->state != PICO_WORKSPACE_OPEN || pico_workspace_count(host) != 1)
    {
        Fail("closing last main agent should leave workspace open with 0 agents");
    }

    /* Creating a new main agent in the 0-agent workspace succeeds */
    PicoAgentId a2 = 0;
    if (pico_main_agent_create(host, idA, &opt, &a2) != PICO_OK || a2 == 0 || wsA->count != 1)
    {
        Fail("creating new main agent in 0-agent workspace should succeed");
    }

    pico_host_free(host);
    rmdir(dirA);
    return 0;
}

static int TestMultiWorkspaceStaleIds(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init stale ids test");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-staleA-XXXXXX";
    if (!mkdtemp(dirA))
    {
        Fail("mkdtemp stale ids test");
        return 1;
    }

    PicoWorkspaceId idA = 0;
    pico_workspace_open(host, dirA, &idA);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);

    /* Close agent a1 */
    pico_agent_close(host, a1);

    /* Stale agent ID operations should return not found / invalid */
    PicoAgentInfo info;
    if (pico_agent_find(host, a1, &info))
    {
        Fail("stale agent id find should return false");
    }
    if (pico_agent_submit(host, a1, "hello", NULL) != PICO_NOT_FOUND)
    {
        Fail("stale agent submit should return PICO_NOT_FOUND");
    }
    if (pico_agent_cancel(host, a1) != PICO_NOT_FOUND)
    {
        Fail("stale agent cancel should return PICO_NOT_FOUND");
    }
    if (pico_agent_close(host, a1) != PICO_NOT_FOUND)
    {
        Fail("stale agent close should return PICO_NOT_FOUND");
    }

    /* Creating a new agent allocates a new unique ID != stale a1 */
    PicoAgentId a2 = 0;
    pico_main_agent_create(host, idA, &opt, &a2);
    if (a2 == a1 || a2 == 0)
    {
        Fail("new agent id must be monotonically unique and not reuse stale id");
    }

    pico_host_free(host);
    rmdir(dirA);
    return 0;
}

static int TestMultiWorkspaceFairPumping(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init fair pumping test");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-fairA-XXXXXX";
    char dirB[] = "/tmp/pico-ws-fairB-XXXXXX";
    if (!mkdtemp(dirA) || !mkdtemp(dirB))
    {
        Fail("mkdtemp fair pumping test");
        return 1;
    }

    PicoWorkspaceId idA = 0, idB = 0;
    pico_workspace_open(host, dirA, &idA);
    pico_workspace_open(host, dirB, &idB);
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);
    PicoWorkspace *wsB = PicoHost_FindWorkspace(host, idB);
    MatrixProviderState stateA, stateB;
    MatrixStateInit(&stateA, MATRIX_PROVIDER_STREAM);
    MatrixStateInit(&stateB, MATRIX_PROVIDER_COMPLETE);
    bool configured = ConfigureMatrixWorkspace(host, wsA, &stateA, false) &&
                      ConfigureMatrixWorkspace(host, wsB, &stateB, false);

    PicoAgentCreateOptions opt = { .kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE };
    PicoAgentId a1 = 0, b1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);
    pico_main_agent_create(host, idB, &opt, &b1);
    PicoAgent *agentA = PicoHost_FindAgent(host, a1);
    PicoAgent *agentB = PicoHost_FindAgent(host, b1);
    if (configured && agentA && agentB)
    {
        PicoAgent_StartTurn(host, agentA, "large stream A");
        PicoAgent_StartTurn(host, agentB, "short B");
    }
    bool b_completed = agentB && PumpUntilIdle(host, agentB);
    bool progress = configured && MatrixStateFlag(&stateA, false) && b_completed &&
                    PicoAgent_IsBusy(agentA) && agentB->message_count > 0;

    MatrixStateRelease(&stateA);
    bool a_completed = agentA && PumpUntilIdle(host, agentA);
    pico_host_free(host);
    MatrixStateDestroy(&stateA);
    MatrixStateDestroy(&stateB);
    rmdir(dirA);
    rmdir(dirB);
    if (!progress || !a_completed)
    {
        Fail("a short workspace turn must complete while another workspace keeps streaming");
        return 1;
    }
    return 0;
}

/* Streaming reparse debounce: while a message streams, reparsing on every
 * landed delta discards the document's wrap/highlight caches and re-pays the
 * full parse+wrap+highlight cost every frame. Mid-stream reparses must be
 * debounced (the document renders a prefix of the source between
 * reparses), the document must never run ahead of the source, and the turn
 * must end with the document caught up to the full streamed text. */
typedef struct StreamProbeState {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool release;
    bool entered;
    int requested;
    int emitted;
} StreamProbeState;

static void StreamProbeInit(StreamProbeState *state)
{
    memset(state, 0, sizeof(*state));
    pthread_mutex_init(&state->mu, NULL);
    pthread_cond_init(&state->cv, NULL);
}

static void StreamProbeRelease(StreamProbeState *state)
{
    pthread_mutex_lock(&state->mu);
    state->release = true;
    pthread_cond_broadcast(&state->cv);
    pthread_mutex_unlock(&state->mu);
}

static void StreamProbeDestroy(StreamProbeState *state)
{
    pthread_mutex_destroy(&state->mu);
    pthread_cond_destroy(&state->cv);
}

/* Streams single-character text deltas until released, then returns an empty
 * result so the streamed text survives as the final message text. */
static int StreamProbeProvider(PicoAgentContext *ctx, const PicoLlmTurn *turn,
                               PicoLlmCancelFn cancel, PicoLlmDeltaFn on_delta,
                               void *user, PicoLlmResult *out, void *opaque)
{
    (void)ctx;
    (void)turn;
    (void)cancel;
    (void)out;
    StreamProbeState *state = (StreamProbeState *)opaque;
    pthread_mutex_lock(&state->mu);
    state->entered = true;
    pthread_cond_broadcast(&state->cv);
    pthread_mutex_unlock(&state->mu);
    pthread_mutex_lock(&state->mu);
    while (!state->release)
    {
        while (!state->release && state->emitted == state->requested)
            pthread_cond_wait(&state->cv, &state->mu);
        if (state->release) break;
        pthread_mutex_unlock(&state->mu);
        if (on_delta)
        {
            PicoLlmDelta delta = {.kind = PICO_LLM_DELTA_TEXT, .text = "x", .len = 1,
                                  .call_index = -1};
            on_delta(user, &delta);
        }
        pthread_mutex_lock(&state->mu);
        state->emitted++;
        pthread_cond_broadcast(&state->cv);
    }
    pthread_mutex_unlock(&state->mu);
    return PICO_LLM_OK;
}

static size_t DocTextBytes(const PicoMessage *message)
{
    size_t bytes = 0;
    for (int b = 0; b < message->doc.block_count; b++)
    {
        const MdBlock *block = &message->doc.blocks[b];
        for (int c = 0; c < block->chunk_count; c++)
        {
            bytes += (size_t)block->chunks[c].length;
        }
    }
    return bytes;
}

static const PicoMessage *LastAssistantMessage(const PicoAgent *agent)
{
    for (int i = agent->message_count - 1; i >= 0; i--)
    {
        if (agent->messages[i].role == PICO_ROLE_ASSISTANT)
        {
            return &agent->messages[i];
        }
    }
    return NULL;
}

static int TestStreamingReparseDebounced(void)
{
#ifndef PICO_TEST_CLOCK
    fprintf(stderr, "SKIP: deterministic stream debounce requires the test clock\n");
    return 0;
#endif
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("stream debounce host init");
        return 1;
    }
    char dir[] = "/tmp/pico-ws-stream-debounce-XXXXXX";
    if (!mkdtemp(dir))
    {
        Fail("stream debounce mkdtemp");
        pico_host_free(host);
        return 1;
    }
    PicoWorkspaceId workspace_id = 0;
    pico_workspace_open(host, dir, &workspace_id);
    PicoWorkspace *workspace = PicoHost_FindWorkspace(host, workspace_id);
    StreamProbeState state;
    StreamProbeInit(&state);
    PicoAgentId agent_id = 0;
    PicoAgent *agent = NULL;
    int rc = 1;
    bool configured = false;
    if (workspace)
    {
        free(workspace->models); /* defaults loaded by pico_workspace_open; replaced below */
        workspace->models = (PicoModel *)calloc(1, sizeof(*workspace->models));
        workspace->model_count = 1;
        snprintf(workspace->models[0].id, sizeof(workspace->models[0].id), "probe-model");
        snprintf(workspace->models[0].name, sizeof(workspace->models[0].name), "probe-model");
        snprintf(workspace->models[0].provider, sizeof(workspace->models[0].provider), "probe");
        snprintf(workspace->settings.default_model, sizeof(workspace->settings.default_model),
                 "probe-model");
        PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, workspace);
        PicoProvider provider = {.name = "probe", .stream = StreamProbeProvider,
                                 .map_context = true, .state = &state};
        pico_add_provider(workspace, &provider);
        PicoHost_PublishRegistration(host, &state);
        configured = pico_workspace_find_provider(workspace, "probe") != NULL;
    }
    PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    pico_main_agent_create(host, workspace_id, &opt, &agent_id);
    agent = PicoHost_FindAgent(host, agent_id);
    if (!configured || !agent)
    {
        Fail("stream debounce agent setup");
        goto done;
    }
    PicoAgent_StartTurn(host, agent, "stream");

    PicoTest_Wait(__func__, "stream provider entered");
    pthread_mutex_lock(&state.mu);
    while (!state.entered) pthread_cond_wait(&state.cv, &state.mu);
    pthread_mutex_unlock(&state.mu);
#ifdef PICO_TEST_CLOCK
    PicoTestClock_Set(100.0);
#endif
    bool lagged = false;
    bool doc_ran_ahead = false;
    bool stream_grew = false;
    size_t previous_source = 0;
    for (int i = 0; i < 2; i++)
    {
        PicoTest_Wait(__func__, "requested stream delta emitted");
        pthread_mutex_lock(&state.mu);
        state.requested++;
        pthread_cond_broadcast(&state.cv);
        while (state.emitted != state.requested) pthread_cond_wait(&state.cv, &state.mu);
        pthread_mutex_unlock(&state.mu);
        pico_host_pump(host);
        const PicoMessage *message = LastAssistantMessage(agent);
        if (!message) { Fail("stream debounce missing assistant message"); goto done; }
        size_t source = message->source ? strlen(message->source) : 0;
        size_t doc = DocTextBytes(message);
        doc_ran_ahead |= doc > source;
        stream_grew |= source > previous_source;
        lagged |= source > 0 && doc < source;
        previous_source = source;
    }

    StreamProbeRelease(&state);
    bool completed = PumpUntilIdle(host, agent);
    const PicoMessage *message = LastAssistantMessage(agent);
    if (!completed || !message || !message->source || !message->source[0])
    {
        Fail("stream debounce turn did not complete with streamed text");
        goto done;
    }
    size_t final_source = strlen(message->source);
    size_t final_doc = DocTextBytes(message);
    if (!stream_grew
#ifdef PICO_TEST_CLOCK
        || !lagged
#endif
       )
    {
        Fail("mid-stream reparses must be debounced: the document must render a "
             "prefix of the source between reparses");
        goto done;
    }
    if (doc_ran_ahead || final_doc != final_source)
    {
        Fail("the streamed document must stay a prefix of the source and catch up "
             "to the full stream at turn end");
        goto done;
    }
    rc = 0;
done:
    StreamProbeRelease(&state);
#ifdef PICO_TEST_CLOCK
    PicoTestClock_Reset();
#endif
    pico_host_free(host);
    StreamProbeDestroy(&state);
    rmdir(dir);
    return rc;
}

static int TestDiffShutdownDoesNotWaitForGit(void)
{
    char bin[] = "/tmp/pico-diff-bin-XXXXXX";
    char workspace[] = "/tmp/pico-diff-ws-XXXXXX";
    char git_path[512];
    char marker[512];
    char release[512];
    PicoHost *host = NULL;
    PicoWorkspaceId id = 0;
    if (!mkdtemp(bin) || !mkdtemp(workspace))
    {
        Fail("mkdtemp diff shutdown");
        return 1;
    }
    snprintf(git_path, sizeof(git_path), "%s/git", bin);
    snprintf(marker, sizeof(marker), "%s/entered", bin);
    snprintf(release, sizeof(release), "%s/release", bin);
    const char *script =
        "#!/bin/sh\n"
        "printf E >> \"$PICO_DIFF_TEST_MARKER\"\n"
        "while [ ! -f \"$PICO_DIFF_TEST_RELEASE\" ]; do sleep 0.01; done\n"
        "printf D >> \"$PICO_DIFF_TEST_MARKER\"\n"
        "exit 0\n";
    char *old_path = DupStr(getenv("PATH") ? getenv("PATH") : "");
    char test_path[8192];
    snprintf(test_path, sizeof(test_path), "%s:%s", bin, old_path ? old_path : "");
    if (WriteFile(git_path, script) != 0 || chmod(git_path, 0755) != 0)
    {
        Fail("write fake git");
        free(old_path);
        RmRf(bin);
        RmRf(workspace);
        return 1;
    }
    setenv("PATH", test_path, 1);
    setenv("PICO_DIFF_TEST_MARKER", marker, 1);
    setenv("PICO_DIFF_TEST_RELEASE", release, 1);
    bool opened = pico_host_init(&host, NULL, true) == PICO_OK && host &&
                  pico_workspace_open(host, workspace, &id) == PICO_OK;
    PICO_TEST_WAIT(opened && access(marker, F_OK) != 0)
    {
    }
    bool blocked = access(marker, F_OK) == 0;
    if (blocked)
    {
        pico_workspace_request_close(host, id);
        pico_host_pump(host);
    }
    WriteFile(release, "release\n");
    if (host)
    {
        pico_host_free(host);
    }
    free(old_path);
    /* The worker is intentionally detached. Keep its fake executable,
     * workspace, PATH, and release marker valid until this test process exits
     * instead of imposing a timing-dependent cleanup wait on the main thread. */
    if (!opened || !blocked)
    {
        Fail("diff workspace shutdown must detach without waiting for blocked git");
        return 1;
    }
    return 0;
}

static int TestPersistenceShutdownUsesSharedDeadline(void)
{
    int ready[2];
    int proceed[2];
    if (pipe(ready) != 0 || pipe(proceed) != 0)
    {
        Fail("persist shutdown pipes");
        return 1;
    }
    pid_t child = fork();
    if (child < 0)
    {
        Fail("persist shutdown fork");
        close(ready[0]); close(ready[1]); close(proceed[0]); close(proceed[1]);
        return 1;
    }
    if (child == 0)
    {
        char dir[] = "/tmp/pico-persist-shutdown-XXXXXX";
        char cfg[] = "/tmp/pico-persist-shutdown-cfg-XXXXXX";
        PicoHost *host = NULL;
        PicoWorkspaceId ws = 0;
        PicoAgentCreateOptions opt = {
            .kind = PICO_AGENT_MAIN,
            .session_start = PICO_SESSION_NEW,
            .select = true,
        };
        PicoAgentId id = 0;
        if (!mkdtemp(dir) || !mkdtemp(cfg))
        {
            _exit(2);
        }
        setenv("XDG_CONFIG_HOME", cfg, 1);
        if (pico_host_init(&host, NULL, true) != PICO_OK ||
            pico_workspace_open(host, dir, &ws) != PICO_OK ||
            pico_main_agent_create(host, ws, &opt, &id) != PICO_OK)
        {
            _exit(3);
        }
        PicoAgent *agent = PicoHost_FindAgent(host, id);
        if (!agent)
        {
            _exit(4);
        }
        snprintf(agent->model, sizeof(agent->model), "shutdown-model");
        g_persist_ready_fd = ready[1];
        g_persist_continue_fd = proceed[0];
        PicoSession_EnqueueModelChange(host, agent);
        /* Ready means the persist hook is blocked on proceed. Leave it blocked
         * until shutdown returns: the parent must not release it on a timer. */
        if (!TransferTestByte(ready[0], false))
        {
            _exit(5);
        }
        /* CTest catches a shutdown that never returns while this gate is held. */

#ifdef PICO_TEST_CLOCK
        PicoTestClock_ExpireWaits();
#endif
        PicoHostShutdownResult result = PicoHost_Shutdown(host);
#ifdef PICO_TEST_CLOCK
        bool waited = PicoTestClock_SharedFutureDeadline(1);
        PicoTestClock_ResumeWaits();
        if (!waited) _exit(7);
#endif
        (void)TransferTestByte(proceed[1], true);
        /* The blocked writer stays held across shutdown. Controlled expiry
         * verifies a future drain deadline, not elapsed scheduler time. */
        if (result != PICO_HOST_SHUTDOWN_RETAINED)
        {
            _exit(6);
        }
        _exit(0);
    }

    int status = 0;

    pid_t waited = -1;
    do
    {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    close(ready[0]); close(ready[1]); close(proceed[0]); close(proceed[1]);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        char message[160];
        if (waited != child)
        {
            snprintf(message, sizeof(message),
                     "blocked persistence must consume the process-wide shutdown deadline (wait failed)");
        }
        else if (WIFEXITED(status))
        {
            snprintf(message, sizeof(message),
                     "blocked persistence must consume the process-wide shutdown deadline (exit %d)",
                     WEXITSTATUS(status));
        }
        else if (WIFSIGNALED(status))
        {
            snprintf(message, sizeof(message),
                     "blocked persistence must consume the process-wide shutdown deadline (signal %d)",
                     WTERMSIG(status));
        }
        else
        {
            snprintf(message, sizeof(message),
                     "blocked persistence must consume the process-wide shutdown deadline");
        }
        Fail(message);
        return 1;
    }
    return 0;
}

static int TestProcessShutdownUsesSharedDeadline(void)
{
    char dirA[] = "/tmp/pico-deadline-a-XXXXXX";
    char dirB[] = "/tmp/pico-deadline-b-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId idA = 0, idB = 0;
    MatrixProviderState stateA, stateB;
    MatrixStateInit(&stateA, MATRIX_PROVIDER_BLOCK);
    MatrixStateInit(&stateB, MATRIX_PROVIDER_BLOCK);
    if (!mkdtemp(dirA) || !mkdtemp(dirB) ||
        pico_host_init(&host, NULL, true) != PICO_OK || !host ||
        pico_workspace_open(host, dirA, &idA) != PICO_OK ||
        pico_workspace_open(host, dirB, &idB) != PICO_OK)
    {
        Fail("setup shared shutdown deadline");
        return 1;
    }
    PicoWorkspace *wsA = PicoHost_FindWorkspace(host, idA);
    PicoWorkspace *wsB = PicoHost_FindWorkspace(host, idB);
    bool configured = ConfigureMatrixWorkspace(host, wsA, &stateA, false) &&
                      ConfigureMatrixWorkspace(host, wsB, &stateB, false);
    PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    PicoAgentId a1 = 0, b1 = 0;
    pico_main_agent_create(host, idA, &opt, &a1);
    pico_main_agent_create(host, idB, &opt, &b1);
    PicoAgent *agentA = PicoHost_FindAgent(host, a1);
    PicoAgent *agentB = PicoHost_FindAgent(host, b1);
    if (configured && agentA && agentB)
    {
        PicoAgent_StartTurn(host, agentA, "block A at shutdown");
        PicoAgent_StartTurn(host, agentB, "block B at shutdown");
    }
    PICO_TEST_WAIT((!MatrixStateFlag(&stateA, false) || !MatrixStateFlag(&stateB, false)))
    {
    }
    bool both_blocked = MatrixStateFlag(&stateA, false) && MatrixStateFlag(&stateB, false);
#ifdef PICO_TEST_CLOCK
    PicoTestClock_ExpireWaits();
#endif
    PicoHostShutdownResult result = PicoHost_Shutdown(host);
#ifdef PICO_TEST_CLOCK
    bool waited = PicoTestClock_SharedFutureDeadline(2);
    PicoTestClock_ResumeWaits();
#else
    bool waited = true;
#endif
    MatrixStateRelease(&stateA);
    MatrixStateRelease(&stateB);
    PICO_TEST_WAIT((!MatrixStateFlag(&stateA, true) || !MatrixStateFlag(&stateB, true)))
    {
    }
    bool shared = both_blocked && waited && result == PICO_HOST_SHUTDOWN_RETAINED;
    if (!shared)
    {
        Fail("shutdown must return with all blocked workspaces retained before release");
        return 1;
    }
    return 0;
}

static int TestMultiWorkspaceDeletedDirectoryIntegrity(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init deleted dir test");
        return 1;
    }

    char dirA[] = "/tmp/pico-ws-delA-XXXXXX";
    if (!mkdtemp(dirA))
    {
        Fail("mkdtemp deleted dir test");
        return 1;
    }

    PicoWorkspaceId idA = 0;
    pico_workspace_open(host, dirA, &idA);
    PicoWorkspaceInfo info;
    pico_workspace_info(host, 0, &info);

    /* Delete directory from filesystem */
    rmdir(dirA);

    /* Workspace identity and stored canonical path remain intact */
    PicoWorkspaceInfo info_after;
    if (!pico_workspace_info(host, 0, &info_after) || info_after.id != idA ||
        strcmp(info_after.path, info.path) != 0)
    {
        Fail("deleted directory should not change workspace identity or canonical path");
    }

    pico_host_free(host);
    return 0;
}

static int TestUnusedPendingDraftDiscardedOnSelectedCreate(void)
{
    char dir[] = "/tmp/pico-ws-side-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-side-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId ws = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId first = 0;
    PicoAgentId second = 0;
    PicoAgentInfo info;

    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("mkdtemp sidebar agents");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || pico_workspace_open(host, dir, &ws) != PICO_OK)
    {
        Fail("init sidebar agents");
        unsetenv("XDG_CONFIG_HOME");
        if (host)
        {
            pico_host_free(host);
        }
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NEW;
    opt.select = true;
    if (pico_main_agent_create(host, ws, &opt, &first) != PICO_OK || first == 0)
    {
        Fail("create first main agent");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    if (pico_main_agent_create(host, ws, &opt, &second) != PICO_OK || second == 0 || second == first)
    {
        Fail("create second main agent");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    if (pico_agent_active(host) != second || pico_agent_find(host, first, &info) ||
        !pico_agent_find(host, second, &info) || pico_agent_count(host) != 1)
    {
        Fail("creating a selected new session must discard the unused pending draft");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(dir);
    return 0;
}

static int TestUnusedPendingDraftDiscardedOnSelect(void)
{
    char dir[] = "/tmp/pico-ws-draft-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-draft-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId ws = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId first = 0;
    PicoAgentId other = 0;
    PicoAgentInfo info;

    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("mkdtemp select discards draft");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || pico_workspace_open(host, dir, &ws) != PICO_OK)
    {
        Fail("init select discards draft");
        unsetenv("XDG_CONFIG_HOME");
        if (host)
        {
            pico_host_free(host);
        }
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NEW;
    opt.select = true;
    if (pico_main_agent_create(host, ws, &opt, &first) != PICO_OK || first == 0)
    {
        Fail("create pending draft");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    opt.session_start = PICO_SESSION_NONE;
    opt.select = false;
    if (pico_main_agent_create(host, ws, &opt, &other) != PICO_OK || other == 0)
    {
        Fail("create other main agent");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    if (!pico_agent_select(host, other) || pico_agent_active(host) != other ||
        pico_agent_find(host, first, &info) || !pico_agent_find(host, other, &info))
    {
        Fail("selecting another agent must discard the unused pending draft");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(dir);
    return 0;
}

static int TestPersistedSessionKeptOnSelect(void)
{
    char dir[] = "/tmp/pico-ws-keep-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-keep-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId ws = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId first = 0;
    PicoAgentId second = 0;
    PicoAgent *agent;
    PicoAgentInfo info;
    char session_path[4096];

    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("mkdtemp keep persisted");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || pico_workspace_open(host, dir, &ws) != PICO_OK)
    {
        Fail("init keep persisted");
        unsetenv("XDG_CONFIG_HOME");
        if (host)
        {
            pico_host_free(host);
        }
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NEW;
    opt.select = true;
    if (pico_main_agent_create(host, ws, &opt, &first) != PICO_OK || first == 0)
    {
        Fail("create first session");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    agent = PicoHost_FindAgent(host, first);
    if (!agent || PicoSession_LogUser(host, agent, "hello", "hello", NULL) != PICO_SESSION_WRITE_OK ||
        !agent->session_id[0] || !agent->session_path[0])
    {
        Fail("first user write must persist the session");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    /* Session appends are written by the persist thread; wait for the file. */
    PicoSession_DrainPersist(host, agent);
    snprintf(session_path, sizeof(session_path), "%s", agent->session_path);
    if (pico_main_agent_create(host, ws, &opt, &second) != PICO_OK || second == 0 || second == first)
    {
        Fail("create second session");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        unlink(session_path);
        return 1;
    }
    if (pico_agent_active(host) != second || !pico_agent_find(host, first, &info) ||
        !pico_agent_find(host, second, &info) || pico_agent_count(host) != 2 || access(session_path, F_OK) != 0)
    {
        Fail("a persisted session must stay live after selecting another agent");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        unlink(session_path);
        return 1;
    }
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unlink(session_path);
    rmdir(dir);
    return 0;
}

static int TestModelChangeKeepsUnusedDraftOnSelect(void)
{
    char dir[] = "/tmp/pico-ws-model-draft-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-model-draft-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId ws = 0;
    PicoWorkspace *workspace;
    PicoAgentCreateOptions opt;
    PicoAgentId first = 0;
    PicoAgentId other = 0;
    PicoAgent *agent;
    PicoAgentInfo info;
    PicoModel models[1];

    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("mkdtemp model draft");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || pico_workspace_open(host, dir, &ws) != PICO_OK)
    {
        Fail("init model draft");
        unsetenv("XDG_CONFIG_HOME");
        if (host)
        {
            pico_host_free(host);
        }
        return 1;
    }
    workspace = PicoHost_FindWorkspace(host, ws);
    memset(models, 0, sizeof(models));
    snprintf(models[0].id, sizeof(models[0].id), "kept-model");
    snprintf(models[0].name, sizeof(models[0].name), "kept-model");
    workspace->models = models;
    workspace->model_count = 1;
    snprintf(workspace->settings.default_model, sizeof(workspace->settings.default_model), "kept-model");

    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NEW;
    opt.select = true;
    if (pico_main_agent_create(host, ws, &opt, &first) != PICO_OK || first == 0)
    {
        Fail("create model draft");
        workspace->models = NULL;
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    agent = PicoHost_FindAgent(host, first);
    if (!agent || !PicoSettings_SetModel(agent, "kept-model") || !agent->session_id[0] ||
        !agent->session_path[0])
    {
        Fail("SetModel must assign a session identity immediately");
        workspace->models = NULL;
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    opt.session_start = PICO_SESSION_NONE;
    opt.select = false;
    if (pico_main_agent_create(host, ws, &opt, &other) != PICO_OK || other == 0)
    {
        Fail("create other agent");
        workspace->models = NULL;
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    if (!pico_agent_select(host, other) || pico_agent_active(host) != other ||
        !pico_agent_find(host, first, &info) || !pico_agent_find(host, other, &info))
    {
        Fail("model change must keep the unused draft when selecting another agent");
        workspace->models = NULL;
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    /* SetModel queues durable session work. Finish it before shutdown so a
     * slow persist thread cannot retain the process and poison later tests. */
    if (!DrainSessionForAssertion(host, PicoHost_FindAgent(host, first)))
    {
        Fail("model-change draft persistence must finish before shutdown");
        workspace->models = NULL;
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        return 1;
    }
    workspace->models = NULL;
    if (pico_host_free(host) == PICO_HOST_SHUTDOWN_RETAINED)
    {
        Fail("model-change draft shutdown must complete before later tests");
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        return 1;
    }
    unsetenv("XDG_CONFIG_HOME");
    rmdir(dir);
    return 0;
}

static int TestAgentCloseAppliesQueuedPersistenceFailure(void)
{
    char dir[] = "/tmp/pico-ws-close-persist-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-close-persist-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId ws = 0;
    PicoWorkspace *workspace;
    PicoAgentCreateOptions opt;
    PicoAgentId id = 0;
    PicoAgent *agent;
    PicoModel models[1];
    char missing_path[4096];

    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("mkdtemp close persistence");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK ||
        pico_workspace_open(host, dir, &ws) != PICO_OK)
    {
        Fail("init close persistence");
        return 1;
    }
    workspace = PicoHost_FindWorkspace(host, ws);
    memset(models, 0, sizeof(models));
    snprintf(models[0].id, sizeof(models[0].id), "close-model");
    snprintf(models[0].name, sizeof(models[0].name), "close-model");
    workspace->models = models;
    workspace->model_count = 1;
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NEW;
    opt.select = true;
    if (pico_main_agent_create(host, ws, &opt, &id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, id)))
    {
        workspace->models = NULL;
        pico_host_free(host);
        Fail("create close persistence agent");
        return 1;
    }
    snprintf(agent->session_id, sizeof(agent->session_id), "close-persist-id");
    snprintf(missing_path, sizeof(missing_path), "%s/missing/session.jsonl", dir);
    snprintf(agent->session_path, sizeof(agent->session_path), "%s", missing_path);
    if (!PicoSettings_SetModel(agent, "close-model") || pico_agent_close(host, id) != PICO_OK ||
        !host->status_warn || !strstr(host->status_warn, "Session persistence failed"))
    {
        workspace->models = NULL;
        pico_host_free(host);
        Fail("agent close must apply queued persistence failure before removal");
        return 1;
    }
    workspace->models = NULL;
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(dir);
    return 0;
}

static int HoldSessionLockChild(const char *session_path, int ready_fd, int continue_fd)
{
    char lock_path[4102];
    int fd;
    struct flock lock;
    if (!session_path ||
        (size_t)snprintf(lock_path, sizeof(lock_path), "%s.lock", session_path) >= sizeof(lock_path))
    {
        return 2;
    }
    fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        return 3;
    }
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    while (fcntl(fd, F_SETLKW, &lock) != 0)
    {
        if (errno == EINTR)
        {
            continue;
        }
        close(fd);
        return 4;
    }
    if (!TransferTestByte(ready_fd, true) || !TransferTestByte(continue_fd, false))
    {
        close(fd);
        return 5;
    }
    close(fd);
    return 0;
}

static int TestTitleRewriteDoesNotBlockOtherWorkspace(void)
{
    char dirA[] = "/tmp/pico-title-lock-A-XXXXXX";
    char dirB[] = "/tmp/pico-title-lock-B-XXXXXX";
    char cfg[] = "/tmp/pico-title-lock-cfg-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId idA = 0, idB = 0;
    PicoAgentId agentA = 0, agentB = 0;
    PicoAgentCreateOptions opt;
    PicoAgent *session_agent;
    char session_path[4096];
    int ready[2] = {-1, -1};
    int proceed[2] = {-1, -1};
    pid_t child = -1;
    int status = 0;
    int result = 1;
    size_t file_len = 0;
    char *file = NULL;

    if (!mkdtemp(dirA) || !mkdtemp(dirB) || !mkdtemp(cfg))
    {
        Fail("title lock fixture");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NEW;
    opt.select = true;
    if (pico_host_init(&host, NULL, true) != PICO_OK ||
        pico_workspace_open(host, dirA, &idA) != PICO_OK ||
        pico_workspace_open(host, dirB, &idB) != PICO_OK ||
        pico_main_agent_create(host, idA, &opt, &agentA) != PICO_OK)
    {
        Fail("title lock host setup");
        goto done;
    }
    session_agent = PicoHost_FindAgent(host, agentA);
    if (!session_agent ||
        PicoSession_LogUser(host, session_agent, "seed", "seed", NULL) != PICO_SESSION_WRITE_OK ||
        !DrainSessionForAssertion(host, session_agent) || !session_agent->session_path[0])
    {
        Fail("title lock seed session");
        goto done;
    }
    opt.select = false;
    if (pico_main_agent_create(host, idB, &opt, &agentB) != PICO_OK ||
        !pico_agent_select(host, agentB) ||
        !PicoHost_FindAgent(host, agentA))
    {
        Fail("title lock other workspace");
        goto done;
    }
    snprintf(session_path, sizeof(session_path), "%s", session_agent->session_path);
    if (pipe(ready) != 0 || pipe(proceed) != 0)
    {
        Fail("title lock pipes");
        goto done;
    }
    child = fork();
    if (child < 0)
    {
        Fail("title lock fork");
        goto done;
    }
    if (child == 0)
    {
        close(ready[0]);
        close(proceed[1]);
        _exit(HoldSessionLockChild(session_path, ready[1], proceed[0]));
    }
    close(ready[1]);
    close(proceed[0]);
    ready[1] = proceed[0] = -1;

    if (!TransferTestByte(ready[0], false))
    {

        Fail("title lock holder did not acquire the session lock");
        goto done;
    }
    if (PicoSession_LogTitle(host, session_agent, "Locked title") != PICO_SESSION_WRITE_OK)
    {

        Fail("title rewrite was not accepted while the session lock was held");
        goto done;
    }
    pico_host_request_submit_cancel(host);
    pico_host_pump(host);

    if (!host->submit_cancel)
    {
        Fail("other workspace must still accept cancellation while a title rewrite is pending");
        goto done;
    }
    if (!TransferTestByte(proceed[1], true))
    {
        Fail("could not release the held session lock");
        goto done;
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        child = -1;
        Fail("session lock holder exited unsuccessfully");
        goto done;
    }
    child = -1;
    if (!DrainSessionForAssertion(host, session_agent))
    {
        Fail("pending title rewrite did not complete after the lock was released");
        goto done;
    }
    file = Pico_ReadFile(session_agent->session_path, &file_len);
    if (!file || !strstr(file, "\"title\":\"Locked title\""))
    {
        Fail("title rewrite did not land after the lock holder released");
        goto done;
    }
    result = 0;
done:

    if (child > 0)
    {
        if (proceed[1] >= 0)
        {
            (void)TransferTestByte(proceed[1], true);
        }
        waitpid(child, &status, 0);
    }
    if (ready[0] >= 0) close(ready[0]);
    if (ready[1] >= 0) close(ready[1]);
    if (proceed[0] >= 0) close(proceed[0]);
    if (proceed[1] >= 0) close(proceed[1]);
    free(file);
    if (host) pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    RmRf(cfg);
    RmRf(dirA);
    RmRf(dirB);
    if (result && !g_failed) Fail("title lock workspace pump setup failed");
    return result;
}

/* Opening at capacity can reenter through the victim's destroy hook. */
typedef struct CapacityOpenState {
    const char *path;
    PicoResult result;
    PicoWorkspaceId opened;
    bool called;
} CapacityOpenState;

static void CapacityDestroyOpen(PicoWorkspace *workspace, const PicoHookEvent *event, void *opaque)
{
    CapacityOpenState *state = opaque;
    (void)event;
    if (state->called) return;
    state->called = true;
    state->result = pico_workspace_open(workspace->host, state->path, &state->opened);
}

static void RegisterCapacityDestroyOpen(PicoHost *host, PicoWorkspaceId id, CapacityOpenState *state)
{
    PicoWorkspace *workspace = PicoHost_FindWorkspace(host, id);
    PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, workspace);
    pico_workspace_add_hook(workspace, PICO_HOOK_ON_AGENT_DESTROY, CapacityDestroyOpen);
    PicoHost_PublishRegistration(host, state);
}

static int TestCapacityOpenRechecksCanonicalPath(void)
{
    char root[] = "/tmp/pico-capacity-reentrant-XXXXXX";
    char dirs[PICO_MAX_WORKSPACES + 1][128] = {{0}};
    PicoWorkspaceId ids[PICO_MAX_WORKSPACES] = {0}, opened = 0;
    PicoHost *host = NULL;
    PicoAgentId selected = 0, victim = 0;
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    CapacityOpenState state = {.result = PICO_INVALID};
    int rc = 1;
    if (!mkdtemp(root)) return 1;
    for (int i = 0; i <= PICO_MAX_WORKSPACES; i++)
    {
        snprintf(dirs[i], sizeof(dirs[i]), "%s/%d", root, i);
        if (mkdir(dirs[i], 0700)) goto done;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK) goto done;
    for (int i = 0; i < PICO_MAX_WORKSPACES; i++)
        if (pico_workspace_open(host, dirs[i], &ids[i]) != PICO_OK) goto done;
    if (pico_main_agent_create(host, ids[PICO_MAX_WORKSPACES - 1], &options, &selected) != PICO_OK ||
        pico_main_agent_create(host, ids[0], &options, &victim) != PICO_OK) goto done;
    state.path = dirs[PICO_MAX_WORKSPACES];
    RegisterCapacityDestroyOpen(host, ids[0], &state);
    PicoResult result = pico_workspace_open(host, state.path, &opened);
    int matches = 0;
    PicoWorkspaceInfo info;
    for (int i = 0; i < pico_workspace_count(host); i++)
        if (pico_workspace_info(host, i, &info) && !strcmp(info.path, state.path)) matches++;
    if (state.result != PICO_OK || result != PICO_ALREADY_OPEN ||
        opened != state.opened || matches != 1)
    {
        Fail("an open reentered during eviction must reuse the newly opened canonical workspace");
        goto done;
    }
    rc = 0;
done:
    if (host) pico_host_free(host);
    RmRf(root);
    if (rc && !g_failed) Fail("reentrant capacity open fixture failed");
    return rc;
}

static int TestInvalidCreateAtCapacityPreservesAgents(void)
{
    char dir[] = "/tmp/pico-capacity-invalid-XXXXXX";
    char cfg[] = "/tmp/pico-capacity-invalid-cfg-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId ws = 0;
    PicoAgentId ids[PICO_MAX_AGENTS] = {0}, corrupt = 0;
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NEW};
    char session_id[40] = {0}, corrupt_id[40] = {0}, corrupt_path[4096] = {0};
    int rc = 1;
    if (!mkdtemp(dir) || !mkdtemp(cfg)) return 1;
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK ||
        pico_workspace_open(host, dir, &ws) != PICO_OK) goto done;
    /* A durable idle agent is itself the oldest eligible eviction victim. */
    options.session_start = PICO_SESSION_NONE;
    if (pico_main_agent_create(host, ws, &options, &ids[0]) != PICO_OK) goto done;
    options.session_start = PICO_SESSION_NEW;
    if (pico_main_agent_create(host, ws, &options, &ids[1]) != PICO_OK ||
        pico_main_agent_create(host, ws, &options, &corrupt) != PICO_OK) goto done;
    PicoAgent *durable = PicoHost_FindAgent(host, ids[1]);
    PicoAgent *bad = PicoHost_FindAgent(host, corrupt);
    if (PicoSession_LogUser(host, durable, "saved", "saved", NULL) != PICO_SESSION_WRITE_OK ||
        PicoSession_LogUser(host, bad, "corrupt", "corrupt", NULL) != PICO_SESSION_WRITE_OK ||
        !DrainSessionForAssertion(host, durable) || !DrainSessionForAssertion(host, bad)) goto done;
    snprintf(session_id, sizeof(session_id), "%s", durable->session_id);
    snprintf(corrupt_id, sizeof(corrupt_id), "%s", bad->session_id);
    snprintf(corrupt_path, sizeof(corrupt_path), "%s", bad->session_path);
    if (pico_agent_close(host, corrupt) != PICO_OK) goto done;
    FILE *file = fopen(corrupt_path, "ab");
    if (!file) goto done;
    fputs("{invalid json}\n", file);
    if (fclose(file)) goto done;
    options.session_start = PICO_SESSION_NONE;
    for (int i = 2; i < PICO_MAX_AGENTS; i++)
        if (pico_main_agent_create(host, ws, &options, &ids[i]) != PICO_OK) goto done;
    PicoAgentCreateOptions requests[] = {
        {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE, .model = "unknown-model"},
        {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_RESUME, .session_id = "unknown-session"},
        {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_RESUME, .session_id = corrupt_id},
        {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_RESUME, .session_id = session_id},
    };
    PicoResult expected[] = {PICO_INVALID, PICO_SESSION_INVALID, PICO_SESSION_INVALID, PICO_SESSION_IN_USE};
    for (int request = 0; request < (int)(sizeof(requests) / sizeof(requests[0])); request++)
    {
        PicoAgentId created = 0;
        if (pico_main_agent_create(host, ws, &requests[request], &created) != expected[request] || created)
        {
            Fail("invalid creation at capacity must report its validation error");
            goto done;
        }
        for (int i = 0; i < PICO_MAX_AGENTS; i++)
            if (!PicoHost_FindAgent(host, ids[i]))
            {
                Fail("invalid options and invalid or reserved resumes must not evict existing agents");
                goto done;
            }
    }
    rc = 0;
done:
    if (host) pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    RmRf(cfg); RmRf(dir);
    if (rc && !g_failed) Fail("invalid capacity creation fixture failed");
    return rc;
}

static bool WriteCapacityReplay(const char *path, int messages)
{
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    fputs("{\"type\":\"session\",\"version\":4,\"kind\":\"normal\",\"id\":\"capacity-replay\"}\n", file);
    for (int i = 0; i < messages; i++)
        fputs("{\"type\":\"message\",\"role\":\"user\",\"content\":\"capacity replay\"}\n", file);
    return fclose(file) == 0;
}

static int TestCapacityCreateProtectsPendingReplacement(void)
{
    char dir[] = "/tmp/pico-capacity-replace-XXXXXX";
    char path[128];
    PicoHost *host = NULL;
    PicoWorkspaceId ws_id = 0;
    PicoAgentId ids[PICO_MAX_AGENTS] = {0}, created = 0;
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    int ready[2] = {-1, -1}, release[2] = {-1, -1};
    bool released = false;
    int rc = 1;
    if (!mkdtemp(dir)) return 1;
    snprintf(path, sizeof(path), "%s/replay.jsonl", dir);
    if (!WriteCapacityReplay(path, 1) || pico_host_init(&host, NULL, true) != PICO_OK ||
        pico_workspace_open(host, dir, &ws_id) != PICO_OK) goto done;
    for (int i = 0; i < PICO_MAX_AGENTS; i++)
        if (pico_main_agent_create(host, ws_id, &options, &ids[i]) != PICO_OK) goto done;
    if (pipe(ready) || pipe(release)) goto done;
    g_replay_ready_fd = ready[1];
    g_replay_continue_fd = release[0];
    if (PicoSession_LoadAsync(host, ws_id, ids[1], path, false, false, true, false) != PICO_OK ||
        !TransferTestByte(ready[0], false)) goto done;
    if (pico_main_agent_create(host, ws_id, &options, &created) != PICO_OK ||
        !PicoHost_FindAgent(host, ids[1]) || !PicoSession_LoadPending(host))
    {
        Fail("capacity creation must not evict the target of a pending replacement load");
        goto done;
    }
    if (!TransferTestByte(release[1], true)) goto done;
    released = true;
    PICO_TEST_WAIT(PicoSession_LoadPending(host)) pico_host_pump(host);
    bool restored = false;
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, ws_id);
    for (int i = 0; i < ws->count; i++)
        if (ws->agents[i]->message_count == 1 &&
            !strcmp(ws->agents[i]->messages[0].source, "capacity replay")) restored = true;
    if (!restored || PicoHost_FindAgent(host, ids[1]))
    {
        Fail("a replacement load must still commit after concurrent capacity eviction");
        goto done;
    }
    rc = 0;
done:
    if (!released && release[1] >= 0) (void)TransferTestByte(release[1], true);
    if (host) pico_host_free(host);
    g_replay_ready_fd = g_replay_continue_fd = -1;
    for (int i = 0; i < 2; i++)
    {
        if (ready[i] >= 0) close(ready[i]);
        if (release[i] >= 0) close(release[i]);
    }
    RmRf(dir);
    if (rc && !g_failed) Fail("pending replacement capacity fixture failed");
    return rc;
}

static int TestLoadTargetSurvivesPreviousLoadCancellation(void)
{
    char root[] = "/tmp/pico-capacity-load-XXXXXX";
    char dirs[PICO_MAX_WORKSPACES + 1][128] = {{0}}, path[128];
    PicoWorkspaceId ids[PICO_MAX_WORKSPACES] = {0};
    PicoHost *host = NULL;
    PicoAgentId selected = 0;
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};
    CapacityOpenState state = {.result = PICO_INVALID};
    int rc = 1;
    if (!mkdtemp(root)) return 1;
    snprintf(path, sizeof(path), "%s/replay.jsonl", root);
    if (!WriteCapacityReplay(path, 80)) goto done;
    for (int i = 0; i <= PICO_MAX_WORKSPACES; i++)
    {
        snprintf(dirs[i], sizeof(dirs[i]), "%s/%d", root, i);
        if (mkdir(dirs[i], 0700)) goto done;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK) goto done;
    for (int i = 0; i < PICO_MAX_WORKSPACES; i++)
        if (pico_workspace_open(host, dirs[i], &ids[i]) != PICO_OK) goto done;
    if (pico_main_agent_create(host, ids[PICO_MAX_WORKSPACES - 1], &options, &selected) != PICO_OK) goto done;
    state.path = dirs[PICO_MAX_WORKSPACES];
    RegisterCapacityDestroyOpen(host, ids[1], &state);
    int adopted_before = g_replay_adopted;
    if (PicoSession_LoadAsync(host, ids[1], 0, path, false, false, true, false) != PICO_OK) goto done;
    PICO_TEST_WAIT(g_replay_adopted == adopted_before) pico_host_pump(host);
    if (!PicoSession_LoadPending(host)) goto done;
    /* The private candidate has received replay callbacks, so cancelling it
     * dispatches a destroy hook. The new target is the oldest idle workspace. */
    if (PicoSession_LoadAsync(host, ids[0], 0, path, false, false, true, false) != PICO_OK ||
        !state.called || state.result != PICO_OK || !PicoHost_FindWorkspace(host, ids[0]))
    {
        Fail("the new load target must survive workspace opens from old candidate destroy hooks");
        goto done;
    }
    PICO_TEST_WAIT(PicoSession_LoadPending(host)) pico_host_pump(host);
    PicoAgent *loaded = PicoHost_SelectedAgent(host);
    if (!loaded || loaded->workspace->id != ids[0] || loaded->message_count != 80)
    {
        Fail("a superseding load must finish in its original target workspace");
        goto done;
    }
    rc = 0;
done:
    if (host) pico_host_free(host);
    RmRf(root);
    if (rc && !g_failed) Fail("load cancellation capacity fixture failed");
    return rc;
}

static int TestAsyncSessionReplay(void)

{
    char dir[] = "/tmp/pico-async-session-XXXXXX";
    char cfg[] = "/tmp/pico-async-cfg-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId ws_id = 0;
    PicoAgentId seed_id = 0, current_id = 0, other_id = 0;
    PicoHost *startup = NULL;
    PicoHost *bad_startup = NULL;
    PicoHost *no_session_startup = NULL;
    char bad_path[4096] = {0};
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NEW,
                                      .select = true};
    char session_id[40] = {0}, path[4096] = {0};
    int ready[2] = {-1, -1}, release[2] = {-1, -1};
    int rc = 1;
    if (!mkdtemp(dir) || !mkdtemp(cfg)) { Fail("async replay fixture"); return 1; }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK ||
        pico_workspace_open(host, dir, &ws_id) != PICO_OK) goto done;
    WaitPluginLoad(host);
    if (pico_main_agent_create(host, ws_id, &options, &seed_id) != PICO_OK) goto done;
    PicoAgent *seed = PicoHost_FindAgent(host, seed_id);
    if (!seed || PicoSession_LogUser(host, seed, "persisted", "persisted", NULL) != PICO_SESSION_WRITE_OK ||
        !DrainSessionForAssertion(host, seed)) goto done;
    snprintf(session_id, sizeof(session_id), "%s", seed->session_id);
    snprintf(path, sizeof(path), "%s", seed->session_path);
    /* Many records ensure adoption and commit cannot occur in one pump. */
    FILE *f = fopen(path, "ab");
    if (!f) goto done;
    for (int i = 0; i < 80; i++)
        fputs("{\"type\":\"message\",\"role\":\"user\",\"content\":\"*replayed*\"}\n", f);
    if (fclose(f) != 0 || pico_agent_close(host, seed_id) != PICO_OK) goto done;
    options.session_start = PICO_SESSION_NONE;
    if (pico_main_agent_create(host, ws_id, &options, &current_id) != PICO_OK) goto done;
    PicoAgent_AddMessage(host, PicoHost_FindAgent(host, current_id), PICO_ROLE_USER, "original");
    if (pipe(ready) != 0 || pipe(release) != 0) goto done;
    g_replay_ready_fd = ready[1];
    g_replay_continue_fd = release[0];
    int adopted_before = g_replay_adopted;
    /* Exercise the actual /resume command registration, not just its loader. */
    PicoWorkspace *ws = PicoHost_FindWorkspace(host, ws_id);
    const PicoCommand *command = NULL;
    for (int i = 0; i < ws->command_count; i++)
        if (ws->commands[i].name && strcmp(ws->commands[i].name, "resume") == 0)
            command = &ws->commands[i];
    if (!command) goto done;
    command->workspace_run(ws, current_id, session_id, command->state);
    if (!TransferTestByte(ready[0], false)) goto done;
    for (int i = 0; i < 3; i++) pico_host_pump(host);
    PicoAgent *current = PicoHost_FindAgent(host, current_id);
    if (!PicoSession_LoadPending(host) || pico_agent_active(host) != current_id ||
        !current || current->message_count != 1 || strcmp(current->messages[0].source, "original") != 0)
    {
        Fail("/resume must keep the old chat usable while the worker is loading");
        goto done;
    }
    if (!TransferTestByte(release[1], true)) goto done;
    PICO_TEST_WAIT(g_replay_adopted == adopted_before)
    {
        pico_host_pump(host);
    }
    if (g_replay_adopted == adopted_before || !PicoSession_LoadPending(host) ||
        !PicoHost_FindAgent(host, current_id))
    {
        Fail("worker adoption must not publish a partly replayed transcript");
        goto done;
    }
    PICO_TEST_WAIT(PicoSession_LoadPending(host)) pico_host_pump(host);
    PicoAgent *loaded = PicoHost_SelectedAgent(host);
    if (PicoSession_LoadPending(host) || !loaded || loaded->id == current_id ||
        loaded->message_count != 81 || PicoHost_FindAgent(host, current_id) ||
        !loaded->messages[loaded->message_count - 1].doc.block_count)
    {
        Fail("/resume must publish the fully parsed transcript atomically");
        goto done;
    }
    /* Resuming the already-open session must be a no-op, not an in-use error. */
    if (PicoSession_LoadAsync(host, ws_id, loaded->id, session_id, false,
                              false, false, false) != PICO_OK) goto done;
    PICO_TEST_WAIT(PicoSession_LoadPending(host))
    {
        pico_host_pump(host);
    }
    if (PicoSession_LoadPending(host) || PicoHost_SelectedAgent(host) != loaded)
    {
        Fail("resuming the current session must preserve its agent");
        goto done;
    }
    /* An invalid load leaves the selected chat and session reservation alone. */
    if (PicoSession_LoadAsync(host, ws_id, loaded->id, "unknown", false,
                              false, false, false) != PICO_OK) goto done;
    PICO_TEST_WAIT(PicoSession_LoadPending(host))
    {
        pico_host_pump(host);
    }
    if (PicoSession_LoadPending(host) || PicoHost_SelectedAgent(host) != loaded)
    {
        Fail("failed async replay replaced the previous chat");
        goto done;
    }
    /* Selecting another chat supersedes a worker still reading a prior choice. */
    close(ready[0]); close(ready[1]); close(release[0]); close(release[1]);
    ready[0] = ready[1] = release[0] = release[1] = -1;
    if (pipe(ready) != 0 || pipe(release) != 0) goto done;
    g_replay_ready_fd = ready[1];
    g_replay_continue_fd = release[0];
    if (PicoSession_LoadAsync(host, ws_id, loaded->id, session_id, false,
                              false, false, false) != PICO_OK ||
        !TransferTestByte(ready[0], false)) goto done;
    options.session_start = PICO_SESSION_NONE;
    options.select = false;
    if (pico_main_agent_create(host, ws_id, &options, &other_id) != PICO_OK ||
        !pico_agent_select(host, other_id) || PicoSession_LoadPending(host) ||
        !TransferTestByte(release[1], true))
    {
        Fail("selecting another agent must cancel a pending session load");
        goto done;
    }
    for (int i = 0; i < 30; i++) pico_host_pump(host);
    if (pico_agent_active(host) != other_id || !PicoHost_FindAgent(host, loaded->id))
    {
        Fail("cancelled load changed the selected or existing agent");
        goto done;
    }

    /* Sidebar session selection creates a new agent rather than replacing an
     * existing one; it must also keep the current chat until completion. */
    if (pico_agent_close(host, loaded->id) != PICO_OK ||
        PicoSession_LoadAsync(host, ws_id, 0, session_id, false,
                              false, false, false) != PICO_OK ||
        pico_agent_active(host) != other_id) goto done;
    PICO_TEST_WAIT(PicoSession_LoadPending(host))
    {
        pico_host_pump(host);
    }
    PicoAgent *sidebar_loaded = PicoHost_SelectedAgent(host);
    if (PicoSession_LoadPending(host) || !sidebar_loaded ||
        sidebar_loaded->id == other_id || sidebar_loaded->message_count != 81 ||
        !PicoHost_FindAgent(host, other_id))
    {
        Fail("sidebar-style load must select the complete new agent without losing the old chat");
        goto done;
    }

    /* Startup must return while reading is gated, and must reject submits into
     * the empty initial agent until the replay is ready for atomic publication. */
    if (pico_host_init(&startup, NULL, true) != PICO_OK) goto done;
    PicoHost_Start(startup, NULL, dir, true, PICO_SESSION_RESUME, path);
    PicoAgentId initial = pico_agent_active(startup);
    if (!PicoSession_LoadPending(startup) || !initial ||
        pico_agent_submit(startup, initial, "premature", NULL) != PICO_BUSY)
    {
        Fail("startup must defer replay and block submitting to an empty agent");
        goto done;
    }
    PICO_TEST_WAIT(PicoSession_LoadPending(startup))
    {
        pico_host_pump(startup);
    }
    PicoAgent *started = PicoHost_SelectedAgent(startup);
    if (PicoSession_LoadPending(startup) || !started || started->id == initial ||
        started->message_count != 81)
    {
        Fail("startup must publish the complete resumed session");
        goto done;
    }
    if (snprintf(bad_path, sizeof(bad_path), "%s/bad.jsonl", dir) >= (int)sizeof(bad_path)) goto done;
    FILE *bad = fopen(bad_path, "wb");
    if (!bad) goto done;
    fputs("{bad json}\n", bad);
    if (fclose(bad) != 0 || pico_host_init(&bad_startup, NULL, true) != PICO_OK) goto done;
    PicoHost_Start(bad_startup, NULL, dir, true, PICO_SESSION_RESUME, bad_path);
    PicoAgentId empty_id = pico_agent_active(bad_startup);
    PICO_TEST_WAIT(PicoSession_LoadPending(bad_startup))
    {
        pico_host_pump(bad_startup);
    }
    PicoAgent *empty = PicoHost_SelectedAgent(bad_startup);
    if (!empty_id || PicoSession_LoadPending(bad_startup) || !empty ||
        empty->id != empty_id || empty->message_count != 0 ||
        PicoSession_LoadBlocksSubmit(bad_startup, empty_id))
    {
        Fail("invalid startup session must leave the empty agent available");
        goto done;
    }
    if (pico_host_init(&no_session_startup, NULL, true) != PICO_OK) goto done;
    PicoWorkspaceId empty_ws = 0;
    if (pico_workspace_open(no_session_startup, dir, &empty_ws) != PICO_OK) goto done;
    PicoHost_FindWorkspace(no_session_startup, empty_ws)->settings.resume_last = true;
    PicoHost_Start(no_session_startup, NULL, dir, true, PICO_SESSION_NONE, NULL);
    if (PicoSession_LoadPending(no_session_startup) ||
        !PicoHost_SelectedAgent(no_session_startup) ||
        PicoHost_SelectedAgent(no_session_startup)->message_count != 0)
    {
        Fail("explicit no-session startup must ignore resume_last");
        goto done;
    }
    rc = 0;
done:
    /* Always release the test worker before host shutdown, even on failure. */
    if (release[1] >= 0) (void)TransferTestByte(release[1], true);
    g_replay_ready_fd = g_replay_continue_fd = -1;
    if (no_session_startup) pico_host_free(no_session_startup);
    if (bad_startup) pico_host_free(bad_startup);
    if (startup) pico_host_free(startup);
    if (host) pico_host_free(host);
    if (ready[0] >= 0) close(ready[0]);
    if (ready[1] >= 0) close(ready[1]);
    if (release[0] >= 0) close(release[0]);
    if (release[1] >= 0) close(release[1]);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    if (bad_path[0]) unlink(bad_path);
    rmdir(dir);
    if (rc && !g_failed) Fail("async replay setup failed");
    return rc;
}

/* Parsing and decoding a single huge message belongs on the read worker.
 * The one-record replay remains main-thread so extension hooks keep their
 * ordering and threading, but should not reparse its large Markdown source. */
static int TestAsyncReplayLargeMessage(void)
{
    char dir[] = "/tmp/pico-large-replay-XXXXXX";
    char cfg[] = "/tmp/pico-large-replay-cfg-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId ws_id = 0;
    PicoAgentId old_id = 0, current_id = 0;
    int result = 1;
    int ready[2] = {-1, -1}, proceed[2] = {-1, -1};
    bool released = false;
    char path[4096] = {0}, session_id[40] = {0};
    if (!mkdtemp(dir) || !mkdtemp(cfg)) return 1;
    setenv("XDG_CONFIG_HOME", cfg, 1);
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN,
                                      .session_start = PICO_SESSION_NEW, .select = true};
    if (pico_host_init(&host, NULL, true) != PICO_OK ||
        pico_workspace_open(host, dir, &ws_id) != PICO_OK ||
        pico_main_agent_create(host, ws_id, &options, &old_id) != PICO_OK) goto done;
    PicoAgent *old = PicoHost_FindAgent(host, old_id);
    if (!old || PicoSession_LogUser(host, old, "seed", "seed", NULL) != PICO_SESSION_WRITE_OK ||
        !DrainSessionForAssertion(host, old)) goto done;
    snprintf(path, sizeof(path), "%s", old->session_path);
    snprintf(session_id, sizeof(session_id), "%s", old->session_id);
    FILE *file = fopen(path, "ab");
    if (!file) goto done;
    fputs("{\"type\":\"message\",\"role\":\"assistant\",\"message_group\":1,\"content\":\"", file);
    for (int i = 0; i < 256 * 1024; i++) fputc('a', file);
    fputs(" saved-record-target\"}\n", file);
    for (int i = 0; i < 150; i++)
        fprintf(file, "{\"type\":\"message\",\"role\":\"assistant\",\"message_group\":1,\"content\":\" fragment-%d\"}\n", i);
    if (fclose(file) != 0 || pico_agent_close(host, old_id) != PICO_OK) goto done;
    options.session_start = PICO_SESSION_NONE;
    if (pipe(ready) || pipe(proceed)) goto done;
    g_replay_ready_fd = ready[1];
    g_replay_continue_fd = proceed[0];
    if (pico_main_agent_create(host, ws_id, &options, &current_id) != PICO_OK ||
        PicoSession_LoadAsync(host, ws_id, current_id, session_id, false,
                              false, false, false) != PICO_OK) goto done;
    if (!TransferTestByte(ready[0], false)) goto done;
    pico_host_pump(host);
    if (!PicoSession_LoadPending(host) || PicoHost_SelectedAgent(host)->id != current_id) goto done;
    if (!TransferTestByte(proceed[1], true)) goto done;
    released = true;
    PICO_TEST_WAIT(PicoSession_LoadPending(host))
    {
        pico_host_pump(host);
    }
    PicoAgent *loaded = PicoHost_SelectedAgent(host);
    if (PicoSession_LoadPending(host) || !loaded || loaded->id == current_id ||
        loaded->message_count != 2 || !loaded->messages[1].doc.block_count ||
        !strstr(loaded->messages[1].source, "saved-record-target") ||
        !strstr(loaded->messages[1].source, "fragment-149") ||
        DocTextBytes(&loaded->messages[1]) != strlen(loaded->messages[1].source))
    {
        Fail("large validated message must replay atomically without a long UI pump");
        goto done;
    }
    result = 0;
done:
    if (!released && proceed[1] >= 0) (void)TransferTestByte(proceed[1], true);
    if (host) { pico_host_free(host); host = NULL; }
    g_replay_ready_fd = g_replay_continue_fd = -1;
    for (int i = 0; i < 2; i++)
    {
        if (ready[i] >= 0) close(ready[i]);
        if (proceed[i] >= 0) close(proceed[i]);
    }
    if (host) pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    RmRf(cfg); RmRf(dir);
    return result;
}

static int TestSavedSubagentInspectLoadsOffThread(void)
{
    char dir[] = "/tmp/pico-inspect-async-XXXXXX";
    char cfg[] = "/tmp/pico-inspect-async-cfg-XXXXXX";
    PicoHost *seed_host = NULL, *viewer = NULL;
    PicoWorkspaceId ws_id = 0;
    PicoAgentId parent_id = 0, child_id = 0;
    char session_id[40] = {0}, inspect_path[4096] = {0};
    int result = 1;
    if (!mkdtemp(dir) || !mkdtemp(cfg)) return 1;
    setenv("XDG_CONFIG_HOME", cfg, 1);
    PicoAgentCreateOptions main_options = {.kind = PICO_AGENT_MAIN,
                                           .session_start = PICO_SESSION_NONE, .select = true};
    if (pico_host_init(&seed_host, NULL, true) != PICO_OK ||
        pico_workspace_open(seed_host, dir, &ws_id) != PICO_OK ||
        pico_main_agent_create(seed_host, ws_id, &main_options, &parent_id) != PICO_OK) goto done;
    PicoWorkspace *ws = PicoHost_FindWorkspace(seed_host, ws_id);
    PicoAgentCreateOptions child_options = {.kind = PICO_AGENT_SUBAGENT,
        .parent_id = parent_id, .profile = "inspect-test", .purpose = "inspect session",
        .session_start = PICO_SESSION_NEW};
    if (PicoWorkspace_CreateAgent(ws, &child_options, &child_id) != PICO_OK) goto done;
    PicoAgent *child = PicoHost_FindAgent(seed_host, child_id);
    if (!child || PicoSession_LogUser(seed_host, child, "saved child content", "saved child content", NULL) != PICO_SESSION_WRITE_OK ||
        !DrainSessionForAssertion(seed_host, child)) goto done;
    snprintf(session_id, sizeof(session_id), "%s", child->session_id);
    snprintf(inspect_path, sizeof(inspect_path), "%s", child->session_path);
    pico_host_free(seed_host);
    seed_host = NULL;
    if (pico_host_init(&viewer, NULL, true) != PICO_OK ||
        pico_workspace_open(viewer, dir, &ws_id) != PICO_OK) goto done;
    PicoTraceLine line = {0};
    snprintf(line.child_session_id, sizeof(line.child_session_id), "%s", session_id);
    PicoSubagentInspect info = {0};
#ifdef PICO_TEST_IO
    PicoTestIo_Hold(inspect_path);
#endif
    bool immediate = PicoWorkspace_InspectSubagent(viewer, &line, &info);
    if (immediate) goto done;
#ifdef PICO_TEST_IO
    if (PicoTestIo_OnOwner()) { Fail("saved inspection must not read sessions on the UI thread"); goto done; }
    PicoTestIo_WaitEntered();
    pico_host_pump(viewer);
    if (PicoWorkspace_InspectSubagent(viewer, &line, &info)) goto done;
    PicoTestIo_Release();
#endif
    ws = PicoHost_FindWorkspace(viewer, ws_id);
    PICO_TEST_WAIT(ws->inspect_loading_id[0])
    {
        pico_host_pump(viewer);
    }
    if (ws->inspect_loading_id[0] ||
        !PicoWorkspace_InspectSubagent(viewer, &line, &info) ||
        info.message_count != 1 || !info.messages ||
        strcmp(info.messages[0].source, "saved child content") != 0)
    { Fail("saved subagent inspection must publish complete worker-loaded transcript"); goto done; }
    result = 0;
done:
#ifdef PICO_TEST_IO
    PicoTestIo_Release();
#endif
    if (seed_host) pico_host_free(seed_host);
    if (viewer) pico_host_free(viewer);
    unsetenv("XDG_CONFIG_HOME");
    RmRf(cfg); RmRf(dir);
    if (result && !g_failed) Fail("async inspection setup failed");
    return result;
}

static int TestResumeCompletionDoesNotScanOnUi(void)
{
    char dir[] = "/tmp/pico-resume-complete-XXXXXX";
    char cfg[] = "/tmp/pico-resume-complete-cfg-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId ws_id = 0;
    PicoAgentId id = 0;
    char session_id[40] = {0};
    int result = 1;
    if (!mkdtemp(dir) || !mkdtemp(cfg)) return 1;
    setenv("XDG_CONFIG_HOME", cfg, 1);
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN,
                                      .session_start = PICO_SESSION_NEW, .select = true};
    if (pico_host_init(&host, NULL, true) != PICO_OK ||
        pico_workspace_open(host, dir, &ws_id) != PICO_OK ||
        pico_main_agent_create(host, ws_id, &options, &id) != PICO_OK) goto done;
    PicoAgent *agent = PicoHost_FindAgent(host, id);
    if (!agent || PicoSession_LogUser(host, agent, "completion seed", "completion seed", NULL) != PICO_SESSION_WRITE_OK ||
        !DrainSessionForAssertion(host, agent)) goto done;
    snprintf(session_id, sizeof(session_id), "%s", agent->session_id);
    const PicoCompleter *command = NULL;
    for (int i = 0; i < host->completer_count; i++)
        if (host->completers[i].trigger == '/' && host->completers[i].host_query)
            command = &host->completers[i];
    if (!command) goto done;
    PicoCompleteItem items[PICO_MAX_COMPLETE_ITEMS];
#ifdef PICO_TEST_IO
    char database[4096];
    snprintf(database, sizeof(database), "%s/pico/sessions/.catalog.sqlite3", cfg);
    PicoTestIo_Hold(database);
#endif
    int count = command->host_query(host, "resume ", items, PICO_MAX_COMPLETE_ITEMS, command->state);
    if (count != 0) goto done;
#ifdef PICO_TEST_IO
    if (PicoTestIo_OnOwner()) { Fail("resume completion must not open its catalog on the UI thread"); goto done; }
    PicoTestIo_WaitEntered();
    pico_host_pump(host);
    if (command->host_query(host, "resume ", items, PICO_MAX_COMPLETE_ITEMS, command->state) != 0) goto done;
    PicoTestIo_Release();
#endif
    PICO_TEST_WAIT(!count)
    {
        pico_host_pump(host);
        count = command->host_query(host, "resume ", items, PICO_MAX_COMPLETE_ITEMS, command->state);
    }
    bool found = false;
    for (int i = 0; i < count; i++)
        if (strstr(items[i].insert, session_id)) found = true;
    if (!found)
    { Fail("resume completion must publish worker-listed parent sessions"); goto done; }
    result = 0;
done:
#ifdef PICO_TEST_IO
    PicoTestIo_Release();
#endif
    if (host) pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    RmRf(cfg); RmRf(dir);
    if (result && !g_failed) Fail("async resume completion setup failed");
    return result;
}

static int TestResumeLoadsStoredModel(void)
{
    char dir[] = "/tmp/pico-ws-model-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-model-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId ws = 0;
    PicoWorkspace *workspace;
    PicoAgentCreateOptions opt;
    PicoAgentId first = 0;
    PicoAgentId resumed = 0;
    PicoAgent *agent;
    PicoAgentInfo info;
    PicoModel models[2];
    char session_id[40];

    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("mkdtemp resume model");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || pico_workspace_open(host, dir, &ws) != PICO_OK)
    {
        Fail("init resume model");
        unsetenv("XDG_CONFIG_HOME");
        if (host)
        {
            pico_host_free(host);
        }
        return 1;
    }
    workspace = PicoHost_FindWorkspace(host, ws);
    memset(models, 0, sizeof(models));
    snprintf(models[0].id, sizeof(models[0].id), "default-model");
    snprintf(models[0].name, sizeof(models[0].name), "default-model");
    snprintf(models[1].id, sizeof(models[1].id), "changed-model");
    snprintf(models[1].name, sizeof(models[1].name), "changed-model");
    snprintf(models[1].effort[0], sizeof(models[1].effort[0]), "high");
    models[1].effort_count = 1;
    workspace->models = models;
    workspace->model_count = 2;
    snprintf(workspace->settings.default_model, sizeof(workspace->settings.default_model), "default-model");

    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NEW;
    opt.select = true;
    if (pico_main_agent_create(host, ws, &opt, &first) != PICO_OK)
    {
        Fail("create agent for model resume");
        workspace->models = NULL;
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    agent = PicoHost_FindAgent(host, first);
    if (!agent || strcmp(agent->model, "default-model") != 0)
    {
        Fail("new main agent must use the workspace default model");
        workspace->models = NULL;
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    if (!PicoSettings_SetModel(agent, "changed-model") || strcmp(agent->model, "changed-model") != 0 ||
        !agent->session_id[0])
    {
        Fail("SetModel must persist a session id for resume");
        workspace->models = NULL;
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    snprintf(session_id, sizeof(session_id), "%s", agent->session_id);
    if (pico_agent_close(host, first) != PICO_OK)
    {
        Fail("close agent before resume");
        workspace->models = NULL;
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_RESUME;
    opt.session_id = session_id;
    opt.select = true;
    if (pico_main_agent_create(host, ws, &opt, &resumed) != PICO_OK ||
        !pico_agent_find(host, resumed, &info) || strcmp(info.model, "changed-model") != 0)
    {
        Fail("resume must load the stored model from jsonl");
        workspace->models = NULL;
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    workspace->models = NULL;
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(dir);
    return 0;
}

static int TestSelectClearsUnseenComplete(void)
{
    char dir[] = "/tmp/pico-ws-unseen-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-unseen-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId ws = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId first = 0;
    PicoAgentId second = 0;
    PicoAgent *background;

    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("mkdtemp unseen complete");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || pico_workspace_open(host, dir, &ws) != PICO_OK)
    {
        Fail("init unseen complete");
        unsetenv("XDG_CONFIG_HOME");
        if (host)
        {
            pico_host_free(host);
        }
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, ws, &opt, &first) != PICO_OK || first == 0)
    {
        Fail("create first agent");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    opt.select = false;
    if (pico_main_agent_create(host, ws, &opt, &second) != PICO_OK || second == 0 || second == first)
    {
        Fail("create background agent");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    background = PicoHost_FindAgent(host, second);
    if (!background || pico_agent_active(host) != first)
    {
        Fail("background agent must remain unselected");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    background->unseen_complete = true;
    if (!pico_agent_select(host, second) || pico_agent_active(host) != second ||
        background->unseen_complete)
    {
        Fail("selecting a session must clear unseen completion");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(dir);
    return 0;
}

/* Restart checks run in the same process. Finish their asynchronous writes
 * outside the production shutdown budget: slow build storage must not turn a
 * persistence assertion into a terminal, retained-host shutdown. */
static bool ShutdownAfterSessionPersist(PicoHost *host, PicoAgent *agent)
{
    bool session_drained = DrainSessionForAssertion(host, agent);
    bool catalog_drained = PicoCatalog_DrainOrderPersistBefore(host, NULL);
    PicoHostShutdownResult shutdown = pico_host_free(host);
    if (!session_drained || !catalog_drained)
    {
        Fail("restart test persistence must drain before shutdown");
        return false;
    }
    if (shutdown != PICO_HOST_SHUTDOWN_CLEAN)
    {
        Fail("restart test shutdown must complete cleanly");
        return false;
    }
    return true;
}

static int TestUnseenCompletePersistsAcrossRestart(void)
{
    char dir[] = "/tmp/pico-ws-done-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-done-XXXXXX";
    PicoHost *host = NULL;
    PicoWorkspaceId ws = 0;
    PicoAgentCreateOptions opt;
    PicoAgentId first = 0;
    PicoAgentId background = 0;
    PicoAgentId resumed = 0;
    PicoAgent *agent;
    PicoCatalogWorkspace *catalog = NULL;
    int catalog_n = 0;
    const PicoCatalogWorkspace *found;
    char session_id[40];
    bool catalog_done = false;
    int i;

    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("mkdtemp persist done");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || pico_workspace_open(host, dir, &ws) != PICO_OK)
    {
        Fail("init persist done");
        unsetenv("XDG_CONFIG_HOME");
        if (host)
        {
            pico_host_free(host);
        }
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NEW;
    opt.select = true;
    if (pico_main_agent_create(host, ws, &opt, &first) != PICO_OK || first == 0)
    {
        Fail("create selected agent persist done");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    opt.select = false;
    if (pico_main_agent_create(host, ws, &opt, &background) != PICO_OK || background == 0)
    {
        Fail("create background agent persist done");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    agent = PicoHost_FindAgent(host, background);
    if (!agent || PicoSession_LogUser(host, agent, "hello", "hello", NULL) != PICO_SESSION_WRITE_OK)
    {
        Fail("background persist write");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    snprintf(session_id, sizeof(session_id), "%s", agent->session_id);
    PicoSession_SetUnseenComplete(host, agent, true);
    if (!ShutdownAfterSessionPersist(host, agent))
    {
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        return 1;
    }
    host = NULL;

    catalog_n = PicoCatalog_Scan(&catalog);
    found = NULL;
    for (i = 0; i < catalog_n; i++)
    {
        if (strcmp(catalog[i].path, dir) == 0)
        {
            found = &catalog[i];
            break;
        }
    }
    if (found)
    {
        for (i = 0; i < found->session_count; i++)
        {
            if (strcmp(found->sessions[i].id, session_id) == 0)
            {
                catalog_done = found->sessions[i].unseen_complete;
                break;
            }
        }
    }
    PicoCatalog_Free(catalog, catalog_n);
    if (!catalog_done)
    {
        Fail("catalog must show unseen complete after restart");
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        return 1;
    }

    if (pico_host_init(&host, NULL, true) != PICO_OK || pico_workspace_open(host, dir, &ws) != PICO_OK)
    {
        Fail("reinit persist done");
        unsetenv("XDG_CONFIG_HOME");
        if (host)
        {
            pico_host_free(host);
        }
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NEW;
    opt.select = true;
    if (pico_main_agent_create(host, ws, &opt, &first) != PICO_OK || first == 0)
    {
        Fail("placeholder selected agent persist done");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_RESUME;
    opt.session_id = session_id;
    opt.select = false;
    if (pico_main_agent_create(host, ws, &opt, &resumed) != PICO_OK)
    {
        Fail("resume unseen complete");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    agent = PicoHost_FindAgent(host, resumed);
    if (!agent || !agent->unseen_complete)
    {
        Fail("resume must restore unseen complete");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    if (!pico_agent_select(host, resumed) || agent->unseen_complete)
    {
        Fail("selecting a resumed session must clear unseen complete");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        return 1;
    }
    if (!ShutdownAfterSessionPersist(host, agent))
    {
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        return 1;
    }
    host = NULL;
    catalog = NULL;
    catalog_done = false;
    catalog_n = PicoCatalog_Scan(&catalog);
    found = NULL;
    for (i = 0; i < catalog_n; i++)
    {
        if (strcmp(catalog[i].path, dir) == 0)
        {
            found = &catalog[i];
            break;
        }
    }
    if (found)
    {
        for (i = 0; i < found->session_count; i++)
        {
            if (strcmp(found->sessions[i].id, session_id) == 0)
            {
                catalog_done = found->sessions[i].unseen_complete;
                break;
            }
        }
    }
    PicoCatalog_Free(catalog, catalog_n);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(dir);
    if (catalog_done)
    {
        Fail("cleared unseen complete must not remain after select");
        return 1;
    }
    return 0;
}

static bool WorkspaceLessToastRenders(PicoHost *host)
{
    const Clay_Dimensions viewport = {1100, 800};
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    bool rendered = false;
    if (!memory)
    {
        return false;
    }
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
        PicoOverlay_Notify(host, "Workspace-less toast");
        Clay_BeginLayout();
        CLAY(CLAY_ID("ToastTestRoot"),
             {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(viewport.width),
                                    .height = CLAY_SIZING_FIXED(viewport.height)}}})
        {
        }
        PicoOverlay_Render(host, NULL);
        (void)Clay_EndLayout(0.0f);
        rendered = Clay_GetElementData(CLAY_ID("NotifyToast")).found;
    }
    Clay_SetCurrentContext(previous);
    free(memory);
    return rendered;
}

static int FirstSidebarSessionRowId(void)
{
    int i;
    for (i = 0; i < 512; i++)
    {
        if (Clay_GetElementData(CLAY_IDI("SidebarSess", i)).found)
        {
            return i;
        }
    }
    return -1;
}

static bool SidebarSessionDotVisible(Clay_RenderCommandArray *commands, int row_id,
                                     Clay_BoundingBox *box)
{
    Clay_ElementId id = CLAY_IDI("SidebarSessDot", row_id);
    int i;
    for (i = 0; i < commands->length; i++)
    {
        Clay_RenderCommand *command = Clay_RenderCommandArray_Get(commands, i);
        if (!command || command->id != id.id ||
            command->commandType != CLAY_RENDER_COMMAND_TYPE_RECTANGLE ||
            command->renderData.rectangle.backgroundColor.a <= 0.0f)
        {
            continue;
        }
        if (box)
        {
            *box = command->boundingBox;
        }
        return true;
    }
    return false;
}

static bool SidebarTextValidUtf8(const Clay_RenderCommand *text)
{
    if (!text) return false;
    Clay_StringSlice value = text->renderData.text.stringContents;
    for (int pos = 0; pos < value.length; )
    {
        utf8proc_int32_t cp;
        utf8proc_ssize_t step = utf8proc_iterate((const utf8proc_uint8_t *)value.chars + pos,
                                                 value.length - pos, &cp);
        if (step <= 0) return false;
        pos += (int)step;
    }
    return true;
}

static bool SidebarPrefixEndsAtGrapheme(const Clay_RenderCommand *text, const char *source)
{
    Clay_StringSlice value = text->renderData.text.stringContents;
    int prefix = value.length - 3;
    int pos = 0;
    utf8proc_int32_t previous = 0, current, state = 0;
    if (prefix <= 0) return false;
    for (; source[pos]; )
    {
        utf8proc_ssize_t step = utf8proc_iterate((const utf8proc_uint8_t *)source + pos,
                                                 strlen(source + pos), &current);
        if (step <= 0) return false;
        if (pos == prefix) return utf8proc_grapheme_break_stateful(previous, current, &state);
        if (pos > 0) (void)utf8proc_grapheme_break_stateful(previous, current, &state);
        pos += (int)step;
        previous = current;
    }
    return pos == prefix;
}

/* Sidebar labels shorten at the rendered row's width, including the hover
 * action, without changing their catalog title or painting into adjacent UI. */
static int TestSidebarDisplayTitlesFitRows(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    const char *workspace_title = "Workspace-éééééééééééééééééééééééééé";
    const char *session_title = "Session-åäö-abcdefghijklmnopqrstuvwxyz-0123456789-very-long-title";
    char dir[] = "/tmp/pico-sidebar-title-ws-XXXXXX";
    char cfg[] = "/tmp/pico-sidebar-title-cfg-XXXXXX";
    float old_scale = Pico_FontScale();
    void *memory = NULL;
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions opt = {0};
    Clay_RenderCommandArray commands;
    Clay_RenderCommand *workspace_text, *session_text;
    Clay_ElementData workspace_row, session_row, plus;
    int rc = 1;
    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("sidebar label fixture directories");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK ||
        pico_workspace_open(host, dir, &workspace_id) != PICO_OK ||
        PicoCatalog_Ensure(dir) != 0 ||
        PicoCatalog_SetProjectName(dir, workspace_title) != 0)
    {
        Fail("sidebar label fixture catalog");
        goto done;
    }
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NEW;
    opt.select = true;
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK ||
        PicoSession_LogUser(host, PicoHost_FindAgent(host, agent_id),
                            session_title, session_title, NULL) != PICO_SESSION_WRITE_OK)
    {
        Fail("sidebar label fixture session");
        goto done;
    }
    if (!ShutdownAfterSessionPersist(host, PicoHost_FindAgent(host, agent_id)))
    {
        host = NULL;
        Fail("sidebar label fixture persistence");
        goto done;
    }
    host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK)
    {
        Fail("sidebar label fixture reload");
        goto done;
    }
    WaitPluginLoad(host);
    if (!WaitCatalogScanDone(host, g_catalog_scan_done_calls + 1))
    {
        Fail("sidebar label fixture catalog scan");
        goto done;
    }
    memory = malloc(Clay_MinMemorySize());
    if (!memory || !Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(Clay_MinMemorySize(), memory),
                                    viewport, (Clay_ErrorHandler){0}))
    {
        Fail("sidebar label fixture layout");
        goto done;
    }
    Clay_SetMeasureTextFunction(Pico_MeasureTextUtf8, NULL);
    Clay_SetPointerState((Clay_Vector2){0}, false);
    Clay_SetLayoutDimensions(viewport);
    commands = PicoHost_LayoutShell(host, viewport.height, 0.0f);
    workspace_text = FindTrimmedCardText(&commands, workspace_title);
    session_text = FindTrimmedCardText(&commands, session_title);
    workspace_row = Clay_GetElementData(CLAY_IDI("SidebarWs", 0));
    session_row = Clay_GetElementData(CLAY_IDI("SidebarSess", 0));
    if (!workspace_text || !session_text || !workspace_row.found || !session_row.found ||
        !SidebarTextValidUtf8(workspace_text) || !SidebarTextValidUtf8(session_text) ||
        !SidebarPrefixEndsAtGrapheme(workspace_text, workspace_title) ||
        workspace_text->boundingBox.x + workspace_text->boundingBox.width >
            workspace_row.boundingBox.x + workspace_row.boundingBox.width ||
        session_text->boundingBox.x + session_text->boundingBox.width >
            session_row.boundingBox.x + session_row.boundingBox.width)
    {
        Fail("sidebar long titles must render as bounded UTF-8 prefixes plus ellipses");
        goto done;
    }

    /* Hover adds a sibling '+' button. The shortened workspace label must
     * still fit to its left at a different font scale on the very next pass. */
    Pico_SetFontScale(1.5f);
    Clay_SetPointerState((Clay_Vector2){workspace_row.boundingBox.x + 8.0f,
                                       workspace_row.boundingBox.y + workspace_row.boundingBox.height / 2.0f}, false);
    Clay_SetLayoutDimensions(viewport);
    commands = PicoHost_LayoutShell(host, viewport.height, 0.0f);
    workspace_text = FindTrimmedCardText(&commands, workspace_title);
    session_text = FindTrimmedCardText(&commands, session_title);
    plus = Clay_GetElementData(CLAY_IDI("SidebarPlus", 0));
    session_row = Clay_GetElementData(CLAY_IDI("SidebarSess", 0));
    if (!workspace_text || !session_text || !plus.found || !session_row.found ||
        workspace_text->boundingBox.x + workspace_text->boundingBox.width > plus.boundingBox.x ||
        session_text->boundingBox.x + session_text->boundingBox.width >
            session_row.boundingBox.x + session_row.boundingBox.width)
    {
        Fail("sidebar titles must fit beside hover actions after font-scale changes");
        goto done;
    }
    rc = 0;
done:
    Clay_SetCurrentContext(previous);
    Pico_SetFontScale(old_scale);
    if (host) pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    RmRf(cfg);
    RmRf(dir);
    return rc;
}

/* Idle sidebar sessions show a visible status dot, smaller than attention dots. */
static int TestIdleSidebarSessionDot(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-idle-dot-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-idle-dot-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions opt;
    PicoAgent *agent;
    Clay_Arena arena;
    Clay_RenderCommandArray commands;
    Clay_BoundingBox idle_dot = {0};
    Clay_BoundingBox running_dot = {0};
    int row_id;
    int rc = 1;

    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("idle session dot setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        RmRf(cfg);
        RmRf(dir);
        Fail("idle session dot host init");
        return 1;
    }
    WaitPluginLoad(host);
    if (PicoCatalog_Ensure(dir) != 0)
    {
        Fail("idle session dot catalog workspace");
        goto done_host;
    }
    if (!WaitCatalogScanDone(host, g_catalog_scan_done_calls + 1))
    {
        Fail("idle session dot catalog scan");
        goto done_host;
    }
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK)
    {
        Fail("idle session dot open workspace");
        goto done_host;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id)))
    {
        Fail("idle session dot create agent");
        goto done_host;
    }

    arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        Clay_SetCurrentContext(previous);
        Fail("idle session dot Clay initialization");
        goto done_host;
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);

    agent->state = PICO_AGENT_IDLE;
    Clay_SetLayoutDimensions(viewport);
    commands = PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
    row_id = FirstSidebarSessionRowId();
    if (row_id < 0 || !SidebarSessionDotVisible(&commands, row_id, &idle_dot))
    {
        Fail("idle session row must render a visible status dot");
        goto done;
    }

    agent->state = PICO_AGENT_LLM_WAIT;
    Clay_SetLayoutDimensions(viewport);
    commands = PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
    if (!SidebarSessionDotVisible(&commands, row_id, &running_dot))
    {
        Fail("running session row must render a visible status dot");
        goto done;
    }
    if (idle_dot.width >= running_dot.width || idle_dot.height >= running_dot.height)
    {
        fprintf(stderr, "idle session dot %.3fx%.3f running %.3fx%.3f\n",
                idle_dot.width, idle_dot.height, running_dot.width, running_dot.height);
        Fail("idle session dots must be smaller than attention dots");
        goto done;
    }
    rc = g_failed ? 1 : 0;

done:
    Clay_SetCurrentContext(previous);
done_host:
    pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    RmRf(cfg);
    RmRf(dir);
    return rc;
}

static bool SidebarCommandsContain(Clay_RenderCommandArray *commands, const char *label)
{
    for (int i = 0; i < commands->length; i++)
    {
        Clay_RenderCommand *command = Clay_RenderCommandArray_Get(commands, i);
        if (command && command->commandType == CLAY_RENDER_COMMAND_TYPE_TEXT)
        {
            Clay_StringSlice text = command->renderData.text.stringContents;
            if (text.length == (int)strlen(label) && !memcmp(text.chars, label, (size_t)text.length))
                return true;
        }
    }
    return false;
}

static int TestSidebarSnapshotBeforeReconcile(bool fail_reconcile)
{
    char dir[] = "/tmp/pico-sidebar-snapshot-ws-XXXXXX";
    char cfg[] = "/tmp/pico-sidebar-snapshot-cfg-XXXXXX";
    char path[4096], id[40], broken[4096];
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    Clay_Context *previous = Clay_GetCurrentContext();
    const Clay_Dimensions viewport = {1100, 800};
    void *memory = NULL;
    int ready[2] = {-1, -1}, proceed[2] = {-1, -1};
    int result = 1;
    bool released = false;
    const char *phase = "fixture";
    if (!mkdtemp(dir) || !mkdtemp(cfg)) goto done;
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK ||
        pico_workspace_open(host, dir, &workspace_id) != PICO_OK) goto done;
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN,
        .session_start = PICO_SESSION_NEW, .select = true};
    if (pico_main_agent_create(host, workspace_id, &options, &agent_id) != PICO_OK) goto done;
    PicoAgent *agent = PicoHost_FindAgent(host, agent_id);
    if (PicoSession_LogUser(host, agent, "Cached row", "Cached row", NULL) != PICO_SESSION_WRITE_OK)
        goto done;
    snprintf(path, sizeof(path), "%s", agent->session_path);
    snprintf(id, sizeof(id), "%s", agent->session_id);
    /* Drain writes without pumping sidebar maintenance, leaving last_reconcile unset. */
    bool shutdown_ok = ShutdownAfterSessionPersist(host, agent);
    host = NULL;
    if (!shutdown_ok || pico_host_init(&host, NULL, true) != PICO_OK) goto done;
    WaitPluginLoad(host);
    if (pipe(ready) != 0 || pipe(proceed) != 0) goto done;
    g_catalog_ready_fd = ready[1];
    g_catalog_continue_fd = proceed[0];
    phase = "initial snapshot";
    int scans_before = g_catalog_scan_calls;
    PICO_TEST_WAIT(g_catalog_scan_calls == scans_before) pico_host_pump(host);
    if (!TransferTestByte(ready[0], false)) goto done;
    memory = malloc(Clay_MinMemorySize());
    if (!memory || !Clay_Initialize(Clay_CreateArenaWithCapacityAndMemory(Clay_MinMemorySize(), memory),
                                   viewport, (Clay_ErrorHandler){0})) goto done;
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    Clay_SetPointerState((Clay_Vector2){0}, false);
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, viewport.height, 0);
    if (!SidebarCommandsContain(&commands, "Cached row")) goto done;
    phase = "external transcript change";
    if (fail_reconcile)
    {
        snprintf(broken, sizeof(broken), "%s", path);
        char *slash = strrchr(broken, '/');
        if (!slash) goto done;
        snprintf(slash + 1, (size_t)(broken + sizeof(broken) - slash - 1), "broken.jsonl");
        if (symlink("missing-transcript", broken) != 0) goto done;
    }
    else
    {
        FILE *file = fopen(path, "wb");
        if (!file) goto done;
        fprintf(file, "{\"type\":\"session\",\"version\":4,\"id\":\"%s\","
                      "\"kind\":\"normal\",\"cwd\":\"%s\",\"title\":\"Updated row\"}\n", id, dir);
        fclose(file);
    }
    if (!TransferTestByte(proceed[1], true)) goto done;
    released = true;
    phase = "completed reconciliation";
    PICO_TEST_WAIT(host->tasks)
    {
        pico_host_pump(host);
    }
    if (host->tasks) goto done;
    commands = PicoHost_LayoutShell(host, viewport.height, 0);
    if (!SidebarCommandsContain(&commands, fail_reconcile ? "Cached row" : "Updated row")) goto done;
    result = 0;
done:
    if (!released && proceed[1] >= 0) (void)TransferTestByte(proceed[1], true);
    if (host) pico_host_free(host);
    g_catalog_ready_fd = g_catalog_continue_fd = -1;
    for (int i = 0; i < 2; i++)
    {
        if (ready[i] >= 0) close(ready[i]);
        if (proceed[i] >= 0) close(proceed[i]);
    }
    Clay_SetCurrentContext(previous);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    RmRf(cfg); RmRf(dir);
    if (result)
    {
        fprintf(stderr, "sidebar snapshot phase: %s (failure=%d)\n", phase, fail_reconcile);
        Fail("sidebar must display its cached snapshot before reconciliation and preserve it on failure");
    }
    return result;
}

static int TestSidebarCatalogChangeToken(void)
{
    char dir[] = "/tmp/pico-sidebar-token-ws-XXXXXX";
    char cfg[] = "/tmp/pico-sidebar-token-cfg-XXXXXX";
    PicoHost *host = NULL;
    int scans_before;
    int done_before;
    int result = 1;

    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("mkdtemp sidebar catalog token");
        goto done;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("init sidebar catalog token host");
        goto done;
    }
    WaitPluginLoad(host);
    scans_before = g_catalog_scan_calls;
    done_before = g_catalog_scan_done_calls;
    if (!WaitCatalogScanDone(host, done_before + 1) ||
        g_catalog_scan_calls != scans_before + 1)
    {
        Fail("sidebar must scan the catalog on its first pump");
        goto done;
    }
    g_sidebar_poll_due = true;
    pico_host_pump(host);
    if (g_catalog_scan_calls != scans_before + 1)
    {
        Fail("unchanged catalog token must skip the periodic full scan");
        goto done;
    }
    if (PicoCatalog_Ensure(dir) != 0)
    {
        Fail("create sidebar catalog token change");
        goto done;
    }
    int snapshots_before = g_catalog_snapshot_done_calls;
    g_sidebar_poll_due = true;
    if (!WaitCatalogSnapshotDone(host, snapshots_before + 1) ||
        g_catalog_scan_calls != scans_before + 1)
    {
        Fail("changed catalog token must refresh from SQLite without a full scan");
        goto done;
    }
    result = 0;

done:
    if (host)
    {
        pico_host_free(host);
    }
    g_sidebar_poll_due = false;
    unsetenv("XDG_CONFIG_HOME");
    RmRf(cfg);
    RmRf(dir);
    return result;
}

static int TestFileCompletionPublishesWorkerSnapshot(void)
{
    char dir[] = "/tmp/pico-files-worker-ws-XXXXXX";
    char cfg[] = "/tmp/pico-files-worker-cfg-XXXXXX";
    char path[4096];
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    int result = 1;
    if (!mkdtemp(dir) || !mkdtemp(cfg) ||
        !PicoPath_Format(path, sizeof(path), "%s/worker-match.txt", dir))
    {
        Fail("async file completion setup");
        return 1;
    }
    FILE *file = fopen(path, "wb");
    if (!file)
    {
        Fail("async file completion fixture");
        goto done;
    }
    fputs("match", file);
    fclose(file);
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK ||
        pico_workspace_open(host, dir, &workspace_id) != PICO_OK)
    {
        Fail("async file completion host");
        goto done;
    }
    WaitPluginLoad(host);
    PicoWorkspace *workspace = PicoHost_FindWorkspace(host, workspace_id);
    if (!workspace || !PicoPlugins_WorkspaceState(workspace, "files"))
    {
        Fail("async file completion registration");
        goto done;
    }
    PicoCompleteItem items[PICO_MAX_COMPLETE_ITEMS];
    if (pico_files_complete(workspace, "worker-match", items, PICO_MAX_COMPLETE_ITEMS, NULL) != -1)
    {
        Fail("file discovery must start without blocking the query");
        goto done;
    }
    bool found = false;
    PICO_TEST_WAIT(!found)
    {
        pico_host_pump(host);
        int n = pico_files_complete(workspace, "worker-match", items, PICO_MAX_COMPLETE_ITEMS, NULL);
        for (int j = 0; j < n; j++)
        {
            if (strcmp(items[j].label, "worker-match.txt") == 0) found = true;
        }
    }
    if (!found)
    {
        Fail("async file completion must publish the finished snapshot");
        goto done;
    }
    result = 0;
done:
    if (host) pico_host_free(host);
    unsetenv("XDG_CONFIG_HOME");
    unlink(path);
    RmRf(cfg);
    RmRf(dir);
    return result;
}

static int TestSidebarCatalogScanDoesNotBlockPump(void)
{
    char dir[] = "/tmp/pico-sidebar-scan-block-ws-XXXXXX";
    char cfg[] = "/tmp/pico-sidebar-scan-block-cfg-XXXXXX";
    int ready[2] = {-1, -1}, proceed[2] = {-1, -1};
    bool released = false;
    PicoHost *host = NULL;
    int result = 1;
    if (!mkdtemp(dir) || !mkdtemp(cfg)) return 1;
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host) goto done;
    WaitPluginLoad(host);
    if (pipe(ready) || pipe(proceed)) goto done;
    int scans_before = g_catalog_scan_calls;
    int target_done = g_catalog_scan_done_calls + 1;
    g_catalog_ready_fd = ready[1];
    g_catalog_continue_fd = proceed[0];
    PICO_TEST_WAIT(g_catalog_scan_calls == scans_before) pico_host_pump(host);
    if (!TransferTestByte(ready[0], false)) goto done;
    /* The catalog worker cannot finish until release. These UI operations must
     * return while that gate is held; CTest catches a synchronous regression. */
    pico_host_pump(host);
    pico_host_request_submit_cancel(host);
    pico_host_pump(host);
    if (!host->submit_cancel) { Fail("UI must accept cancellation while catalog scan is blocked"); goto done; }
    if (!TransferTestByte(proceed[1], true)) goto done;
    released = true;
    if (!WaitCatalogScanDone(host, target_done)) goto done;
    result = 0;
done:
    if (!released && proceed[1] >= 0) (void)TransferTestByte(proceed[1], true);
    if (host) pico_host_free(host);
    g_catalog_ready_fd = g_catalog_continue_fd = -1;
    for (int i = 0; i < 2; i++)
    {
        if (ready[i] >= 0) close(ready[i]);
        if (proceed[i] >= 0) close(proceed[i]);
    }
    unsetenv("XDG_CONFIG_HOME");
    RmRf(cfg); RmRf(dir);
    if (result && !g_failed) Fail("catalog scan gate setup or completion failed");
    return result;
}

static int TestWorkspaceLessHostTransition(void)
{
    char dir[] = "/tmp/pico-ws-empty-start-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-empty-start-XXXXXX";
    PicoHost *host = NULL;
    const PicoAgent *selected;
    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("mkdtemp workspace-less host");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("init workspace-less host");
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        rmdir(cfg);
        return 1;
    }
    WaitPluginLoad(host);
    if (pico_workspace_count(host) != 0 || pico_agent_count(host) != 0 || pico_agent_active(host) != 0 ||
        !PicoPlugins_HostState(host, "sidebar") || !PicoPlugins_HostState(host, "chat"))
    {
        Fail("workspace-less host must load host plugins without creating runtime state");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        return 1;
    }
    if (!WorkspaceLessToastRenders(host))
    {
        Fail("workspace-less host must render notifications without an agent");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        return 1;
    }
    pico_host_pump(host);
    if (pico_workspace_count(host) != 0 || pico_agent_count(host) != 0 || pico_agent_active(host) != 0)
    {
        Fail("workspace-less host pump must tolerate agent id zero");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        return 1;
    }
    if (!WaitHostReload(host) || pico_workspace_count(host) != 0 || pico_agent_count(host) != 0)
    {
        Fail("workspace-less host reload must remain usable");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        return 1;
    }
    if (!PicoHost_ChangeWorkspace(host, NULL, dir))
    {
        Fail("workspace-less host must open its first workspace");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        return 1;
    }
    selected = PicoHost_SelectedAgentConst(host);
    if (pico_workspace_count(host) != 1 || pico_agent_count(host) != 1 || !selected ||
        strcmp(PicoAgent_WorkspacePath(selected), dir) != 0)
    {
        Fail("first workspace must create and select a usable main agent");
        pico_host_free(host);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        return 1;
    }
    if (pico_host_free(host) != PICO_HOST_SHUTDOWN_CLEAN)
    {
        Fail("workspace-less transition must shut down cleanly");
        unsetenv("XDG_CONFIG_HOME");
        rmdir(dir);
        return 1;
    }
    unsetenv("XDG_CONFIG_HOME");
    rmdir(dir);
    return 0;
}

static int TestFooterMainAgentTps(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-footer-tps-XXXXXX";
    char cfg[] = "/tmp/pico-footer-tps-cfg-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId main_id = 0, other_id = 0, child_id = 0;
    ShellTestState state = {.composer_height = 44.0f};
    int rc = 1;
    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg)) goto done;
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host) goto done;
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .select = true,
                                     .session_start = PICO_SESSION_NONE};
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK ||
        pico_main_agent_create(host, workspace_id, &options, &main_id) != PICO_OK) goto done;
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0})) goto done;
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);
    Clay_RenderCommandArray commands = PicoHost_LayoutShell(host, viewport.height, 0.0f);
    if (!FindCardText(&commands, "TPS: --"))
    { Fail("footer must show pending TPS before generation"); goto done; }
    PicoAgent *main_agent = PicoHost_FindAgent(host, main_id);
    main_agent->has_tokens_per_second = true;
    main_agent->tokens_per_second = 42.0;
    commands = PicoHost_LayoutShell(host, viewport.height, 0.0f);
    if (!FindCardText(&commands, "TPS: 42"))
    { Fail("idle footer must show the retained main-agent rate"); goto done; }
    options.select = false;
    if (pico_main_agent_create(host, workspace_id, &options, &other_id) != PICO_OK) goto done;
    PicoAgent *other = PicoHost_FindAgent(host, other_id);
    other->has_tokens_per_second = true;
    other->tokens_per_second = 84.0;
    PicoAgentCreateOptions child_options = {.kind = PICO_AGENT_SUBAGENT, .parent_id = other_id,
        .select = true, .session_start = PICO_SESSION_NONE};
    if (PicoWorkspace_CreateAgent(other->workspace, &child_options, &child_id) != PICO_OK) goto done;
    commands = PicoHost_LayoutShell(host, viewport.height, 0.0f);
    if (!FindCardText(&commands, "TPS: 84"))
    { Fail("subagent footer must show its own main ancestor's TPS, not another main agent"); goto done; }
    rc = 0;
done:
    Clay_SetCurrentContext(previous);
    if (host) pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    return rc;
}

static int TestFooterCacheTooltip(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-cache-tip-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-cache-tip-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions opt;
    PicoAgent *agent = NULL;
    ShellTestState state = {.composer_height = 44.0f};
    Clay_Arena arena;
    Clay_RenderCommandArray commands;
    Clay_ElementData chip;
    int frame;
    int rc = 1;

    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("cache tooltip setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(cfg);
        rmdir(dir);
        Fail("cache tooltip host init");
        return 1;
    }
    WaitPluginLoad(host);
    /* Headless runs have no real fonts; keep chat on the unclamped width path
     * and stub the composer, which measures text with raylib directly. */
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    if (host->view_count[PICO_SLOT_FOOTER] <= 0)
    {
        Fail("cache tooltip requires the builtin footer view");
        goto done_host;
    }
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK)
    {
        Fail("cache tooltip open workspace");
        goto done_host;
    }
    /* Exercise the production footer's additional checkout chip without
     * changing any full-height shell wrapper. */
    PicoWorkspace *footer_workspace = PicoHost_FindWorkspace(host, workspace_id);
    footer_workspace->checkout_root = true;
    snprintf(footer_workspace->project_path, sizeof(footer_workspace->project_path), "%s", dir);
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id)))
    {
        Fail("cache tooltip create agent");
        goto done_host;
    }
    agent->session_input_tokens = 250000;
    agent->session_cached_tokens = 125000;

    arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        Clay_SetCurrentContext(previous);
        Fail("cache tooltip Clay initialization");
        goto done_host;
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);

    Clay_SetLayoutDimensions(viewport);
    Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0.0f);
    commands = PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);

    chip = Clay_GetElementData(CLAY_ID("FooterCache"));
    Clay_ElementData worktree_chip = Clay_GetElementData(CLAY_ID("FooterWorktree"));
    if (!chip.found || !worktree_chip.found || !FindCardText(&commands, "50% cache") ||
        !FindCardText(&commands, "local"))
    {
        Fail("footer must render cache rate and local checkout");
        goto done;
    }
    if (Clay_GetElementData(CLAY_ID("FooterCacheTip")).found)
    {
        Fail("cache tooltip must stay hidden without hover");
        goto done;
    }

    Clay_SetPointerState((Clay_Vector2){chip.boundingBox.x + chip.boundingBox.width / 2.0f,
                                        chip.boundingBox.y + chip.boundingBox.height / 2.0f},
                         false);
    for (frame = 0; frame < 3; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0.0f);
        commands = PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
    }
    if (!Clay_GetElementData(CLAY_ID("FooterCacheTip")).found)
    {
        Fail("hovering the cache rate must show the tooltip");
        goto done;
    }
    if (!FindCardText(&commands, "Total input: 250k") || !FindCardText(&commands, "Cached input: 125k"))
    {
        Fail("cache tooltip must show session totals formatted like the footer");
        goto done;
    }

    Clay_SetPointerState((Clay_Vector2){0, 0}, false);
    for (frame = 0; frame < 3; frame++)
    {
        Clay_SetLayoutDimensions(viewport);
        Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0.0f);
        commands = PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
    }
    if (Clay_GetElementData(CLAY_ID("FooterCacheTip")).found)
    {
        Fail("cache tooltip must hide once the pointer leaves");
        goto done;
    }

    rc = g_failed ? 1 : 0;

done:
    Clay_SetCurrentContext(previous);
done_host:
    if (host)
    {
        pico_host_free(host);
    }
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    return rc;
}

#ifdef PICO_CLAY_FRAME_FAULT_TESTS
static void WorktreeNameLayout(PicoHost *host, Clay_Dimensions viewport)
{
    Clay_SetLayoutDimensions(viewport);
    Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0.0f);
    (void)PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
}

static void WorktreeNamePump(PicoHost *host, int key, bool ctrl, int character)
{
    g_find_key = key;
    g_find_ctrl = ctrl;
    g_find_character = character;
    pico_host_pump(host);
    g_find_key = 0;
    g_find_ctrl = false;
    g_find_character = 0;
}

/* The worktree name field is an editing target: hovering it must request the
 * I-beam (hovered_text), and Ctrl+Backspace must delete the previous word the
 * same way composer and ask_user text fields do. */
static int TestWorktreeNameField(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-worktree-name-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-worktree-name-XXXXXX";
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions opt;
    PicoAgent *agent = NULL;
    ShellTestState state = {.composer_height = 44.0f};
    Clay_Arena arena;
    Clay_RenderCommandArray commands;
    Clay_ElementData chip;
    Clay_ElementData field;
    Clay_Vector2 pointer;
    PicoWorkspace *workspace;
    const char *typed = "alpha beta";
    int i;
    int rc = 1;

    if (!memory || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(memory);
        Fail("worktree name field setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        free(memory);
        unsetenv("XDG_CONFIG_HOME");
        rmdir(cfg);
        rmdir(dir);
        Fail("worktree name field host init");
        return 1;
    }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    if (host->view_count[PICO_SLOT_FOOTER] <= 0)
    {
        Fail("worktree name field requires the builtin footer view");
        goto done_host;
    }
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK)
    {
        Fail("worktree name field open workspace");
        goto done_host;
    }
    workspace = PicoHost_FindWorkspace(host, workspace_id);
    workspace->checkout_root = true;
    workspace->can_create_worktree = true;
    snprintf(workspace->project_path, sizeof(workspace->project_path), "%s", dir);
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id)))
    {
        Fail("worktree name field create agent");
        goto done_host;
    }
    (void)agent;

    arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        Clay_SetCurrentContext(previous);
        Fail("worktree name field Clay initialization");
        goto done_host;
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);

    WorktreeNameLayout(host, viewport);
    chip = Clay_GetElementData(CLAY_ID("FooterWorktree"));
    if (!chip.found)
    {
        Fail("worktree name field needs the checkout chip");
        goto done;
    }

    g_find_input_test = true;
    pointer = (Clay_Vector2){chip.boundingBox.x + chip.boundingBox.width / 2.0f,
                             chip.boundingBox.y + chip.boundingBox.height / 2.0f};
    g_find_pointer = (Vector2){pointer.x, pointer.y};
    g_find_press = true;
    Clay_SetPointerState(pointer, false);
    pico_run_hooks(host, PICO_HOOK_AFTER_LAYOUT, 0);
    g_find_press = false;

    WorktreeNameLayout(host, viewport);
    field = Clay_GetElementData(CLAY_ID("WorktreeName"));
    if (!field.found)
    {
        Fail("clicking checkout must open the worktree name field");
        goto done;
    }

    host->hovered_text = false;
    host->hovered_clickable = false;
    pointer = (Clay_Vector2){field.boundingBox.x + field.boundingBox.width / 2.0f,
                             field.boundingBox.y + field.boundingBox.height / 2.0f};
    g_find_pointer = (Vector2){pointer.x, pointer.y};
    Clay_SetPointerState(pointer, false);
    pico_run_hooks(host, PICO_HOOK_AFTER_LAYOUT, 0);
    if (!host->hovered_text || host->hovered_clickable)
    {
        Fail("hovering the worktree name field must request the I-beam only");
        goto done;
    }

    for (i = 0; i < 128; i++)
        WorktreeNamePump(host, KEY_BACKSPACE, false, 0);
    for (i = 0; typed[i]; i++)
        WorktreeNamePump(host, 0, false, typed[i]);
    WorktreeNamePump(host, KEY_BACKSPACE, true, 0);
    WorktreeNameLayout(host, viewport);
    commands = PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f);
    if (!FindCardText(&commands, "alpha "))
    {
        Fail("Ctrl+Backspace must delete the previous word in the worktree name field");
        goto done;
    }

    rc = g_failed ? 1 : 0;

done:
    g_find_input_test = false;
    g_find_press = false;
    g_find_key = 0;
    g_find_ctrl = false;
    g_find_character = 0;
    Clay_SetCurrentContext(previous);
done_host:
    g_find_input_test = false;
    if (host)
        pico_host_free(host);
    free(memory);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    return rc;
}
#endif

/* Drive the real slash command from the same callback phase as composer input.
 * No graphics context is needed: an exiting frame must never reach drawing. */
static void SubmitQuitOnFrame(PicoHost *host, void *state, float dt)
{
    (void)state;
    (void)dt;
    PicoComposer_SetText(host, "/quit");
    PicoHost_Submit(host);
}

static void UnexpectedFrameAfterQuit(PicoHost *host, void *state, float dt)
{
    (void)host;
    (void)state;
    (void)dt;
    Fail("/quit must stop subsequent host frame callbacks");
}

static int TestQuitDefersTeardownUntilFrameReturns(void)
{
    char dir[] = "/tmp/pico-quit-frame-XXXXXX";
    PicoHost *host = NULL;
    if (!mkdtemp(dir))
    {
        Fail("mkdtemp quit frame");
        return 1;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        rmdir(dir);
        Fail("host init quit frame");
        return 1;
    }
    PicoHost_Start(host, NULL, dir, true, PICO_SESSION_NONE, NULL);
    WaitPluginLoad(host);
    if (!PicoHost_SelectedAgent(host) || host->host_plugin_count + 2 > (int)(sizeof(host->host_plugins) / sizeof(host->host_plugins[0])))
    {
        pico_host_free(host);
        rmdir(dir);
        Fail("prepare quit frame");
        return 1;
    }
    PicoModuleGeneration quit = {.ext = {.host_on_frame = SubmitQuitOnFrame}};
    PicoModuleGeneration after = {.ext = {.host_on_frame = UnexpectedFrameAfterQuit}};
    int slots = host->host_plugin_count;
    host->host_plugins[host->host_plugin_count++] = (PicoPluginSlot){
        .module = &quit, .initialized = true,
    };
    host->host_plugins[host->host_plugin_count++] = (PicoPluginSlot){
        .module = &after, .initialized = true,
    };
    int presented = g_presented_frames;
    PicoHost_Frame(host);
    bool exited_without_present = PicoHost_ShouldExit(host) && g_presented_frames == presented;
    /* Synthetic callback slots are stack-owned, not loader-owned. */
    memset(&host->host_plugins[slots], 0, 2 * sizeof(host->host_plugins[0]));
    host->host_plugin_count = slots;
    PicoHostShutdownResult shutdown = pico_host_free(host);
    rmdir(dir);
    if (!exited_without_present || shutdown != PICO_HOST_SHUTDOWN_CLEAN)
    {
        Fail("/quit must return from the frame without presenting and allow clean host teardown");
        return 1;
    }
    return g_failed ? 1 : 0;
}

#ifdef PICO_CLAY_FRAME_FAULT_TESTS
static int g_redraw_frame_callbacks;
static int g_redraw_render_callbacks;
static int g_redraw_layout_callbacks;

static void RequestRedrawFromFrame(PicoHost *host, void *state, float dt)
{
    (void)state;
    (void)dt;
    if (++g_redraw_frame_callbacks == 3) pico_host_request_redraw(host);
}

static void CountLayouts(PicoHost *host, const PicoHookEvent *event, void *state)
{
    (void)host;
    (void)event;
    (void)state;
    g_redraw_layout_callbacks++;
}

static void RequestRedrawFromRender(PicoHost *host, const PicoHookEvent *event, void *state)
{
    (void)event;
    (void)state;
    if (++g_redraw_render_callbacks == 1) pico_host_request_redraw(host);
}

/* Pump through completion/adoption, including follow-up tasks queued by host
 * callbacks. A worker milestone does not mean its redraw has been consumed. */
static bool WaitHostTasks(PicoHost *host)
{
    PICO_TEST_WAIT_LOOP("asynchronous completion")
    {
        pico_host_pump(host);
        if (!host->tasks) return true;
    }
    return false;
}

/* Several unchanged host pumps must not expire the scroll container belonging
 * to the last laid-out shell. A fresh layout after idle needs its dimensions
 * to mount even a clean, short transcript; a long transcript needs its offset. */
static int TestIdleFrameRetainsChatScroll(void)
{
    char dir[] = "/tmp/pico-idle-scroll-ws-XXXXXX";
    char cfg[] = "/tmp/pico-idle-scroll-cfg-XXXXXX";
    PicoHost *host = NULL;
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgent *agent = NULL;
    ShellTestState shell = {.composer_height = 44.0f};
    bool good = false;
    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("idle chat test workspace setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host ||
        !Pico_InitClay((Clay_Dimensions){1100, 800}))
        goto done;
    WaitPluginLoad(host);
    /* The frame draws through test wrappers, not a graphics context. */
    host->hook_count = 0;
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);
    host->preferences.chat_width = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &shell);
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK ||
        pico_main_agent_create(host, workspace_id,
            &(PicoAgentCreateOptions){.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE,
                                      .select = true}, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id)))
        goto done;
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "A short visible reply.");
    /* Finish sidebar catalog work before testing idle frames or entering the
     * bounded shutdown path. A fixed sleep races slow/loaded build hosts. */
    if (!WaitHostTasks(host))
    {
        Fail("idle chat test background setup must finish");
        goto done;
    }
    g_clay_frame_test = g_find_input_test = true;
    g_find_key = -1;
    PicoHost_Frame(host);
    int idle_frames = 0;
    for (int i = 0; i < 4; i++)
    {
        PicoHost_Frame(host);
        if (!host->frame_presented) idle_frames++;
    }
    if (idle_frames != 4) goto done;
    pico_host_request_redraw(host);
    PicoHost_Frame(host);
    Clay_ElementData reply = Clay_GetElementData(CLAY_IDI("MsgMain", 0));
    Clay_ScrollContainerData scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    if (!reply.found || !scroll.found || !scroll.scrollPosition)
        goto done;
    for (int i = 0; i < 40; i++)
        PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT,
                             "A longer transcript must remain scrollable after idle.");
    pico_host_request_redraw(host);
    for (int i = 0; i < 5; i++) PicoHost_Frame(host);
    scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    if (!scroll.found || !scroll.scrollPosition ||
        scroll.contentDimensions.height <= scroll.scrollContainerDimensions.height + 100.0f)
        goto done;
    host->chat_follow_bottom = false;
    scroll.scrollPosition->y = -60.0f;
    Clay_ElementData chat = Clay_GetElementData(CLAY_ID("ChatScroll"));
    if (!chat.found) goto done;
    g_find_pointer = (Vector2){chat.boundingBox.x + chat.boundingBox.width / 2.0f,
                               chat.boundingBox.y + chat.boundingBox.height / 2.0f};
    pico_host_request_redraw(host);
    PicoHost_Frame(host);
    idle_frames = 0;
    for (int i = 0; i < 4; i++)
    {
        PicoHost_Frame(host);
        if (!host->frame_presented) idle_frames++;
    }
    scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    if (idle_frames != 4 || !scroll.found || !scroll.scrollPosition ||
        scroll.scrollPosition->y >= -1.0f)
        goto done;
    float before_wheel = scroll.scrollPosition->y;
    g_find_wheel.y = -1.0f;
    PicoHost_Frame(host);
    g_find_wheel = (Vector2){0};
    scroll = Clay_GetScrollContainerData(CLAY_ID("ChatScroll"));
    good = host->frame_presented && scroll.found && scroll.scrollPosition &&
           scroll.scrollPosition->y < before_wheel;
    /* A real layout removal, unlike an idle pump, must discard its scroller. */
    host->view_count[PICO_SLOT_MAIN] = 0;
    pico_host_request_redraw(host);
    PicoHost_Frame(host);
    PicoHost_Frame(host);
    good = good && !Clay_GetScrollContainerData(CLAY_ID("ChatScroll")).found;

done:
    g_find_wheel = (Vector2){0};
    g_clay_frame_test = g_find_input_test = false;
    g_find_key = 0;
    if (host && !WaitHostTasks(host))
    {
        Fail("idle chat test background tasks must finish before teardown");
        good = false;
    }
    if (host && pico_host_free(host) != PICO_HOST_SHUTDOWN_CLEAN)
    {
        Fail("idle chat test host teardown must be clean");
        good = false;
    }
    Pico_FreeClay();
    Clay_SetCurrentContext(previous);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    if (!good) Fail("idle pumps must retain short chat content and long chat scroll position");
    return good ? 0 : 1;
}

static int SidebarVisibleSessionRows(void)
{
    int count = 0;
    for (int id = 0; id < 128; id++)
        if (Clay_GetElementData(CLAY_IDI("SidebarSess", id)).found) count++;
    return count;
}

static bool SidebarFrameClickButton(PicoHost *host, Clay_ElementData element, int button)
{
    if (!host || !element.found) return false;
    g_find_button = button;
    g_find_pointer = (Vector2){element.boundingBox.x + element.boundingBox.width / 2.0f,
                               element.boundingBox.y + element.boundingBox.height / 2.0f};
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
    g_find_press = true;
    PicoHost_Frame(host);
    g_find_press = false;
    g_find_button = MOUSE_BUTTON_LEFT;
    return host->frame_presented;
}

static bool SidebarFrameClick(PicoHost *host, Clay_ElementData element)
{
    return SidebarFrameClickButton(host, element, MOUSE_BUTTON_LEFT);
}

static bool SidebarFrameClickWorkspace(PicoHost *host, Clay_ElementData element)
{
    if (!host || !element.found) return false;
    g_find_pointer = (Vector2){element.boundingBox.x + element.boundingBox.width / 2.0f,
                               element.boundingBox.y + element.boundingBox.height / 2.0f};
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, true);
    g_find_down = g_find_press = true;
    PicoHost_Frame(host);
    g_find_down = g_find_press = false;
    g_find_released = true;
    Clay_SetPointerState((Clay_Vector2){g_find_pointer.x, g_find_pointer.y}, false);
    PicoHost_Frame(host);
    g_find_released = false;
    return host->frame_presented;
}

/* Exercise sidebar changes through actual host input frames. The selected
 * session stays pinned when collapsed; nonselected live sessions fill More. */
static int TestSidebarContextMenus(void)
{
    char dir[] = "/tmp/pico-sidebar-menu-ws-XXXXXX";
    char cfg[] = "/tmp/pico-sidebar-menu-cfg-XXXXXX";
    PicoHost *host = NULL;
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    bool good = false;
    const char *phase = "setup";
    char session_file[4096];
    int rows_before = 0;
    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("sidebar menu fixture directories");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host ||
        !Pico_InitClay((Clay_Dimensions){1100, 800})) goto done;
    WaitPluginLoad(host);
    for (int i = 0; i < host->hook_count; )
    {
        if (host->hooks[i].hook == PICO_HOOK_AFTER_RENDER)
        {
            memmove(&host->hooks[i], &host->hooks[i + 1],
                    (size_t)(host->hook_count - i - 1) * sizeof(host->hooks[0]));
            host->hook_count--;
        }
        else i++;
    }
    host->preferences.chat_width = 0;
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);
    if (PicoCatalog_Ensure(dir) != 0 ||
        pico_workspace_open(host, dir, &workspace_id) != PICO_OK) goto done;
    {
        PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN,
            .session_start = PICO_SESSION_NONE, .select = true};
        if (pico_main_agent_create(host, workspace_id, &options, &agent_id) != PICO_OK)
            goto done;
    }
    PicoCatalogWorkspace *fixture = NULL;
    int fixture_count = PicoCatalog_Scan(&fixture);
    const PicoCatalogWorkspace *fixture_ws = NULL;
    for (int j = 0; j < fixture_count; j++)
        if (strcmp(fixture[j].path, dir) == 0) fixture_ws = &fixture[j];
    if (!fixture_ws) { PicoCatalog_Free(fixture, fixture_count); goto done; }
    bool fixture_ok = PicoPath_Format(session_file, sizeof(session_file),
                                      "%s/pico/sessions/%s/2026-01-01T00-00-00Z_menusess.jsonl",
                                      cfg, fixture_ws->key);
    PicoCatalog_Free(fixture, fixture_count);
    if (!fixture_ok) goto done;
    {
        FILE *f = fopen(session_file, "wb");
        if (!f) goto done;
        fprintf(f, "{\"type\":\"session\",\"version\":4,\"id\":\"menusess\","
                   "\"kind\":\"normal\",\"cwd\":\"%s\"}\n"
                   "{\"type\":\"message\",\"role\":\"user\",\"content\":\"menu-title\"}\n",
                dir);
        fclose(f);
    }
    fixture_count = PicoCatalog_Scan(&fixture);
    PicoCatalog_Free(fixture, fixture_count);
    if (fixture_count < 1) goto done;
    g_sidebar_poll_due = true;
    g_clay_frame_test = g_find_input_test = true;
    g_find_key = -1;
    g_find_button = MOUSE_BUTTON_LEFT;
    phase = "catalog";
    if (!WaitCatalogSnapshotDone(host, g_catalog_snapshot_done_calls + 1)) goto done;
    PicoHost_Frame(host);
    rows_before = SidebarVisibleSessionRows();
    phase = "workspace right-click";
    if (rows_before < 2 ||
        !SidebarFrameClickButton(host, Clay_GetElementData(CLAY_IDI("SidebarWs", 0)), MOUSE_BUTTON_RIGHT) ||
        !Clay_GetElementData(CLAY_ID("SidebarEditPopup")).found ||
        SidebarVisibleSessionRows() != rows_before) goto done;
    phase = "pen still opens editor";
    g_find_key = KEY_ESCAPE;
    PicoHost_Frame(host);
    g_find_key = -1;
    if (Clay_GetElementData(CLAY_ID("SidebarEditPopup")).found) goto done;
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_IDI("SidebarEdit", 0))) ||
        !Clay_GetElementData(CLAY_ID("SidebarEditPopup")).found) goto done;
    g_find_key = KEY_ESCAPE;
    PicoHost_Frame(host);
    g_find_key = -1;
    phase = "new session has no menu";
    if (!SidebarFrameClickButton(host, Clay_GetElementData(CLAY_IDI("SidebarSess", 0)), MOUSE_BUTTON_RIGHT) ||
        Clay_GetElementData(CLAY_ID("SidebarSessionPopup")).found) goto done;
    phase = "session menu";
    if (!SidebarFrameClickButton(host, Clay_GetElementData(CLAY_IDI("SidebarSess", 1)), MOUSE_BUTTON_RIGHT) ||
        !Clay_GetElementData(CLAY_ID("SidebarSessionPopup")).found ||
        !Clay_GetElementData(CLAY_ID("SidebarSessionCopy")).found ||
        !Clay_GetElementData(CLAY_ID("SidebarSessionDelete")).found) goto done;
    phase = "copy click";
    if (!Clay_GetElementData(CLAY_ID("SidebarSessionCopy")).found) goto done;
    if (!SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SidebarSessionCopy")))) goto done;
    phase = "copy clipboard";
    if (!GetClipboardText() || strcmp(GetClipboardText(), "menusess") != 0) goto done;
    phase = "delete confirm";
    if (!SidebarFrameClickButton(host, Clay_GetElementData(CLAY_IDI("SidebarSess", 1)), MOUSE_BUTTON_RIGHT) ||
        !SidebarFrameClick(host, Clay_GetElementData(CLAY_ID("SidebarSessionDelete"))) ||
        !Clay_GetElementData(CLAY_ID("SidebarSessionConfirm")).found ||
        !Clay_GetElementData(CLAY_ID("SidebarSessionCancel")).found) goto done;
    g_find_key = KEY_ESCAPE;
    PicoHost_Frame(host);
    g_find_key = -1;
    phase = "busy session stays";
    {
        PicoAgent *agent = PicoHost_FindAgent(host, agent_id);
        if (!agent) goto done;
        snprintf(agent->session_id, sizeof(agent->session_id), "menusess");
        snprintf(agent->session_path, sizeof(agent->session_path), "%s", session_file);
        agent->persistence = PICO_SESSION_DURABLE;
        agent->state = PICO_AGENT_LLM_WAIT;
        if (PicoCatalog_DeleteSession(host, dir, "menusess") != PICO_BUSY ||
            PicoHost_FindAgent(host, agent_id) == NULL || access(session_file, F_OK) != 0)
            goto done;
        agent->state = PICO_AGENT_IDLE;
        phase = "idle delete closes agent";
        if (PicoCatalog_DeleteSession(host, dir, "menusess") != PICO_OK ||
            PicoHost_FindAgent(host, agent_id) != NULL || access(session_file, F_OK) == 0)
            goto done;
    }
    good = true;
done:
    g_find_input_test = g_find_press = g_find_down = g_find_released = false;
    g_find_button = MOUSE_BUTTON_LEFT;
    g_find_key = 0;
    g_clay_frame_test = false;
    g_sidebar_poll_due = false;
    if (host) pico_host_free(host);
    Pico_FreeClay();
    Clay_SetCurrentContext(previous);
    unsetenv("XDG_CONFIG_HOME");
    RmRf(cfg);
    RmRf(dir);
    if (!good)
    {
        fprintf(stderr, "sidebar context menu phase: %s\n", phase);
        Fail("sidebar right-click opens workspace and session actions");
    }
    return good ? 0 : 1;
}

static int TestSidebarSameFrameControls(void)
{
    char dir[] = "/tmp/pico-sidebar-frame-ws-XXXXXX";
    char cfg[] = "/tmp/pico-sidebar-frame-cfg-XXXXXX";
    PicoHost *host = NULL;
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    bool good = false;
    const char *phase = "setup";
    if (!mkdtemp(dir) || !mkdtemp(cfg))
    {
        Fail("sidebar same-frame fixture directories");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host ||
        !Pico_InitClay((Clay_Dimensions){1100, 800})) goto done;
    WaitPluginLoad(host);
    /* Frame presentation is wrapped, but builtin overlay after-render hooks
     * call graphics scissoring directly. Keep input hooks and omit those. */
    for (int i = 0; i < host->hook_count; )
    {
        if (host->hooks[i].hook == PICO_HOOK_AFTER_RENDER)
        {
            memmove(&host->hooks[i], &host->hooks[i + 1],
                    (size_t)(host->hook_count - i - 1) * sizeof(host->hooks[0]));
            host->hook_count--;
        }
        else i++;
    }
    host->preferences.chat_width = 0;
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    RichText_SetMeasureFunction(ShellMeasureText, NULL);
    if (PicoCatalog_Ensure(dir) != 0 ||
        pico_workspace_open(host, dir, &workspace_id) != PICO_OK) goto done;
    for (int i = 0; i < PICO_MAX_AGENTS; i++)
    {
        PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN,
            .session_start = PICO_SESSION_NONE, .select = i == 0};
        if (pico_main_agent_create(host, workspace_id, &options, &agent_id) != PICO_OK)
            goto done;
    }
    /* Real persisted rows must be loaded on demand beyond the live extras. */
    PicoCatalogWorkspace *fixture = NULL;
    int fixture_count = PicoCatalog_Scan(&fixture);
    const PicoCatalogWorkspace *fixture_ws = NULL;
    for (int j = 0; j < fixture_count; j++)
        if (strcmp(fixture[j].path, dir) == 0) fixture_ws = &fixture[j];
    if (!fixture_ws) { PicoCatalog_Free(fixture, fixture_count); goto done; }
    char session_dir[4096];
    bool fixture_ok = PicoPath_Format(session_dir, sizeof(session_dir),
                                      "%s/pico/sessions/%s", cfg, fixture_ws->key);
    PicoCatalog_Free(fixture, fixture_count);
    if (!fixture_ok) goto done;
    for (int j = 0; j < 12; j++)
    {
        char path[4096];
        if (!PicoPath_Format(path, sizeof(path), "%s/2026-01-01T00-00-%02dZ_paging%02d.jsonl",
                             session_dir, j, j)) goto done;
        FILE *f = fopen(path, "wb");
        if (!f) goto done;
        fprintf(f, "{\"type\":\"session\",\"version\":4,\"id\":\"paging%02d\","
                   "\"kind\":\"normal\",\"cwd\":\"%s\"}\n"
                   "{\"type\":\"message\",\"role\":\"user\","
                   "\"content\":\"paging-title-%02d\"}\n", j, dir, j);
        fclose(f);
    }
    fixture_count = PicoCatalog_Scan(&fixture);
    PicoCatalog_Free(fixture, fixture_count);
    if (fixture_count < 1) goto done;
    g_sidebar_poll_due = true;
    g_clay_frame_test = g_find_input_test = true;
    g_find_key = -1;
    phase = "catalog";
    if (!WaitCatalogSnapshotDone(host, g_catalog_snapshot_done_calls + 1)) goto done;
    PicoHost_Frame(host);

    phase = "initial workspace layout";
    Clay_ElementData workspace = Clay_GetElementData(CLAY_IDI("SidebarWs", 0));
    int expanded_rows = SidebarVisibleSessionRows();
    if (!workspace.found || expanded_rows == 0 ||
        !SidebarFrameClickWorkspace(host, workspace)) goto done;
    phase = "collapse frame";
    int collapsed_rows = SidebarVisibleSessionRows();
    if (collapsed_rows >= expanded_rows ||
        !SidebarFrameClickWorkspace(host, Clay_GetElementData(CLAY_IDI("SidebarWs", 0))) ||
        SidebarVisibleSessionRows() != expanded_rows) goto done;

    phase = "expand frame";
    Clay_ElementData more = Clay_GetElementData(CLAY_IDI("SidebarMore", 0));
    int before_more = SidebarVisibleSessionRows();
    if (!more.found || !SidebarFrameClick(host, more)) goto done;
    int after_more = SidebarVisibleSessionRows();
    if (after_more <= before_more || !Clay_GetElementData(CLAY_IDI("SidebarLess", 0)).found)
        goto done;
    phase = "second persisted page";
    int scan_before = g_catalog_snapshot_done_calls;
    if (!WaitCatalogSnapshotDone(host, scan_before + 1)) goto done;
    PicoHost_Frame(host);
    more = Clay_GetElementData(CLAY_IDI("SidebarMore", 0));
    if (!more.found || !SidebarFrameClick(host, more)) goto done;
    scan_before = g_catalog_snapshot_done_calls;
    if (!WaitCatalogSnapshotDone(host, scan_before + 1)) goto done;
    PicoHost_Frame(host);
    if (SidebarVisibleSessionRows() <= after_more ||
        Clay_GetElementData(CLAY_IDI("SidebarMore", 0)).found) goto done;
    Clay_ElementData less = Clay_GetElementData(CLAY_IDI("SidebarLess", 0));
    if (!SidebarFrameClick(host, less) || SidebarVisibleSessionRows() >= after_more + 10)
        goto done;

    phase = "More/Less frames";
    /* Refresh the catalog through the normal poll hook after stashing. */
    if (PicoCatalog_SetProjectStashed(dir, true) != 0) goto done;
    g_sidebar_poll_due = true;
    pico_host_request_redraw(host);
    phase = "catalog";
    if (!WaitCatalogSnapshotDone(host, g_catalog_snapshot_done_calls + 1)) goto done;
    PicoHost_Frame(host);
    phase = "stash refresh";
    Clay_ElementData stash = Clay_GetElementData(CLAY_ID("SidebarStashedHeader"));
    if (!stash.found || !SidebarFrameClick(host, stash) ||
        !Clay_GetElementData(CLAY_IDI("SidebarWs", 0)).found) goto done;
    stash = Clay_GetElementData(CLAY_ID("SidebarStashedHeader"));
    if (!SidebarFrameClick(host, stash) ||
        Clay_GetElementData(CLAY_IDI("SidebarWs", 0)).found) goto done;
    good = true;
done:
    g_find_input_test = g_find_press = g_find_down = g_find_released = false;
    g_find_key = 0;
    g_clay_frame_test = false;
    g_sidebar_poll_due = false;
    if (host) pico_host_free(host);
    Pico_FreeClay();
    Clay_SetCurrentContext(previous);
    unsetenv("XDG_CONFIG_HOME");
    RmRf(cfg);
    RmRf(dir);
    if (!good) { fprintf(stderr, "sidebar same-frame phase: %s\n", phase); Fail("sidebar expand/collapse, stash, and More/Less must relayout in the action frame"); }
    return good ? 0 : 1;
}

static int TestIdleFrameOnlyPresentsOnInvalidation(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host ||
        !Pico_InitClay((Clay_Dimensions){1100, 800}))
    {
        Fail("idle redraw setup");
        if (host) pico_host_free(host);
        return 1;
    }
    WaitPluginLoad(host);
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    memset(host->view_count, 0, sizeof(host->view_count));
    host->hook_count = 0;
    PicoHost_BeginRegistration(host, PICO_REG_HOST, NULL);
    pico_host_add_hook(host, PICO_HOOK_AFTER_LAYOUT, CountLayouts);
    pico_host_add_hook(host, PICO_HOOK_AFTER_RENDER, RequestRedrawFromRender);
    PicoHost_PublishRegistration(host, NULL);

    PicoModuleGeneration module = {.ext = {.host_on_frame = RequestRedrawFromFrame}};
    PicoPluginSlot saved_plugins[PICO_MAX_EXTENSION_SLOTS];
    int slots = host->host_plugin_count;
    memcpy(saved_plugins, host->host_plugins, sizeof(saved_plugins));
    memset(host->host_plugins, 0, sizeof(host->host_plugins));
    host->host_plugin_count = 0;
    host->host_plugins[host->host_plugin_count++] =
        (PicoPluginSlot){.module = &module, .initialized = true};
    g_clay_frame_test = true;
    g_redraw_frame_callbacks = g_redraw_render_callbacks = g_redraw_layout_callbacks = 0;
    int start = g_presented_frames;
    PicoHost_Frame(host); /* First presentation; render hook requests another. */
    PicoHost_Frame(host); /* Honor the after-render request. */
    PicoHost_Frame(host); /* Host callback requests a new image. */
    if (g_redraw_layout_callbacks < 3)
    {
        memcpy(host->host_plugins, saved_plugins, sizeof(saved_plugins));
        host->host_plugin_count = slots;
        Fail("invalidated frames must lay out");
        return 1;
    }
    int layouts_before_idle = g_redraw_layout_callbacks;
    PicoHost_Frame(host); /* No new request: still pump but skip presentation. */
    /* An unchanged idle pump must keep pumping yet skip both layout and
     * presentation; hit-test state from the last layout stays valid. */
    bool good = g_redraw_frame_callbacks == 4 && g_presented_frames == start + 3 &&
                g_redraw_layout_callbacks == layouts_before_idle;
    g_find_input_test = true;
    g_find_key = KEY_F2;
    PicoHost_Frame(host); /* A key after idle must be processed and presented. */
    good = good && g_presented_frames == start + 4 &&
           g_redraw_layout_callbacks > layouts_before_idle;
    g_find_key = 0;
    g_find_input_test = false;
    memcpy(host->host_plugins, saved_plugins, sizeof(saved_plugins));
    host->host_plugin_count = slots;
    g_clay_frame_test = false;
    pico_host_free(host);
    Pico_FreeClay();
    if (!good) Fail("idle frame must keep pumping but lay out and present only on invalidation");
    return g_failed ? 1 : 0;
}

static void RecoveryOverflowView(PicoHost *host, void *state)
{
    (void)host;
    (void)state;
    g_clay_layout_calls++;
    CLAY(CLAY_ID("RecoveryItems"),
         {.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT,
                     .sizing = {.width = CLAY_SIZING_FIXED(200), .height = CLAY_SIZING_FIXED(1)}}})
    {
        for (int i = 0; i < 200; i++)
        {
            CLAY(Clay_GetElementIdWithIndex(CLAY_STRING("RecoveryItem"), (uint32_t)i),
                 {.layout = {.sizing = {.width = CLAY_SIZING_FIXED(1), .height = CLAY_SIZING_FIXED(1)}},
                  .backgroundColor = {255, 255, 255, 255}})
            {
            }
        }
    }
    if (g_clay_layout_calls == 1) g_fail_frame_allocation = true;
}

static void RecoveryAfterLayout(PicoHost *host, const PicoHookEvent *event, void *state)
{
    (void)host;
    (void)event;
    (void)state;
    g_clay_after_layout_calls++;
}

static void RecoveryAfterRender(PicoHost *host, const PicoHookEvent *event, void *state)
{
    (void)host;
    (void)event;
    (void)state;
    g_clay_after_render_calls++;
}

static int TestFrameRetriesFailedArenaReplacement(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("frame recovery host initialization");
        return 1;
    }
    WaitPluginLoad(host);
    if (!Pico_InitClay((Clay_Dimensions){1100, 800}))
    {
        pico_host_free(host);
        Fail("frame recovery Clay initialization");
        return 1;
    }
    Clay_SetMaxElementCount(128);
    if (!Pico_ReinitClay(NULL, false))
    {
        pico_host_free(host);
        Pico_FreeClay();
        Fail("frame recovery small arena initialization");
        return 1;
    }
    Clay_SetMeasureTextFunction(ShellMeasureText, NULL);
    memset(host->view_count, 0, sizeof(host->view_count));
    host->hook_count = 0;
    PicoHost_BeginRegistration(host, PICO_REG_HOST, NULL);
    pico_host_add_view(host, PICO_SLOT_MAIN, 0, RecoveryOverflowView);
    pico_host_add_hook(host, PICO_HOOK_AFTER_LAYOUT, RecoveryAfterLayout);
    pico_host_add_hook(host, PICO_HOOK_AFTER_RENDER, RecoveryAfterRender);
    PicoHost_PublishRegistration(host, NULL);

    g_clay_frame_test = true;
    g_clay_layout_calls = g_clay_failed_allocations = 0;
    g_clay_after_layout_calls = g_clay_after_render_calls = 0;
    g_clay_sentinel_presented = false;
    int presented = g_presented_frames;
    PicoHost_Frame(host);
    if (g_clay_failed_allocations != 1 || g_clay_layout_calls != 1 ||
        g_clay_after_layout_calls != 0 || g_clay_after_render_calls != 0 ||
        g_presented_frames != presented)
    {
        Fail("failed arena replacement must skip layout retries, UI hooks, and presentation");
    }
    if (!g_failed)
    {
        /* Drive the public frame entry point, not a manual reinit in the test. */
        PicoHost_Frame(host);
        if (g_clay_layout_calls != 2 || g_clay_after_layout_calls != 1 ||
            g_clay_after_render_calls != 1 || !g_clay_sentinel_presented)
        {
            Fail("the next frame must retry recovery and consume a complete rebuilt layout");
        }
    }
    g_fail_frame_allocation = false;
    g_clay_frame_test = false;
    pico_host_free(host);
    Pico_FreeClay();
    return g_failed ? 1 : 0;
}
#endif

#ifdef PICO_OPENAI_LOGIN_TESTS
#include "openai_login_test.c"
#endif

static bool WorktreeTestGit(const char *path, const char *a, const char *b,
                            const char *c, const char *d)
{
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0)
    {
        char *args[9];
        int n = 0;
        args[n++] = "git"; args[n++] = "-C"; args[n++] = (char *)path;
        if (a) args[n++] = (char *)a;
        if (b) args[n++] = (char *)b;
        if (c) args[n++] = (char *)c;
        if (d) args[n++] = (char *)d;
        args[n] = NULL;
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
        execvp("git", args);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool WorktreeTestAdd(const char *repo, const char *path)
{
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0)
    {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
        execlp("git", "git", "-C", repo, "worktree", "add", "--relative-paths",
               "-b", "linked", path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int TestWorktreeSuggestNameUsesProjectFolder(void)
{
    char name[129];
    char error[256] = {0};
    if (!PicoWorktree_SuggestName("/tmp/my-app", name, sizeof(name)) ||
        strncmp(name, "my-app-", 7) != 0 ||
        !PicoWorktree_ValidateName(name, error, sizeof(error)))
    {
        Fail("suggested worktree name starts with the project folder");
        return 1;
    }
    if (!PicoWorktree_SuggestName("/tmp/My App", name, sizeof(name)) ||
        !PicoWorktree_ValidateName(name, error, sizeof(error)))
    {
        Fail("suggested worktree name stays valid for unsafe folder names");
        return 1;
    }
    return 0;
}

static int TestWorktreeDiscoveryCreationAndGrouping(void)
{
    char repo[] = "/tmp/pico-worktree-repo-XXXXXX";
    char linked[] = "/tmp/pico-worktree-linked-XXXXXX";
    char cfg[] = "/tmp/pico-worktree-cfg-XXXXXX";
    char data[] = "/tmp/pico-worktree-data-XXXXXX";
    char file[4096], subdir[4096];
    char *old_cfg = getenv("XDG_CONFIG_HOME") ? strdup(getenv("XDG_CONFIG_HOME")) : NULL;
    char *old_data = getenv("XDG_DATA_HOME") ? strdup(getenv("XDG_DATA_HOME")) : NULL;
    PicoHost *host = NULL;
    int rc = 1;
    if (!mkdtemp(repo) || !mkdtemp(linked) || !mkdtemp(cfg) || !mkdtemp(data))
    {
        Fail("worktree temporary directories");
        goto done;
    }
    rmdir(linked);
    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_DATA_HOME", data, 1);
    snprintf(file, sizeof(file), "%s/file.txt", repo);
    snprintf(subdir, sizeof(subdir), "%s/sub", repo);
    mkdir(subdir, 0755);
    if (!WorktreeTestGit(repo, "init", "-q", NULL, NULL) ||
        !WorktreeTestGit(repo, "config", "user.email", "pico@example.test", NULL) ||
        !WorktreeTestGit(repo, "config", "user.name", "Pico Test", NULL) ||
        WriteFile(file, "committed\n") != 0 ||
        !WorktreeTestGit(repo, "add", "file.txt", NULL, NULL) ||
        !WorktreeTestGit(repo, "commit", "-qm", "initial", NULL) ||
        !WorktreeTestAdd(repo, linked))
    {
        Fail("initialize repository and linked worktree");
        goto done;
    }
    PicoWorktreeInfo local_info, linked_info, sub_info;
    if (!PicoWorktree_Discover(repo, &local_info) || !local_info.checkout_root ||
        local_info.linked || !local_info.can_create || strcmp(local_info.project_path, repo) != 0 ||
        !PicoWorktree_Discover(linked, &linked_info) || !linked_info.checkout_root ||
        !linked_info.linked || strcmp(linked_info.project_path, repo) != 0 ||
        !PicoWorktree_Discover(subdir, &sub_info) || sub_info.checkout_root ||
        strcmp(sub_info.project_path, subdir) != 0)
    {
        Fail("discover checkout project identities");
        goto done;
    }
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("worktree host init");
        goto done;
    }
    PicoWorkspaceId local_id = 0, linked_id = 0;
    if (pico_workspace_open(host, repo, &local_id) != PICO_OK ||
        pico_workspace_open(host, linked, &linked_id) != PICO_OK)
    {
        Fail("open checkout runtimes");
        goto done;
    }
    PicoWorkspace *local_ws = PicoHost_FindWorkspace(host, local_id);
    PicoWorkspace *linked_ws = PicoHost_FindWorkspace(host, linked_id);
    if (!local_ws || !linked_ws || local_ws->worktree || !linked_ws->worktree ||
        strcmp(linked_ws->project_path, repo) != 0 || PicoCatalog_Ensure(repo) != 0 ||
        PicoCatalog_Ensure(linked) != 0)
    {
        Fail("checkout runtime and catalog metadata");
        goto done;
    }
    PicoCatalogWorkspace *groups = NULL;
    int group_count = PicoCatalog_ScanGrouped(&groups);
    bool grouped = group_count == 1 && strcmp(groups[0].path, repo) == 0;
    PicoCatalog_Free(groups, group_count);
    if (!grouped)
    {
        Fail("linked checkout catalogs group under main checkout");
        goto done;
    }
    /* A project-level deletion covers exact checkout catalogs, never Git trees. */
    char local_history[4096] = {0}, linked_history[4096] = {0};
    PicoCatalogWorkspace *leaves = NULL;
    int leaves_count = PicoCatalog_Scan(&leaves);
    for (int i = 0; i < leaves_count; i++)
    {
        char *out = !strcmp(leaves[i].path, repo) ? local_history :
                    !strcmp(leaves[i].path, linked) ? linked_history : NULL;
        if (out && !PicoPath_Format(out, 4096, "%s/pico/sessions/%s/fixture.jsonl", cfg, leaves[i].key)) out[0] = 0;
    }
    PicoCatalog_Free(leaves, leaves_count);
    if (!local_history[0] || !linked_history[0] ||
        WriteFile(local_history, "{\"type\":\"session\",\"version\":4,\"kind\":\"normal\",\"id\":\"fixture\"}\n") != 0 ||
        WriteFile(linked_history, "{\"type\":\"session\",\"version\":4,\"kind\":\"normal\",\"id\":\"fixture\"}\n") != 0 ||
        PicoCatalog_DeleteProject(host, repo) != 0 ||
        access(local_history, F_OK) == 0 || access(linked_history, F_OK) == 0 ||
        access(repo, F_OK) != 0 || access(linked, F_OK) != 0 ||
        PicoCatalog_Ensure(repo) != 0 || PicoCatalog_Ensure(linked) != 0)
    {
        Fail("delete project histories across both checkouts without deleting folders");
        goto done;
    }
    PicoAgentCreateOptions options;
    memset(&options, 0, sizeof(options));
    options.kind = PICO_AGENT_MAIN;
    options.session_start = PICO_SESSION_NEW;
    options.select = true;
    PicoAgentId source_id = 0;
    if (pico_main_agent_create(host, local_id, &options, &source_id) != PICO_OK)
    {
        Fail("create worktree source draft");
        goto done;
    }
    if (PicoCatalog_DeleteProject(host, repo) == 0)
    {
        Fail("cannot delete a project with a live agent");
        goto done;
    }
    char dirty_file[4096];
    snprintf(dirty_file, sizeof(dirty_file), "%s/untracked.txt", repo);
    if (WriteFile(file, "dirty working tree\n") != 0 || WriteFile(dirty_file, "untracked\n") != 0)
    {
        Fail("prepare dirty local checkout");
        goto done;
    }
    char error[256] = {0};
    if (PicoWorktree_Request(host, source_id, "created", error, sizeof(error)) != PICO_OK)
    {
        Fail("request worktree creation");
        goto done;
    }
    PICO_TEST_WAIT(PicoWorktree_Pending(host))
    {
        pico_host_pump(host);
    }
    PicoAgent *selected = PicoHost_SelectedAgent(host);
    PicoWorkspace *selected_ws = selected ? selected->workspace : NULL;
    char created_file[8192], created_untracked[8192];
    snprintf(created_file, sizeof(created_file), "%s/file.txt", selected_ws ? selected_ws->path : "");
    snprintf(created_untracked, sizeof(created_untracked), "%s/untracked.txt",
             selected_ws ? selected_ws->path : "");
    size_t created_len = 0;
    char *created_contents = Pico_ReadFile(created_file, &created_len);
    if (PicoWorktree_Pending(host) || !selected_ws || !selected_ws->worktree ||
        strcmp(selected_ws->checkout_name, "created") != 0 ||
        strncmp(selected_ws->path, data, strlen(data)) != 0 ||
        PicoHost_FindAgent(host, source_id) != NULL || !created_contents ||
        strcmp(created_contents, "committed\n") != 0 || access(created_untracked, F_OK) == 0)
    {
        free(created_contents);
        Fail("created worktree uses committed snapshot in selected checkout draft");
        goto done;
    }
    free(created_contents);
    rc = 0;
done:
    if (host) pico_host_free(host);
    if (old_cfg) { setenv("XDG_CONFIG_HOME", old_cfg, 1); free(old_cfg); }
    else unsetenv("XDG_CONFIG_HOME");
    if (old_data) { setenv("XDG_DATA_HOME", old_data, 1); free(old_data); }
    else unsetenv("XDG_DATA_HOME");
    RmRf(linked); RmRf(repo); RmRf(cfg); RmRf(data);
    return rc;
}


#ifdef PICO_CLAY_FRAME_FAULT_TESTS
/* Wraps used to observe the durable rendering contract below: an unchanged
 * expanded thinking body must not be re-parsed or re-measured on later
 * frames, and a streaming append must only measure the new words. */
static long g_think_parse_calls;
static long g_think_measure_calls;

MdDocument __real_MdDocument_ParseEx(const char *src, size_t length, int flags);
MdDocument __wrap_MdDocument_ParseEx(const char *src, size_t length, int flags)
{
    g_think_parse_calls++;
    return __real_MdDocument_ParseEx(src, length, flags);
}

Vector2 __real_MeasureTextEx(Font font, const char *text, float fontSize, float spacing);
Vector2 __wrap_MeasureTextEx(Font font, const char *text, float fontSize, float spacing)
{
    g_think_measure_calls++;
    return __real_MeasureTextEx(font, text, fontSize, spacing);
}
#endif

/* Emulates the production host frame: layout, capacity recovery, harvest,
 * pin-to-bottom, and the post-correction relayout. */
static Clay_RenderCommandArray CachedThinkRecover(PicoHost *host, Clay_RenderCommandArray commands,
                                                   const Clay_Dimensions viewport)
{
    for (int attempt = 0; Pico_NeedsClayReinit() && attempt < 4; attempt++)
    {
        if (!Pico_ReinitClay(NULL, false))
        {
            break;
        }
        Clay_SetMeasureTextFunction(Pico_MeasureTextUtf8, NULL);
        RichText_SetMeasureFunction(Pico_MeasureTextUtf8, NULL);
        commands = PicoHost_LayoutShell(host, viewport.height, 0.0f);
    }
    return commands;
}

static Clay_RenderCommandArray CachedThinkFrame(PicoHost *host, const Clay_Dimensions viewport)
{
    Clay_SetLayoutDimensions(viewport);
    Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0.0f);
    Clay_RenderCommandArray commands =
        CachedThinkRecover(host, PicoHost_LayoutShell(host, viewport.height, 1.0f / 60.0f),
                           viewport);
    PicoChat_HarvestVirtualHeights(host);
    bool relayout = PicoChat_TakeVirtualRelayout();
    if (host->chat_follow_bottom)
    {
        Clay_ScrollContainerData data =
            Clay_GetScrollContainerData(Clay_GetElementId(CLAY_STRING("ChatScroll")));
        if (data.found && data.scrollPosition &&
            PicoScrollbar_PinToBottom(data.scrollContainerDimensions.height,
                                      data.contentDimensions.height,
                                      &data.scrollPosition->y))
        {
            relayout = true;
        }
    }
    if (relayout)
    {
        commands = CachedThinkRecover(host, PicoHost_LayoutShell(host, viewport.height, 0.0f), viewport);
        PicoChat_HarvestVirtualHeights(host);
        (void)PicoChat_TakeVirtualRelayout();
    }
    return commands;
}

#ifdef PICO_CLAY_FRAME_FAULT_TESTS
/* Expanded thinking bodies used to be re-parsed and re-measured from scratch
 * on every frame, so a long thinking block pegged one core and froze the UI.
 * The durable contract: unchanged content costs no per-frame parsing or font
 * measurement, and a streaming append only measures the appended words. */
static int TestExpandedThinkRenderingIsCached(void)
{
    const Clay_Dimensions viewport = {1100, 800};
    char dir[] = "/tmp/pico-ws-think-cache-XXXXXX";
    char cfg[] = "/tmp/pico-cfg-think-cache-XXXXXX";
    Clay_Context *previous = Clay_GetCurrentContext();
    PicoHost *host = NULL;
    PicoWorkspaceId workspace_id = 0;
    PicoAgentId agent_id = 0;
    PicoAgentCreateOptions opt;
    PicoAgent *agent = NULL;
    ShellTestState state = {.composer_height = 44.0f};
    PicoMessage *think_msg;
    enum { THINK_TEST_WORDS = 6200, THINK_TEST_APPEND = 24 };
    size_t body_cap = (size_t)THINK_TEST_WORDS * 11 + 2;
    char *body = malloc(body_cap);
    int rc = 1;

    if (!body || !mkdtemp(dir) || !mkdtemp(cfg))
    {
        free(body);
        Fail("think cache setup");
        return 1;
    }
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("think cache host init");
        goto done;
    }
    WaitPluginLoad(host);
    host->preferences.chat_width = 0;
    host->view_count[PICO_SLOT_COMPOSER] = 0;
    host->view_count[PICO_SLOT_SIDEBAR] = 0;
    ShellTestAddView(host, PICO_SLOT_COMPOSER, ShellTestComposer, &state);
    if (pico_workspace_open(host, dir, &workspace_id) != PICO_OK)
    {
        Fail("think cache open workspace");
        goto done;
    }
    memset(&opt, 0, sizeof(opt));
    opt.kind = PICO_AGENT_MAIN;
    opt.session_start = PICO_SESSION_NONE;
    opt.select = true;
    if (pico_main_agent_create(host, workspace_id, &opt, &agent_id) != PICO_OK ||
        !(agent = PicoHost_FindAgent(host, agent_id)))
    {
        Fail("think cache create agent");
        goto done;
    }
    for (int i = 0; i < 8; i++)
    {
        PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "think cache history message");
    }
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    think_msg = &agent->messages[agent->message_count - 1];
    think_msg->trace = calloc(1, sizeof(PicoTraceLine));
    think_msg->trace_count = 1;
    /* Distinct words so every cached measurement is an exact-content match. */
    size_t body_len = 0;
    for (int i = 0; i < THINK_TEST_WORDS; i++)
    {
        if (i == 1)
        {
            /* A wide early token exercises richtext's split-word path;
             * its fragments must not displace the cached source words. */
            memset(body + body_len, 'x', 160);
            body_len += 160;
            body[body_len++] = ' ';
        }
        else
        {
            body_len += (size_t)snprintf(body + body_len, body_cap - body_len, "word%05d ", i);
        }
    }
    body[body_len] = '\0';
    think_msg->trace[0].text = strdup(body);
    think_msg->trace[0].think_steps = 1;
    think_msg->trace[0].think_parts = calloc(1, sizeof(char *));
    think_msg->trace[0].think_parts[0] = strdup(body);
    think_msg->trace[0].think_part_count = 1;
    think_msg->trace[0].expanded = true;
    agent->state = PICO_AGENT_LLM_WAIT;

    if (!Pico_InitClay(viewport))
    {
        Fail("think cache Clay initialization");
        goto done;
    }
    Clay_SetMeasureTextFunction(Pico_MeasureTextUtf8, NULL);
    RichText_SetMeasureFunction(Pico_MeasureTextUtf8, NULL);
    Clay_SetPointerState((Clay_Vector2){0}, false);
    Pico_MeasureCacheReset();

    host->chat_follow_bottom = true;
    PicoChat_ResetBottomSpace(host);
    CachedThinkFrame(host, viewport);
    long initial_measures = g_think_measure_calls;
    if (g_think_parse_calls == 0 || initial_measures == 0)
    {
        Fail("think cache warm-up must parse and measure the body once");
        goto done;
    }

    /* Unchanged content: later frames must not re-parse or re-measure. */
    g_think_parse_calls = 0;
    g_think_measure_calls = 0;
    for (int frame = 0; frame < 3; frame++)
    {
        CachedThinkFrame(host, viewport);
    }
    if (g_think_parse_calls != 0 || g_think_measure_calls != 0)
    {
        Fail("unchanged expanded thinking body must not be re-parsed or re-measured per frame");
        goto done;
    }
    Clay_ElementData msg_el = Clay_GetElementData(CLAY_IDI("MsgMain", agent->message_count - 1));
    Clay_ElementData think_row =
        Clay_GetElementData(MainTraceRowId(agent->message_count - 1, 0, "ThinkRow"));
    float body_height = msg_el.found ? msg_el.boundingBox.height : 0.0f;
    if (!msg_el.found || !think_row.found || body_height <= viewport.height)
    {
        Fail("expanded thinking body must render tall enough to overflow the viewport");
        goto done;
    }

    /* More distinct words than a small shared cache can hold: the active
     * thinking part still reuses all prior prefix measurements. */
    /* Streaming append: the part buffer is replaced with the grown text, as
     * the agent does on every summary delta. Only new words may be measured. */
    size_t grown_cap = (size_t)(THINK_TEST_WORDS + THINK_TEST_APPEND) * 11 + 2;
    char *grown = malloc(grown_cap);
    if (!grown)
    {
        Fail("think cache grown body allocation");
        goto done;
    }
    memcpy(grown, body, body_len + 1);
    size_t grown_len = body_len;
    for (int i = 0; i < THINK_TEST_APPEND; i++)
    {
        grown_len += (size_t)snprintf(grown + grown_len, grown_cap - grown_len, "word%05d ",
                                      THINK_TEST_WORDS + i);
    }
    grown[grown_len] = '\0';
    free(think_msg->trace[0].think_parts[0]);
    think_msg->trace[0].think_parts[0] = grown;
    free(think_msg->trace[0].text);
    think_msg->trace[0].text = strdup(grown);
    g_think_parse_calls = 0;
    g_think_measure_calls = 0;
    CachedThinkFrame(host, viewport);
    if (g_think_measure_calls >= initial_measures / 4)
    {
        Fail("streaming append must not remeasure the long unchanged prefix");
        goto done;
    }
    msg_el = Clay_GetElementData(CLAY_IDI("MsgMain", agent->message_count - 1));
    if (!msg_el.found || msg_el.boundingBox.height <= body_height)
    {
        Fail("grown thinking body must render taller than before");
        goto done;
    }

    /* The AI stops thinking: the live row folds into the collapsed group on
     * the first idle frame. Measure the shrunken message on that transition
     * frame before later virtualization can unmount it. */
    agent->state = PICO_AGENT_IDLE;
    pico_run_hooks(host, PICO_HOOK_ON_TURN_END, agent->id);
    g_think_parse_calls = 0;
    g_think_measure_calls = 0;
    CachedThinkFrame(host, viewport);
    if (!Clay_GetElementData(MainTraceRowId(agent->message_count - 1, 0, "TraceGroupRow")).found ||
        Clay_GetElementData(MainTraceRowId(agent->message_count - 1, 0, "ThinkRow")).found)
    {
        Fail("finished thinking must fold into the trace group header");
        goto done;
    }
    msg_el = Clay_GetElementData(CLAY_IDI("MsgMain", agent->message_count - 1));
    if (!msg_el.found || msg_el.boundingBox.height >= body_height)
    {
        Fail("collapsed thinking body must shrink the message");
        goto done;
    }
    /* The newly rendered group header is new content; steady collapsed
     * frames must do no further work. */
    g_think_parse_calls = 0;
    g_think_measure_calls = 0;
    for (int frame = 0; frame < 2; frame++)
    {
        CachedThinkFrame(host, viewport);
    }
    if (g_think_parse_calls != 0 || g_think_measure_calls != 0)
    {
        Fail("collapsed thinking body must not be parsed or measured");
        goto done;
    }

    /* Direct measurement-cache contract: identical short text is served from
     * the cache, different text or a reset forces a fresh measurement. */
    {
        Clay_StringSlice slice = {.length = 9, .chars = "cached abc"};
        Clay_TextElementConfig config = {.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI};
        Clay_Dimensions first = Pico_MeasureTextUtf8(slice, &config, NULL);
        g_think_measure_calls = 0;
        Clay_Dimensions again = Pico_MeasureTextUtf8(slice, &config, NULL);
        if (g_think_measure_calls != 0 || first.width != again.width ||
            first.height != again.height)
        {
            Fail("identical short text must be served from the measurement cache");
            goto done;
        }
        Clay_StringSlice other = {.length = 9, .chars = "cached xyz"};
        Pico_MeasureTextUtf8(other, &config, NULL);
        if (g_think_measure_calls != 1)
        {
            Fail("different text with the same length must miss the measurement cache");
            goto done;
        }
        Pico_MeasureCacheReset();
        g_think_measure_calls = 0;
        Pico_MeasureTextUtf8(slice, &config, NULL);
        if (g_think_measure_calls != 1)
        {
            Fail("cache reset must force a fresh measurement");
            goto done;
        }
        config.fontSize = PICO_FONT_CAPTION;
        g_think_measure_calls = 0;
        Pico_MeasureTextUtf8(slice, &config, NULL);
        if (g_think_measure_calls != 1)
        {
            Fail("a different font size must not reuse a cached width");
            goto done;
        }
    }
    /* More expanded parts than the cache's initial capacity must not free
     * the first part while Clay's render commands still refer to its text. */
    PicoAgent_AddMessage(host, agent, PICO_ROLE_ASSISTANT, "");
    PicoMessage *many = &agent->messages[agent->message_count - 1];
    many->trace = calloc(1, sizeof(PicoTraceLine));
    many->trace_count = 1;
    many->trace[0].text = strdup("first unique segment");
    many->trace[0].think_steps = 1;
    many->trace[0].expanded = true;
    many->trace[0].think_part_count = 16;
    many->trace[0].think_parts = calloc(16, sizeof(char *));
    for (int i = 0; i < 16; i++)
    {
        char part[64];
        snprintf(part, sizeof(part), "unique thinking segment %02d", i);
        many->trace[0].think_parts[i] = strdup(i ? part : "first unique segment");
    }
    agent->state = PICO_AGENT_LLM_WAIT;
    PicoChat_ResetBottomSpace(host);
    Clay_RenderCommandArray commands = CachedThinkFrame(host, viewport);
    commands = CachedThinkFrame(host, viewport);
    bool first_found = false;
    for (int i = 0; i < commands.length; i++)
    {
        Clay_RenderCommand *command = Clay_RenderCommandArray_Get(&commands, i);
        if (command && command->commandType == CLAY_RENDER_COMMAND_TYPE_TEXT)
        {
            Clay_StringSlice text = command->renderData.text.stringContents;
            if (text.length == (int)strlen("first unique segment") &&
                memcmp(text.chars, "first unique segment", (size_t)text.length) == 0)
            {
                first_found = true;
                break;
            }
        }
    }
    if (!first_found)
    {
        Fail("render commands must retain text from the first expanded thinking part");
        goto done;
    }

    /* Font replacement invalidates exact-text measurement hits. */
    Clay_StringSlice slice = {.length = 9, .chars = "cached abc"};
    Clay_TextElementConfig config = {.fontId = FONT_REGULAR, .fontSize = PICO_FONT_UI};
    Pico_MeasureTextUtf8(slice, &config, NULL);
    Pico_UnloadFonts(NULL);
    g_think_measure_calls = 0;
    Pico_MeasureTextUtf8(slice, &config, NULL);
    if (g_think_measure_calls != 1)
    {
        Fail("reloaded fonts must trigger a fresh text measurement");
        goto done;
    }
    g_think_parse_calls = 0;
    CachedThinkFrame(host, viewport);
    if (g_think_parse_calls == 0)
    {
        Fail("font replacement must rebuild retained thinking layouts");
        goto done;
    }
    rc = 0;
done:
    if (host)
    {
        PicoAgent_ClearMessages(agent);
        pico_host_free(host);
    }
    Pico_FreeClay();
    Clay_SetCurrentContext(previous);
    free(body);
    unsetenv("XDG_CONFIG_HOME");
    rmdir(cfg);
    rmdir(dir);
    return rc;
}
#endif

static int TestCopiedMessageSourceMetadata(void)
{
    PicoMessage source = {.role = PICO_ROLE_NOTICE, .notice_severity = PICO_NOTICE_ERROR,
                          .source = "copied answer",
                          .source_len = strlen("copied answer"), .revision = 7};
    PicoMessage *copy = NULL;
    int count = 0;
    if (!PicoMessages_Copy(&source, 1, &copy, &count)) return 1;
    bool ok = count == 1 && copy[0].role == PICO_ROLE_NOTICE &&
              copy[0].notice_severity == PICO_NOTICE_ERROR && copy[0].source &&
              strcmp(copy[0].source, source.source) == 0 &&
              copy[0].source_len == strlen(copy[0].source) &&
              copy[0].source_cap >= copy[0].source_len + 1 &&
              copy[0].revision == source.revision;
    PicoMessages_Free(copy, count);
    if (!ok) fprintf(stderr, "FAIL: copied message source metadata is inconsistent\n");
    return !ok;
}

static char g_test_home[4096];

static void RemoveTestHome(void)
{
    RmRf(g_test_home);
}

static bool IsolateTestHome(void)
{
    char home[] = "/tmp/pico-host-test-home-XXXXXX";
    if (!mkdtemp(home)) return false;
    snprintf(g_test_home, sizeof(g_test_home), "%s", home);
    if (atexit(RemoveTestHome) != 0 || setenv("HOME", home, 1) != 0) return false;
    /* Fixtures clear XDG overrides between tests. Their fallback must never
     * write workspace/session metadata into the developer's real HOME. */
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    unsetenv("XDG_DATA_HOME");
    return true;
}

int TestClipboardPaste(void);

int main(int argc, char **argv)
{
#ifdef PICO_OPENAI_LOGIN_TESTS
    if (argc == 2 && strcmp(argv[1], "--openai-retained-shutdown") == 0)
        return PICO_TEST_RUN(TestOpenAiBlockedShutdownChild());
#endif
    if (!IsolateTestHome()) return 1;
    if (argc == 2 && strcmp(argv[1], "--clipboard") == 0) return PICO_TEST_RUN(TestClipboardPaste());
    if (argc == 2 && strcmp(argv[1], "--fast-persistence") == 0) return PICO_TEST_RUN(TestFastSelectionPersistence());
#ifdef PICO_OPENAI_LOGIN_TESTS
    if (argc == 2 && strcmp(argv[1], "--openai-login") == 0)
        return PICO_TEST_RUN(TestOpenAiLogin()) || PICO_TEST_RUN(TestOpenAiBrowserLauncher()) || PICO_TEST_RUN(TestOpenAiBlockedShutdown());
    if (PICO_TEST_RUN(TestOpenAiLogin()) || PICO_TEST_RUN(TestOpenAiBrowserLauncher()) || PICO_TEST_RUN(TestOpenAiBlockedShutdown())) return 1;
#endif
#ifdef PICO_CLAY_FRAME_FAULT_TESTS
    if (argc == 2 && strcmp(argv[1], "--clay-recovery") == 0)
    {
        return PICO_TEST_RUN(TestFrameRetriesFailedArenaReplacement());
    }
    if (PICO_TEST_RUN(TestSidebarContextMenus()) != 0) return 1;
    if (PICO_TEST_RUN(TestSidebarSameFrameControls()) != 0) return 1;
    if (PICO_TEST_RUN(TestIdleFrameOnlyPresentsOnInvalidation()) != 0) return 1;
    if (PICO_TEST_RUN(TestIdleFrameRetainsChatScroll()) != 0) return 1;
    if (PICO_TEST_RUN(TestFrameRetriesFailedArenaReplacement()) != 0)
    {
        return 1;
    }
#else
    (void)argc;
    (void)argv;
#endif
    if (PICO_TEST_RUN(TestClipboardPaste()) != 0) return 1;
    if (PICO_TEST_RUN(TestCopiedMessageSourceMetadata()) != 0) return 1;
    if (PICO_TEST_RUN(TestWorktreeSuggestNameUsesProjectFolder()) != 0) return 1;
    if (PICO_TEST_RUN(TestWorktreeDiscoveryCreationAndGrouping()) != 0) return 1;
    if (PICO_TEST_RUN(TestQuitDefersTeardownUntilFrameReturns()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestFooterMainAgentTps()) != 0) return 1;
    if (PICO_TEST_RUN(TestFooterCacheTooltip()) != 0)
    {
        return 1;
    }
#ifdef PICO_CLAY_FRAME_FAULT_TESTS
    if (PICO_TEST_RUN(TestWorktreeNameField()) != 0)
    {
        return 1;
    }
#endif
    if (PICO_TEST_RUN(TestFastSelectionPersistence()) != 0)
        return 1;
    if (PICO_TEST_RUN(TestBottomFollowShellGeometryStable()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestEmptyCardsTwoColumnTrim()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestEmptyCardsOverflowScroll()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestChatBottomFollowClearsComposer()) != 0)
    {
        return 1;
    }
#ifdef PICO_CLAY_FRAME_FAULT_TESTS
    if (argc == 2 && strcmp(argv[1], "--think-cache") == 0)
        return PICO_TEST_RUN(TestExpandedThinkRenderingIsCached());
    if (PICO_TEST_RUN(TestExpandedThinkRenderingIsCached()) != 0) return 1;
#endif
    if (PICO_TEST_RUN(TestChatFindTranscript()) != 0) return 1;
    if (PICO_TEST_RUN(TestExpandedStreamingThinkStaysInsideChat()) != 0) return 1;
    if (PICO_TEST_RUN(TestExpandedThinkFoldResizesChat()) != 0) return 1;
    if (PICO_TEST_RUN(TestChatTraceRowsShareHeight()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestChatCompletedToolRowDwells()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestChatToolStatusDotCentered()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestIdleSidebarSessionDot()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestSidebarDisplayTitlesFitRows()) != 0) return 1;
    if (PICO_TEST_RUN(TestCanonicalOpenAndDuplicate()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestSidebarSnapshotBeforeReconcile(false)) != 0 ||
        PICO_TEST_RUN(TestSidebarSnapshotBeforeReconcile(true)) != 0) return 1;
    if (PICO_TEST_RUN(TestSidebarCatalogChangeToken()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestSidebarCatalogScanDoesNotBlockPump()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestFileCompletionPublishesWorkerSnapshot()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestWorkspaceLessHostTransition()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestSortedViewRegistrationAssignsStateAndRollsBack()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestSubmitSettersTakeOwnership()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestSidebarDragBehavior()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestWorkspaceBuiltinsRegisterThroughWorkspaceInit()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestFailedWorkspaceInitKeepsHostSlot()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestWorkspaceShutdownSeesOwningWorkspace()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestWorkspaceChangeSeesOwningWorkspace()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestCdOpensSelectsAndReusesWorkspace()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestBusySlashCommandsOpenBackgroundWithoutJobs()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestBackgroundJobsSurviveWorkspaceReload()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestReloadTargetsSelectedWorkspace()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestHostReloadIgnoresWorkspaceLocalCompileFailure()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestSkillSubmissionPreservesFileMentions()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestCdResolvesAgainstCommandWorkspace()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestWorkspaceOpenEvictsLeastRecentlyActiveIdle()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestWorkspaceOpenLimitWhenAllBusy()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMainAgentCreateEvictsIdleAgent()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMainAgentCreateEvictsPerWorkspaceCap()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMainAgentCreateLimitWhenAllBusy()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestSessionResetHookCreateProtectsDispatchingAgent()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestSessionResetHookOpenProtectsDispatchingWorkspace()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestCdRollsBackNewWorkspaceOnAgentLimit()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestCdRejectsClosingWorkspace()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestModelChangeDoesNotMutateWorkspaceDefault()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestWorkspacePluginIsolation()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestHostPluginIsolation()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestSettingsModalWheelOverField()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestHostSettingsPersistence()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestScopeEnforcement()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestStagingRollbackOnFailedInit()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestFailedHostReloadPreservesLiveInstances()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestWorkspaceReloadUsesLiveOwnerAndSettings()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestNestedWorkspaceExtensionOwnership()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestHeaderReloadIsAsynchronous())) return 1;
    if (PICO_TEST_RUN(TestPluginSourceScanDoesNotBlockUi())) return 1;
    if (PICO_TEST_RUN(TestBlockedSourceScanRetainsShutdown())) return 1;
    if (PICO_TEST_RUN(TestSdkDependencyManifestsDoNotCrossReloadHosts()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestWorkspaceLocalPollingReloadsOnlyOwner()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestHostCompileFailureQuarantinesUnchangedPoll()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestWorkspaceCompileFailureQuarantinesUnchangedPoll()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestWorkspaceLocalExtensionWithHostCallbacksRejected()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestReloadInitRollbackPreservesActiveState()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestGenerationRolloutAndDlcloseOnRelease()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestReloadReusesReleasedModuleSlots()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestScopedExtensionListingRecords()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestDualScopeIndependentPublicationRollback()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestRetainedActiveGenerationsReceiveFrameCallbacks()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestExtensionSlotsUseSourceIdentity()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestStatelessExtensionRollbackDoesNotLeakModule()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestBusyReloadQueuesAndRejectsNewWork()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMultiWorkspaceInstructionsIsolation()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMultiWorkspaceToolNameIsolation()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMultiWorkspaceMailboxIsolation()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestTurnKeepsPinnedModelAndEffort()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestFastSurvivesMidTurnModelSwitch()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestQuestionnaireClarification()) != 0) return 1;
    if (PICO_TEST_RUN(TestMultiWorkspaceAskOrderingAndRouting()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMultiWorkspaceReloadAndCloseIsolation()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMultiWorkspaceStuckWorkerIsolation()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMultiWorkspaceMainAgentDelegationDrain()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMultiWorkspaceModelAndSettingsIsolation()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMultiWorkspaceFrameCallbacks()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMultiWorkspaceCloseLastMainAgentAndZeroAgents()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMultiWorkspaceStaleIds()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMultiWorkspaceFairPumping()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestStreamingReparseDebounced()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestMultiWorkspaceDeletedDirectoryIntegrity()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestDiffShutdownDoesNotWaitForGit()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestUnusedPendingDraftDiscardedOnSelectedCreate()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestUnusedPendingDraftDiscardedOnSelect()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestPersistedSessionKeptOnSelect()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestModelChangeKeepsUnusedDraftOnSelect()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestAgentCloseAppliesQueuedPersistenceFailure()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestTitleRewriteDoesNotBlockOtherWorkspace()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestCapacityOpenRechecksCanonicalPath()) != 0) return 1;
    if (PICO_TEST_RUN(TestInvalidCreateAtCapacityPreservesAgents()) != 0) return 1;
    if (PICO_TEST_RUN(TestCapacityCreateProtectsPendingReplacement()) != 0) return 1;
    if (PICO_TEST_RUN(TestLoadTargetSurvivesPreviousLoadCancellation()) != 0) return 1;
    if (PICO_TEST_RUN(TestAsyncSessionReplay()) != 0) return 1;
    if (PICO_TEST_RUN(TestAsyncReplayLargeMessage()) != 0) return 1;
    if (PICO_TEST_RUN(TestSavedSubagentInspectLoadsOffThread()) != 0) return 1;
    if (PICO_TEST_RUN(TestResumeCompletionDoesNotScanOnUi()) != 0) return 1;
    if (PICO_TEST_RUN(TestResumeLoadsStoredModel()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestSelectClearsUnseenComplete()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestUnseenCompletePersistsAcrossRestart()) != 0)
    {
        return 1;
    }
    if (PICO_TEST_RUN(TestPersistenceShutdownUsesSharedDeadline()) != 0)
    {
        return 1;
    }
    if (g_failed)
    {
        return 1;
    }
    /* Retained shutdown permanently retires this test process, so it is last. */
    if (PICO_TEST_RUN(TestProcessShutdownUsesSharedDeadline()) != 0)
    {
        return 1;
    }
    return 0;
}
