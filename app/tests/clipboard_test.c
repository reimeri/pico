#include "test_wait.h"
#include "pico/host.h"
#include "composer_internal.h"
#include "host_internal.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <stdatomic.h>

static int s_platform;
static const char *s_fallback;
static int s_fallback_calls;
static atomic_bool s_woken;

int __real_glfwGetPlatform(void);
int __wrap_glfwGetPlatform(void)
{
    return s_platform ? s_platform : __real_glfwGetPlatform();
}

const char *__real_GetClipboardText(void);
const char *__wrap_GetClipboardText(void)
{
    if (!s_platform) return __real_GetClipboardText();
    s_fallback_calls++;
    return s_fallback;
}

void __real_glfwPostEmptyEvent(void);
void __wrap_glfwPostEmptyEvent(void)
{
    if (s_platform) atomic_store(&s_woken, true);
    else __real_glfwPostEmptyEvent();
}

static bool ClipboardWrite(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static void ClipboardDrain(PicoHost *host)
{
    PICO_TEST_WAIT(host->tasks)
    {
        pico_host_pump(host);
    }
}

/* Wait for the worker, deliberately without pumping the host. This guards
 * against clipboard progress depending on successive UI frame intervals. */
static bool ClipboardWaitWake(void)
{
    PICO_TEST_WAIT_LOOP("asynchronous completion")
    {
        if (atomic_load(&s_woken)) return true;
    }
    return false;
}

static void ClipboardArmWake(void)
{
    PicoHost_EnableWakeups(false);
    atomic_store(&s_woken, false);
    PicoHost_EnableWakeups(true);
}

int TestClipboardPaste(void)
{
    static const char helper[] =
        "#!/bin/sh\n"
        "printf '%s %s\\n' \"$0\" \"$*\" >>\"$PICO_CLIP_FIXTURE/calls\"\n"
        "case \"$*\" in\n"
        "  '--list-types'|'-selection clipboard -o -t TARGETS')\n"
        "    if [ \"$PICO_CLIP_MODE\" = stall ]; then\n"
        "      : >\"$PICO_CLIP_FIXTURE/started\"; while :; do :; done\n"
        "    fi\n"
        "    if [ \"$PICO_CLIP_MODE\" = image ]; then printf 'image/png\\n'; fi\n"
        "    if [ \"$PICO_CLIP_MODE\" = latin1 ]; then printf 'STRING\\n';\n"
        "    else printf 'text/plain;charset=utf-8\\n'; fi;;\n"
        "  '--no-newline --type text/plain;charset=utf-8'|'-selection clipboard -o -t text/plain;charset=utf-8')\n"
        "    if [ \"$PICO_CLIP_MODE\" = fail_data ]; then exit 1; fi\n"
        "    printf 'first line\\n\\nlast line\\n';;\n"
        "  '--no-newline --type image/png'|'-selection clipboard -o -t image/png')\n"
        "    printf '\\211\\120\\116\\107\\015\\012\\032\\012\\000\\000\\000\\015\\111\\110\\104\\122\\000\\000\\000\\001\\000\\000\\000\\001\\010\\004\\000\\000\\000\\265\\034\\014\\002\\000\\000\\000\\013\\111\\104\\101\\124\\170\\332\\143\\374\\377\\037\\000\\003\\003\\002\\000\\357\\243\\177\\133\\000\\000\\000\\000\\111\\105\\116\\104\\256\\102\\140\\202';;\n"
        "  '-selection clipboard -o -t STRING') printf 'caf\\351';;\n"
        "  *) exit 1;;\n"
        "esac\n";
    char dir[] = "/tmp/pico-clipboard-XXXXXX";
    char cfg[4096], wl[4096], xclip[4096], calls[4096], started[4096];
    char *old_path = getenv("PATH") ? JsonDup(getenv("PATH")) : NULL;
    char *old_config = getenv("XDG_CONFIG_HOME") ? JsonDup(getenv("XDG_CONFIG_HOME")) : NULL;
    PicoHost *host = NULL;
    const char *phase = "setup";
    int rc = 1;
    if (!mkdtemp(dir)) goto done;
    snprintf(cfg, sizeof(cfg), "%s/config", dir);
    snprintf(wl, sizeof(wl), "%s/wl-paste", dir);
    snprintf(xclip, sizeof(xclip), "%s/xclip", dir);
    snprintf(calls, sizeof(calls), "%s/calls", dir);
    snprintf(started, sizeof(started), "%s/started", dir);
    if (mkdir(cfg, 0700) != 0 || !ClipboardWrite(wl, helper) || !ClipboardWrite(xclip, helper) ||
        chmod(wl, 0700) != 0 || chmod(xclip, 0700) != 0) goto done;
    setenv("XDG_CONFIG_HOME", cfg, 1);
    if (pico_host_init(&host, NULL, true) != PICO_OK) goto done;
    PICO_TEST_WAIT(!PicoPlugins_HostState(host, "composer"))
    {
        PicoPlugins_Load(host);
    }
    if (!PicoPlugins_HostState(host, "composer")) goto done;
    PicoWorkspaceId ws;
    if (pico_workspace_open(host, dir, &ws) != PICO_OK) goto done;
    PicoAgentCreateOptions options = {.kind = PICO_AGENT_MAIN, .session_start = PICO_SESSION_NONE, .select = true};
    PicoAgentId agent;
    if (pico_main_agent_create(host, ws, &options, &agent) != PICO_OK) goto done;
    /* Keep the wake assertion specific to clipboard completion. */
    PicoPlugins_StopScanner(host);
    ClipboardDrain(host);
    setenv("PATH", dir, 1);
    setenv("PICO_CLIP_FIXTURE", dir, 1);
    setenv("PICO_CLIP_MODE", "text", 1);
    s_platform = GLFW_PLATFORM_WAYLAND;
    s_fallback = "fallback";

    phase = "multiline text completes and wakes without UI pumping";
    ClipboardArmWake();
    PicoComposer_BeginClipboardPaste(host);
    if (!ClipboardWaitWake()) goto done;
    ClipboardDrain(host);
    if (!host->composer.text || strcmp(host->composer.text, "first line\n\nlast line\n") != 0 ||
        PicoComposer_HasAttachments(host)) goto done;
    FILE *log = fopen(calls, "rb");
    if (!log) goto done;
    char commands[4096] = {0};
    size_t n = fread(commands, 1, sizeof(commands) - 1, log);
    fclose(log);
    commands[n] = '\0';
    if (strstr(commands, "image/") || strstr(commands, "/xclip")) goto done;

    phase = "image wins over offered text";
    host->composer.text[0] = '\0';
    host->composer.length = host->composer.cursor = host->composer.sel_anchor = 0;
    setenv("PICO_CLIP_MODE", "image", 1);
    PicoComposer_BeginClipboardPaste(host);
    ClipboardDrain(host);
    if (!PicoComposer_HasAttachments(host) || host->composer.length != 0) goto done;
    PicoComposer_DiscardAttachments();

    phase = "X11 uses only its active backend";
    unlink(calls);
    setenv("PICO_CLIP_MODE", "text", 1);
    s_platform = GLFW_PLATFORM_X11;
    PicoComposer_BeginClipboardPaste(host);
    ClipboardDrain(host);
    log = fopen(calls, "rb");
    if (!log) goto done;
    n = fread(commands, 1, sizeof(commands) - 1, log);
    fclose(log);
    commands[n] = '\0';
    if (!host->composer.text || strcmp(host->composer.text, "first line\n\nlast line\n") != 0 ||
        strstr(commands, "/wl-paste")) goto done;

    phase = "X11 Latin-1 STRING is converted to UTF-8";
    host->composer.text[0] = '\0';
    host->composer.length = host->composer.cursor = host->composer.sel_anchor = 0;
    setenv("PICO_CLIP_MODE", "latin1", 1);
    PicoComposer_BeginClipboardPaste(host);
    ClipboardDrain(host);
    if (strcmp(host->composer.text, "caf\xc3\xa9") != 0) goto done;

    phase = "missing helper falls back to Raylib text";
    host->composer.text[0] = '\0';
    host->composer.length = host->composer.cursor = host->composer.sel_anchor = 0;
    s_platform = GLFW_PLATFORM_WAYLAND;
    setenv("PICO_CLIP_MODE", "text", 1);
    unlink(wl);
    PicoComposer_BeginClipboardPaste(host);
    ClipboardDrain(host);
    if (strcmp(host->composer.text, s_fallback) != 0) goto done;

    phase = "presentation switch discards a fetched result awaiting adoption";
    if (!ClipboardWrite(wl, helper) || chmod(wl, 0700) != 0) goto done;
    ClipboardArmWake();
    PicoComposer_BeginClipboardPaste(host);
    if (!ClipboardWaitWake()) goto done;
    PicoComposer_ResetPresentation(host);
    ClipboardDrain(host);
    if (strcmp(host->composer.text, s_fallback) != 0) goto done;

    phase = "cancellation stops a live clipboard helper";
    if (!ClipboardWrite(wl, helper) || chmod(wl, 0700) != 0) goto done;
    setenv("PICO_CLIP_MODE", "stall", 1);
    PicoComposer_BeginClipboardPaste(host);
    PICO_TEST_WAIT(access(started, F_OK) != 0) ;
    if (access(started, F_OK) != 0) goto done;
    PicoComposer_CancelClipboardPaste();
    if (PicoComposer_ClipboardPasteBusy()) goto done;
    ClipboardDrain(host);
    if (host->tasks || strcmp(host->composer.text, s_fallback) != 0) goto done;
    phase = "stalled owner times out without synchronous clipboard fallback";
    int fallback_calls = s_fallback_calls;
    free(host->status_warn);
    host->status_warn = NULL;
    PicoComposer_BeginClipboardPaste(host);
    ClipboardDrain(host);
    if (host->tasks || s_fallback_calls != fallback_calls || !host->status_warn ||
        strcmp(host->composer.text, s_fallback) != 0) goto done;
    phase = "failed transfer warns without synchronous fallback";
    setenv("PICO_CLIP_MODE", "fail_data", 1);
    free(host->status_warn);
    host->status_warn = NULL;
    PicoComposer_BeginClipboardPaste(host);
    ClipboardDrain(host);
    if (host->tasks || s_fallback_calls != fallback_calls || !host->status_warn ||
        strcmp(host->composer.text, s_fallback) != 0) goto done;
    rc = 0;
done:
    PicoHost_EnableWakeups(false);
    if (old_path) setenv("PATH", old_path, 1); else unsetenv("PATH");
    if (host) pico_host_free(host);
    s_platform = 0;
    s_fallback = NULL;
    if (old_config) setenv("XDG_CONFIG_HOME", old_config, 1); else unsetenv("XDG_CONFIG_HOME");
    unsetenv("PICO_CLIP_FIXTURE");
    unsetenv("PICO_CLIP_MODE");
    free(old_path);
    free(old_config);
    if (rc) fprintf(stderr, "FAIL: clipboard paste: %s\n", phase);
    /* Remove nested workspace/config fixture files without touching user data. */
    if (dir[0] && access(dir, F_OK) == 0)
    {
        char command[8192];
        snprintf(command, sizeof(command), "rm -rf '%s'", dir);
        int cleanup_result = system(command);
        if (cleanup_result != 0) rc = 1;
    }
    return rc;
}
#else
int TestClipboardPaste(void) { return 0; }
#endif
