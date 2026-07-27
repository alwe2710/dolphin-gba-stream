#!/usr/bin/env bash
# Builds the WASM web client bridge from a clean checkout: activates the
# vendored emsdk toolchain (installing it on first run -- network access
# needed exactly once, then it's cached under Externals/emsdk, which
# .gitignore excludes from the repo itself, see its own comment there),
# configures+builds finlink/core (Externals/finlink submodule) against
# that toolchain, links this directory's bridge (wasm_bridge.c) against it
# as one self-contained JS file (-s SINGLE_FILE=1 -- the .wasm binary is
# base64-embedded inside, matching how this feature has no separate static
# file serving at all, only the one big embedded HTML string in
# GBAStreamClientPage.h), verifies the result with bridge_test.mjs (real
# finlink_core behavior, not just "it compiled"), then wraps that JS as a
# C++ header (GBAStreamWebClientJs.h) GBAStreamClientPage.h's HTML can
# inline as a <script> block.
#
# Invoked by CMake (see CMakeLists.txt in this directory) when
# BUILD_GBA_WEB_CLIENT_WASM=ON, but also runnable standalone for iterating
# on the bridge without a full Dolphin CMake reconfigure:
#   Source/Core/Core/HW/GBAStreamWebClient/build_wasm.sh
#
# $1 (optional): output directory for intermediate build artifacts (the
# nested finlink_core wasm build, mainly). Defaults to this script's own
# directory (matches running it standalone); CMake passes its own build
# directory instead, so those intermediates don't land in the source tree
# during a normal `cmake --build`. GBAStreamWebClientJs.h is always written
# into this script's own directory regardless -- it's a source file
# GBAStreamClientPage.h #includes, not a build artifact.
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

echo "== building the wasm bridge =="
# Every finlink_wasm_* function wasm_bridge.c defines with EMSCRIPTEN_KEEPALIVE
# -- kept as one explicit list (not -s EXPORT_ALL=1) so an unused/typo'd
# export fails the link loudly instead of silently doing nothing.
EXPORTED_FUNCTIONS=_finlink_wasm_parse_hello,_finlink_wasm_hello_protocol_version,_finlink_wasm_hello_stream_type,_finlink_wasm_hello_input_encoding,_finlink_wasm_hello_video_width,_finlink_wasm_hello_video_height,_finlink_wasm_hello_video_fps,_finlink_wasm_hello_has_audio,_finlink_wasm_hello_audio_sample_rate,_finlink_wasm_hello_audio_channels,_finlink_wasm_hello_slot_count,_finlink_wasm_hello_slot_index,_finlink_wasm_hello_slot_label,_finlink_wasm_hello_slot_occupied,_finlink_wasm_build_hello_ack,_finlink_wasm_parse_session_ready,_finlink_wasm_ready_slot,_finlink_wasm_ready_video_width,_finlink_wasm_ready_video_height,_finlink_wasm_ready_video_fps,_finlink_wasm_ready_has_audio,_finlink_wasm_ready_audio_sample_rate,_finlink_wasm_ready_audio_channels,_finlink_wasm_ready_has_redirect,_finlink_wasm_ready_redirect_host,_finlink_wasm_ready_redirect_port,_finlink_wasm_parse_handshake_error,_finlink_wasm_error_code,_finlink_wasm_error_detail,_finlink_wasm_peek_handshake_message,_finlink_wasm_parse_video_header,_finlink_wasm_video_width,_finlink_wasm_video_height,_finlink_wasm_video_format,_finlink_wasm_video_compressed_offset,_finlink_wasm_video_compressed_size,_finlink_wasm_video_max_inflated_size,_finlink_wasm_decode_video_frame,_finlink_wasm_parse_audio_frame,_finlink_wasm_audio_sample_rate,_finlink_wasm_audio_channels,_finlink_wasm_audio_sample_count,_finlink_wasm_audio_samples_offset,_finlink_wasm_build_input_frame,_malloc,_free

emcc "$SCRIPT_DIR/wasm_bridge.c" \
  -I "$FINLINK_CORE_DIR/include" \
  "$CORE_BUILD_DIR/libfinlink_core.a" \
  -o "$OUT_DIR/finlink_core.js" \
  -s MODULARIZE=1 -s EXPORT_NAME=FinlinkCore -s SINGLE_FILE=1 \
  -s EXPORTED_FUNCTIONS="$EXPORTED_FUNCTIONS" \
  -s EXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,HEAPU8,HEAP32 \
  -O2

echo "== running the Node.js bridge test =="
# Only copy when OUT_DIR actually differs (the CMake-invoked case, whose
# build dir is separate from this script's own) -- when run standalone,
# OUT_DIR defaults to SCRIPT_DIR itself (see above), and `cp` refuses to
# copy a file onto itself.
if [ "$SCRIPT_DIR" != "$OUT_DIR" ]; then
  cp "$SCRIPT_DIR/bridge_test.mjs" "$OUT_DIR/bridge_test.mjs"
fi
(cd "$OUT_DIR" && "$EMSDK_DIR/node/"*/bin/node bridge_test.mjs)

echo "== wrapping finlink_core.js as a C++ header =="
JS_HEADER="$SCRIPT_DIR/GBAStreamWebClientJs.h"
{
  echo "// Generated by build_wasm.sh from finlink_core.js -- DO NOT EDIT BY HAND."
  echo "// Regenerate with: Source/Core/Core/HW/GBAStreamWebClient/build_wasm.sh"
  echo "// SPDX-License-Identifier: GPL-2.0-or-later"
  echo "#pragma once"
  echo "#ifdef HAS_LIBMGBA"
  echo "#include <string_view>"
  echo "namespace HW::GBA {"
  # The "sv" suffix (not a bare raw-string literal implicitly converted to
  # string_view) is load-bearing here, not stylistic: emscripten's
  # SINGLE_FILE=1 output embeds the compiled .wasm binary as raw bytes
  # inside a JS string (binaryDecode() reverses a per-character bit trick
  # rather than base64-decoding), and a WASM binary's very first bytes are
  # its magic number, "\0asm" -- an embedded NUL. std::string_view's
  # constructor from a bare `const char*` computes length via strlen(),
  # silently truncating at that very first NUL and losing >95% of the
  # actual content; the "sv" literal instead captures the raw string
  # literal's true, full compile-time array length. Confirmed via a real
  # browser: the truncated bare-pointer version produced "SyntaxError:
  # Invalid or unexpected token" (an unterminated JS string) followed by
  # "FinlinkCore is not defined", not caught by bridge_test.mjs above since
  # that runs directly against finlink_core.js and never goes through this
  # string_view construction step at all.
  echo "using namespace std::string_view_literals;"
  echo 'inline constexpr std::string_view GBA_STREAM_WEB_CLIENT_JS = R"FINLINK_WASM_JS('
  cat "$OUT_DIR/finlink_core.js"
  echo ')FINLINK_WASM_JS"sv;'
  echo "}  // namespace HW::GBA"
  echo "#endif  // HAS_LIBMGBA"
} > "$JS_HEADER"

echo "== OK: $JS_HEADER ($(wc -c < "$JS_HEADER") bytes) =="
