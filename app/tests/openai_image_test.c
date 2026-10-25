#define _XOPEN_SOURCE 700
#include "builtins/openai_image.h"
#include "builtins/openai_auth.h"
#include "pico/agent.h"
#include "pico/auth.h"
#include "http_internal.h"
#include "image_fixtures.h"
#include "json.h"
#include "path.h"

#include <dirent.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct PicoAgentContext { const char *workspace; bool vision, cancelled; };
static PicoToolFn tool;
static PicoLlmHookFn filter;
static int failed, requests, refreshes;
static bool oauth = true, expired, refresh_fails, cancel_on_response;
static long status = 200;
static int transport = PICO_HTTP_OK;
static char *response, *request_body;
static char url[256], bearer[128];
static bool first_401, always_401;
static const char *race_destination;

static void Check(bool ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); failed = 1; }
}

bool pico_add_tool(PicoWorkspace *ws, const char *name, const char *description, const char *params,
                   PicoToolFn run, PicoToolApplyFn apply, PicoToolExecution execution)
{
    (void)ws; (void)description; (void)apply; (void)execution;
    Check(!strcmp(name, "generate_image") && JsonValidSyntax(params, strlen(params)), "register valid image tool schema");
    tool = run; return true;
}
bool pico_set_tool_group_title(PicoWorkspace *ws, const char *name, const char *one, const char *many)
{ (void)ws; (void)name; (void)one; (void)many; return true; }
void pico_add_llm_hook(PicoWorkspace *ws, PicoLlmHookFn hook) { (void)ws; filter = hook; }
PicoHost *pico_workspace_host(PicoWorkspace *ws) { (void)ws; return NULL; }
const char *pico_agent_context_workspace(const PicoAgentContext *ctx) { return ctx->workspace; }
bool pico_agent_context_accepts_tool_images(const PicoAgentContext *ctx) { return ctx->vision; }
bool pico_agent_context_cancelled(const PicoAgentContext *ctx) { return ctx->cancelled; }

void pico_auth_entry_free(PicoAuthEntry *e)
{
    free(e->access_token); free(e->refresh_token); free(e->api_key); free(e->account_id);
    memset(e, 0, sizeof(*e));
}
bool pico_auth_copy(PicoHost *host, const char *provider, PicoAuthEntry *out)
{
    (void)host; (void)provider;
    memset(out, 0, sizeof(*out));
    snprintf(out->active, sizeof(out->active), "%s", oauth ? PICO_AUTH_OAUTH : PICO_AUTH_API_KEY);
    out->access_token = JsonDup("test-access"); out->refresh_token = JsonDup("test-refresh");
    out->account_id = JsonDup("test-account"); out->api_key = JsonDup("never-use-api-key");
    out->expires_at = time(NULL) + (expired ? -60 : 3600);
    return true;
}
bool pico_auth_copy_ctx(PicoAgentContext *ctx, const char *provider, PicoAuthEntry *out)
{ (void)ctx; return pico_auth_copy(NULL, provider, out); }
bool pico_openai_refresh_oauth(PicoAgentContext *ctx, PicoAuthEntry *auth, PicoHttpCancelFn cancel, void *user, bool force)
{
    (void)ctx;
    refreshes++;
    Check(force == !expired, "refresh distinguishes rejected tokens from expiry");
    if (refresh_fails || cancel(user)) return false;
    free(auth->access_token); auth->access_token = JsonDup("refreshed-access");
    return true;
}

