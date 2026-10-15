#!/usr/bin/env bash
# Linux counterpart of package_release.ps1: builds dist/Refract-<version>-linux-x86_64.tar.gz, which another Linux PC
# runs by unpacking it and starting ./Refract. It carries the launcher, its Node.js, the host-side programs
# (build_host.sh), Refract's Android packages and the Digitalis translator for Waydroid. Waydroid itself and its
# Android image are set up on that PC by Settings > Setup (scripts/waydroid.sh setup).
#
#   tools/package_release.sh [--skip-build] [--out DIR]
#
# Needs the launcher's build tools (Rust, Node.js, WebKitGTK 4.1 dev files), build_host.sh's, and patchelf. The Android packages
# must be built first (android-runtime-apk/build_apk.sh, platform-sdk/build_apk.sh); they are the same ARM64 code on
# every host, so copies built on Windows can be put in build-android-runtime-linux-arm64-v8a and build-platform-sdk-linux.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/dist"
SKIP_BUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1 ;;
        --out) OUT="$2"; shift ;;
        *) echo "usage: $0 [--skip-build] [--out DIR]" >&2; exit 2 ;;
    esac
    shift
done
VERSION="$(node -p "require('$ROOT/launcher/src-tauri/tauri.conf.json').version")"
NAME="Refract-$VERSION-linux-x86_64"
STAGE="$OUT/$NAME"
LAUNCHER_TARGET="${CARGO_TARGET_DIR:-$ROOT/build-launcher-package}"

if [[ $SKIP_BUILD == 0 ]]; then
    echo "Building the launcher"
    (cd "$ROOT/launcher" && npm ci --no-audit --no-fund && CARGO_TARGET_DIR="$LAUNCHER_TARGET" npm run tauri -- build --no-bundle)
    echo "Building the host programs"
    "$ROOT/build_host.sh"
fi

echo "Staging $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE"
add() {  # add SOURCE [TARGET]: copies a file or folder from the repository into the package.
    local source="$1" target="${2:-$1}"
    [[ -e "$ROOT/$source" ]] || { echo "Missing $source. Build it first." >&2; exit 1; }
    mkdir -p "$(dirname "$STAGE/$target")"
    cp -a "$ROOT/$source" "$STAGE/$target"
}

cp "$LAUNCHER_TARGET/release/refract-launcher" "$STAGE/Refract"
for file in "$ROOT"/launcher/backend/*.mjs "$ROOT"/launcher/core/*.mjs; do add "${file#"$ROOT/"}"; done
add launcher/inspect_apk.py
add launcher/package.json  # The backend reads its version from it for update checks.
for file in waydroid.sh pose_input_server.py owned_games.example.txt; do add "scripts/$file"; done
for file in android_runtime_policy.py unreal_memory_policy.py android_app_label.py steamvr_app_identity.py; do add "tools/$file"; done
add build-linux/refract-viewer
add build-linux/host-bridge/refract-host-bridge
add build-linux/gpu-layer
add build-android-runtime-linux-arm64-v8a/refract-openxr-runtime-debug.apk
add build-android-runtime-linux-arm64-v8a/refract-systemdriver-debug.apk
add build-platform-sdk-linux/refract-platform-debug.apk
add prebuilts/digitalis-qpr2
for file in NOTICE README.md MANIFEST.txt SHA256SUMS; do [[ -e "$ROOT/prebuilts/digitalis/$file" ]] && add "prebuilts/digitalis/$file"; done
add LICENSE
add CREDITS.md
add docs/linux_waydroid.md
# The PC viewer's SDL3 goes beside it in build-linux/lib, since many distributions don't package SDL3 yet.
sdl="$(ldd "$STAGE/build-linux/refract-viewer" | awk '/libSDL3\.so/ { print $3 }')"
[[ -f "$sdl" ]] || { echo "refract-viewer does not find libSDL3.so.0" >&2; exit 1; }
mkdir -p "$STAGE/build-linux/lib"
cp -L "$sdl" "$STAGE/build-linux/lib/libSDL3.so.0"
patchelf --set-rpath '$ORIGIN/lib' "$STAGE/build-linux/refract-viewer"
sdl_version="$(readlink -f "$sdl")"; sdl_version="3.${sdl_version##*libSDL3.so.0.}"  # libSDL3.so.0.4.16 is SDL 3.4.16
curl -fsSL "https://raw.githubusercontent.com/libsdl-org/SDL/release-$sdl_version/LICENSE.txt" -o "$STAGE/build-linux/lib/SDL3.LICENSE.txt"
# Only the GPU layer's library and manifest; the build folder also holds CMake's files.
find "$STAGE/build-linux/gpu-layer" -mindepth 1 -maxdepth 1 ! -name '*.so' ! -name '*.json' -exec rm -rf {} +

# Node.js runs the launcher backend (the launcher prefers node/node beside it).
NODE="$(command -v node)"
mkdir -p "$STAGE/node"
cp "$NODE" "$STAGE/node/node"
curl -fsSL "https://raw.githubusercontent.com/nodejs/node/$(node --version)/LICENSE" -o "$STAGE/node/LICENSE"

cp "$ROOT/tools/package_readme_linux.txt" "$STAGE/README.txt"

echo "Packing $OUT/$NAME.tar.gz"
tar -C "$OUT" --owner=0 --group=0 -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "Done: $OUT/$NAME.tar.gz ($(du -m "$OUT/$NAME.tar.gz" | cut -f1) MB)"
