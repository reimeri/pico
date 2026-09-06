# Subagents

Pico can delegate a task to a child agent through the builtin `subagent` tool. Every child uses a named user profile; calls cannot supply ad-hoc purpose, model, effort, or tool overrides. Children are not selected as the main chat session. Click a `subagent` tool row to open a view-only nested chat of that child's session, both while it runs and after it finishes. The row also shows the child's live activity instead of a generic running placeholder.

## Profile directory

Pico creates this directory when it starts:

- `$XDG_CONFIG_HOME/pico/subagents/` when `XDG_CONFIG_HOME` is set;
- otherwise `~/.config/pico/subagents/`.

Pico does not create or install profile files. Only direct, regular, non-hidden `*.json` files are discovered. The filename stem is the profile name and must match `[A-Za-z0-9][A-Za-z0-9._-]*` (at most 64 bytes).

To install the bundled examples (paths from this file):

```sh
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/pico/subagents"
cp ../examples/subagents/explore.json ../examples/subagents/review.json \
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
  "tools": ["sh"],

  // Explicit opt-in to overlap other calls in the parent's batch. Default false.
  "parallel_safe": true,

  // Optional concurrency limit for this child's parallel-eligible tools (1–16).
  "max_parallel_tools": 2
}
```

`purpose` is required and non-empty. `description`, `model`, `effort`, `tools`, `parallel_safe`, and `max_parallel_tools` are optional. Unknown keys warn but do not invalidate the file. Wrong types, invalid or oversized values, duplicate or unknown tools, unknown models, and unsupported efforts make only that profile unavailable; other valid profiles still load.

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

`profile` and `task` are required. When `subagent` is offered, parent extra instructions say children start with no parent context, so `task` must carry the briefing; `session_id` resumes that child only and still needs parent-side changes. A fresh child receives current system/workspace instructions, the profile purpose, and only the delegated task. It does not inherit the parent transcript, provider history, compaction briefing, TODO state, or prompt cache key. The parent waits synchronously while Pico pumps the child. Click the `subagent` row to watch that session; nested `subagent` rows inside the inspect chat open on a Back stack. Asks from the child still use the normal confirm/questionnaire overlays.

The result identifies the profile, resolved model and effort, status, final answer, and whether the child can be resumed. A reusable result also includes its exact `session_id`. If `profile` is not a discovered name, the tool fails and the error lists the profiles that are currently available.

## Parallel delegation

Ask the model to issue independent `subagent` calls whose profiles declare `parallel_safe: true` together in one response. Pico runs eligible calls concurrently up to the parent's effective `max_parallel_tools` limit (default 4). There is no wrapper tool. Each sibling has its own transcript, result, inspection row, asks, and cancellation ownership. A failed child does not cancel siblings; cancelling the parent cancels all outstanding descendants. The parent makes its next model request only after every call in the response settles.

`parallel_safe` is a strict boolean, **default false**. A missing or false value makes that profile's delegation a sequential barrier: earlier calls must finish before it starts, and later calls wait until it finishes. For example, `explore → worker → review` executes in that order if worker has not opted in, even if explore/review are parallel-safe. Invalid or unknown profiles are conservatively scheduled as barriers and return normal tool errors.

This is **parent-batch ordering, not workspace-wide exclusivity**. Another parent or independently running agent may still operate concurrently. Use it for a worker profile that must not overlap siblings; do not mistake it for filesystem locking or a sandbox. The setting is never inferred from the purpose text or tool allowlist.

A worker definition can omit the field or be explicit:

```json
{
  "purpose": "Implement the delegated changes.",
  "parallel_safe": false,
  "tools": ["sh"]
}
```

Pico snapshots eligibility before any calls in a response start. Before-tool hooks may edit a delegation's `task` or deny the call, but cannot change `profile` or `session_id` (including adding/removing `session_id`); such rewrites return a controlled error without starting a child. Equivalent JSON string encoding/key order is allowed. Profile changes on reload affect future batches, not already accepted work.

Set `max_parallel_tools` in user-global or workspace `settings.json` (integer 1–16). `parallel_safe` controls whether this delegation may overlap calls in the parent; `max_parallel_tools` controls **that child's** tool concurrency. A profile limit override controls that child only; omission uses the workspace setting, not the parent's profile override. The limit is copied for each accepted turn. A limit of 1 serializes eligible calls. Other builtins, including `sh`, remain sequential within one agent initially; separate child agents can still execute their own `sh` calls concurrently. Existing workspace/host agent caps remain enforced and return tool errors when exhausted.

The bundled exploration and review profiles explicitly set `parallel_safe: true` and request read-only work through their purpose instructions. They do not enforce filesystem restrictions. Parallel-safe registration is a reentrancy contract, not a sandbox or proof of independence. See [extension tool contracts](extend/tools.md#execution-policy-and-parallel-calls).

## Continuing a child session

Pass the exact previous child `session_id` with the same profile name. Pico reserves and replays that JSONL session, then refreshes purpose, model, effort, tools, and concurrency policy from the current profile. This preserves the child's conversation while applying updated policy. A session cannot be open twice or continued under a different profile.

`/resume` autocomplete lists parent sessions only. A child remains openable by typing its session ID.

A child is advertised as resumable only after its session header and delegated task are durably written. `--no-session`, an ephemeral parent, or a persistence failure produces `"resumable": false` and no reusable ID.

## Reload and errors

Profiles load at startup and on F5 or `/reload`, after extensions have registered tools. Workspace reload is queued until that workspace's live and retired workers, asks, offered tool catalogs, provider/tool events, and delegation jobs are quiescent. While queued, that workspace refuses new turns and delegations but keeps pumping current work, cancellation, and ask UI. Other workspaces are not blocked.

After reload, Pico validates profiles against the new model/tool registries. A profile that names a missing tool or model is unavailable until the configuration or registration returns. Existing restricted agents are also revalidated. Errors appear in the warning overlay with the profile path and reason.

`/cd` opens or selects a workspace and leaves the previous workspace running. Profiles for the target workspace load with that workspace; they are not process-global.
