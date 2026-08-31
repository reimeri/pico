#include "cli.h"

#include <string.h>

PicoCliParseResult PicoCli_Parse(int argc, char **argv, PicoCliOptions *out)
{
    PicoCliOptions options = {
        .session_start = PICO_SESSION_NEW,
    };
    if (!out || argc < 0 || (argc > 0 && !argv))
    {
        return PICO_CLI_ERROR;
    }
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--safe") == 0)
        {
            options.safe_mode = true;
        }
        else if (strcmp(argv[i], "--no-workspace") == 0)
        {
            options.no_workspace = true;
        }
        else if (strcmp(argv[i], "--resume") == 0)
        {
            options.session_start = PICO_SESSION_RESUME;
            options.session_option_explicit = true;
        }
        else if (strcmp(argv[i], "--no-session") == 0)
        {
            options.session_start = PICO_SESSION_NONE;
            options.session_option_explicit = true;
        }
        else if (strcmp(argv[i], "--session") == 0)
        {
            if (i + 1 >= argc)
            {
                return PICO_CLI_ERROR;
            }
            options.session_file = argv[++i];
            options.session_start = PICO_SESSION_RESUME;
            options.session_option_explicit = true;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            *out = options;
            return PICO_CLI_HELP;
        }
        else
        {
            return PICO_CLI_ERROR;
        }
    }
    *out = options;
    return PICO_CLI_OK;
}

bool PicoCli_ShouldOpenDefaultWorkspace(const PicoCliOptions *options)
{
    return options && (!options->no_workspace || options->session_option_explicit);
}
