# Viewing images

Pico can send images to a vision-capable model in user attachments, browser
observations, and tool results. Displaying a file in another application, or
printing its filename/base64 with `sh`, does not attach visual content.

For generating/editing images with ChatGPT login, see
[`generate_image`](image-generation.md). It saves to an explicit caller-owned path
and can return an image using the same preview/ownership contracts below.

## Local files: `view_image`

The builtin tool accepts one file:

```json
{"path":"screenshots/result.webp"}
```

- Relative paths resolve against the calling agent's workspace, not the process
  working directory. Absolute paths may point outside the workspace. Symlinks
  are followed; this is not a workspace-only filesystem sandbox.
- The path must identify a readable regular local file. Directories, pipes,
  missing files, and invalid arguments produce tool errors.
- Format is detected from content, not the extension. Supported formats are
  PNG, JPEG, and WebP. Unsupported or corrupt content is rejected.
- The tool is offered only when the calling agent's active model has vision
  enabled and its retained provider supports image-bearing tool results. This
  is checked for each request using the running turn's pinned model/provider.
  Changing models between turns updates the catalog without reloading. The
  execution-time capability guard remains and fails before file access.
- Images must be at most **4096 × 4096 pixels** and **64 MiB encoded**. Oversized
  images fail rather than being resized or converted.
- The result includes an actual model-visible image plus text describing the
  resolved source path, detected format, dimensions, and byte size. Image
  content is untrusted tool data, not instructions.

`view_image` is parallel-eligible and can be used by delegated agents when their
profile allows the tool. Browser access remains main-session-only. A profile's
`sh` allowlist does not automatically expose `view_image`.

## Original bytes and animated WebP

Pico validates by decoding, then preserves the **exact original encoded bytes**:
no re-encoding, resizing, metadata stripping, or animation flattening. Validation
checks all frames of an animated WebP. The local transcript shows a static
first-frame preview; the complete original animation is sent to the provider.
A backend may accept, reject, or interpret it as a still image. Pico does not
promise temporal understanding and does not retry rejected images as text.

Core makes an independent session-owned copy under
`<workspace>/.pico/media/tools/<session-id>/`, with the detected MIME and matching
extension. Editing/removing the source afterward does not change that accepted
result. Saved images survive session replay and extension reload; compaction
can remove them from active model context without removing saved history.
Agents must not move, rename, delete, or modify Pico-owned `.pico/media/` files
or their parent directories as project cleanup. If any local image in history
is missing, core omits that image only from the outgoing request and emits a
user-only transcript warning once per path per saved session (including after
resume). No omission warning is added to model instructions or context.
The original history/reference remains intact; restoring the file includes it
again. This recovery also applies to compaction requests; normal compaction
replacement rules still apply. Unreadable/corrupt images, invalid media, audio,
and remote image URLs are not covered by this missing-file recovery.
Existing corrupt or MIME-mismatched saved assets still fail requests explicitly.

## Extension tools

Use `PicoToolResult.images` with `PicoToolImage.data` and `size`. Core detects
PNG/JPEG/WebP from bytes; extensions do not supply a filename or MIME as an
image transport. Up to four images and 64 MiB total are allowed per result,
with the same per-image dimension limit. All result allocations transfer to
core on callback return. Check `pico_agent_context_accepts_tool_images(ctx)`
before capture or expensive work; do not call Raylib on a tool worker.

See [image-bearing results](tools.md#image-bearing-results),
[provider projection](providers.md#image-bearing-tool-results),
[ownership contracts](contracts.md#image-bearing-tool-results), and
[`image_tool.c`](../../examples/image_tool.c). Browser observations continue to
produce PNG and use the same core image-result transport.
