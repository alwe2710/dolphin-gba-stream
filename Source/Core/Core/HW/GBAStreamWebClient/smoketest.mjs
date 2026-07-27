// Toolchain proof-of-concept: loads the emcc-built WASM module in Node
// (same MODULARIZE'd JS glue a browser would run) and exercises a real
// finlink_core function across the JS/WASM boundary, not just a constant.
// See wasm_bridge_smoketest.c's comment -- delete both once the real
// browser bridge subsumes this.

import FinlinkCoreSmoketest from "./finlink_core_smoketest.js";

const mod = await FinlinkCoreSmoketest();

const version = mod.ccall("finlink_wasm_protocol_version", "number", [], []);
console.log("protocol_version:", version);
if (version !== 2) {
  throw new Error(`expected protocol_version 2, got ${version}`);
}

const outCapacity = 512;
const outPtr = mod._malloc(outCapacity);
try {
  const written = mod.ccall(
    "finlink_wasm_build_hello_ack",
    "number",
    ["number", "number", "number", "number", "number", "number"],
    [0, 240, 160, 60.0, outPtr, outCapacity]
  );
  console.log("hello_ack bytes written:", written);
  if (written <= 0) {
    throw new Error("finlink_build_hello_ack reported failure (0 bytes written)");
  }
  const json = mod.UTF8ToString(outPtr, written);
  console.log("hello_ack JSON:", json);

  const parsed = JSON.parse(json);
  if (parsed.message !== "hello_ack") throw new Error("unexpected message field");
  if (parsed.protocol_version !== 2) throw new Error("unexpected protocol_version field");
  if (parsed.requested_slot !== 0) throw new Error("unexpected requested_slot field");
  if (parsed.video_limits.max_width !== 240) throw new Error("unexpected max_width field");
  if (parsed.video_limits.max_height !== 160) throw new Error("unexpected max_height field");
  if ("audio_limits" in parsed) throw new Error("audio_limits should be absent (wants_audio=0)");
} finally {
  mod._free(outPtr);
}

console.log("ALL OK -- emsdk + finlink_core + WASM/JS round trip verified");
