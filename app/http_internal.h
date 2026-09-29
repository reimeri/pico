#ifndef PICO_HTTP_INTERNAL_H
#define PICO_HTTP_INTERNAL_H
#include <stdbool.h>
/* Release idle transport handles before host curl_global_cleanup. */
/* False means a caller still owns a live transfer; defer curl global cleanup. */
bool PicoHttp_ShutdownConnections(void);
#endif
