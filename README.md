# Pico

![Pico banner](app/resources/banner.png)

A small (~3MB) C99 AI agent harness with a native chat UI. The core is a loader, agent loop, and session; most behavior is extensions (builtins plus your own `.c` files).

> In active development. Expect breakage

## Features

- Markdown chat UI, composer, and footer
- Find in the current conversation with Ctrl+F
- Diff viewer
- Native model support (api key or `/login {provider}`)
  - OpenAI
  - Charm Hyper
  - xAI
- By default the agent gets just the `sh` tool for reading, writing, and editing
- Built-in extensions for questionnaires, TODO tracking, and subagents
- Concurrent agents across multiple workspaces in one window
- Hot-reloadable C99 extensions (views, tools, commands, providers). Just ask the agent to build one
- Slash commands (`/help`, `/docs`, `/reload`, …)
- Spell checking (uses `enchant`)

## Getting started

On x86-64 Linux, download the AppImage or portable `.tar.gz` from the **[latest GitHub release](https://github.com/reimeri/pico/releases/latest)**:

```bash
chmod +x pico-*-linux-x86_64.AppImage && ./pico-*-linux-x86_64.AppImage
# or
tar -xzf pico-*-linux-x86_64.tar.gz && cd pico-*-linux-x86_64 && ./bin/pico
```

A host C compiler (`cc`) is needed only to compile hot-reloadable C extensions; releases do not bundle one.

<details>
<summary>NixOS / Nix</summary>

```bash
nix profile install github:reimeri/pico   # or: nix run github:reimeri/pico
```

For a declarative NixOS install, add Pico as an input and package in your system flake (replace `hostname` with your configuration name):

```nix
{
  inputs.pico = {
     url = "github:reimeri/pico";
     inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { nixpkgs, pico, ... }: {
    nixosConfigurations.hostname = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        ./configuration.nix
        ({ pkgs, ... }: {
          environment.systemPackages = [
            pico.packages.${pkgs.system}.default

            # For en_US spell checking use this instead
            # inputs.pico.packages.${system}.pico-spellcheck
          ];
        })
      ];
    };
  };
}
```

Supports `x86_64-linux` and `aarch64-linux`; the package puts GCC on `PATH` so extensions compile out of the box.

</details>

Authenticate with `/login openai`, `/login hyper`, or `/login xai`, or set `PICO_API_KEY`, `OPENAI_API_KEY`, `HYPER_API_KEY`, or `XAI_API_KEY`.

## Build from source

Needs a C99 compiler, CMake 3.27+, Ninja, pkg-config, Git, libcurl, OpenSSL Crypto, SQLite3, utf8proc, and Raylib's native deps (OpenGL, X11/Wayland, audio). Ubuntu 26.04:

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build meson pkg-config git curl \
  libcurl4-openssl-dev libssl-dev libsqlite3-dev libutf8proc-dev \
  libgl1-mesa-dev libx11-dev libx11-xcb-dev \
  libxcb1-dev libxcursor-dev libxext-dev libxfixes-dev libxi-dev \
  libxinerama-dev libxrandr-dev libxrender-dev libxkbcommon-dev \
  libwayland-dev wayland-protocols libffi-dev libexpat1-dev \
  libdecor-0-dev libasound2-dev libpulse-dev xdg-utils
```

```bash
cmake -S app --preset debug && cmake --build app/build/debug && ./app/build/debug/pico
# release: cmake -S app --preset release && cmake --build app/build/release
# install: cmake --install app/build/release --prefix "$HOME/.local"
# archive: cmake --build app/build/release --target package
# tests:   ctest --test-dir app/build/debug --output-on-failure
```

The first configure fetches Raylib unless `FETCHCONTENT_SOURCE_DIR_RAYLIB` is set. Nix: `nix develop` (or [direnv](https://direnv.net/)). After a NixOS graphics-driver upgrade, EGL failures can mean a glibc mismatch (`LD_DEBUG=libs ./app/build/debug/pico`); `nix flake update nixpkgs`, then recreate `app/build/debug` inside a new `nix develop` shell before rebuilding.

Install includes the desktop entry and `app/resources/logo.png`. Debug builds keep the newest 100 SSE capture pairs under `$XDG_CONFIG_HOME/pico/debug/sse/` (sensitive: prompts, reasoning, tools); release builds do not. Dev builds keep `pico` next to `resources/`, `docs/`, `examples/`, `builtins/`, and `sdk/`; installed layouts use `bin/pico` and `share/pico/{...}`. Discovery is relative to the executable (`PICO_DATA_DIR` overrides). Extensions compile with `${PICO_CC:-cc}` against packaged `sdk/include`.

F5 and `/reload` reload host extensions and the selected workspace. Extension API: [`docs/extend/`](docs/extend/README.md).

## Stack

C99, [Clay](https://github.com/nicbarker/clay) layout, [Raylib](https://www.raylib.com/) 5.5, [md4c](https://github.com/mity/md4c), [tinyfiledialogs](https://github.com/native-toolkit/libtinyfiledialogs), libcurl, OpenSSL Crypto. Build: CMake 3.27+, Ninja.

## Confused about something?

Just ask pico and it and it will explain.

- How subagents are configured?
- What providers are supported?
- How to login?
- What X tool does?
- ...
