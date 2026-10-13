#!/bin/sh
# Downloads every third-party source the engine needs into one directory, so a later build can run with no network:
#
#   Rust/tools/prefetch-deps.sh /path/to/deps            # online machine: fills /path/to/deps
#   tar -C /path/to -czf sturdy-deps.tar.gz deps          # ship it (Flatpak source archive, CI cache, ...)
#   ...
#   STURDY_RS_DEPS_DIR=/path/to/deps STURDY_RS_OFFLINE=1 cargo build   # offline machine
#
# It configures the engine once with the same dependency-affecting options the Rust build uses. Cargo features that
# pull extra sources (for example `webgpu`, which fetches Dawn) need the matching CMake option passed through:
#
#   Rust/tools/prefetch-deps.sh /path/to/deps -DSTURDY_ENABLE_WEBGPU=ON
#
# Only configure-time fetching happens here; nothing is compiled.
set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <deps-dir> [extra cmake arguments...]" >&2
    exit 2
fi
deps=$1
shift
engine=$(cd "$(dirname "$0")/../.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

mkdir -p "$deps"
cmake -S "$engine" -B "$scratch/build" -G Ninja \
    -DSTURDY_DEPS_CACHE_DIR="$deps" \
    -DSTURDY_BUILD_RUNTIME_DEMO=OFF \
    -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER \
    "$@"
echo "Dependencies are in $deps"
