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

## Getting started

### 1. Install Pico


On x86-64 Linux, the easiest option is to download the AppImage from the **[latest GitHub release](https://github.com/reimeri/pico/releases/latest)**. Then run it from your download directory:

```bash
chmod +x pico-*-linux-x86_64.AppImage
./pico-*-linux-x86_64.AppImage
```

The release page also provides a portable `.tar.gz` archive. Extract it anywhere and run Pico from the extracted directory:

```bash
tar -xzf pico-*-linux-x86_64.tar.gz
cd pico-*-linux-x86_64
./bin/pico
```

A host C compiler (`cc`) is only needed if you want Pico to compile hot-reloadable C extensions. The release downloads do not bundle one.

<details>
<summary>NixOS / Nix</summary>

For a quick flake-based install into your user profile:

```bash
nix profile install github:reimeri/pico
pico
```

Or try Pico without installing it:

```bash
nix run github:reimeri/pico
```

For a declarative NixOS install, add Pico as an input and package in your system flake (replace `hostname` with your configuration name):

```nix
{
  inputs.pico.url = "github:reimeri/pico";

  outputs = { nixpkgs, pico, ... }: {
    nixosConfigurations.hostname = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        ./configuration.nix
        ({ pkgs, ... }: {
          environment.systemPackages = [
            pico.packages.${pkgs.system}.default
          ];
        })
      ];
    };
  };
}
```

Then apply the configuration:

```bash
sudo nixos-rebuild switch --flake .#hostname
```

The flake supports `x86_64-linux` and `aarch64-linux`. Its package includes GCC on `PATH`, so C extensions compile out of the box.
</details>

### 2. Start Pico

Run Pico from the project you want it to work on. Use the executable from the installation method you chose:

```bash
cd /path/to/your/project

# Nix profile install
pico

# AppImage download
/path/to/pico-*-linux-x86_64.AppImage

# Portable archive
/path/to/pico-*-linux-x86_64/bin/pico
```

On first launch, Pico automatically copies its bundled example to `~/.config/pico/settings.json` (or `$XDG_CONFIG_HOME/pico/settings.json`). It never replaces an existing settings file.

Authenticate in Pico with `/login openai`, `/login hyper`, or `/login xai`. You can also provide `PICO_API_KEY`, `OPENAI_API_KEY`, `HYPER_API_KEY`, or `XAI_API_KEY` in the environment.

### 3. Add or customize models

Open `~/.config/pico/settings.json`, add or uncomment the models you want to use, and set the top-level `model` value to one of their IDs. The generated example includes entries for OpenAI, Charm Hyper, and xAI. Restart Pico after editing so new workspaces load the updated model catalog.

## Build from source

Building requires a C99 compiler, CMake 3.27+, Ninja, pkg-config, Git, libcurl, and Raylib's native dependencies (OpenGL, X11/Wayland, and audio).

Debian/Ubuntu:

```bash
sudo apt install build-essential ninja-build meson pkg-config git curl python3-venv \
  libcurl4-openssl-dev libgl1-mesa-dev libx11-dev libx11-xcb-dev \
  libxcb1-dev libxcursor-dev libxext-dev libxfixes-dev libxi-dev \
  libxinerama-dev libxrandr-dev libxrender-dev libxkbcommon-dev \
  libwayland-dev wayland-protocols libffi-dev libexpat1-dev \
  libdecor-0-dev libasound2-dev libpulse-dev
python3 -m venv "$HOME/.local/share/pico-build-tools"
"$HOME/.local/share/pico-build-tools/bin/pip" install 'cmake>=3.27'
export PATH="$HOME/.local/share/pico-build-tools/bin:$PATH"
cmake --version
```

Ubuntu 22.04's repository CMake is too old, so the commands above install a current version in an isolated virtual environment. From the repo root, the first configure fetches Raylib unless `FETCHCONTENT_SOURCE_DIR_RAYLIB` is set. Nix users can enter the development environment with `nix develop` (or [direnv](https://direnv.net/) via `.envrc`).

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

Running `pico` directly starts in the current directory as the first workspace. The installed desktop launcher starts without a workspace; choose an existing workspace or session from the sidebar, or use the Projects `+` button. `/cd` opens or selects another workspace without replacing the others. `pico --no-workspace` requests the same landing state, and `pico -h` lists all flags.

Named subagents are configured as JSONC files under `$XDG_CONFIG_HOME/pico/subagents/` or `~/.config/pico/subagents/`. Pico creates the directory but does not install profiles. Copy the exploration/review templates from [`examples/subagents/`](examples/subagents/) and see the [subagent guide](docs/subagents.md). Tool allowlists control Pico's offered/executable catalog; they are not process or filesystem sandboxes.

Skills follow the [Agent Skills](https://agentskills.io) format and load from `~/.agents/skills/`, `$XDG_CONFIG_HOME/pico/skills/` (or `~/.config/pico/skills/`), `<workspace>/.agents/skills/`, and `<workspace>/.pico/skills/` (later locations shadow earlier ones of the same name); the agent activates them with the `use_skill` tool or you can load one with `/skill <name>`. See the [skills guide](docs/skills.md) and the [`examples/skills/`](examples/skills/) template.

F5 and `/reload` reload host extensions and the selected workspace. `/cd` opens or selects a workspace without replacing the others. Extension API: [`docs/extend/`](docs/extend/README.md).

## Stack

C99, [Clay](https://github.com/nicbarker/clay) layout, [Raylib](https://www.raylib.com/) 5.5, [md4c](https://github.com/mity/md4c), [tinyfiledialogs](https://github.com/native-toolkit/libtinyfiledialogs), libcurl. Build: CMake 3.27+, Ninja.
