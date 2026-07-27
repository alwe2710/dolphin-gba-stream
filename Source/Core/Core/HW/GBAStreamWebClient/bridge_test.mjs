// Verifies the real wasm_bridge.c (not just the earlier toolchain
// smoketest) against realistic messages -- handshake JSON matching what
// GBAStreamHost.cpp actually emits, and a hand-built video/audio binary
// message matching protocol.md's byte layout -- run under Node the same
// way a browser would load this MODULARIZE'd, SINGLE_FILE build.

import FinlinkCore from "./finlink_core.js";

const mod = await FinlinkCore();

function toBytes(str) {
  return new TextEncoder().encode(str);
}

function withHeapCopy(bytes, fn) {
  const ptr = mod._malloc(bytes.length);
  try {
    mod.HEAPU8.set(bytes, ptr);
    return fn(ptr, bytes.length);
  } finally {
    mod._free(ptr);
  }
}

let failures = 0;
function check(name, cond) {
  if (cond) {
    console.log("OK  ", name);
  } else {
    console.error("FAIL", name);
    failures++;
  }
}

// ---------------- hello ----------------
const helloJson = toBytes(
  '{"message":"hello","protocol_version":2,"stream_type":"GC_GBA_LINK",' +
    '"slots":[{"index":0,"label":"P1","occupied":false},{"index":1,"label":"P2","occupied":true}],' +
    '"video":{"width":240,"height":160,"pixel_format":"rgb565","fps":59.7275},' +
    '"audio":{"sample_rate":32768,"channels":2},"input_encoding":"gba_buttons"}'
);
withHeapCopy(helloJson, (ptr, len) => {
  check("peek(hello) == HELLO(1)", mod.ccall("finlink_wasm_peek_handshake_message", "number", ["number", "number"], [ptr, len]) === 1);
  check("parse_hello ok", mod.ccall("finlink_wasm_parse_hello", "number", ["number", "number"], [ptr, len]) === 1);
});
check("hello.protocol_version", mod.ccall("finlink_wasm_hello_protocol_version", "number", [], []) === 2);
check("hello.stream_type", mod.ccall("finlink_wasm_hello_stream_type", "string", [], []) === "GC_GBA_LINK");
check("hello.video.width", mod.ccall("finlink_wasm_hello_video_width", "number", [], []) === 240);
check("hello.video.height", mod.ccall("finlink_wasm_hello_video_height", "number", [], []) === 160);
check("hello.has_audio", mod.ccall("finlink_wasm_hello_has_audio", "number", [], []) === 1);
check("hello.audio.sample_rate", mod.ccall("finlink_wasm_hello_audio_sample_rate", "number", [], []) === 32768);
check("hello.slot_count", mod.ccall("finlink_wasm_hello_slot_count", "number", [], []) === 2);
check("hello.slots[1].label", mod.ccall("finlink_wasm_hello_slot_label", "string", ["number"], [1]) === "P2");
check("hello.slots[1].occupied", mod.ccall("finlink_wasm_hello_slot_occupied", "number", ["number"], [1]) === 1);

// ---------------- hello_ack ----------------
{
  const cap = 512;
  const outPtr = mod._malloc(cap);
  const written = mod.ccall(
    "finlink_wasm_build_hello_ack",
    "number",
    ["number", "number", "number", "number", "number", "number", "number", "number", "number"],
    [0, 240, 160, 59.7275, 1, 32768, 2, outPtr, cap]
  );
  const json = mod.UTF8ToString(outPtr, written);
  mod._free(outPtr);
  const parsed = JSON.parse(json);
  check("hello_ack.message", parsed.message === "hello_ack");
  check("hello_ack.requested_slot", parsed.requested_slot === 0);
  check("hello_ack.audio_limits present", "audio_limits" in parsed);
  check("hello_ack.audio_limits.max_channels", parsed.audio_limits.max_channels === 2);
}

// ---------------- session_ready (with redirect) ----------------
const readyJson = toBytes(
  '{"message":"session_ready","slot":2,"video":{"width":240,"height":160,"fps":59.7275},' +
    '"redirect":{"host":"192.168.1.42","port":6803}}'
);
withHeapCopy(readyJson, (ptr, len) => {
  check("peek(session_ready) == SESSION_READY(2)", mod.ccall("finlink_wasm_peek_handshake_message", "number", ["number", "number"], [ptr, len]) === 2);
  check("parse_session_ready ok", mod.ccall("finlink_wasm_parse_session_ready", "number", ["number", "number"], [ptr, len]) === 1);
});
check("ready.slot", mod.ccall("finlink_wasm_ready_slot", "number", [], []) === 2);
check("ready.has_redirect", mod.ccall("finlink_wasm_ready_has_redirect", "number", [], []) === 1);
check("ready.redirect_host", mod.ccall("finlink_wasm_ready_redirect_host", "string", [], []) === "192.168.1.42");
check("ready.redirect_port", mod.ccall("finlink_wasm_ready_redirect_port", "number", [], []) === 6803);
check("ready.has_audio (should be false, no audio field)", mod.ccall("finlink_wasm_ready_has_audio", "number", [], []) === 0);

