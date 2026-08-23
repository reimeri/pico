# Subagents

Pico can delegate a task to a child agent through the builtin `subagent` tool. Every child uses a named user profile; calls cannot supply ad-hoc purpose, model, effort, or tool overrides. Children are not selected as the main chat session. Click a `subagent` tool row to open a view-only nested chat of that child's session, both while it runs and after it finishes. The row also shows the child's live activity instead of a generic running placeholder.

## Profile directory

Pico creates this directory when it starts:

- `$XDG_CONFIG_HOME/pico/subagents/` when `XDG_CONFIG_HOME` is set;
- otherwise `~/.config/pico/subagents/`.

Pico does not create or install profile files. Only direct, regular, non-hidden `*.json` files are discovered. The filename stem is the profile name and must match `[A-Za-z0-9][A-Za-z0-9._-]*` (at most 64 bytes).

To install the repository examples:

```sh
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/pico/subagents"
cp examples/subagents/exploration.json examples/subagents/review.json \
  "${XDG_CONFIG_HOME:-$HOME/.config}/pico/subagents/"
```

## JSONC schema

Profiles are parsed as JSONC, so line and block comments are allowed.

```jsonc
{
  // Optional text shown in profile listings.
  "description": "Fast repository exploration",

  // Required system-level role, at most 1024 UTF-8 bytes.
  "purpose": "Explore the delegated question and return concise findings.",

  // Optional exact model-catalog ID and supported effort.
  "model": "gpt-5.6-sol",
  "effort": "low",

  // Optional exact-name tool allowlist.
  "tools": ["sh"]
}
```

`purpose` is required and non-empty. `description`, `model`, `effort`, and `tools` are optional. Unknown keys warn but do not invalidate the file. Wrong types, invalid or oversized values, duplicate or unknown tools, unknown models, and unsupported efforts make only that profile unavailable; other valid profiles still load.

Omitting `tools` allows every registered tool. An empty array allows no tools. A non-empty array exposes only those exact tool names. This is authorization at Pico's tool-catalog and execution boundaries, **not a sandbox**. In particular, allowing `sh` does not restrict which files or commands the shell can access.

## Model and effort resolution

For a fresh child:

1. `model` uses the profile value when present; otherwise it inherits the parent's current model.
2. Explicit `effort` wins.
3. Without explicit effort, a child using the parent's model inherits the parent's current effort.
4. When the profile changes model, effort uses that model's configured default, then its first supported effort, then `none`.

The resolved values must be supported before a child is created. Child selection never changes the parent, another live agent, or workspace defaults.

## Delegation

The model-facing tool accepts:

```json
{
  "profile": "exploration",
  "task": "Find the session replay and cancellation boundaries.",
  "session_id": "optional exact previous child session ID"
}
```

`profile` and `task` are required. A fresh child receives current system/workspace instructions, the profile purpose, and only the delegated task. It does not inherit the parent transcript, provider history, compaction briefing, TODO state, or prompt cache key. The parent waits synchronously while Pico pumps the child. Click the `subagent` row to watch that session; nested `subagent` rows inside the inspect chat open on a Back stack. Asks from the child still use the normal confirm/questionnaire overlays.

The result identifies the profile, resolved model and effort, status, final answer, and whether the child can be resumed. A reusable result also includes its exact `session_id`. If `profile` is not a discovered name, the tool fails and the error lists the profiles that are currently available.

## Continuing a child session

Pass the exact previous child `session_id` with the same profile name. Pico reserves and replays that JSONL session, then refreshes purpose, model, effort, and tools from the current profile. This preserves the child's conversation while applying updated policy. A session cannot be open twice or continued under a different profile.

`/resume` autocomplete lists parent sessions only. A child remains openable by typing its session ID.

A child is advertised as resumable only after its session header and delegated task are durably written. `--no-session`, an ephemeral parent, or a persistence failure produces `"resumable": false` and no reusable ID.

## Reload and errors

Profiles load at startup and on F5 or `/reload`, after extensions have registered tools. Reload is queued until all live and retired workers, asks, offered tool catalogs, provider/tool events, and delegation jobs are quiescent. While queued, Pico refuses new turns and delegations but keeps pumping current work, cancellation, and ask UI.

After reload, Pico validates profiles against the new model/tool registries. A profile that names a missing tool or model is unavailable until the configuration or registration returns. Existing restricted agents are also revalidated. Errors appear in the warning overlay with the profile path and reason.

Workspace changes use the same barrier. They finish current work first, then replace the old agent set and revalidate the user-global profiles against the new workspace registrations and model catalog.
