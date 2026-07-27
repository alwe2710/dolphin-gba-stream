# WASM web client bridge (finlink/protocol.md's "Web-Client-Vereinheitlichung",
# item 3.6 of the protocol-v2 task): compiles the finlink_core submodule
# (Externals/finlink/core) for wasm32 via the vendored emsdk submodule
# (Externals/emsdk), so the browser client can share the same
# protocol/codec logic as every other finlink client instead of maintaining
# a parallel JS implementation.
#
# Currently only builds+runs the toolchain proof-of-concept
# (wasm_bridge_smoketest.c + smoketest.mjs) -- the real bridge (actual
# handshake/video/audio/input wired into GBAStreamClientPage.h's existing
# UI) is a separate, larger follow-up step; this establishes that the
# emsdk-submodule-to-JS-callable-WASM pipeline itself genuinely works
# end-to-end before building anything real on top of it.
#
# Off by default: the very first build needs network access to install the
# emsdk toolchain (Externals/emsdk/emsdk install, a few hundred MB, cached
# under Externals/emsdk/ afterward -- see .gitignore's comment there), which
# a normal Dolphin build shouldn't silently require.
option(BUILD_GBA_WEB_CLIENT_WASM
       "Build the WASM web client bridge (finlink_core for wasm32 + smoketest). Needs network access on first build to install emsdk."
       OFF)

if(BUILD_GBA_WEB_CLIENT_WASM)
  add_custom_target(gba_web_client_wasm_smoketest ALL
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/build_wasm.sh ${CMAKE_CURRENT_BINARY_DIR}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building+verifying the finlink_core WASM toolchain (see build_wasm.sh)"
    VERBATIM
  )
endif()
