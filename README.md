# Pico

A small C99 AI agent harness with a native chat UI. The core is a loader, agent loop, and session; most behavior is extensions (builtins plus your own `.c` files).

## Features

- Markdown chat UI, composer, and footer
- OpenAI-compatible models (API key or ChatGPT/`/login`)
- Workspace tools (`sh`), sessions, compaction
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

The cwd is the workspace. `pico -h` lists flags. Sign in with `/login` or `PICO_API_KEY` / `OPENAI_API_KEY`. Extension API: [`docs/extend/`](docs/extend/README.md).
