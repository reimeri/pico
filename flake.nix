{
  description = "Pico native AI agent harness";

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
      projectLine = nixpkgs.lib.findFirst
        (line: nixpkgs.lib.hasPrefix "project(pico VERSION " line)
        (throw "Pico version is missing from app/CMakeLists.txt")
        (nixpkgs.lib.splitString "\n" (builtins.readFile ./app/CMakeLists.txt));
      version = builtins.elemAt (nixpkgs.lib.splitString " " projectLine) 2;
      picoFor =
        pkgs:
        let
          runtimeLibraries = with pkgs; [
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
            libffi
            libdecor
            alsa-lib
            libpulseaudio
          ];
        in
        pkgs.stdenv.mkDerivation {
          pname = "pico";
          inherit version;
          src = pkgs.lib.cleanSourceWith {
            src = self;
            filter = path: type:
              let
                base = builtins.baseNameOf path;
              in
              base != ".git" && !(type == "directory" && base == "build");
          };
          cmakeDir = "../app";

          strictDeps = true;
          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            makeWrapper
            wayland-scanner
            wayland-protocols
          ];
          buildInputs = runtimeLibraries;

          cmakeFlags = [
            "-DFETCHCONTENT_SOURCE_DIR_RAYLIB=${raylib}"
            "-DCMAKE_BUILD_TYPE=Release"
            "-DBUILD_TESTING=ON"
          ];

          doCheck = true;
          checkPhase = ''
            runHook preCheck
            ctest --output-on-failure
            runHook postCheck
          '';

          postFixup = ''
            wrapProgram "$out/bin/pico" \
              --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.gcc pkgs.git pkgs.libnotify ]} \
              --prefix LD_LIBRARY_PATH : ${pkgs.lib.makeLibraryPath [
                pkgs.wayland
                pkgs.libxkbcommon
                pkgs.libxcursor
                pkgs.libdecor
              ]}
          '';

          meta = {
            description = "Small C99 AI agent harness with a native chat UI";
            homepage = "https://github.com/reimeri/pico";
            license = pkgs.lib.licenses.mit;
            mainProgram = "pico";
            platforms = systems;
          };
        };
    in
    {
      packages = forEachSystem (pkgs: {
        pico = picoFor pkgs;
        default = picoFor pkgs;
      });

      apps = forEachSystem (pkgs: {
        pico = {
          type = "app";
          program = "${picoFor pkgs}/bin/pico";
        };
        default = {
          type = "app";
          program = "${picoFor pkgs}/bin/pico";
        };
      });

      checks = forEachSystem (pkgs: {
        pico = picoFor pkgs;
        default = picoFor pkgs;
      });

      devShells = forEachSystem (
        pkgs:
        let
          pico = picoFor pkgs;
        in
        {
          default = pkgs.mkShell {
            name = "pico";
            inputsFrom = [ pico ];

            packages = with pkgs; [
              cmake
              ninja
              pkg-config
              gcc
              gdb
              git
              curl
              libnotify
            ];

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
        }
      );
    };
}
