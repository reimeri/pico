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
The script derives the 512x512 AppImage icon from `app/resources/logo.png` and explicitly bundles GLFW's dlopen-only Wayland, Xcursor, XKB, and libdecor dependencies. Build a pinned Wayland 1.24 runtime with `packaging/appimage/build-wayland-runtime.sh OUTPUT_DIR` and pass that new, non-existing directory as `PICO_APPIMAGE_WAYLAND_PREFIX`; this prevents an older bundled `libwayland-client` from breaking newer host EGL/Mesa drivers. The helper requires curl, Meson, Ninja, a C toolchain, pkg-config, libffi and Expat development files, binutils, tar, and sha256sum. A libdecor runtime plugin must also be installed on the packaging host.

OpenSSL Crypto is a direct runtime dependency. `linuxdeploy` must bundle
`libcrypto` and `libsqlite3` along with Pico's other linked libraries;
the AppImage build checks that both are present. Local browser login uses the host's `xdg-open`; if unavailable,
Pico still displays a sign-in link.

### AppImage update discovery

The build embeds GitHub update information in the AppImage's `.upd_info` ELF
section so managers such as Gear Lever can populate update settings:

```text
gh-releases-zsync|reimeri|pico|latest|pico-*-linux-x86_64.AppImage.zsync
```

The owner and repository must be separate pipe-delimited fields. The source
repository defaults to `GITHUB_REPOSITORY` (or `reimeri/pico` outside GitHub
Actions); set `PICO_APPIMAGE_UPDATE_REPO=OWNER/REPO` to override it.
The build verifies the embedded information and the generated `.zsync` file's
SHA-1 against the final AppImage. Publish both the AppImage and its matching
`.AppImage.zsync` control file on the same GitHub release; draft releases are
not available to update managers.

To inspect a built artifact:

```bash
readelf --string-dump=.upd_info --wide dist/pico-<version>-linux-x86_64.AppImage
```

Previously built AppImages with malformed metadata must be replaced by a newly
built artifact before automatic update discovery can work.

## GitHub release

Pushing a `v<version>` tag runs `.github/workflows/release.yml`. The tag must
match `project(pico VERSION ...)` in `app/CMakeLists.txt`. The workflow builds
and tests on Ubuntu 22.04, verifies linuxdeploy against a repository-pinned
SHA-256 digest, creates the archive and AppImage, verifies final checksums, and
creates or updates a **draft** GitHub release.

Review and publish the draft manually. Release archives and AppImages require a
host `cc` compiler for user extensions. The Nix package instead adds GCC, Git, and `xdg-open`
to Pico's runtime `PATH`.

## OpenAI browser-login release check

Automated OAuth tests use loopback fixtures and synthetic credentials. Before
shipping, manually test `/login openai` with a real account on the same machine:

1. With port 1455 free, complete sign-in, make a Codex request, and verify refresh.
2. Verify cancel, logout, and `/reload` during sign-in do not later sign back in.
