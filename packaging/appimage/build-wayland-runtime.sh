#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 OUTPUT_DIR" >&2
    exit 2
fi

version=1.24.0
sha256=82892487a01ad67b334eca83b54317a7c86a03a89cfadacfef5211f11a5d0536
archive="wayland-$version.tar.xz"
url="https://gitlab.freedesktop.org/wayland/wayland/-/releases/$version/downloads/$archive"
output_arg=$1
if [[ -e $output_arg || -L $output_arg ]]; then
    echo "output directory already exists: $output_arg" >&2
    exit 1
fi
output_dir=$(mkdir -p "$(dirname -- "$output_arg")" && realpath -m "$output_arg")
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

for command in curl meson ninja readelf sha256sum tar; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "$command is required to build the AppImage Wayland runtime" >&2
        exit 1
    fi
done

curl --fail --location --retry 3 --connect-timeout 30 --max-time 180 \
    --output "$work_dir/$archive" "$url"
echo "$sha256  $work_dir/$archive" | sha256sum --check --strict
mkdir "$work_dir/source"
tar --extract --file "$work_dir/$archive" --directory "$work_dir/source" --strip-components=1

meson setup "$work_dir/build" "$work_dir/source" \
    --prefix="$output_dir" \
    --libdir=lib \
    --buildtype=release \
    -Ddocumentation=false \
    -Ddtd_validation=false \
    -Dlibraries=true \
    -Dscanner=true \
    -Dtests=false
meson compile -C "$work_dir/build"
meson install -C "$work_dir/build" --no-rebuild

client="$output_dir/lib/libwayland-client.so.0"
if [[ ! -f $client ]] || ! readelf --dyn-syms --wide "$client" | grep -w wl_fixes_interface >/dev/null; then
    echo "built Wayland runtime does not export wl_fixes_interface" >&2
    exit 1
fi

printf '%s\n' "$output_dir"
