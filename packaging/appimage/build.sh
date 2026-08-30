#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 BUILD_DIR VERSION OUTPUT_DIR" >&2
    exit 2
fi

build_dir=$(realpath "$1")
version=$2
output_dir=$(mkdir -p "$3" && realpath "$3")
repo_root=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
appdir="$build_dir/AppDir"
linuxdeploy=${LINUXDEPLOY:-linuxdeploy-x86_64.AppImage}
output="$output_dir/pico-${version}-linux-x86_64.AppImage"
wayland_prefix=${PICO_APPIMAGE_WAYLAND_PREFIX:-}

if [[ -n $wayland_prefix ]]; then
    wayland_prefix=$(realpath "$wayland_prefix")
    export PKG_CONFIG_PATH="$wayland_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi

if ! command -v "$linuxdeploy" >/dev/null 2>&1 && [[ ! -x "$linuxdeploy" ]]; then
    echo "linuxdeploy is unavailable: $linuxdeploy" >&2
    exit 1
fi

rm -rf "$appdir"
DESTDIR="$appdir" cmake --install "$build_dir" --prefix /usr
install -Dm755 "$repo_root/packaging/appimage/AppRun" "$appdir/AppRun"
install -Dm644 "$repo_root/packaging/linux/io.github.reimeri.pico.desktop" \
    "$appdir/io.github.reimeri.pico.desktop"
if command -v magick >/dev/null 2>&1; then
    magick "$repo_root/app/resources/logo.png" -resize 512x512 "$appdir/pico.png"
elif command -v convert >/dev/null 2>&1; then
    convert "$repo_root/app/resources/logo.png" -resize 512x512 "$appdir/pico.png"
else
    echo "ImageMagick is required to create the 512x512 AppImage icon" >&2
    exit 1
fi
ln -sfn pico.png "$appdir/.DirIcon"
# linuxdeploy installs the resized icon in the icon theme; the original is
# still shipped as Pico runtime data under usr/share/pico/resources.
rm -f "$appdir/usr/share/pixmaps/pico.png"

if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$appdir/io.github.reimeri.pico.desktop"
fi

# GLFW resolves these at runtime with dlopen(), so ELF dependency scanning does
# not discover them. Deploy each one explicitly and preserve libdecor plugins.
required_modules=(wayland-client wayland-cursor wayland-egl xkbcommon xcursor libdecor-0)
required_libraries=(wayland-client wayland-cursor wayland-egl xkbcommon Xcursor decor-0)
required_sonames=()
dependency_args=()
install -d "$appdir/usr/lib"
for index in "${!required_modules[@]}"; do
    module=${required_modules[$index]}
    name=${required_libraries[$index]}
    libdir=$(pkg-config --variable=libdir "$module")
    mapfile -t matches < <(find "$libdir" -maxdepth 1 \( -type f -o -type l \) \
        -name "lib${name}.so*" -print | sort -V)
    library=${matches[0]:-}
    if [[ -z $library ]]; then
        echo "could not locate the $module runtime library in $libdir" >&2
        exit 1
    fi
    library=$(realpath "$library")
    soname=$(patchelf --print-soname "$library")
    if [[ -z $soname ]]; then
        echo "could not read the $module runtime library SONAME" >&2
        exit 1
    fi
    if [[ $module == wayland-client ]] &&
        ! readelf --dyn-syms --wide "$library" | grep -w wl_fixes_interface >/dev/null; then
        echo "Wayland 1.24 or newer is required for the AppImage runtime" >&2
        echo "build it with packaging/appimage/build-wayland-runtime.sh and set PICO_APPIMAGE_WAYLAND_PREFIX" >&2
        exit 1
    fi
    target="$appdir/usr/lib/$soname"
    cp -L "$library" "$target"
    chmod u+w "$target"
    required_sonames+=("$soname")
    dependency_args+=(--deploy-deps-only "$target")
done

decor_libdir=$(pkg-config --variable=libdir libdecor-0)
decor_plugins="$decor_libdir/libdecor/plugins-1"
mapfile -t decor_plugin_files < <(find "$decor_plugins" -maxdepth 1 -type f -name '*.so' -print 2>/dev/null | sort)
if [[ ${#decor_plugin_files[@]} -eq 0 ]]; then
    echo "could not locate a libdecor runtime plugin in $decor_plugins" >&2
    exit 1
fi
install -d "$appdir/usr/lib/libdecor/plugins-1"
for plugin in "${decor_plugin_files[@]}"; do
    target="$appdir/usr/lib/libdecor/plugins-1/$(basename "$plugin")"
    cp -L "$plugin" "$target"
    chmod u+w "$target"
    dependency_args+=(--deploy-deps-only "$target")
done

rm -f "$output"
ARCH=x86_64 LDAI_OUTPUT="$output" LDAI_NO_APPSTREAM=1 \
    APPIMAGE_EXTRACT_AND_RUN=1 "$linuxdeploy" \
    --appdir "$appdir" \
    --executable "$appdir/usr/bin/pico" \
    --desktop-file "$appdir/io.github.reimeri.pico.desktop" \
    --icon-file "$appdir/pico.png" \
    "${dependency_args[@]}" \
    --output appimage

for soname in "${required_sonames[@]}"; do
    if [[ ! -f "$appdir/usr/lib/$soname" ]]; then
        echo "AppImage is missing dlopen dependency $soname" >&2
        exit 1
    fi
done
if ! readelf --dyn-syms --wide "$appdir/usr/lib/${required_sonames[0]}" | grep -w wl_fixes_interface >/dev/null; then
    echo "AppImage Wayland runtime is older than 1.24" >&2
    exit 1
fi
if ! find "$appdir/usr/lib/libdecor/plugins-1" -maxdepth 1 -type f -name '*.so' -print -quit | grep -q .; then
    echo "AppImage is missing its libdecor runtime plugin" >&2
    exit 1
fi

test -x "$output"
APPIMAGE_EXTRACT_AND_RUN=1 "$output" --help >/dev/null 2>&1
printf '%s\n' "$output"
