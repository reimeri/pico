#!/usr/bin/env bash
set -euo pipefail

# Exercise build.sh's packaging boundary without building Pico or downloading
# linuxdeploy. The fixture packager embeds the supplied metadata in a real ELF
# section and writes a matching zsync header; it does not produce an AppImage.
repo_root=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
export FIXTURE="$fixture"
export FIXTURE_READELF=$(command -v readelf)
cp -L "/proc/$$/exe" "$fixture/runtime"
mkdir -p "$fixture/bin" "$fixture/build" "$fixture/lib/libdecor/plugins-1"
for name in wayland-client wayland-cursor wayland-egl xkbcommon Xcursor decor-0 sqlite3; do
    touch "$fixture/lib/lib${name}.so"
done
touch "$fixture/lib/libdecor/plugins-1/plugin.so"

cat > "$fixture/bin/mock" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
case ${0##*/} in
    cmake)
        mkdir -p "$DESTDIR/usr/bin"
        cp "$FIXTURE/runtime" "$DESTDIR/usr/bin/pico"
        ;;
    magick) touch "${@: -1}" ;;
    pkg-config) echo "$FIXTURE/lib" ;;
    patchelf) basename "$2" ;;
    readelf)
        if [[ $1 == --dyn-syms ]]; then
            echo 'wl_fixes_interface'
        else
            exec "$FIXTURE_READELF" "$@"
        fi
        ;;
    linuxdeploy)
        while [[ $# -gt 0 ]]; do
            if [[ $1 == --appdir ]]; then appdir=$2; break; fi
            shift
        done
        touch "$appdir/usr/lib/libcrypto.so"
        printf '%s' "$LDAI_UPDATE_INFORMATION" > "$FIXTURE/update-info"
        objcopy --add-section ".upd_info=$FIXTURE/update-info" \
            "$FIXTURE/runtime" "$LDAI_OUTPUT"
        printf 'SHA-1: %s\n' "$(sha1sum "$LDAI_OUTPUT" | cut -d ' ' -f 1)" \
            > "$LDAI_OUTPUT.zsync"
        ;;
    *) exit 1 ;;
esac
MOCK
chmod +x "$fixture/bin/mock"
for tool in cmake magick pkg-config patchelf readelf linuxdeploy; do
    ln -s mock "$fixture/bin/$tool"
done
export PATH="$fixture/bin:$PATH"
export LINUXDEPLOY="$fixture/bin/linuxdeploy"
unset PICO_APPIMAGE_WAYLAND_PREFIX PICO_APPIMAGE_UPDATE_REPO GITHUB_REPOSITORY

check_metadata() {
    local expected_repo=$1
    local output
    output=$(bash "$repo_root/packaging/appimage/build.sh" "$fixture/build" 1.2.3 "$fixture/dist")
    objcopy --dump-section ".upd_info=$fixture/embedded" "$output"
    local fields
    IFS='|' read -r -a fields < <(cat "$fixture/embedded"; echo)
    [[ ${#fields[@]} == 5 ]]
    [[ ${fields[0]} == gh-releases-zsync ]]
    [[ ${fields[1]}/${fields[2]} == "$expected_repo" ]]
    [[ ${fields[3]} == latest ]]
    # The updater must be able to find the control file for this artifact.
    [[ ${output##*/}.zsync == ${fields[4]} ]]
}

check_metadata reimeri/pico
export GITHUB_REPOSITORY=example/project
check_metadata "$GITHUB_REPOSITORY"
export PICO_APPIMAGE_UPDATE_REPO=other/project.name
check_metadata "$PICO_APPIMAGE_UPDATE_REPO"
export PICO_APPIMAGE_UPDATE_REPO='owner|repo'
if bash "$repo_root/packaging/appimage/build.sh" "$fixture/build" 1.2.3 "$fixture/dist" \
    > "$fixture/invalid.log" 2>&1; then
    echo 'build accepted a malformed update repository' >&2
    exit 1
fi
grep -q 'must be OWNER/REPO' "$fixture/invalid.log"
echo 'AppImage update metadata tests passed'
