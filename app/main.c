// Pico — a small C agent harness. Clay is only the layout library.

#define CLAY_IMPLEMENTATION
#include "clay/clay.h"
#include "../clay/renderers/raylib/clay_renderer_raylib.c"
#include "host_internal.h"
#include "theme_internal.h"

#include "pico/app.h"
#include "agent_internal.h"
#include "docs_path.h"
#include "richtext.h"
#include "cli.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Raylib owns the input snapshot, but GLFW can dispatch events inside the
 * explicit pre-wait PollInputEvents(). Track those callbacks without reading
 * (and consuming) Raylib's pressed-character/key queues. */
static bool s_input_polled;
static GLFWkeyfun s_raylib_key;
static GLFWcharfun s_raylib_char;
static GLFWmousebuttonfun s_raylib_mouse_button;
static GLFWcursorposfun s_raylib_cursor_pos;
static GLFWcursorenterfun s_raylib_cursor_enter;
static GLFWscrollfun s_raylib_scroll;

static void OnKey(GLFWwindow *window, int key, int scan, int action, int mods)
{
    s_input_polled = true;
    if (s_raylib_key) s_raylib_key(window, key, scan, action, mods);
}

static void OnChar(GLFWwindow *window, unsigned int codepoint)
{
    s_input_polled = true;
    if (s_raylib_char) s_raylib_char(window, codepoint);
}

static void OnMouseButton(GLFWwindow *window, int button, int action, int mods)
{
    s_input_polled = true;
    if (s_raylib_mouse_button) s_raylib_mouse_button(window, button, action, mods);
}

static void OnCursorPos(GLFWwindow *window, double x, double y)
{
    s_input_polled = true;
    if (s_raylib_cursor_pos) s_raylib_cursor_pos(window, x, y);
}

static void OnCursorEnter(GLFWwindow *window, int entered)
{
    s_input_polled = true;
    if (s_raylib_cursor_enter) s_raylib_cursor_enter(window, entered);
}

static void OnScroll(GLFWwindow *window, double x, double y)
{
    s_input_polled = true;
    if (s_raylib_scroll) s_raylib_scroll(window, x, y);
}

static void WatchPolledInput(void)
{
    GLFWwindow *window = (GLFWwindow *)GetWindowHandle();
    s_raylib_key = glfwSetKeyCallback(window, OnKey);
    s_raylib_char = glfwSetCharCallback(window, OnChar);
    s_raylib_mouse_button = glfwSetMouseButtonCallback(window, OnMouseButton);
    s_raylib_cursor_pos = glfwSetCursorPosCallback(window, OnCursorPos);
    s_raylib_cursor_enter = glfwSetCursorEnterCallback(window, OnCursorEnter);
    s_raylib_scroll = glfwSetScrollCallback(window, OnScroll);
}

static void FputsQuoted(FILE *out, const char *s)
{
    fputc('\'', out);
    for (; s && *s; s++)
    {
        if (*s == '\'')
        {
            fputs("'\\''", out);
        }
        else
        {
            fputc(*s, out);
        }
    }
    fputc('\'', out);
}

static void PrintUsage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--safe] [--no-workspace] [--resume] [--no-session] [--session FILE]\n"
            "  --safe          load builtin UI only (skip ~/.config/pico/extensions and .pico/extensions)\n"
            "  --no-workspace  start without opening the current directory\n"
            "  --resume        continue the most recent session for this directory\n"
            "  --no-session  do not persist a JSONL session file\n"
            "  --session F   open an existing session file\n"
            "  -h            this help\n"
            "\n"
            "Auth:\n"
            "  PICO_API_KEY / OPENAI_API_KEY     API-key auth for the openai provider\n"
            "  HYPER_API_KEY                     API-key auth for the hyper provider\n"
            "  XAI_API_KEY                       API-key auth for the xai provider\n"
            "  PICO_MODEL                        default gpt-4o\n"
            "  PICO_EFFORT                       override selected_effort of the active model\n"
            "  PICO_FONT_SCALE                   override font_scale (0.5-3.0, default 1.0)\n"
            "  ~/.config/pico/settings.json      {model, models, compact_at, resume_last, font_scale,\n"
            "                                    chat_width, disabled_extensions, disabled_host_extensions}\n"
            "  <workspace>/.pico/settings.json   workspace model/defaults override\n"
            "  ~/.config/pico/auth.json          per-provider credentials (api_key or oauth)\n"
            "  /login openai [browser]           Local browser login (ChatGPT subscription)\n"
            "  /login openai device              Device-code login (remote/headless)\n"
            "  /login openai key|cancel          Select API key or cancel login\n"
            "  /login hyper                      Hyper device-code (Charm subscription)\n"
            "  /login xai                        xAI device-code (SuperGrok / X Premium)\n"
            "  models[].provider                 LLM extension name (e.g. openai, hyper, xai)\n"
            "  models[].base_url                 optional; omit to use the extension default\n"
            "  ~/.config/pico/SYSTEM.md          optional system prompt\n"
            "  <workspace>/AGENTS.md             optional project instructions\n"
            "  ~/.config/pico/sessions/          JSONL transcripts (Pi-style path encoding)\n"
            "  ~/.config/pico/subagents/         named JSONC subagent profiles (see /docs subagents)\n"
            "  /cd DIR                           open or select a workspace; previous stays open\n"
            "  F2                                 open the extension manager\n"
            "  F5 or /reload                     reload host extensions and the selected workspace\n",
            argv0);
}

