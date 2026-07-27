# WASM web client bridge (finlink/protocol.md's "Web-Client-Vereinheitlichung",
# item 3.6 of the protocol-v2 task): compiles the finlink_core submodule
# (Externals/finlink/core) for wasm32 via the vendored emsdk submodule
# (Externals/emsdk) and links wasm_bridge.c against it, so the browser
# client (GBAStreamClientPage.h) shares the same protocol/codec logic
# (handshake message fields, video tile-diff/palette decode, audio frame
# layout, input frame encoding) as every other finlink client instead of
# maintaining a parallel JS implementation of it -- see wasm_bridge.c's own
# top comment for what's deliberately *not* bridged (WS framing, deflate)
# and why.
#
# build_wasm.sh's own output is GBAStreamWebClientJs.h, wrapping the built
# JS as a C++ raw string GBAStreamClientPage.h #includes -- that header IS
# committed to the repo (unlike the intermediate .js/.wasm build products,
# see .gitignore), precisely so a normal build never needs emsdk/network
# access at all. This option only controls whether that committed header
# gets regenerated+reverified from source, not whether it exists.
option(BUILD_GBA_WEB_CLIENT_WASM
       "Regenerate+verify GBAStreamWebClientJs.h from finlink_core (wasm32). Needs network access on first build to install emsdk."
       OFF)

if(BUILD_GBA_WEB_CLIENT_WASM)
  add_custom_target(gba_web_client_wasm ALL
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build_wasm.sh ${CMAKE_CURRENT_BINARY_DIR}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building+verifying the finlink_core WASM bridge (see build_wasm.sh)"
    VERBATIM
  )
endif()
