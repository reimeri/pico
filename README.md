# Pico

![Pico banner](app/resources/banner.png)

A small (~3MB) C99 AI agent harness with a native chat UI. The core is a loader, agent loop, and session; most behavior is extensions (builtins plus your own `.c` files).

> Alpha software

## Features

- Markdown chat UI, composer, and footer
- Git working-tree indicator in the footer (`+adds -dels`) with a unified diff modal (Myers line diff; untracked files shown as fully added)
- OpenAI-compatible models (API key or ChatGPT `/login openai`)
- Charm Hyper (`HYPER_API_KEY` or `/login hyper`)
- xAI (`XAI_API_KEY` or `/login xai`)
- Workspace tools (`sh`), structured `ask_user` questionnaires, built-in agent TODO tracking, sessions, compaction
- Concurrent main agents across multiple workspaces in one window; `/cd` opens or selects without replacing the others
- Synchronous named-profile subagent delegation with exact session continuation
- Hot-reloadable C99 extensions (views, tools, commands, providers)
- Slash commands (`/help`, `/docs`, `/reload`, …)

## Stack

C99, [Clay](https://github.com/nicbarker/clay) layout, [Raylib](https://www.raylib.com/) 5.5, [md4c](https://github.com/mity/md4c), libcurl. Build: CMake 3.27+, Ninja.

## Install

Needs a C99 compiler, CMake 3.27+, Ninja, pkg-config, Git, libcurl, and Raylib’s native deps (OpenGL, X11/Wayland, audio).

Debian/Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build pkg-config git \
  libcurl4-openssl-dev libgl1-mesa-dev libx11-dev libxcursor-dev \
  libxinerama-dev libxi-dev libxrandr-dev libxkbcommon-dev \
  libwayland-dev libasound2-dev
```

With Nix: `nix develop` (or [direnv](https://direnv.net/) via `.envrc`). The flake supplies the toolchain and pinned Raylib sources.

## Build

From the repo root. First configure fetches Raylib unless `FETCHCONTENT_SOURCE_DIR_RAYLIB` is set (Nix does this).

```bash
cmake -S app --preset debug && cmake --build app/build/debug
./app/build/debug/pico
```

Release:

```bash
cmake -S app --preset release && cmake --build app/build/release
./app/build/release/pico
```

Tests: `ctest --test-dir app/build/debug --output-on-failure`

Debug builds save each raw SSE response under
`$XDG_CONFIG_HOME/pico/debug/sse/` (or `~/.config/pico/debug/sse/`) as a private
`.sse` file with companion `.json` metadata. Only the newest 100 completed capture
pairs are retained. These files can contain prompts, reasoning, tool arguments, and
outputs; handle them as sensitive data. Release builds do not capture responses.

A relocatable copy of the build output needs `pico`, `resources/` (fonts), `docs/` (markdown for `/docs` and the agent hint), `examples/` (templates the docs link to), and `builtins/` (reference sources for `sh`, OpenAI, Hyper, and xAI).

The process starts in the current directory as the first workspace. `/cd` opens or selects another workspace without replacing the others. `pico -h` lists flags. Sign in with `/login openai` / `/login hyper` / `/login xai`, or `PICO_API_KEY` / `OPENAI_API_KEY` / `HYPER_API_KEY` / `XAI_API_KEY`.

Named subagents are configured as JSONC files under `$XDG_CONFIG_HOME/pico/subagents/` or `~/.config/pico/subagents/`. Pico creates the directory but does not install profiles. Copy the exploration/review templates from [`examples/subagents/`](examples/subagents/) and see the [subagent guide](docs/subagents.md). Tool allowlists control Pico's offered/executable catalog; they are not process or filesystem sandboxes.

F5 and `/reload` reload host extensions and the selected workspace. `/cd` opens or selects a workspace without replacing the others. Extension API: [`docs/extend/`](docs/extend/README.md).