int main(int argc, char **argv)
{
    PicoCliOptions options;
    PicoCliParseResult parsed = PicoCli_Parse(argc, argv, &options);
    if (parsed != PICO_CLI_OK)
    {
        PrintUsage(argv[0]);
        return parsed == PICO_CLI_HELP ? 0 : 1;
    }

    Pico_PathsInit(GetApplicationDirectory());

    if (!Pico_InitClay((Clay_Dimensions){1100, 800}))
    {
        fprintf(stderr, "Pico could not initialize Clay.\n");
        return 1;
    }
    Clay_Raylib_Initialize(1100, 800, "Pico", FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    SetExitKey(KEY_NULL);
    WatchPolledInput();

    char workspace[4096];
    if (PicoCli_ShouldOpenDefaultWorkspace(&options) && !getcwd(workspace, sizeof(workspace)))
    {
        snprintf(workspace, sizeof(workspace), ".");
    }
    Font fonts[FONT_COUNT];
    Pico_LoadFonts(fonts);
    Clay_SetMeasureTextFunction(Pico_MeasureTextUtf8, fonts);
    RichText_SetMeasureFunction(Pico_MeasureTextUtf8, fonts);

    PicoHost *app = NULL;
    if (pico_host_init(&app, fonts, options.safe_mode) != PICO_OK || !app)
    {
        fprintf(stderr, "Pico could not initialize.\n");
        Pico_FreeClay();
        Pico_UnloadFonts(fonts);
        Clay_Raylib_Close();
        return 1;
    }
    PicoHost_EnableWakeups(true);
    if (PicoCli_ShouldOpenDefaultWorkspace(&options))
    {
        PicoHost_Start(app, fonts, workspace, options.safe_mode, options.session_start, options.session_file);
    }
    else
    {
        PicoPlugins_Load(app);
    }
    while (!PicoHost_ShouldExit(app) && !WindowShouldClose())
    {
        double frame_start = GetTime();
        PicoHost_Frame(app);
        if (!app->frame_presented && !PicoHost_ShouldExit(app))
        {
            /* EndDrawing normally paces a presented frame and polls Raylib
             * input. A non-presented frame means nothing visible changed, so
             * instead of the display cadence, block until the nearest real
             * deadline: a scheduled redraw (caret blink) when one is due, and
             * never longer than the idle floor, which keeps polling on_frame
             * callbacks and any missed worker completion responsive. Input,
             * window close, and pico_host_wakeup() interrupt the wait.
             *
             * Raylib snapshots previous input and clears its pressed queues
             * before polling GLFW. Do this before a blocking wait: GLFW
             * callbacks dispatched by that wait must remain visible to the
             * next PicoHost_Frame(), not be cleared on the way there. */
            s_input_polled = false;
            PollInputEvents();
            if (!s_input_polled && !IsWindowResized() &&
                IsWindowFocused() == app->window_focused && !WindowShouldClose())
            {
                double remaining = PICO_HOST_IDLE_WAIT_CAP - (GetTime() - frame_start);
                if (app->redraw_at > 0.0)
                {
                    double to_redraw = app->redraw_at - GetTime();
                    if (to_redraw < remaining)
                        remaining = to_redraw;
                }
                PicoHost_WaitIdle(app, remaining);
            }
        }
    }

    char session_path[4096];
    session_path[0] = '\0';
    const PicoAgent *active = PicoHost_SelectedAgentConst(app);
    if (active && active->persistence != PICO_SESSION_EPHEMERAL && active->session_path[0])
    {
        snprintf(session_path, sizeof(session_path), "%s", active->session_path);
    }
    PicoHost_EnableWakeups(false);
    PicoHostShutdownResult shutdown = pico_host_free(app);

    Pico_FreeClay();
    Pico_UnloadFonts(fonts);
    Clay_Raylib_Close();
    if (shutdown == PICO_HOST_SHUTDOWN_RETAINED)
    {
        fprintf(stderr, "Pico retained a blocked worker and is exiting without unloading extensions.\n");
    }
    if (session_path[0] && access(session_path, F_OK) == 0)
    {
        fprintf(stderr, "Resume: ");
        FputsQuoted(stderr, argv[0]);
        fprintf(stderr, " --session ");
        FputsQuoted(stderr, session_path);
        fputc('\n', stderr);
    }
    return 0;
}
