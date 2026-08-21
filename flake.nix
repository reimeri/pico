{
  description = "Development environment for the Clay + Raylib app";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    raylib = {
      url = "github:raysan5/raylib/5.5";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      raylib,
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forEachSystem = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forEachSystem (pkgs: {
        default = pkgs.mkShell {
          name = "pico";

          packages = with pkgs; [
            cmake
            ninja
            pkg-config
            gcc
            gdb
            git
            curl
          ];

          # Native libraries used when CMake builds Raylib 5.5 via FetchContent.
          buildInputs = with pkgs; [
            curl
            openssl
            glfw
            libGL
            libx11
            libxcursor
            libxext
            libxfixes
            libxi
            libxinerama
            libxrandr
            libxrender
            libxkbcommon
            wayland
            wayland-scanner
            wayland-protocols
            libffi
            libdecor
            alsa-lib
            libpulseaudio
          ];

          # Point FetchContent at the flake-pinned Raylib 5.5 sources.
          FETCHCONTENT_SOURCE_DIR_RAYLIB = "${raylib}";
          CMAKE_EXPORT_COMPILE_COMMANDS = "ON";

          shellHook = ''
            echo "Pico development shell"
            echo "  cmake -S app --preset debug && cmake --build app/build/debug"
            echo "  ./app/build/debug/pico"
            echo "  cmake -S app --preset release && cmake --build app/build/release"
            echo "  ./app/build/release/pico"
          '';
        };
      });
    };
}
