// WASM bridge for the browser client (GBAStreamClientPage.h): exposes just
// the parts of finlink/core that are genuinely protocol-specific knowledge
// worth sharing with the other clients (Android/3DS/Switch) -- handshake
// message field semantics, the video tile-diff/palette decode algorithm,
// audio frame layout, input frame encoding.
//
// Deliberately NOT bridged, on purpose, not as an oversight: WebSocket
// framing/handshake (the browser's native WebSocket already speaks RFC6455,
// unlike jni_bridge.c's raw-socket platforms which have no such thing) and
// raw-deflate decompression (DecompressionStream('deflate-raw') is a native
// browser API -- reimplementing/re-linking miniz via WASM for something the
// platform already does, likely faster, wouldn't reduce duplicated protocol
// knowledge, since deflate itself isn't part of this app's protocol design
// at all, just its chosen transport-level compression). Using those two
// native APIs isn't "a parallel protocol implementation" in the sense
// protocol.md's clients are meant to avoid -- it's using what the platform
// already provides correctly, the same way Android's jni_bridge.c doesn't
// reimplement TCP either.
//
// One parse call per message populates a module-static struct; the getters
// below read out of it. Not thread-safe, but this bridge is only ever
// driven from the single browser JS thread that owns the WebSocket, so
// there's no concurrent access to guard against.

#include <emscripten.h>
#include <stdint.h>
#include <string.h>

#include "finlink/handshake.h"
#include "finlink/protocol.h"

/* ---------------- hello ---------------- */

static finlink_hello g_hello;

EMSCRIPTEN_KEEPALIVE
int finlink_wasm_parse_hello(const uint8_t *data, int size) {
    return finlink_parse_hello(data, (size_t)size, &g_hello) == FINLINK_HANDSHAKE_OK;
}
EMSCRIPTEN_KEEPALIVE int finlink_wasm_hello_protocol_version(void) { return g_hello.protocol_version; }
EMSCRIPTEN_KEEPALIVE const char *finlink_wasm_hello_stream_type(void) { return g_hello.stream_type; }
EMSCRIPTEN_KEEPALIVE const char *finlink_wasm_hello_input_encoding(void) { return g_hello.input_encoding; }
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_hello_video_width(void) { return g_hello.video.width; }
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_hello_video_height(void) { return g_hello.video.height; }
EMSCRIPTEN_KEEPALIVE double finlink_wasm_hello_video_fps(void) { return g_hello.video.fps; }
EMSCRIPTEN_KEEPALIVE int finlink_wasm_hello_has_audio(void) { return g_hello.has_audio; }
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_hello_audio_sample_rate(void) {
    return g_hello.audio.sample_rate;
}
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_hello_audio_channels(void) { return g_hello.audio.channels; }
EMSCRIPTEN_KEEPALIVE int finlink_wasm_hello_slot_count(void) { return (int)g_hello.slot_count; }
EMSCRIPTEN_KEEPALIVE int finlink_wasm_hello_slot_index(int i) {
    return (i >= 0 && (size_t)i < g_hello.slot_count) ? g_hello.slots[i].index : -1;
}
EMSCRIPTEN_KEEPALIVE const char *finlink_wasm_hello_slot_label(int i) {
    return (i >= 0 && (size_t)i < g_hello.slot_count) ? g_hello.slots[i].label : "";
}
EMSCRIPTEN_KEEPALIVE int finlink_wasm_hello_slot_occupied(int i) {
    return (i >= 0 && (size_t)i < g_hello.slot_count) ? g_hello.slots[i].occupied : 0;
}

/* ------------- hello_ack (build only, client -> server) ------------- */

EMSCRIPTEN_KEEPALIVE
int finlink_wasm_build_hello_ack(int requested_slot, unsigned int max_width, unsigned int max_height,
                                  double max_fps, int wants_audio, unsigned int max_sample_rate,
                                  unsigned int max_channels, char *out_buf, int out_capacity) {
    finlink_hello_ack_request req;
    memset(&req, 0, sizeof(req));
    req.requested_slot = requested_slot;
    req.max_width = max_width;
    req.max_height = max_height;
    req.max_fps = max_fps;
    req.wants_audio = wants_audio;
    req.max_sample_rate = max_sample_rate;
    req.max_channels = (uint8_t)max_channels;
    return (int)finlink_build_hello_ack(&req, out_buf, (size_t)out_capacity);
}

/* ---------------- session_ready ---------------- */

static finlink_session_ready g_ready;

EMSCRIPTEN_KEEPALIVE
int finlink_wasm_parse_session_ready(const uint8_t *data, int size) {
    return finlink_parse_session_ready(data, (size_t)size, &g_ready) == FINLINK_HANDSHAKE_OK;
}
EMSCRIPTEN_KEEPALIVE int finlink_wasm_ready_slot(void) { return g_ready.slot; }
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_ready_video_width(void) { return g_ready.video.width; }
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_ready_video_height(void) { return g_ready.video.height; }
EMSCRIPTEN_KEEPALIVE double finlink_wasm_ready_video_fps(void) { return g_ready.video.fps; }
EMSCRIPTEN_KEEPALIVE int finlink_wasm_ready_has_audio(void) { return g_ready.has_audio; }
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_ready_audio_sample_rate(void) {
    return g_ready.audio.sample_rate;
}
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_ready_audio_channels(void) { return g_ready.audio.channels; }
EMSCRIPTEN_KEEPALIVE int finlink_wasm_ready_has_redirect(void) { return g_ready.has_redirect; }
EMSCRIPTEN_KEEPALIVE const char *finlink_wasm_ready_redirect_host(void) { return g_ready.redirect_host; }
EMSCRIPTEN_KEEPALIVE int finlink_wasm_ready_redirect_port(void) { return g_ready.redirect_port; }

