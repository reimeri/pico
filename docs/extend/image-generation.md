# Image generation

The builtin `generate_image` tool generates one PNG from a prompt, or edits using
explicitly supplied local reference images. **Settings → Image** selects the
global backend, independently of the chat model/provider:

- **Codex** (default): uses the ChatGPT subscription login selected by
  `/login openai`, consuming image/Codex usage. Generation uses
  `gpt-image-2.5-flare`; editing uses `gpt-image-2.5-sunburst`. A shared quality
  control applies to both operations.
- **OpenAI API**: uses the existing effective OpenAI API key (including an
  environment override), even when OAuth is the active chat authentication mode.
  Requests use the official `https://api.openai.com/v1/images/` endpoint;
  chat provider base URLs do not override it. API usage is billed separately.
  Generation and editing each have a free-text model ID and a separate quality
  control. Both model IDs default to `gpt-image-2.6-sunburst`.

Quality choices are `auto` (default), `low`, `medium`, `high`, `xhigh`, and `max`.
They are passed through to the selected backend/model; unsupported combinations
return an error rather than silently changing quality or models. Size is automatic.
The API switch is disabled without an effective OpenAI API key; hovering explains
that a key is required. Settings are global, staged until Apply, and retained when
switching providers. Cancel discards edits.

The OpenAI workspace builtin registers the tool. A per-request hook offers it
only when the selected backend's credentials are available. The worker copies
current applied image settings under the host settings lock at execution start,
then checks credentials again; later settings changes do not mutate that call.
Login/logout and applied settings changes update availability on the next request
without an extension reload. There is no automatic fallback between API and
Codex. Agent and subagent profile tool restrictions still apply. An agent using
another chat provider can use the configured image backend.

## Generate

```json
{
  "prompt": "A simple flat blue circle icon on a white background",
  "output_path": "assets/circle.png"
}
```

The `assets` directory must already exist. Both `prompt` and `output_path` are
required. There is no implicit output directory or generated filename.

## Edit

```json
{
  "prompt": "Change the blue circle to green; preserve the composition",
  "output_path": "assets/green-circle.png",
  "referenced_image_paths": ["assets/circle.png"],
  "transparent_background": true
}
```

- Omitted/empty `referenced_image_paths` selects generation. A nonempty array
  selects editing and uploads those images to OpenAI. Transcript images are not
  implicitly included: provide the paths explicitly.
- Up to five readable regular local PNG, JPEG, or WebP references are accepted,
  at most **64 MiB combined** and **4096 × 4096 per image**. Content determines
  the reference format, not its extension. Bytes are not resized or converted.
  Animated WebP acceptance/interpretation depends on the backend.
- `transparent_background` defaults to false (opaque); true requests transparency.
- Each call requests one PNG image, with model and quality from Image settings.
  The result reports the **requested** model, not a verified effective server
  model. There is no automatic model fallback.

## Files and previews

Relative input and output paths resolve against the **calling agent's workspace**,
not the process working directory or the currently selected workspace. Absolute
paths may be outside the workspace; this is not a filesystem sandbox.

The output must be a new `.png` file in an existing directory. Existing files,
directories, and symlinks (including dangling symlinks) are never overwritten.
Pico rejects outputs in its owned `.pico/media/` storage, including resolved
parent symlink targets. It resolves and retains the parent directory, stages the
complete validated image there, then publishes it atomically without replacing
an existing destination. A competing writer that creates the destination while
generation runs causes publication to fail, not overwrite that file. Staging
files are removed on ordinary failure/cancellation; a process crash can leave
an uncommitted `.pico-image-*.tmp` file in the output directory.

Backend output must be one valid PNG within the same 64 MiB / 4096 × 4096 limits.
Malformed, unsupported, or oversized output is rejected, not resized/converted.
Accepted bytes are saved unchanged. The saved output is **caller-owned**: moving
or editing it does not change an accepted session preview.

If the calling turn's model/provider accepts tool images, the result includes
both the path and a real model-visible image. Core manages its independent
session-owned preview copy using the ordinary [image contracts](images.md).
Otherwise the tool still saves the image and returns its path/metadata, explicitly
reporting that no visual preview was returned. Generation does not require a
vision-capable chat model.

## Requests, cancellation, and lifecycle

The tool is sequential within one agent's tool batch; other agents/workspaces
may run concurrently. It runs entirely on the core tool worker with callback-
local request state, no UI calls, and no background job that outlives its callback.
Normal tool cancellation, reload quiescence, and result ownership apply.

Codex image requests use the fixed ChatGPT Codex `images/generations` or
`images/edits` endpoint. This is a Codex backend integration, **not a versioned
public Images API guarantee**; account entitlement and backend availability can
change. Tokens and raw server responses are not returned to the agent or saved
in tool details. Public extension auth/HTTP APIs are unchanged.

For Codex, expiry and a definite HTTP 401 use the same synchronized OAuth refresh mechanism
as the OpenAI provider. A rejected request is retried once after refresh; a
second 401 is an error. API requests never refresh OAuth or retry a 401.
API generation and editing use JSON requests to the official Images endpoints,
with explicit references encoded as data URLs. Image POSTs otherwise make one attempt, do not follow
redirects, bound the buffered response size, and allow up to ten minutes per
attempt. Quota errors, other HTTP errors, timeouts, and uncertain transport
outcomes do not trigger automatic retries or model fallback.

Cancellation cooperatively stops HTTP and prevents file publication when observed
before the final publish. It cannot undo backend work or restore usage already
consumed or API charges incurred. Cancellation after publication does not delete the caller-owned file,
even if core subsequently discards the tool result. Check for the requested
output before deciding whether to repeat an interrupted call.

See [auth](auth.md), [tools](tools.md), and [contracts](contracts.md#image-generation).
Builtin reference: [`openai_image.c`](../../builtins/openai_image.c), with shared
OAuth refresh in [`openai.c`](../../builtins/openai.c).
