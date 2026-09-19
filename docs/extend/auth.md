# Auth

Register a provider so `/login` and `/logout` have something to call. Credentials live in `~/.config/pico/auth.json` (not in settings).

```c
#include "pico/plugin.h"
#include "pico/auth.h"

static void MyLogin(PicoHost *host, PicoAgentId agent_id, const char *args, void *state)
{
    /* parse args: empty, "key", "cancel", … */
    (void)args;
    (void)state;
    pico_auth_set_active(host, "myllm", PICO_AUTH_API_KEY);
    PicoHost_AddMessage(host, agent_id, PICO_ROLE_ASSISTANT, "Logged in.");
}

static void MyLogout(PicoHost *host, PicoAgentId agent_id, void *state)
{
    (void)state;
    pico_auth_clear_oauth(host, "myllm");
    PicoHost_AddMessage(host, agent_id, PICO_ROLE_ASSISTANT, "Logged out.");
}

static int MyInit(PicoHost *host, void **state_out)
{
    (void)state_out;
    pico_add_auth(host, &(PicoAuth){
                            .provider = "myllm",
                            .help = "API key",
                            .verbs = "key cancel",
                            .login = MyLogin,
                            .logout = MyLogout,
                        });
    pico_auth_set_env_key(host, "myllm", getenv("MYLLM_API_KEY"));
    return 0;
}
```

`/login myllm` and `/logout myllm` dispatch by `.provider`. With one auth registration, `/login` needs no name. With `openai`, `hyper`, and `xai` loaded, name the provider: `/login openai`, `/login hyper`, or `/login xai`. `.verbs` is a space-separated list offered as completions and passed through in `args`.

## Builtin OpenAI login

- `/login openai` or `/login openai browser` starts local browser OAuth with PKCE
  (`S256`). Pico opens the default browser with `xdg-open` and shows a clickable
  authorization link. It does not require the device-code account setting.
- The callback listener binds only to `127.0.0.1`, first on `1455`, then on `1457`
  only if the preferred port is occupied. Both use the same flow and the selected
  `http://localhost:<port>/auth/callback` URI throughout authorization and exchange.
  Pico never contacts or cancels the existing listener. If neither port is
  available, login fails with instructions to retry or choose device login.
- `/login openai device` retains device-code login for remote/headless use. There
  is no automatic fallback, SSH forwarding, pasted callback, or credential import.
- `/login openai key` selects the environment API key; `/login openai cancel`
  cancels either flow. `/logout openai` cancels login and removes OAuth credentials.
- A replacement login, key selection, logout, or host-extension replacement also
  cancels the old attempt. Cancelling a pending login leaves existing credentials
  alone; a successful exchange replaces them using the normal auth store.

OpenAI login uses a **compiled-in, core-tracked worker**, not an extension-owned
thread. Each attempt owns its cancellation wakeup and sockets; the worker touches
only attempt-owned state. The main thread displays messages for the original
agent ID, launches the browser without waiting, and commits validated initial
credentials. Obsolete attempts cannot launch a browser or commit tokens after
cancellation/replacement. Retiring attempts remain tracked until finished, even
across host reload, and consume the host's shared shutdown deadline before curl
or auth cleanup. Callback codes, PKCE verifiers, tokens, and token responses are
not put in chat or logs. Only the initial authorization link is displayed.

This builtin mechanism is private; it does **not** permit user `.so` auth
extensions to leave their own threads running across frames. See
[contracts](contracts.md#threads).

## Store

- Main thread: `pico_auth_copy(host, name, &e)`.
- Worker callback: `pico_auth_copy_ctx(ctx, name, &e)`.
- `e.api_key` is the environment overlay if set. Free snapshots with `pico_auth_entry_free`.
- `pico_auth_set_env_key(host, name, key)` — process-only key; not written. NULL clears.
- `pico_auth_set_oauth(host, ...)` — main-thread access/refresh/account/expiry.
- `pico_auth_set_oauth_ctx(ctx, ...)` — synchronized worker variant for provider token refresh.
- `pico_auth_begin_refresh_ctx(ctx, name, &out_entry)` / `pico_auth_end_refresh_ctx(ctx, name)` — coordinate single-flight token refresh across concurrent worker threads; callers refresh only when `begin` returns true, or recheck `out_entry` when false.
- `pico_auth_set_active(host, name, PICO_AUTH_API_KEY or PICO_AUTH_OAUTH)` — which credential to use.
- `pico_auth_clear_oauth(host, name)` — drop stored tokens.

`PicoAuthEntry.active` is `"api_key"` or `"oauth"`. Mutators update memory even when the file write fails.

## Contract

- `.provider`, `.help`, `.verbs` must outlive the extension.
- `login` runs on the main thread from builtin `/login` (submit is already cancelled) and receives the snapshotted `PicoAgentId` from that submit. Route user-visible notes to that ID; do not call `pico_agent_active`. Asynchronous login text must keep the same ID, so a later selection change cannot append results to another agent. `logout` is `void (*)(PicoHost *, PicoAgentId, void *)` and uses the same snapshotted ID; `/logout` already cancels submit.
- Auth storage is process-global and mutex-protected. Worker callbacks for different agents may overlap safely when using context variants. Builtin OpenAI, Hyper, and xAI OAuth refresh are each single-flight; waiters recheck the latest token, and a replacement fails promptly instead of racing an abandoned rotating-token exchange.
- If shutdown detaches a callback, Pico retains the auth store and skips auth destruction so callback-scoped access cannot observe freed credentials. Pico then rejects reinitialization and must exit.
- Max 16 auth providers (`PICO_MAX_AUTH`).
- Builtin OpenAI, Hyper, and xAI: [`../../builtins/openai.c`](../../builtins/openai.c) (uses [`openai_auth.c`](../../builtins/openai_auth.c)), [`../../builtins/hyper.c`](../../builtins/hyper.c) (`HYPER_API_KEY`, `/login hyper`), [`../../builtins/xai.c`](../../builtins/xai.c) (`XAI_API_KEY`, `/login xai`).
