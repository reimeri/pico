// Pico — a small C agent harness. Clay is only the layout library.

#define CLAY_IMPLEMENTATION
#include "clay/clay.h"
#include "../clay/renderers/raylib/clay_renderer_raylib.c"
#include "host_internal.h"

#include "pico/app.h"
#include "agent_internal.h"
#include "docs_path.h"
#include "richtext.h"
#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
            "  /login openai                     Codex device-code (ChatGPT subscription)\n"
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

    uint64_t total_memory_size = Clay_MinMemorySize();
    Clay_Arena clay_memory = Clay_CreateArenaWithCapacityAndMemory(total_memory_size, malloc(total_memory_size));
    Clay_Initialize(clay_memory, (Clay_Dimensions){1100, 800}, (Clay_ErrorHandler){Pico_HandleClayErrors, 0});
    Clay_Raylib_Initialize(1100, 800, "Pico", FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    SetExitKey(KEY_NULL);

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
        Pico_UnloadFonts(fonts);
        Clay_Raylib_Close();
        return 1;
    }
    if (PicoCli_ShouldOpenDefaultWorkspace(&options))
    {
        PicoHost_Start(app, fonts, workspace, options.safe_mode, options.session_start, options.session_file);
    }
    else
    {
        PicoPlugins_Load(app);
    }
    while (!WindowShouldClose())
    {
        if (Pico_NeedsClayReinit())
        {
            Pico_ReinitClay(fonts, app->debug_enabled);
        }
        PicoHost_Frame(app);
    }

    char session_path[4096];
    session_path[0] = '\0';
    const PicoAgent *active = PicoHost_SelectedAgentConst(app);
    if (active && active->persistence != PICO_SESSION_EPHEMERAL && active->session_path[0] &&
        access(active->session_path, F_OK) == 0)
    {
        snprintf(session_path, sizeof(session_path), "%s", active->session_path);
    }
    PicoHostShutdownResult shutdown = pico_host_free(app);

    Pico_UnloadFonts(fonts);
    Clay_Raylib_Close();
    if (shutdown == PICO_HOST_SHUTDOWN_RETAINED)
    {
        fprintf(stderr, "Pico retained a blocked worker and is exiting without unloading extensions.\n");
    }
    if (session_path[0])
    {
        fprintf(stderr, "Resume: ");
        FputsQuoted(stderr, argv[0]);
        fprintf(stderr, " --session ");
        FputsQuoted(stderr, session_path);
        fputc('\n', stderr);
    }
    return 0;
}