static bool Write(const char *path, const void *bytes, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(bytes, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static bool SameFile(const char *path, const void *bytes, size_t n)
{
    size_t size = 0; char *data = Pico_ReadFile(path, &size);
    bool ok = data && size == n && !memcmp(data, bytes, n); free(data); return ok;
}
int PicoHttp_PostOnce(const PicoHttpReq *req, long timeout, size_t limit, long *http, char **body, char **error)
{
    (void)timeout; (void)limit;
    requests++;
    if (error) *error = NULL;
    Check(!req->cancel(req->user), "cancelled calls never reach the image backend");
    free(request_body); request_body = JsonDup(req->body);
    snprintf(url, sizeof(url), "%s", req->url);
    bool account = false, originator = false, turn = false;
    bearer[0] = 0;
    for (int i = 0; i < req->header_count; i++) {
        if (!strncmp(req->headers[i], "Authorization:", 14)) snprintf(bearer, sizeof(bearer), "%s", req->headers[i]);
        account |= !strcmp(req->headers[i], "ChatGPT-Account-Id: test-account");
        originator |= !strcmp(req->headers[i], "originator: pico");
        turn |= !strncmp(req->headers[i], "x-codex-image-turn-id: ", 23);
    }
    Check(account && originator && turn && !strstr(bearer, "never-use-api-key"), "Codex request has OAuth/account/client headers, never the API key");
    if (race_destination) Check(Write(race_destination, "racing writer", 13), "simulate destination appearing while request runs");
    if (cancel_on_response) ((PicoAgentContext *)req->user)->cancelled = true;
    *http = always_401 || (first_401 && requests == 1) ? 401 : status;
    *body = JsonDup(response);
    return transport;
}

static void Fixture(const void *bytes, size_t size)
{
    char *base64 = malloc(4 * ((size + 2) / 3) + 1);
    EVP_EncodeBlock((unsigned char *)base64, bytes, (int)size);
    JsonBuf b; JsonBuf_Init(&b); JsonBuf_Puts(&b, "{\"data\":[{\"b64_json\":");
    JsonBuf_String(&b, base64); JsonBuf_Puts(&b, "}]}"); free(base64);
    free(response); response = JsonBuf_Steal(&b);
}
static void Reset(PicoAgentContext *ctx)
{
    requests = refreshes = 0; oauth = true; expired = refresh_fails = cancel_on_response = false;
    ctx->cancelled = false; ctx->vision = true;
    status = 200; transport = PICO_HTTP_OK; first_401 = always_401 = false; race_destination = NULL;
    Fixture(image_fixtures[0].data, image_fixtures[0].size);
}
static PicoToolResult Run(PicoAgentContext *ctx, const char *args)
{ PicoToolResult out = {0}; tool(ctx, args, &out, NULL); return out; }
static void Free(PicoToolResult *out)
{
    Check(!out->output || (!strstr(out->output, "test-access") && !strstr(out->output, "test-refresh") &&
          !strstr(out->output, "refreshed-access") && !strstr(out->output, "never-use-api-key")), "credentials never enter tool output");
    for (int i = 0; i < out->image_count; i++) free(out->images[i].data);
    free(out->images); free(out->output); free(out->details_json); free(out->media_namespace);
}
static void NoOutput(const char *directory)
{
    DIR *d = opendir(directory); struct dirent *e; bool empty = true;
    while (d && (e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) empty = false;
    if (d) closedir(d);
    Check(d && empty, "failure leaves no output or staging files");
}

int main(void)
{
    char root[] = "/tmp/pico-image-generation-XXXXXX", path[4096], other[4096];
    if (!mkdtemp(root)) return 1;
    PicoAgentContext ctx = {.workspace = root, .vision = true};
    Check(pico_openai_image_init(NULL) == 0 && tool && filter, "image tool initializes");
    const char *args = "{\"prompt\":\"a blue circle\",\"output_path\":\"result.png\"}";
    PicoPath_Format(path, sizeof(path), "%s/result.png", root);
    Reset(&ctx);
    PicoToolResult out = Run(&ctx, args);
    Check(!out.is_error && out.image_count == 1 && out.images && requests == 1 &&
          SameFile(path, image_fixtures[0].data, image_fixtures[0].size) &&
          out.images[0].size == image_fixtures[0].size && !memcmp(out.images[0].data, image_fixtures[0].data, out.images[0].size) &&
          strstr(url, "/images/generations") && strstr(out.output, path), "generation saves unchanged PNG relative to workspace and returns an image");
    Free(&out);
    int previous = requests; out = Run(&ctx, args);
    Check(out.is_error && requests == previous && SameFile(path, image_fixtures[0].data, image_fixtures[0].size), "existing output is rejected before spending usage");
    Free(&out);

    Reset(&ctx);
    out = Run(&ctx, "{\"prompt\":\"make it green\",\"output_path\":\"edited.png\",\"referenced_image_paths\":[\"result.png\"],\"transparent_background\":true}");
    JsonDoc doc; bool parsed = !JsonParse(&doc, request_body, strlen(request_body));
    int refs = parsed ? JsonObjGet(&doc, 0, "images") : -1;
    char *ref = parsed ? JsonObjStr(&doc, JsonArrayAt(&doc, refs, 0), "image_url") : NULL;
    Check(!out.is_error && strstr(url, "/images/edits") && ref && !strncmp(ref, "data:image/png;base64,", 22) &&
          JsonEq(&doc, JsonObjGet(&doc, 0, "background"), "transparent"), "local references use JSON edit protocol and transparency");
    free(ref); if (parsed) JsonFree(&doc); Free(&out);
    PicoPath_Format(other, sizeof(other), "%s/edited.png", root); unlink(other); unlink(path);

    Reset(&ctx); ctx.vision = false;
    JsonBuf absolute; JsonBuf_Init(&absolute); JsonBuf_Puts(&absolute, "{\"prompt\":\"icon\",\"output_path\":");
    JsonBuf_String(&absolute, path); JsonBuf_Putc(&absolute, '}');
    out = Run(&ctx, absolute.data); JsonBuf_Free(&absolute);
    Check(!out.is_error && out.image_count == 0 && SameFile(path, image_fixtures[0].data, image_fixtures[0].size) && strstr(out.output, "No visual preview"),
          "non-vision callers receive a saved file without unsupported image parts; absolute paths work");
    Free(&out); unlink(path);

    Reset(&ctx); oauth = false;
    PicoTool catalog[] = {{.name = "generate_image"}, {.name = "another_tool"}}; bool excluded[2] = {0};
    PicoLlmEvent event = {.include_tools = true, .tools = catalog, .tool_count = 2, .exclude = excluded};
    filter(NULL, 0, &event, NULL);
    out = Run(&ctx, args);
    Check(excluded[0] && !excluded[1] && out.is_error && requests == 0, "API-key selection hides and refuses subscription tool even when OAuth tokens remain");
    Free(&out); oauth = true; excluded[0] = false; filter(NULL, 0, &event, NULL);
    Check(!excluded[0], "login makes tool available on the next catalog without reload");

    const char *bad[] = {
        "{}", "{\"prompt\":\"x\",\"output_path\":42}",
        "{\"prompt\":\"x\",\"output_path\":\"no-dir/result.png\"}",
        "{\"prompt\":\"x\",\"output_path\":\"result.png\",\"transparent_background\":\"true\"}",
        "{\"prompt\":\"x\",\"output_path\":\"result.png\\u0000ignored\"}",
        "{\"prompt\":\"x\",\"output_path\":\"result.png\",\"referenced_image_paths\":[\"missing.png\"]}",
        "{\"prompt\":\"x\",\"output_path\":\"result.png\",\"referenced_image_paths\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\"]}",
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); i++) {
        Reset(&ctx); out = Run(&ctx, bad[i]);
        Check(out.is_error && requests == 0, "invalid arguments/paths/references fail before HTTP"); Free(&out);
    }
    NoOutput(root);
    Reset(&ctx);
    out = Run(&ctx, "{\"prompt\":\"Render the literal sequence \\\\u0000\",\"output_path\":\"result.png\"}");
    Check(!out.is_error, "literal escaped backslashes are not confused with decoded NULs"); Free(&out); unlink(path);

    /* Synthetic storage in this test's temp directory, never real session media.
     * Both parents exist, so refusal cannot pass just because realpath fails. */
    char pico[4096], media[4096], alias[4096];
    PicoPath_Format(pico, sizeof(pico), "%s/.pico", root);
    PicoPath_Format(media, sizeof(media), "%s/.pico/media", root);
    PicoPath_Format(alias, sizeof(alias), "%s/alias", root);
    Check(mkdir(pico, 0700) == 0 && mkdir(media, 0700) == 0 && symlink(media, alias) == 0, "create synthetic owned-media fixture and parent alias");
    const char *protected_args[] = {
        "{\"prompt\":\"icon\",\"output_path\":\".pico/media/image.png\"}",
        "{\"prompt\":\"icon\",\"output_path\":\"alias/image.png\"}",
    };
    for (size_t i = 0; i < sizeof(protected_args)/sizeof(*protected_args); i++) {
        Reset(&ctx); out = Run(&ctx, protected_args[i]);
        Check(out.is_error && requests == 0, "owned media and resolved parent aliases are refused before HTTP");
        Free(&out); NoOutput(media);
    }
    unlink(alias); rmdir(media); rmdir(pico);

    /* A dangling destination symlink is an existing file, not an output target. */
    Check(symlink("missing-target.png", path) == 0, "create dangling destination symlink");
    Reset(&ctx); out = Run(&ctx, args);
    Check(out.is_error && requests == 0, "destination symlinks are never followed or overwritten"); Free(&out); unlink(path);
    Reset(&ctx); race_destination = path; out = Run(&ctx, args);
    Check(out.is_error && SameFile(path, "racing writer", 13), "publication cannot overwrite a destination created during generation");
    Free(&out); unlink(path); NoOutput(root);

    Reset(&ctx); expired = true; out = Run(&ctx, args);
    Check(!out.is_error && refreshes == 1 && requests == 1 && strstr(bearer, "refreshed-access"), "expiry refreshes before generation"); Free(&out); unlink(path);
    Reset(&ctx); first_401 = true; out = Run(&ctx, args);
    Check(!out.is_error && refreshes == 1 && requests == 2 && strstr(bearer, "refreshed-access"), "definite 401 refreshes and retries once"); Free(&out); unlink(path);
    Reset(&ctx); always_401 = true; out = Run(&ctx, args);
    Check(out.is_error && refreshes == 1 && requests == 2, "a second 401 is not retried"); Free(&out); NoOutput(root);
    Reset(&ctx); expired = refresh_fails = true; out = Run(&ctx, args);
    Check(out.is_error && requests == 0, "failed refresh does not send stale credentials"); Free(&out);

    Reset(&ctx); transport = PICO_HTTP_FAIL; out = Run(&ctx, args);
    Check(out.is_error && requests == 1 && refreshes == 0, "uncertain generation outcome is never automatically replayed"); Free(&out);
    Reset(&ctx); status = 429; out = Run(&ctx, args);
    Check(out.is_error && requests == 1 && strstr(out.output, "429"), "quota errors remain explicit, without fallback requests"); Free(&out);
    Reset(&ctx); ctx.cancelled = true; out = Run(&ctx, args);
    Check(out.is_error && requests == 0, "pre-cancelled call has no network or file effects"); Free(&out);
    Reset(&ctx); cancel_on_response = true; out = Run(&ctx, args);
    Check(out.is_error && out.image_count == 0, "cancellation after backend completion prevents publication"); Free(&out); NoOutput(root);

    const char *invalid[] = {"not-json", "{\"data\":[]}", "{\"data\":[{\"b64_json\":\"A===\"}]}", "{\"data\":[{\"b64_json\":\"dGV4dA==\"}]}"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); i++) {
        Reset(&ctx); free(response); response = JsonDup(invalid[i]); out = Run(&ctx, args);
        Check(out.is_error && !out.image_count, "malformed or non-image backend output cannot be saved or returned as a preview"); Free(&out);
    }
    Reset(&ctx); Fixture(image_oversized_png, sizeof(image_oversized_png)); out = Run(&ctx, args);
    Check(out.is_error && !out.image_count, "oversized output is rejected rather than resized"); Free(&out); NoOutput(root);
    free(request_body); free(response); rmdir(root);
    return failed;
}
