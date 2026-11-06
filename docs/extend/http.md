# HTTP helpers

Include `pico/http.h`. `pico_http_post_sse`, `pico_http_post`, and `pico_http_get`
are blocking helpers for provider/tool workers, not the UI thread. Independent
calls can overlap; a bounded idle curl-handle pool reuses connections without
retaining request pointers, callbacks, or credentials. Connection reuse does not
provide per-session inference-cache affinity.

`PicoHttpPost` streams SSE or a bare JSON response through `on_json`. Its callback
receives the optional SSE event name and borrowed JSON bytes; false aborts delivery.
`cancel`, `on_retry`, `on_json`, and `on_headers` all receive `req.user`. The request,
body, header strings, and callback context must stay valid until the helper returns.
`PicoHttpReq` is the buffered GET/POST form and returns a malloc'd body/error.

HTTP status is returned separately from transport success: completed HTTP 4xx/5xx
responses can return `PICO_HTTP_OK`. Check both. Cancellation returns
`PICO_HTTP_CANCEL`. Default transport retries are limited to failures before any response
headers/body. Builtin providers manage bounded request recovery themselves; see
[providers](providers.md). See `pico/http.h` for deadline and retry details.

## Response headers

ABI 27 adds an optional callback on streaming requests:

```c
static void Headers(void *user, long status, const char *headers, size_t len)
{
    /* Worker thread. Parse only within len; bytes are not necessarily NUL-terminated.
       Ignore informational/redirect responses unless your protocol needs them.
       Copy only needed fields into this request/turn's owned state. */
    (void)user; (void)status; (void)headers; (void)len;
}

/* Alongside on_json/cancel and your callback context: */
/* PicoHttpPost req = { .url = url, .body = body, .on_headers = Headers, .user = ctx }; */
```

The callback runs synchronously once a complete origin response header block has
arrived, before that response's body. The bytes include the status line and final
blank line. Interim responses and redirect hops produce separate callbacks with
separate blocks; they are not concatenated into the final response. Proxy CONNECT
headers are suppressed, and trailers are not delivered as new response blocks.
An interrupted/incomplete header block does not produce a callback.

Treat headers as untrusted. Names are case-insensitive; values can contain leading
or trailing whitespace. Never interpolate control characters into outgoing headers.
Do not retain callback pointers, log authentication/routing secrets, or store them
in session JSONL or model input. Transport routing that must cross tool follow-ups
belongs in [provider turn-local state](providers.md#cache-affinity-and-turn-local-routing),
not a shared provider instance or global connection pool.

The OpenAI builtin is a working example: it captures the first valid Codex routing
token, preserves it across retries/continuations within one turn and account, and
never sends it to an API-key/custom-gateway route. See
[`../../builtins/openai.c`](../../builtins/openai.c) and
[`../../builtins/responses.c`](../../builtins/responses.c).

## Provider-managed recovery (ABI 30)

`PicoHttpPost.provider_managed_retry = true` makes the helper perform **one**
transfer, without its default connection-establishment retry loop. It does not
make a request safe to replay. In this mode an SSE EOF inside an unfinished
event is a transient partial-transfer failure, even for an EOF-delimited HTTP
body; only blank-line-terminated events are complete. `fresh_connection = true` bypasses cached
connections for that transfer. Other HTTP callers retain the default policy;
these options are not added to buffered POST/GET requests.

The optional borrowed `PicoHttpPost.failure` output points to a `PicoHttpFailure`.
It is initialized on every call and distinguishes `NONE` (completed transport or
provider callback abort), `TRANSIENT` (DNS/connect/handshake, HTTP/2 connection or
stream errors, send/receive errors, empty/partial transfers, timeouts), and
`PERMANENT` (other failures, including certificate verification and local errors).
Always check the function return and HTTP status as well: cancellation is not a
retry instruction, and a completed HTTP 503 has no transport failure. The pointer
must remain valid until the call returns; Pico never retains it. Failure text is
still malloc'd and caller-owned. Interrupted transfers do not flush an unfinished
SSE event or partial bare JSON body into `on_json`; complete events already
received have been delivered normally.

A caller opting into recovery owns replay safety, attempt cleanup, cancellation,
and a shared elapsed-time budget. Do not use this to blindly retry tools, image
generation, or arbitrary side-effecting POSTs. See
[provider recovery](providers.md#connection-retries-and-live-status).
