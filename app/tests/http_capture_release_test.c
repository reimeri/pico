#define _POSIX_C_SOURCE 200809L

#include "http_capture.h"
#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    char temp[] = "/tmp/pico-http-capture-release-XXXXXX";
    if (!mkdtemp(temp) || setenv("XDG_CONFIG_HOME", temp, 1) != 0)
    {
        fprintf(stderr, "release capture: fixture setup failed\n");
        return 1;
    }

    PicoHttpCapture capture = {0};
    PicoHttpCapture_Begin(&capture);
    PicoHttpCapture_Write(&capture, "data: secret\n\n", sizeof("data: secret\n\n") - 1);
    PicoHttpCapture_Finish(&capture, "https://example.test", 200, "completed", 0, "");

    char debug[4096];
    bool path_ok = PicoPath_Format(debug, sizeof(debug), "%s/pico/debug", temp);
    bool disabled = path_ok && access(debug, F_OK) != 0;
    unsetenv("XDG_CONFIG_HOME");
    rmdir(temp);
    if (!disabled)
    {
        fprintf(stderr, "release capture: non-Debug build wrote capture files\n");
        return 1;
    }
    return 0;
}
