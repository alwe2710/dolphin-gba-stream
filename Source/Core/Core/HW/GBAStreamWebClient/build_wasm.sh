#!/usr/bin/env bash
# Builds the WASM web client bridge from a clean checkout: activates the
# vendored emsdk toolchain (installing it on first run -- network access
# needed exactly once, then it's cached under Externals/emsdk, which
# .gitignore excludes from the repo itself, see its own comment there),
# configures+builds finlink/core (Externals/finlink submodule) against
# that toolchain, then links this directory's bridge C file against it.
#
# Invoked by CMake (see CMakeLists.txt in this directory) when
# BUILD_GBA_WEB_CLIENT_WASM=ON, but also runnable standalone for iterating
# on the bridge without a full Dolphin CMake reconfigure:
#   Source/Core/Core/HW/GBAStreamWebClient/build_wasm.sh
#
# $1 (optional): output directory for build artifacts. Defaults to this
# script's own directory (matches running it standalone); CMake passes its
# own build directory instead, so generated files don't land in the
# source tree during a normal `cmake --build`.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../../.." && pwd)"
EMSDK_DIR="$REPO_ROOT/Externals/emsdk"
FINLINK_CORE_DIR="$REPO_ROOT/Externals/finlink/core"
EMSDK_VERSION="6.0.4" # keep in sync with the version this was last verified against

OUT_DIR="${1:-$SCRIPT_DIR}"
mkdir -p "$OUT_DIR"

if [ ! -x "$EMSDK_DIR/upstream/emscripten/emcc" ]; then
  echo "== emsdk toolchain not found, installing $EMSDK_VERSION (one-time, needs network) =="
  "$EMSDK_DIR/emsdk" install "$EMSDK_VERSION"
fi
"$EMSDK_DIR/emsdk" activate "$EMSDK_VERSION" > /dev/null
# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" > /dev/null

echo "== configuring+building finlink_core for wasm32 =="
CORE_BUILD_DIR="$OUT_DIR/finlink_core_wasm"
emcmake cmake -S "$FINLINK_CORE_DIR" -B "$CORE_BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release -DFINLINK_BUILD_TESTS=OFF > /dev/null
cmake --build "$CORE_BUILD_DIR" -j"$(nproc)"

echo "== building the wasm bridge smoketest =="
emcc "$SCRIPT_DIR/wasm_bridge_smoketest.c" \
  -I "$FINLINK_CORE_DIR/include" \
  "$CORE_BUILD_DIR/libfinlink_core.a" \
  -o "$OUT_DIR/finlink_core_smoketest.js" \
  -s MODULARIZE=1 -s EXPORT_NAME=FinlinkCoreSmoketest \
  -s EXPORTED_FUNCTIONS=_finlink_wasm_protocol_version,_finlink_wasm_build_hello_ack,_malloc,_free \
  -s EXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString \
  -O2

echo "== running the Node.js smoketest =="
cp "$SCRIPT_DIR/smoketest.mjs" "$OUT_DIR/smoketest.mjs"
(cd "$OUT_DIR" && "$EMSDK_DIR/node/"*/bin/node smoketest.mjs)

echo "== OK: $OUT_DIR/finlink_core_smoketest.{js,wasm} =="
