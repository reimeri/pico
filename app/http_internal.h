#ifndef PICO_HTTP_INTERNAL_H
#define PICO_HTTP_INTERNAL_H
#include <stdbool.h>
/* Register a host after curl_global_init succeeds. */
void PicoHttp_HostStarted(void);
/* Close idle handles when the last host exits, before curl_global_cleanup. */
/* False means a caller still owns a live transfer; defer curl global cleanup. */
bool PicoHttp_ShutdownConnections(void);
#endif
