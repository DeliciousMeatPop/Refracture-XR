#!/usr/bin/env bash
# Linux counterpart of build_host.cmd: builds the host bridge, the PC viewer and the GPU sharing layer into
# build-linux/ and runs the native tests. Needs CMake, Ninja, a C++20 compiler, curl, and the Vulkan, OpenXR and SDL3
# libraries. The headers come from the dev packages (Fedora: vulkan-headers openxr-devel SDL3-devel; Debian/Ubuntu:
# libvulkan-dev libopenxr-dev libsdl3-dev) or from these directories:
#   REFRACT_OPENXR_HEADERS=/path/include REFRACT_VULKAN_HEADERS=/path/include REFRACT_SDL3_HEADERS=/path/include ./build_host.sh
# Without either, the headers matching the installed libraries are downloaded into build-linux/_deps.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS="$ROOT/build-linux/_deps"

# The version in an installed library's file name (libvulkan.so -> 1.4.341 from libvulkan.so.1.4.341), or the
# default if it is not installed.
library_version() {
    local library="$1" default="$2" file
    file="$(ls /usr/lib64/"$library".* /usr/lib/x86_64-linux-gnu/"$library".* /usr/lib/"$library".* 2>/dev/null | sort -V | tail -1)"
    [[ -n "$file" ]] && echo "${file#*"$library".}" || echo "$default"
}

# Prints the directory holding the header $2 (vulkan/vulkan.h, ...): $3 if given, nothing if the system has it, else
# the include/ directory of the source archive $4, downloaded into build-linux/_deps/$1.
headers() {
    local name="$1" header="$2" given="$3" url="$4" dir
    [[ -n "$given" ]] && { echo "$given"; return; }
    for dir in /usr/include /usr/local/include; do [[ -f "$dir/$header" ]] && return; done
    if [[ ! -f "$DEPS/$name/include/$header" ]]; then
        echo "build_host.sh: no $header installed; downloading it from $url" >&2
        mkdir -p "$DEPS"
        dir="$(mktemp -d "$DEPS/.download.XXXXXX")"
        curl -fsSL "$url" | tar -xz -C "$dir" --strip-components=1 --wildcards '*/include/*'
        [[ -f "$dir/include/$header" ]] || { rm -rf "$dir"; echo "build_host.sh: $url has no include/$header" >&2; exit 1; }
        rm -rf "$DEPS/$name"
        mkdir -p "$DEPS/$name"
        mv "$dir/include" "$DEPS/$name/include"
        rm -rf "$dir"
    fi
    echo "$DEPS/$name/include"
}

vulkan="$(headers vulkan vulkan/vulkan.h "${REFRACT_VULKAN_HEADERS:-}" \
    "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v$(library_version libvulkan.so 1.4.341).tar.gz")"
openxr="$(headers openxr openxr/openxr.h "${REFRACT_OPENXR_HEADERS:-}" \
    "https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-$(library_version libopenxr_loader.so 1.1.62).tar.gz")"
# SDL 3.x.y installs libSDL3.so.0.x.y.
sdl3_version="$(library_version libSDL3.so 0.4.14)"
sdl3="$(headers sdl3 SDL3/SDL.h "${REFRACT_SDL3_HEADERS:-}" \
    "https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.${sdl3_version#*.}.tar.gz")"

cmake -S "$ROOT" -B "$ROOT/build-linux" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DREFRACT_OPENXR_HEADERS="$openxr" -DREFRACT_VULKAN_HEADERS="$vulkan" -DREFRACT_SDL3_HEADERS="$sdl3" \
    -UREFRACT_SDL3_INCLUDE
cmake --build "$ROOT/build-linux"
ctest --test-dir "$ROOT/build-linux" --output-on-failure
