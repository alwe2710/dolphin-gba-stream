// Toolchain proof-of-concept only -- verifies the emsdk + finlink_core
// pipeline actually produces a JS-callable WASM module before the real
// browser client (protocol handshake, video/audio decode, input encode,
// wired into GBAStreamClientPage.h) gets built on top of it. Not part of
// that real client; safe to delete once the real bridge exists and has
// been verified to subsume what this checks.

#include <emscripten.h>
#include <string.h>

#include "finlink/handshake.h"

EMSCRIPTEN_KEEPALIVE
int finlink_wasm_protocol_version(void) {
    return FINLINK_PROTOCOL_VERSION;
}

// Round-trip proof that a real finlink/core function (JSON building, not
// just a constant) works correctly when called across the JS/WASM
// boundary: `out_buf` is a buffer the JS caller allocated via _malloc and
// will read back via UTF8ToString, `req` scalar fields cross the boundary
// as plain ints/doubles (no pointer args needed for this one, unlike the
// eventual real bridge which will need to hand raw WebSocket frame bytes
// both ways).
EMSCRIPTEN_KEEPALIVE
int finlink_wasm_build_hello_ack(int requested_slot, unsigned int max_width,
                                  unsigned int max_height, double max_fps, char *out_buf,
                                  int out_capacity) {
    finlink_hello_ack_request req;
    memset(&req, 0, sizeof(req));
    req.requested_slot = requested_slot;
    req.max_width = max_width;
    req.max_height = max_height;
    req.max_fps = max_fps;
    req.wants_audio = 0;
    return (int)finlink_build_hello_ack(&req, out_buf, (size_t)out_capacity);
}
