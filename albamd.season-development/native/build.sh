#!/usr/bin/env bash
# Builds native/season_development.dll with zig c++, as Swofty's addon template does. The first run downloads
# Swofty's MIT addon SDK headers into native/include, pinned to one commit of Swofty-Developments/SwoftyLauncher.
set -euo pipefail
SELF=$(readlink -f "$0")
cd "$(dirname "$SELF")"
SDK_COMMIT=2d66119fa4aacb9b9122b503feffc22e5bc734aa
if [ ! -f include/swofty_api.h ]; then
    curl -fsSL "https://codeload.github.com/Swofty-Developments/SwoftyLauncher/tar.gz/$SDK_COMMIT" |
        tar -xz --strip-components=4 "SwoftyLauncher-$SDK_COMMIT/templates/addon/native/include"
    curl -fsSL -o include/LICENSE-swofty-sdk \
        "https://raw.githubusercontent.com/Swofty-Developments/SwoftyLauncher/$SDK_COMMIT/templates/addon/LICENSE"
fi
if ! command -v zig >/dev/null; then
    exec nix shell nixpkgs#zig -c "$SELF" "$@"
fi
export ZIG_GLOBAL_CACHE_DIR=$PWD/build/zig-cache ZIG_LOCAL_CACHE_DIR=$PWD/build/zig-cache
mkdir -p build
zig c++ -target x86_64-windows-gnu -std=c++23 -O2 -Wall -Wextra -Iinclude -shared -s \
    -o season_development.dll src/season_development.cpp
rm -f season_development.lib
echo "built $(pwd)/season_development.dll"