// ---------------- handshake_error ----------------
const errJson = toBytes('{"message":"handshake_error","code":"slot_unavailable","detail":"Slot P2 belegt."}');
withHeapCopy(errJson, (ptr, len) => {
  check("peek(handshake_error) == HANDSHAKE_ERROR(3)", mod.ccall("finlink_wasm_peek_handshake_message", "number", ["number", "number"], [ptr, len]) === 3);
  check("parse_handshake_error ok", mod.ccall("finlink_wasm_parse_handshake_error", "number", ["number", "number"], [ptr, len]) === 1);
});
check("error.code", mod.ccall("finlink_wasm_error_code", "string", [], []) === "slot_unavailable");
check("error.detail", mod.ccall("finlink_wasm_error_detail", "string", [], []).includes("P2"));

// ---------------- video header + decode (raw, non-indexed, non-tiled: format=0) ----------------
{
  const width = 8, height = 8;
  // [u8 type=1][u32le width][u32le height][u8 format][...compressed bytes...]
  const header = new Uint8Array(10);
  const dv = new DataView(header.buffer);
  header[0] = 1;
  dv.setUint32(1, width, true);
  dv.setUint32(5, height, true);
  header[9] = 0; // format = raw, full frame
  const fakeCompressed = new Uint8Array([0xde, 0xad, 0xbe, 0xef]); // content irrelevant to parse_video_header itself
  const msg = new Uint8Array(header.length + fakeCompressed.length);
  msg.set(header, 0);
  msg.set(fakeCompressed, header.length);

  withHeapCopy(msg, (ptr, len) => {
    check("parse_video_header ok", mod.ccall("finlink_wasm_parse_video_header", "number", ["number", "number"], [ptr, len]) === 1);
    check("video.width", mod.ccall("finlink_wasm_video_width", "number", [], []) === width);
    check("video.height", mod.ccall("finlink_wasm_video_height", "number", [], []) === height);
    check("video.format", mod.ccall("finlink_wasm_video_format", "number", [], []) === 0);
    const offset = mod.ccall("finlink_wasm_video_compressed_offset", "number", ["number"], [ptr]);
    check("video.compressed_offset", offset === header.length);
    check("video.compressed_size", mod.ccall("finlink_wasm_video_compressed_size", "number", [], []) === fakeCompressed.length);
  });

  // Now feed a hand-built *inflated* (raw, uncompressed) RGB565 frame --
  // 8x8 pixels, each pixel set to its own index so decode order is
  // verifiable, not just "didn't crash".
  const pixelCount = width * height;
  const inflated = new Uint8Array(pixelCount * 2);
  const idv = new DataView(inflated.buffer);
  for (let i = 0; i < pixelCount; i++) idv.setUint16(i * 2, i, true);

  const fbCap = pixelCount * 2;
  const fbPtr = mod._malloc(fbCap);
  withHeapCopy(inflated, (inPtr, inLen) => {
    const ok = mod.ccall(
      "finlink_wasm_decode_video_frame",
      "number",
      ["number", "number", "number", "number", "number", "number", "number"],
      [0, inPtr, inLen, width, height, fbPtr, fbCap]
    );
    check("decode_video_frame ok", ok === 1);
  });
  const fb = mod.HEAPU8.subarray(fbPtr, fbPtr + fbCap);
  const fbView = new DataView(fb.buffer, fb.byteOffset, fb.byteLength);
  let pixelsMatch = true;
  for (let i = 0; i < pixelCount; i++) {
    if (fbView.getUint16(i * 2, true) !== i) pixelsMatch = false;
  }
  check("decoded framebuffer matches input pixels (raw format passthrough)", pixelsMatch);
  mod._free(fbPtr);
}

// ---------------- audio ----------------
{
  // [u8 type=3][u32le sampleRate][u8 channels][s16le samples...]
  const samples = [100, -200, 300, -400]; // 2 frames of stereo
  const msg = new Uint8Array(6 + samples.length * 2);
  const dv = new DataView(msg.buffer);
  msg[0] = 3;
  dv.setUint32(1, 32768, true);
  msg[5] = 2;
  samples.forEach((s, i) => dv.setInt16(6 + i * 2, s, true));

  withHeapCopy(msg, (ptr, len) => {
    check("parse_audio_frame ok", mod.ccall("finlink_wasm_parse_audio_frame", "number", ["number", "number"], [ptr, len]) === 1);
    check("audio.sample_rate", mod.ccall("finlink_wasm_audio_sample_rate", "number", [], []) === 32768);
    check("audio.channels", mod.ccall("finlink_wasm_audio_channels", "number", [], []) === 2);
    check("audio.sample_count", mod.ccall("finlink_wasm_audio_sample_count", "number", [], []) === samples.length);
    const offset = mod.ccall("finlink_wasm_audio_samples_offset", "number", ["number"], [ptr]);
    check("audio.samples_offset", offset === 6);
  });
}

// ---------------- input ----------------
{
  const bufPtr = mod._malloc(3);
  const n = mod.ccall("finlink_wasm_build_input_frame", "number", ["number", "number"], [0b1010101010, bufPtr]);
  check("build_input_frame size", n === 3);
  const bytes = mod.HEAPU8.subarray(bufPtr, bufPtr + 3);
  check("input_frame[0] == type 2", bytes[0] === 2);
  check("input_frame keymask LE", (bytes[2] << 8 | bytes[1]) === 0b1010101010);
  mod._free(bufPtr);
}

if (failures > 0) {
  console.error(`\n${failures} check(s) FAILED`);
  process.exit(1);
}
console.log("\nALL OK");
