#!/usr/bin/env bash
# Native release builds use the same reviewed CEF pin as flake.nix.
set -euo pipefail
if [[ $# != 1 || -e $1 ]]; then
    echo "usage: $0 NEW_OUTPUT_DIRECTORY" >&2
    exit 2
fi
root=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mapfile -t pin < <(python3 - "$root/packaging/cef.json" <<'PY'
import base64,json,platform,sys
p=json.load(open(sys.argv[1]))
arch={'x86_64':'linux64','aarch64':'linuxarm64'}[platform.machine()]
print(f"https://cef-builds.spotifycdn.com/cef_binary_{p['version']}+g{p['gitRevision']}+chromium-{p['chromiumVersion']}_{arch}_minimal.tar.bz2")
print(base64.b64decode(p['srcHashes'][platform.machine()+'-linux'].split('-',1)[1]).hex())
PY
)
[[ ${#pin[@]} == 2 ]]
archive=$(mktemp)
trap 'rm -f "$archive"' EXIT
curl --fail --location --retry 3 "${pin[0]}" -o "$archive"
printf '%s  %s\n' "${pin[1]}" "$archive" | sha256sum --check --strict -
mkdir -p "$1"
tar -xjf "$archive" --strip-components=1 -C "$1"
