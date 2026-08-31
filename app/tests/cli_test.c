#include "cli.h"

#include <stdio.h>
#include <string.h>

static int g_failed;

static void Check(bool ok, const char *message)
{
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        g_failed = 1;
    }
}

static PicoCliOptions Parse(int argc, char **argv, PicoCliParseResult expected)
{
    PicoCliOptions options;
    memset(&options, 0, sizeof(options));
    Check(PicoCli_Parse(argc, argv, &options) == expected, "unexpected parse result");
    return options;
}

static void TestDefaultWorkspacePolicy(void)
{
    char *plain[] = {"pico"};
    PicoCliOptions options = Parse(1, plain, PICO_CLI_OK);
    Check(PicoCli_ShouldOpenDefaultWorkspace(&options), "plain launch should open cwd");
    Check(options.session_start == PICO_SESSION_NEW, "plain launch should create a new session");

    char *empty[] = {"pico", "--no-workspace"};
    options = Parse(2, empty, PICO_CLI_OK);
    Check(!PicoCli_ShouldOpenDefaultWorkspace(&options), "no-workspace launch should not open cwd");
}

static void TestSessionOptionsTakePrecedence(void)
{
    char *resume[] = {"pico", "--no-workspace", "--resume"};
    PicoCliOptions options = Parse(3, resume, PICO_CLI_OK);
    Check(PicoCli_ShouldOpenDefaultWorkspace(&options), "resume should require a workspace");
    Check(options.session_start == PICO_SESSION_RESUME, "resume mode");

    char *session[] = {"pico", "--session", "one.jsonl", "--no-workspace"};
    options = Parse(4, session, PICO_CLI_OK);
    Check(PicoCli_ShouldOpenDefaultWorkspace(&options), "session file should require a workspace");
    Check(options.session_file && strcmp(options.session_file, "one.jsonl") == 0, "session file path");

    char *ephemeral[] = {"pico", "--no-session", "--no-workspace"};
    options = Parse(3, ephemeral, PICO_CLI_OK);
    Check(PicoCli_ShouldOpenDefaultWorkspace(&options), "no-session should retain startup workspace behavior");
    Check(options.session_start == PICO_SESSION_NONE, "no-session mode");
}

static void TestOtherOptions(void)
{
    char *safe[] = {"pico", "--safe", "--no-workspace"};
    PicoCliOptions options = Parse(3, safe, PICO_CLI_OK);
    Check(options.safe_mode, "safe mode");
    Check(!PicoCli_ShouldOpenDefaultWorkspace(&options), "safe mode should not override no-workspace");

    char *missing[] = {"pico", "--session"};
    (void)Parse(2, missing, PICO_CLI_ERROR);
    char *unknown[] = {"pico", "--unknown"};
    (void)Parse(2, unknown, PICO_CLI_ERROR);
    char *help[] = {"pico", "--help"};
    (void)Parse(2, help, PICO_CLI_HELP);
}

int main(void)
{
    TestDefaultWorkspacePolicy();
    TestSessionOptionsTakePrecedence();
    TestOtherOptions();
    return g_failed ? 1 : 0;
}
