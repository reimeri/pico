#include "pico/host.h"
#include "pico/plugin.h"
#include "host_internal.h"
#include "workspace_internal.h"
#include "settings.h"
#include "session.h"
#include "scrollbar.h"
#include "richtext.h"
#include "builtins/chat.h"
#include "builtins/background_model.h"
#include "builtins/sidebar.h"
#include "agent_internal.h"
#include "agent.h"
#include "overlay.h"
#include "clay/clay.h"

#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void Clay_Raylib_Render(Clay_RenderCommandArray renderCommands, Font *fonts)
{
    (void)renderCommands;
    (void)fonts;
}

_Static_assert((PicoWorkspaceId)0 == 0, "zero is an invalid workspace id");
_Static_assert((PicoAgentId)0 == 0, "zero is an invalid agent id");

/* Existing lifecycle scenarios assert the final generation. Wait for compilation
 * while keeping the actual production entry points nonblocking. */
static void WaitPluginLoad(PicoHost *host)
{
    for (int i = 0; i < 10000; i++)
    {
        PicoPlugins_Load(host);
        if (!host->plugin_compile) return;
        usleep(1000);
    }
}
static void WaitPluginPoll(PicoHost *host)
{
    for (int i = 0; i < 10000; i++)
    {
        host->plugin_last_poll = -1;
        PicoPlugins_Poll(host);
        if (!host->plugin_compile) return;
        usleep(1000);
    }
}
static bool WaitHostReload(PicoHost *host)
{
    for (int i = 0; i < 10000; i++)
    {
        bool ok = PicoPlugins_ReloadHost(host);
        if (!host->plugin_compile) return ok;
        usleep(1000);
    }
    return false;
}
static bool WaitWorkspaceReload(PicoWorkspace *workspace)
{
    for (int i = 0; i < 10000; i++)
    {
        bool ok = PicoWorkspace_Reload(workspace);
        if (!workspace->host->plugin_compile) return ok;
        PicoPlugins_Poll(workspace->host);
        usleep(1000);
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
static int g_catalog_scan_calls;
static bool g_sidebar_poll_due;

static bool TransferTestByte(int fd, bool write_byte)
{
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
    if (stage && strcmp(stage, "catalog_scan") == 0)
    {
        g_catalog_scan_calls++;
    }
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

static int RunShellStabilityCase(bool with_sidebar)
{
    const Clay_Dimensions viewport = {1714, 1392};
    const int frames = 400;
    uint32_t arena_size = Clay_MinMemorySize();
    void *memory = malloc(arena_size);
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
    if (!memory)
    {
        Fail("shell stability arena allocation");
        return 1;
    }
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(arena_size, memory);
    if (!Clay_Initialize(arena, viewport, (Clay_ErrorHandler){0}))
    {
        free(memory);
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
        Clay_SetLayoutDimensions(viewport);
        Clay_UpdateScrollContainers(false, (Clay_Vector2){0}, 0.0f);
        (void)PicoHost_LayoutShell(&host, viewport.height, 1.0f / 60.0f);
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

    Clay_SetCurrentContext(previous);
    free(memory);
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
    for (int i = 0; i < 30; i++)
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

    for (frame = 0; frame < 12; frame++)
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

        char overflow[] = "/tmp/pico-ws-overflow-XXXXXX";
        PicoWorkspaceId overflow_id = 0;
        if (mkdtemp(overflow))
        {
            if (pico_workspace_open(host, overflow, &overflow_id) != PICO_LIMIT || overflow_id != 0)
            {
                Fail("opening a workspace beyond capacity should return PICO_LIMIT");
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
    json = PicoBgTable_Spawn(table, agent_id, dir, "sleep", "sleep 30", &error);
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

static int TestCdRollsBackNewWorkspaceOnAgentLimit(void)
{
    const int workspace_count = (PICO_MAX_TOTAL_AGENTS + PICO_MAX_AGENTS - 1) / PICO_MAX_AGENTS;
    /* This scenario needs room to open a workspace after filling the agent
     * budget. Otherwise workspace capacity rejects /cd before agent creation. */
    if (workspace_count >= PICO_MAX_WORKSPACES)
    {
        return 0;
    }
    PicoHost *host = NULL;
    char root[] = "/tmp/pico-cdlim-XXXXXX";
    char directories[PICO_MAX_WORKSPACES][128] = {{0}};
    int created = 0;
    PicoWorkspaceId first = 0;
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
        for (int i = 0; i < PICO_MAX_AGENTS && total < PICO_MAX_TOTAL_AGENTS; i++, total++)
        {
            PicoAgentId id = 0;
            if (pico_main_agent_create(host, workspace, &opt, &id) != PICO_OK)
            {
                Fail("fill host agent budget for cd rollback");
                goto done;
            }
        }
    }
    int count_before = pico_workspace_count(host);
    if (PicoHost_ChangeWorkspace(host, PicoHost_FindWorkspace(host, first), directories[workspace_count]) ||
        pico_workspace_count(host) != count_before)
    {
        Fail("cd must roll back a newly opened workspace when agent creation fails");
    }

done:
    pico_host_free(host);
    for (int i = 0; i < created; i++)
    {
        rmdir(directories[i]);
    }
    rmdir(root);
    return g_failed ? 1 : 0;
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
    if (pico_add_tool(ws, "invalid_tool", "desc", "{}", DummyTool, NULL))
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
    pico_add_tool(ws, "rollback_tool", "desc", "{}", DummyTool, NULL);
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

static double TestMonotonicTime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int TestHeaderReloadIsAsynchronous(void)
{
    char cfg[] = "/tmp/pico-header-cfg-XXXXXX";
    char cache[] = "/tmp/pico-header-cache-XXXXXX";
    char ws[] = "/tmp/pico-header-ws-XXXXXX";
    char ext_dir[4096], source[8192], header[8192], compiler[4096];
    PicoHost *host = NULL;
    int failed = 1;
    if (!mkdtemp(cfg) || !mkdtemp(cache) || !mkdtemp(ws)) return 1;
    snprintf(ext_dir, sizeof(ext_dir), "%s/pico/extensions", cfg);
    snprintf(source, sizeof(source), "%s/probe.c", ext_dir);
    snprintf(header, sizeof(header), "%s/value header.h", ext_dir);
    snprintf(compiler, sizeof(compiler), "%s/compiler", cfg);
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
        WriteFile(compiler, "#!/bin/sh\nsleep 1\nexec cc \"$@\"\n") || chmod(compiler, 0700)) goto done;
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    if (utimensat(AT_FDCWD, header, times, 0)) goto done;
    setenv("PICO_CC", compiler, 1);
    double start = TestMonotonicTime();
    bool immediate = PicoPlugins_ReloadHost(host);
    if (immediate || TestMonotonicTime() - start > 0.5 ||
        PicoPlugins_HostState(host, "header_probe") != value) goto done;
    double deadline = TestMonotonicTime() + 8;
    while (TestMonotonicTime() < deadline)
    {
        pico_host_pump(host);
        value = PicoPlugins_HostState(host, "header_probe");
        if (value && *value == 20) break;
        usleep(1000);
    }
    if (!value || *value != 20 || PicoHost_FindWorkspace(host, id)->active_registration != registration) goto done;
    /* A compiler that never produces output must still expire, keeping the
     * previous generation active. The generous harness deadline is not an
     * assertion of the configurable production duration. */
    if (WriteFile(compiler, "#!/bin/sh\nsleep 60\n") || WriteFile(header, "#define VALUE 25\n")) goto done;
    PicoPlugins_ReloadHost(host);
    deadline = TestMonotonicTime() + 45;
    while (TestMonotonicTime() < deadline &&
           (!host->status_warn || !strstr(host->status_warn, "compiler timed out")))
    {
        pico_host_pump(host);
        usleep(10000);
    }
    value = PicoPlugins_HostState(host, "header_probe");
    if (!value || *value != 20 || !host->status_warn || !strstr(host->status_warn, "compiler timed out")) goto done;
    if (WriteFile(compiler, "#!/bin/sh\nsleep 1\nexec cc \"$@\"\n")) goto done;
    /* Removing a queued source must cancel its build, without blocking future builds. */
    if (WriteFile(header, "#define VALUE 30\n")) goto done;
    PicoPlugins_ReloadHost(host);
    unlink(source);
    deadline = TestMonotonicTime() + 5;
    while (host->plugin_compile && TestMonotonicTime() < deadline)
    {
        pico_host_pump(host);
        usleep(1000);
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
    pico_add_tool(wsA, "custom_worker_tool", "desc A", "{}", RunToolA, NULL);
    PicoHost_PublishRegistration(host, NULL);

    PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, wsB);
    pico_add_tool(wsB, "custom_worker_tool", "desc B", "{}", RunToolB, NULL);
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
} MatrixProviderMode;

static struct MatrixProviderState *g_matrix_states[PICO_MAX_WORKSPACES + 1];

typedef struct MatrixProviderState {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    MatrixProviderMode mode;
    bool entered;
    bool release;
    bool exited;
    int calls;
    char *answer;
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
    (void)turn;
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
    state->entered = true;
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
                on_delta(user, PICO_LLM_DELTA_TEXT, "x", 1);
            }
            usleep(100);
        }
    }
    if (mode == MATRIX_PROVIDER_ASK && call == 0)
    {
        pico_llm_result_add_tool_call(out, "matrix-ask", "matrix_ask", "{}", NULL);
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
    int rc = pico_tool_ask(ctx, "{\"type\":\"confirm\",\"message\":\"matrix ask\"}",
                           &answer);
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
    pico_add_provider(workspace, &(PicoProvider){
        .name = "matrix", .stream = MatrixProvider, .map_context = true, .state = state,
    });
    bool tool_ok = !add_ask_tool ||
                   pico_add_tool(workspace, "matrix_ask", "matrix ask", "{}",
                                 MatrixAskTool, NULL);
    PicoHost_PublishRegistration(host, state);
    return tool_ok && pico_workspace_find_provider(workspace, "matrix") != NULL;
}

static bool PumpUntilIdle(PicoHost *host, PicoAgent *agent, int attempts)
{
    for (int i = 0; i < attempts; i++)
    {
        pico_host_pump(host);
        if (!PicoAgent_IsBusy(agent))
        {
            return true;
        }
        usleep(1000);
    }
    return false;
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
    for (int i = 0; started && i < 3000 && (ask_a.id == 0 || ask_b.id == 0); i++)
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
        if (ask_a.id == 0 || ask_b.id == 0)
        {
            usleep(1000);
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
    for (int i = 0; stays_hidden && i < 50; i++)
    {
        pico_host_pump(host);
        PicoToolAsk still_a = {0};
        if (!PicoAgent_PendingAsk(agentA, &still_a))
        {
            break;
        }
        PicoToolAsk now = {0};
        if (pico_tool_pending_ask(host, &now) && now.id == ask_a.id)
        {
            stays_hidden = false;
        }
        usleep(1000);
    }

    /* Reopening a1 surfaces its ask again; answering it completes both turns. */
    bool back_to_a = stays_hidden && pico_agent_select(host, a1);
    PicoToolAsk final_ask = {0};
    bool resurfaces = back_to_a && pico_tool_pending_ask(host, &final_ask) &&
                      final_ask.id == ask_a.id;
    bool answered_a = resurfaces && pico_tool_answer(host, ask_a.id, "{\"step\":1}") &&
                      !pico_tool_answer(host, ask_a.id, "{\"stale\":true}");
    bool completed = answered_a && PumpUntilIdle(host, agentA, 3000) &&
                     PumpUntilIdle(host, agentB, 3000);
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
    for (int i = 0; i < 3000 && !MatrixStateFlag(&stateA, false); i++)
    {
        pico_host_pump(host);
        usleep(1000);
    }
    pico_workspace_request_close(host, idA);
    bool b_completed = agentB && PumpUntilIdle(host, agentB, 3000);
    bool isolated_while_blocked = MatrixStateFlag(&stateA, false) &&
                                  PicoHost_FindWorkspace(host, idA) == wsA &&
                                  wsA->state == PICO_WORKSPACE_CLOSING &&
                                  PicoHost_FindWorkspace(host, idB) == wsB &&
                                  wsB->state == PICO_WORKSPACE_OPEN && b_completed;

    MatrixStateRelease(&stateA);
    for (int i = 0; i < 3000 && PicoHost_FindWorkspace(host, idA); i++)
    {
        pico_host_pump(host);
        usleep(1000);
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

static int TestMultiWorkspaceAgentLimits(void)
{
    PicoHost *host = NULL;
    if (pico_host_init(&host, NULL, true) != PICO_OK || !host)
    {
        Fail("host init limits test");
        return 1;
    }

    char directories[PICO_MAX_WORKSPACES][64] = {{0}};
    int opened = 0;
    int total = 0;
    PicoAgentCreateOptions opt = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE};

    /* Fill workspaces until both the per-workspace and shared host limits have
     * been exercised. A fresh workspace must still reject at the host limit. */
    for (int w = 0; w < PICO_MAX_WORKSPACES; w++)
    {
        snprintf(directories[w], sizeof(directories[w]), "/tmp/pico-ws-limit-%d-XXXXXX", w);
        if (!mkdtemp(directories[w]))
        {
            Fail("mkdtemp limits test");
            break;
        }
        opened++;
        PicoWorkspaceId workspace = 0;
        if (pico_workspace_open(host, directories[w], &workspace) != PICO_OK)
        {
            Fail("open workspace for capacity test");
            break;
        }
        int available = PICO_MAX_TOTAL_AGENTS - total;
        int count = available < PICO_MAX_AGENTS ? available : PICO_MAX_AGENTS;
        for (int i = 0; i < count; i++)
        {
            PicoAgentId id = 0;
            if (pico_main_agent_create(host, workspace, &opt, &id) != PICO_OK || id == 0)
            {
                Fail("create agent within workspace and host capacity");
                goto done;
            }
            total++;
        }
        PicoAgentId overflow = 0;
        if (pico_main_agent_create(host, workspace, &opt, &overflow) != PICO_LIMIT ||
            overflow != 0 || PicoHost_TotalAgentCount(host) != total)
        {
            Fail("capacity overflow must reject creation without adding an agent");
        }
        if (count < PICO_MAX_AGENTS)
        {
            break;
        }
    }

done:
    pico_host_free(host);
    for (int i = 0; i < opened; i++)
    {
        rmdir(directories[i]);
    }
    return g_failed ? 1 : 0;
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
    bool b_completed = agentB && PumpUntilIdle(host, agentB, 3000);
    bool progress = configured && MatrixStateFlag(&stateA, false) && b_completed &&
                    PicoAgent_IsBusy(agentA) && agentB->message_count > 0;

    MatrixStateRelease(&stateA);
    bool a_completed = agentA && PumpUntilIdle(host, agentA, 3000);
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

static double ElapsedSeconds(const struct timespec *start, const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
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
    for (int i = 0; opened && i < 3000 && access(marker, F_OK) != 0; i++)
    {
        usleep(1000);
    }
    bool blocked = access(marker, F_OK) == 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    if (blocked)
    {
        pico_workspace_request_close(host, id);
        pico_host_pump(host);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = ElapsedSeconds(&start, &end);
    WriteFile(release, "release\n");
    if (host)
    {
        pico_host_free(host);
    }
    free(old_path);
    /* The worker is intentionally detached. Keep its fake executable,
     * workspace, PATH, and release marker valid until this test process exits
     * instead of imposing a timing-dependent cleanup wait on the main thread. */
    if (!opened || !blocked || elapsed >= 0.1)
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
        if (!TransferTestByte(ready[0], false))
        {
            _exit(5);
        }
        struct timespec start;
        struct timespec end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        PicoHostShutdownResult result = PicoHost_Shutdown(host);
        clock_gettime(CLOCK_MONOTONIC, &end);
        (void)TransferTestByte(proceed[1], true);
        double elapsed = ElapsedSeconds(&start, &end);
        _exit(result == PICO_HOST_SHUTDOWN_RETAINED && elapsed >= 0.75 && elapsed < 1.7 ? 0 : 6);
    }

    sleep(2);
    (void)TransferTestByte(proceed[1], true);
    int status = 0;
    waitpid(child, &status, 0);
    close(ready[0]); close(ready[1]); close(proceed[0]); close(proceed[1]);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        Fail("blocked persistence must consume the process-wide shutdown deadline");
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
    for (int i = 0; i < 3000 &&
                    (!MatrixStateFlag(&stateA, false) || !MatrixStateFlag(&stateB, false)); i++)
    {
        usleep(1000);
    }
    bool both_blocked = MatrixStateFlag(&stateA, false) && MatrixStateFlag(&stateB, false);
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    PicoHostShutdownResult result = PicoHost_Shutdown(host);
    clock_gettime(CLOCK_MONOTONIC, &end);
    MatrixStateRelease(&stateA);
    MatrixStateRelease(&stateB);
    for (int i = 0; i < 3000 &&
                    (!MatrixStateFlag(&stateA, true) || !MatrixStateFlag(&stateB, true)); i++)
    {
        usleep(1000);
    }
    double elapsed = ElapsedSeconds(&start, &end);
    bool shared = both_blocked && result == PICO_HOST_SHUTDOWN_RETAINED &&
                  elapsed >= 0.75 && elapsed < 1.7;
    if (!shared)
    {
        Fail("all workspaces must consume one process-wide shutdown deadline");
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
    workspace->models = NULL;
    pico_host_free(host);
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
    pico_host_free(host);
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
    pico_host_free(host);
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

static int TestSidebarCatalogChangeToken(void)
{
    char dir[] = "/tmp/pico-sidebar-token-ws-XXXXXX";
    char cfg[] = "/tmp/pico-sidebar-token-cfg-XXXXXX";
    PicoHost *host = NULL;
    int scans_before;
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
    pico_host_pump(host);
    if (g_catalog_scan_calls != scans_before + 1)
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
    g_sidebar_poll_due = true;
    pico_host_pump(host);
    if (g_catalog_scan_calls != scans_before + 2)
    {
        Fail("changed catalog token must refresh the sidebar");
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

int main(void)
{
    if (TestBottomFollowShellGeometryStable() != 0)
    {
        return 1;
    }
    if (TestEmptyCardsTwoColumnTrim() != 0)
    {
        return 1;
    }
    if (TestChatBottomFollowClearsComposer() != 0)
    {
        return 1;
    }
    if (TestChatTraceRowsShareHeight() != 0)
    {
        return 1;
    }
    if (TestCanonicalOpenAndDuplicate() != 0)
    {
        return 1;
    }
    if (TestSidebarCatalogChangeToken() != 0)
    {
        return 1;
    }
    if (TestWorkspaceLessHostTransition() != 0)
    {
        return 1;
    }
    if (TestSortedViewRegistrationAssignsStateAndRollsBack() != 0)
    {
        return 1;
    }
    if (TestSubmitSettersTakeOwnership() != 0)
    {
        return 1;
    }
    if (TestSidebarDragBehavior() != 0)
    {
        return 1;
    }
    if (TestWorkspaceBuiltinsRegisterThroughWorkspaceInit() != 0)
    {
        return 1;
    }
    if (TestFailedWorkspaceInitKeepsHostSlot() != 0)
    {
        return 1;
    }
    if (TestWorkspaceShutdownSeesOwningWorkspace() != 0)
    {
        return 1;
    }
    if (TestWorkspaceChangeSeesOwningWorkspace() != 0)
    {
        return 1;
    }
    if (TestCdOpensSelectsAndReusesWorkspace() != 0)
    {
        return 1;
    }
    if (TestBackgroundJobsSurviveWorkspaceReload() != 0)
    {
        return 1;
    }
    if (TestReloadTargetsSelectedWorkspace() != 0)
    {
        return 1;
    }
    if (TestHostReloadIgnoresWorkspaceLocalCompileFailure() != 0)
    {
        return 1;
    }
    if (TestCdResolvesAgainstCommandWorkspace() != 0)
    {
        return 1;
    }
    if (TestCdRollsBackNewWorkspaceOnAgentLimit() != 0)
    {
        return 1;
    }
    if (TestCdRejectsClosingWorkspace() != 0)
    {
        return 1;
    }
    if (TestModelChangeDoesNotMutateWorkspaceDefault() != 0)
    {
        return 1;
    }
    if (TestWorkspacePluginIsolation() != 0)
    {
        return 1;
    }
    if (TestHostPluginIsolation() != 0)
    {
        return 1;
    }
    if (TestHostSettingsPersistence() != 0)
    {
        return 1;
    }
    if (TestScopeEnforcement() != 0)
    {
        return 1;
    }
    if (TestStagingRollbackOnFailedInit() != 0)
    {
        return 1;
    }
    if (TestFailedHostReloadPreservesLiveInstances() != 0)
    {
        return 1;
    }
    if (TestWorkspaceReloadUsesLiveOwnerAndSettings() != 0)
    {
        return 1;
    }
    if (TestNestedWorkspaceExtensionOwnership() != 0)
    {
        return 1;
    }
    if (TestHeaderReloadIsAsynchronous()) return 1;
    if (TestWorkspaceLocalPollingReloadsOnlyOwner() != 0)
    {
        return 1;
    }
    if (TestHostCompileFailureQuarantinesUnchangedPoll() != 0)
    {
        return 1;
    }
    if (TestWorkspaceCompileFailureQuarantinesUnchangedPoll() != 0)
    {
        return 1;
    }
    if (TestWorkspaceLocalExtensionWithHostCallbacksRejected() != 0)
    {
        return 1;
    }
    if (TestReloadInitRollbackPreservesActiveState() != 0)
    {
        return 1;
    }
    if (TestGenerationRolloutAndDlcloseOnRelease() != 0)
    {
        return 1;
    }
    if (TestReloadReusesReleasedModuleSlots() != 0)
    {
        return 1;
    }
    if (TestScopedExtensionListingRecords() != 0)
    {
        return 1;
    }
    if (TestDualScopeIndependentPublicationRollback() != 0)
    {
        return 1;
    }
    if (TestRetainedActiveGenerationsReceiveFrameCallbacks() != 0)
    {
        return 1;
    }
    if (TestExtensionSlotsUseSourceIdentity() != 0)
    {
        return 1;
    }
    if (TestStatelessExtensionRollbackDoesNotLeakModule() != 0)
    {
        return 1;
    }
    if (TestBusyReloadQueuesAndRejectsNewWork() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceInstructionsIsolation() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceToolNameIsolation() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceMailboxIsolation() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceAskOrderingAndRouting() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceReloadAndCloseIsolation() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceStuckWorkerIsolation() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceMainAgentDelegationDrain() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceModelAndSettingsIsolation() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceFrameCallbacks() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceCloseLastMainAgentAndZeroAgents() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceAgentLimits() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceStaleIds() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceFairPumping() != 0)
    {
        return 1;
    }
    if (TestMultiWorkspaceDeletedDirectoryIntegrity() != 0)
    {
        return 1;
    }
    if (TestDiffShutdownDoesNotWaitForGit() != 0)
    {
        return 1;
    }
    if (TestUnusedPendingDraftDiscardedOnSelectedCreate() != 0)
    {
        return 1;
    }
    if (TestUnusedPendingDraftDiscardedOnSelect() != 0)
    {
        return 1;
    }
    if (TestPersistedSessionKeptOnSelect() != 0)
    {
        return 1;
    }
    if (TestModelChangeKeepsUnusedDraftOnSelect() != 0)
    {
        return 1;
    }
    if (TestAgentCloseAppliesQueuedPersistenceFailure() != 0)
    {
        return 1;
    }
    if (TestResumeLoadsStoredModel() != 0)
    {
        return 1;
    }
    if (TestSelectClearsUnseenComplete() != 0)
    {
        return 1;
    }
    if (TestUnseenCompletePersistsAcrossRestart() != 0)
    {
        return 1;
    }
    if (TestPersistenceShutdownUsesSharedDeadline() != 0)
    {
        return 1;
    }
    if (g_failed)
    {
        return 1;
    }
    /* Retained shutdown permanently retires this test process, so it is last. */
    if (TestProcessShutdownUsesSharedDeadline() != 0)
    {
        return 1;
    }
    return 0;
}