/* ---------------- handshake_error ---------------- */

static finlink_handshake_error g_error;

EMSCRIPTEN_KEEPALIVE
int finlink_wasm_parse_handshake_error(const uint8_t *data, int size) {
    return finlink_parse_handshake_error(data, (size_t)size, &g_error) == FINLINK_HANDSHAKE_OK;
}
EMSCRIPTEN_KEEPALIVE const char *finlink_wasm_error_code(void) { return g_error.code; }
EMSCRIPTEN_KEEPALIVE const char *finlink_wasm_error_detail(void) { return g_error.detail; }

// So JS knows which parse_* above to call for a given text frame, without
// hand-checking the "message" field itself (see this file's top comment on
// why *this* still goes through core rather than a JS-side `switch` on
// `JSON.parse(text).message` -- the discriminator string itself is schema
// knowledge, same reasoning as the field getters above).
EMSCRIPTEN_KEEPALIVE
int finlink_wasm_peek_handshake_message(const uint8_t *data, int size) {
    return (int)finlink_peek_handshake_message(data, (size_t)size);
}

/* ---------------- video (type=1) ----------------
 * JS still owns decompression (DecompressionStream('deflate-raw'), see this
 * file's top comment) -- finlink_wasm_parse_video_header locates the
 * compressed span within the original message for JS to slice out and
 * inflate itself; finlink_wasm_decode_video_frame then takes the resulting
 * *inflated* bytes (which JS must copy into WASM memory first, e.g. via
 * HEAPU8.set()) and does the actual tile-diff/palette decode. */

static finlink_video_header g_video_header;

EMSCRIPTEN_KEEPALIVE
int finlink_wasm_parse_video_header(const uint8_t *data, int size) {
    return finlink_parse_video_header(data, (size_t)size, &g_video_header) == FINLINK_OK;
}
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_video_width(void) { return g_video_header.width; }
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_video_height(void) { return g_video_header.height; }
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_video_format(void) { return g_video_header.format; }
// compressed_data points into the same `data` buffer finlink_wasm_parse_video_header
// was given -- returns its offset from that buffer's start (not a separate
// pointer) so JS can slice `data.subarray(offset, offset+size)` itself.
EMSCRIPTEN_KEEPALIVE
unsigned int finlink_wasm_video_compressed_offset(const uint8_t *data) {
    return (unsigned int)(g_video_header.compressed_data - data);
}
EMSCRIPTEN_KEEPALIVE
unsigned int finlink_wasm_video_compressed_size(void) {
    return (unsigned int)g_video_header.compressed_size;
}

EMSCRIPTEN_KEEPALIVE
unsigned int finlink_wasm_video_max_inflated_size(unsigned int width, unsigned int height) {
    return (unsigned int)finlink_video_max_inflated_size(width, height);
}

// `framebuffer_rgb565` must be the caller's PERSISTENT buffer across calls
// for the same stream (see finlink/protocol.h) -- JS keeps this as a WASM
// heap allocation for the stream's lifetime, not something reallocated per
// frame, so a FINLINK_VIDEO_FORMAT_TILES frame's partial update lands on
// top of what the previous call actually wrote, not a blank buffer.
EMSCRIPTEN_KEEPALIVE
int finlink_wasm_decode_video_frame(unsigned int format, const uint8_t *inflated, int inflated_size,
                                     unsigned int width, unsigned int height,
                                     uint8_t *framebuffer_rgb565, int framebuffer_capacity) {
    return finlink_decode_video_frame((uint8_t)format, inflated, (size_t)inflated_size, width, height,
                                       framebuffer_rgb565,
                                       (size_t)framebuffer_capacity) == FINLINK_OK;
}

/* ---------------- audio (type=3) ---------------- */

static finlink_audio_frame g_audio_frame;

EMSCRIPTEN_KEEPALIVE
int finlink_wasm_parse_audio_frame(const uint8_t *data, int size) {
    return finlink_parse_audio_frame(data, (size_t)size, &g_audio_frame) == FINLINK_OK;
}
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_audio_sample_rate(void) { return g_audio_frame.sample_rate; }
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_audio_channels(void) { return g_audio_frame.channels; }
EMSCRIPTEN_KEEPALIVE unsigned int finlink_wasm_audio_sample_count(void) {
    return (unsigned int)g_audio_frame.sample_count;
}
// Same offset-into-original-buffer convention as the video span above.
EMSCRIPTEN_KEEPALIVE
unsigned int finlink_wasm_audio_samples_offset(const uint8_t *data) {
    return (unsigned int)(g_audio_frame.samples - data);
}

/* ---------------- input (type=2, client -> server) ---------------- */

EMSCRIPTEN_KEEPALIVE
int finlink_wasm_build_input_frame(unsigned int key_mask, uint8_t *out_buf) {
    return (int)finlink_build_input_frame((uint16_t)key_mask, out_buf);
}
