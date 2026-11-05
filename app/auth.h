#ifndef AUTH_H
#define AUTH_H

#include "pico/auth.h"

/* Main-thread builtin operation: saves a key without changing active auth.
 * Environment precedence is retained. Like other auth mutators, false means
 * persistence failed (memory may still have changed). */
bool PicoAuth_SetApiKey(PicoHost *host, const char *provider, const char *key, bool *env_override);

#endif
