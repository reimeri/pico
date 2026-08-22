# Auth

Register a provider so `/login` and `/logout` have something to call. Credentials live in `~/.config/pico/auth.json` (not in settings).

```c
#include "pico/plugin.h"
#include "pico/auth.h"

static void MyLogin(PicoApp *app, const char *args)
{
    /* parse args: empty, "key", "cancel", … */
    (void)args;
    pico_auth_set_active(app, "myllm", PICO_AUTH_API_KEY);
    PicoApp_AddMessage(app, PICO_ROLE_ASSISTANT, "Logged in.");
}

static void MyLogout(PicoApp *app)
{
    pico_auth_clear_oauth(app, "myllm");
}

static void MyInit(PicoApp *app)
{
    pico_add_auth(app, &(PicoAuth){
                           .provider = "myllm",
                           .help = "API key",
                           .verbs = "key cancel",
                           .login = MyLogin,
                           .logout = MyLogout,
                       });
    pico_auth_set_env_key(app, "myllm", getenv("MYLLM_API_KEY"));
}
```

`/login myllm` and `/logout myllm` dispatch by `.provider`. With one auth registration, `/login` needs no name. `.verbs` is a space-separated list offered as completions and passed through in `args`.

## Store

- Main thread: `pico_auth_copy(app, name, &e)`.
- Worker callback: `pico_auth_copy_ctx(ctx, name, &e)`.
- `e.api_key` is the environment overlay if set. Free snapshots with `pico_auth_entry_free`.
- `pico_auth_set_env_key(app, name, key)` — process-only key; not written. NULL clears.
- `pico_auth_set_oauth(app, ...)` — main-thread access/refresh/account/expiry.
- `pico_auth_set_oauth_ctx(ctx, ...)` — synchronized worker variant for provider token refresh.
- `pico_auth_set_active(app, name, PICO_AUTH_API_KEY or PICO_AUTH_OAUTH)` — which credential to use.
- `pico_auth_clear_oauth(app, name)` — drop stored tokens.

`PicoAuthEntry.active` is `"api_key"` or `"oauth"`. Mutators update memory even when the file write fails.

## Contract

- `.provider`, `.help`, `.verbs` must outlive the extension.
- `login` runs on the main thread from builtin `/login` (submit is already cancelled). `logout` is `void (*)(PicoApp *)`; `/logout` already cancels submit.
- Auth storage is process-global and mutex-protected. Worker callbacks for different agents may overlap safely when using context variants. Builtin OpenAI OAuth refresh is single-flight; waiters recheck the latest token, and a replacement fails promptly instead of racing an abandoned rotating-token exchange.
- If shutdown detaches a callback, Pico retains the auth store and skips auth destruction so callback-scoped access cannot observe freed credentials. Pico then rejects reinitialization and must exit.
- Max 16 auth providers (`PICO_MAX_AUTH`).
- Builtin reference: `app/builtins/openai.c`.
