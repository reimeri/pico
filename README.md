# Pico

![Pico banner](app/resources/banner.png)

A small (~3MB) C99 AI agent harness with a native chat UI. The core is a loader, agent loop, and session; most behavior is extensions (builtins plus your own `.c` files).

> Alpha software

## Features

- Markdown chat UI, composer, and footer
- Diff viewer
- Native model support (api key or `/login {provider}`)
  - OpenAI
  - Charm Hyper
  - xAI
- By default agent gets just `sh` tool for reading, writing, and editing
- Includes built-in extensions for questionnaires, TODO tracking, and subagents
- Concurrent agents across multiple workspaces in one window
- Hot-reloadable C99 extensions (views, tools, commands, providers). Just ask the agent to build one
- Slash commands (`/help`, `/docs`, `/reload`, …)

## Stack

C99, [Clay](https://github.com/nicbarker/clay) layout, [Raylib](https://www.raylib.com/) 5.5, [md4c](https://github.com/mity/md4c), [tinyfiledialogs](https://github.com/native-toolkit/libtinyfiledialogs), libcurl. Build: CMake 3.27+, Ninja.

## Install

Needs a C99 compiler, CMake 3.27+, Ninja, pkg-config, Git, libcurl, and Raylib’s native deps (OpenGL, X11/Wayland, audio).

Debian/Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build pkg-config git \
  libcurl4-openssl-dev libgl1-mesa-dev libx11-dev libxcursor-dev \
  libxinerama-dev libxi-dev libxrandr-dev libxkbcommon-dev \
  libwayland-dev libasound2-dev
```

With Nix: `nix run github:reimeri/pico` installs and runs the packaged app. For development, use `nix develop` (or [direnv](https://direnv.net/) via `.envrc`). The flake exports `packages`, `apps`, and `checks` for `x86_64-linux` and `aarch64-linux`; the packaged app puts GCC on `PATH` so C extensions compile out of the box.

Tagged releases provide an x86-64 AppImage and portable tar archive. The AppImage can be run directly:

```bash
chmod +x pico-*-linux-x86_64.AppImage
./pico-*-linux-x86_64.AppImage
```

The tar archive uses a standard `bin/` + `share/` prefix. Extract it and run `bin/pico`, or install that tree under a prefix. Both release formats require a host `cc` compiler to build hot-reloadable C extensions.

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

Install into a prefix or build the portable archive:

```bash
cmake --install app/build/release --prefix "$HOME/.local"
cmake --build app/build/release --target package
```

The install includes the desktop entry and `app/resources/logo.png` as its icon. Tests: `ctest --test-dir app/build/debug --output-on-failure`

Debug builds save each raw SSE response under
`$XDG_CONFIG_HOME/pico/debug/sse/` (or `~/.config/pico/debug/sse/`) as a private
`.sse` file with companion `.json` metadata. Only the newest 100 completed capture
pairs are retained. These files can contain prompts, reasoning, tool arguments, and
outputs; handle them as sensitive data. Release builds do not capture responses.

Development builds keep `pico`, `resources/`, `docs/`, `examples/`, `builtins/`, and `sdk/` together in the build directory. Installed builds use `bin/pico` and `share/pico/{resources,docs,examples,builtins,sdk}`. Pico discovers either layout relative to its executable; `PICO_DATA_DIR` can override the data root.

User extensions are compiled with `${PICO_CC:-cc}` against the packaged `sdk/include` tree and their own source directory. Release archives and AppImages intentionally do not bundle a compiler.

The process starts in the current directory as the first workspace. `/cd` opens or selects another workspace without replacing the others. `pico -h` lists flags. Sign in with `/login openai` / `/login hyper` / `/login xai`, or `PICO_API_KEY` / `OPENAI_API_KEY` / `HYPER_API_KEY` / `XAI_API_KEY`.

Named subagents are configured as JSONC files under `$XDG_CONFIG_HOME/pico/subagents/` or `~/.config/pico/subagents/`. Pico creates the directory but does not install profiles. Copy the exploration/review templates from [`examples/subagents/`](examples/subagents/) and see the [subagent guide](docs/subagents.md). Tool allowlists control Pico's offered/executable catalog; they are not process or filesystem sandboxes.

F5 and `/reload` reload host extensions and the selected workspace. `/cd` opens or selects a workspace without replacing the others. Extension API: [`docs/extend/`](docs/extend/README.md).
