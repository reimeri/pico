# Packaging and releases

CMake install rules define Pico's release contents. Keep distro packages and the
AppImage based on `cmake --install` rather than copying a separate file list.

## Linux archive

```bash
cmake -S app --preset release
cmake --build app/build/release
cmake --build app/build/release --target package
```

CPack writes `pico-<version>-linux-<architecture>.tar.gz` and its SHA-256 file
under `app/build/release/`.

## AppImage

`packaging/appimage/build.sh BUILD_DIR VERSION OUTPUT_DIR` stages the CMake
install under an AppDir and invokes `linuxdeploy`. It requires ImageMagick,
`desktop-file-validate`, and an x86-64 linuxdeploy executable. Set
`LINUXDEPLOY` when the executable is not named `linuxdeploy-x86_64.AppImage`.
The script derives the 512x512 AppImage icon from `app/resources/logo.png` and explicitly bundles GLFW's dlopen-only Wayland, Xcursor, XKB, and libdecor dependencies.

## GitHub release

Pushing a `v<version>` tag runs `.github/workflows/release.yml`. The tag must
match `project(pico VERSION ...)` in `app/CMakeLists.txt`. The workflow builds
and tests on Ubuntu 22.04, verifies linuxdeploy against a repository-pinned
SHA-256 digest, creates the archive and AppImage, verifies final checksums, and
creates or updates a **draft** GitHub release.

Review and publish the draft manually. Release archives and AppImages require a
host `cc` compiler for user extensions. The Nix package instead adds GCC and Git
to Pico's runtime `PATH`.
