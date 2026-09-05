# Skills

Pico loads [Agent Skills](https://agentskills.io): directories of instructions, scripts, and resources that the agent pulls in only when a task calls for them. The landing page's **Skills** card lists the skills loaded for the selected workspace.

## Skill directories

Skills are discovered from two places:

- `$XDG_CONFIG_HOME/pico/skills/<name>/` when `XDG_CONFIG_HOME` is set, otherwise `~/.config/pico/skills/<name>/` — available in every workspace;
- `<workspace>/.pico/skills/<name>/` — available only in that workspace.

Only direct subdirectories containing a `SKILL.md` file are discovered. A workspace skill shadows a global skill with the same name. The catalog is rescanned whenever a prompt is built, so adding or editing a skill takes effect on the next turn without a reload.

Invalid skills are skipped with a status warning and never block other skills from loading.

## `SKILL.md` format

Each `SKILL.md` starts with YAML frontmatter followed by Markdown instructions:

```markdown
---
name: pdf-processing
description: Extracts text and tables from PDF files, fills PDF forms, and merges multiple PDFs. Use when working with PDF documents.
license: Apache-2.0
metadata:
  author: example-org
  version: "1.0"
---

# PDF processing

Step-by-step instructions for the agent...
```

Frontmatter fields:

| Field | Required | Constraints |
| --- | --- | --- |
| `name` | Yes | 1-64 chars of `a-z`, `0-9`, `-`; no leading, trailing, or consecutive hyphens; must equal the skill's directory name |
| `description` | Yes | 1-1024 bytes; what the skill does and when to use it |
| `license` | No | License name or bundled license file |
| `compatibility` | No | 1-500 bytes; environment requirements |
| `metadata` | No | Map of string keys to string values |
| `allowed-tools` | No | Parsed and stored, but currently **not enforced** |

The frontmatter parser accepts plain, single-quoted, and double-quoted scalars, `|` literal and `>` folded block scalars, multi-line folded plain scalars, and a one-level nested map for `metadata`. Exotic YAML (anchors, arrays, deeper nesting) is rejected with a clear error. Unknown top-level keys are ignored so newer spec fields stay loadable.

## Progressive disclosure

Skills load in stages:

1. **Metadata** — every loaded skill's `name` and `description` is listed in the agent's instructions (visible with `/prompt`) when `use_skill` is offered. Agents whose tool allowlist excludes `use_skill`, and prompts without tools, receive no skill-loading guidance.
2. **Instructions** — when the task matches a skill, the agent calls the `use_skill` tool with the skill's name. The tool result carries the full `SKILL.md` body plus the skill's base directory, and stays in the transcript for the rest of the session.
3. **Resources** — relative references (`scripts/`, `references/`, `assets/`) resolve from the skill's base directory; the agent reaches them with the regular shell and file tools.

You can also load a skill by hand: `/skill <name>` submits the skill's instructions as your message (the chat row keeps showing `/skill <name>`), and `/skill` completes skill names as you type. Extra text after the name is appended as the request, e.g. `/skill pdf-processing merge these two files`. File mentions such as `/skill code-review review @README.md` include both the skill instructions and the referenced file.

Keep `SKILL.md` under ~500 lines and move detailed material into `references/` files that the agent reads on demand.

## Example

See [`examples/skills/code-review/`](../examples/skills/code-review/) for a minimal skill. To try it:

```sh
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/pico/skills"
cp -r ../examples/skills/code-review "${XDG_CONFIG_HOME:-$HOME/.config}/pico/skills/"
```
