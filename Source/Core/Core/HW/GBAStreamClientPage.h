// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef HAS_LIBMGBA

#include <string_view>

namespace HW::GBA
{
// Single-page client shared by GBAStreamLobby (always-on port 6800) and
// GBAStreamHost (one player-port WebSocket server per GC port configured as
// GBA (Client-Stream), 6801-6804). Landing view is a P1-P4 lobby: for each of
// the four possible player ports it probes http://<host>:(6801+n)/status to
// find out which ones currently have a GBAStreamHost running at all (i.e.
// are configured as GBA (Client-Stream)) and whether each is already
// occupied by another client, then shows a picker with unavailable slots
// grayed out. Picking a slot opens a WebSocket directly to that player port
// and switches to the canvas+input view, which decodes raw-deflate RGB565
// video frames, plays PCM audio via the Web Audio API, and sends a 3-byte
// input message whenever the locally-held button state changes. On a
// touch-capable device (phones/tablets, which have no physical keyboard) an
// on-screen D-pad and button overlay is shown automatically.
inline constexpr std::string_view kGBAStreamClientHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Dolphin GBA Stream</title>
<style>
  html,body{margin:0;background:#111;color:#ddd;font-family:sans-serif;height:100%;
            display:flex;flex-direction:column;align-items:center;justify-content:center;
            overscroll-behavior:none}
  canvas{image-rendering:pixelated;width:min(96vw,720px);height:auto;border:1px solid #444}
  #status{margin:8px;font-size:14px}
  #settings{margin-top:8px;font-size:13px}
  #settings button{margin:2px;min-width:80px}
  #game{display:none;flex-direction:column;align-items:center;width:100%}
  #lobbyButtons{display:flex;gap:10px;margin-top:12px}
  #lobbyButtons button{font-size:20px;min-width:64px;min-height:64px;cursor:pointer}
  #lobbyButtons button:disabled{opacity:0.35;cursor:not-allowed}
  #touchControls{position:fixed;left:0;right:0;bottom:0;display:none;justify-content:space-between;
                 align-items:flex-end;padding:16px;box-sizing:border-box;pointer-events:none}
  #touchDpad{display:grid;grid-template-columns:repeat(3, 52px);grid-template-rows:repeat(3, 52px);
             grid-template-areas:". u ." "l . r" ". d .";gap:4px;pointer-events:auto}
  #touchActions{display:flex;gap:10px;align-items:flex-end;pointer-events:auto}
  .tbtn{font-size:16px;border-radius:8px;border:1px solid #666;background:rgba(255,255,255,0.15);
        color:#fff;user-select:none;-webkit-user-select:none;touch-action:none}
  .tbtn.round{border-radius:50%;width:52px;height:52px}
  .tbtn.big{width:64px;height:64px;font-size:20px}
</style></head>
<body>
<div id="lobby">
  <div id="lobbyStatus">Suche nach aktiven GBA-Slots...</div>
  <div id="lobbyButtons"></div>
</div>
<div id="game">
<div id="status">connecting...</div>
<canvas id="screen" width="240" height="160"></canvas>
<div id="settings"></div>
<div id="touchControls">
  <div id="touchDpad">
    <button class="tbtn" data-name="Up" style="grid-area:u">▲</button>
    <button class="tbtn" data-name="Left" style="grid-area:l">◀</button>
    <button class="tbtn" data-name="Right" style="grid-area:r">▶</button>
    <button class="tbtn" data-name="Down" style="grid-area:d">▼</button>
  </div>
  <div id="touchActions">
    <button class="tbtn round" data-name="Select">SEL</button>
    <button class="tbtn round" data-name="Start">STA</button>
    <button class="tbtn round" data-name="L">L</button>
    <button class="tbtn round" data-name="R">R</button>
    <button class="tbtn round big" data-name="B">B</button>
    <button class="tbtn round big" data-name="A">A</button>
  </div>
</div>
</div>
<script>
const PLAYER_BASE_PORT = 6801;
const lobbyEl = document.getElementById('lobby');
const lobbyStatusEl = document.getElementById('lobbyStatus');
const lobbyButtonsEl = document.getElementById('lobbyButtons');
const gameEl = document.getElementById('game');

async function checkSlot(n) {
  const port = PLAYER_BASE_PORT + n;
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 800);
  try {
    const res = await fetch('http://' + location.hostname + ':' + port + '/status',
                            {signal: controller.signal});
    if (!res.ok) return {exists: false};
    const data = await res.json();
    return {exists: true, occupied: !!data.occupied};
  } catch (e) {
    return {exists: false};
  } finally {
    clearTimeout(timeout);
  }
}

async function buildLobby() {
  const results = await Promise.all([0, 1, 2, 3].map(checkSlot));
  lobbyButtonsEl.innerHTML = '';
  let anyExists = false;
  results.forEach((r, i) => {
    if (!r.exists) return;
    anyExists = true;
    const btn = document.createElement('button');
    btn.textContent = 'P' + (i + 1);
    btn.disabled = r.occupied;
    btn.title = r.occupied ? 'Bereits verbunden' : 'Verbinden';
    btn.onclick = () => {
      lobbyEl.style.display = 'none';
      gameEl.style.display = 'flex';
      startStream(PLAYER_BASE_PORT + i);
    };
    lobbyButtonsEl.appendChild(btn);
  });
  lobbyStatusEl.textContent = anyExists ?
      'Spieler wählen:' :
      'Kein GameCube-Port ist aktuell auf "GBA (Client-Stream)" gestellt.';
}
buildLobby();

function startStream(port) {
const BUTTONS = [
  ['A', 1<<0, 'KeyX'], ['B', 1<<1, 'KeyZ'], ['Select', 1<<2, 'ShiftRight'],
  ['Start', 1<<3, 'Enter'], ['Right', 1<<4, 'ArrowRight'], ['Left', 1<<5, 'ArrowLeft'],
  ['Up', 1<<6, 'ArrowUp'], ['Down', 1<<7, 'ArrowDown'], ['R', 1<<8, 'KeyS'], ['L', 1<<9, 'KeyA'],
];
const nameToBit = {};
for (const [name, bit] of BUTTONS) nameToBit[name] = bit;

const stored = JSON.parse(localStorage.getItem('gbaStreamBindings') || '{}');
const bindings = {};
for (const [name, , def] of BUTTONS) bindings[name] = stored[name] || def;
function saveBindings() { localStorage.setItem('gbaStreamBindings', JSON.stringify(bindings)); }

let keyState = 0;
const codeToButton = {};
function rebuildCodeMap() {
  for (const k in codeToButton) delete codeToButton[k];
  for (const [name, bit] of BUTTONS) codeToButton[bindings[name]] = bit;
}
rebuildCodeMap();

const statusEl = document.getElementById('status');
const canvas = document.getElementById('screen');
const ctx = canvas.getContext('2d');
let imageData = ctx.createImageData(240, 160);

// Created here (inside the P-button's click handler call chain) so the
// browser's autoplay policy -- which requires audio to start from a user
// gesture -- is satisfied without any extra "click to enable sound" step.
const audioCtx = new (window.AudioContext || window.webkitAudioContext)();
let nextAudioTime = 0;

function playAudioChunk(sampleRate, channels, view, byteOffset, sampleCount) {
  const frameCount = sampleCount / channels;
  if (frameCount <= 0) return;
  const buffer = audioCtx.createBuffer(channels, frameCount, sampleRate);
  for (let ch = 0; ch < channels; ch++) {
    const channelData = buffer.getChannelData(ch);
    for (let i = 0; i < frameCount; i++) {
      channelData[i] = view.getInt16(byteOffset + (i * channels + ch) * 2, true) / 32768;
    }
  }
  const source = audioCtx.createBufferSource();
  source.buffer = buffer;
  source.connect(audioCtx.destination);
  const now = audioCtx.currentTime;
  // Small safety cushion, and resync if playback ever falls behind real time
  // (e.g. after a stall) instead of trying to catch up by queueing forever.
  if (nextAudioTime < now + 0.05) nextAudioTime = now + 0.05;
  source.start(nextAudioTime);
  nextAudioTime += buffer.duration;
}

const ws = new WebSocket('ws://' + location.hostname + ':' + port + '/');
ws.binaryType = 'arraybuffer';
ws.onopen = () => statusEl.textContent = 'connected';
ws.onclose = () => statusEl.textContent = 'disconnected';
ws.onerror = () => statusEl.textContent = 'error';

ws.onmessage = async (ev) => {
  const view = new DataView(ev.data);
  const type = view.getUint8(0);
  if (type === 3) {
    const sampleRate = view.getUint32(1, true);
    const channels = view.getUint8(5);
    const sampleCount = (ev.data.byteLength - 6) / 2;
    playAudioChunk(sampleRate, channels, view, 6, sampleCount);
    return;
  }
  if (type !== 1) return;
  const width = view.getUint32(1, true);
  const height = view.getUint32(5, true);
  const compressed = ev.data.slice(9);
  const raw = await new Response(
      new Blob([compressed]).stream().pipeThrough(new DecompressionStream('deflate-raw'))
  ).arrayBuffer();
  const pixels = new DataView(raw);
  if (imageData.width !== width || imageData.height !== height) {
    canvas.width = width; canvas.height = height;
    imageData = ctx.createImageData(width, height);
  }
  const data = imageData.data;
  for (let i = 0; i < width * height; i++) {
    const p = pixels.getUint16(i * 2, true);
    const r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    data[i*4+0] = (r * 255 / 31) | 0;
    data[i*4+1] = (g * 255 / 63) | 0;
    data[i*4+2] = (b * 255 / 31) | 0;
    data[i*4+3] = 255;
  }
  ctx.putImageData(imageData, 0, 0);
};

function sendKeys() {
  if (ws.readyState !== WebSocket.OPEN) return;
  const msg = new Uint8Array(3);
  msg[0] = 2; msg[1] = keyState & 0xFF; msg[2] = (keyState >> 8) & 0xFF;
  ws.send(msg);
}
window.addEventListener('keydown', (e) => {
  const bit = codeToButton[e.code];
  if (!bit) return;
  e.preventDefault();
  if (!(keyState & bit)) { keyState |= bit; sendKeys(); }
});
window.addEventListener('keyup', (e) => {
  const bit = codeToButton[e.code];
  if (!bit) return;
  e.preventDefault();
  if (keyState & bit) { keyState &= ~bit; sendKeys(); }
});

// Phones/tablets have no physical keyboard, so show an on-screen D-pad and
// button overlay for any touch-capable device instead of relying on the
// keybinding settings below (which still work fine for a Bluetooth keyboard
// on such a device, if the player has one).
const isMobile = ('ontouchstart' in window) || navigator.maxTouchPoints > 0;
const touchControlsEl = document.getElementById('touchControls');
if (isMobile) {
  touchControlsEl.style.display = 'flex';
  touchControlsEl.querySelectorAll('.tbtn').forEach((btn) => {
    const bit = nameToBit[btn.dataset.name];
    const press = (e) => { e.preventDefault(); if (!(keyState & bit)) { keyState |= bit; sendKeys(); } };
    const release = (e) => { e.preventDefault(); if (keyState & bit) { keyState &= ~bit; sendKeys(); } };
    btn.addEventListener('touchstart', press, {passive: false});
    btn.addEventListener('touchend', release, {passive: false});
    btn.addEventListener('touchcancel', release, {passive: false});
  });
}

const settingsEl = document.getElementById('settings');
function renderSettings() {
  settingsEl.innerHTML = '';
  for (const [name, bit] of BUTTONS) {
    const btn = document.createElement('button');
    btn.textContent = name + ': ' + bindings[name];
    btn.onclick = () => {
      btn.textContent = name + ': press a key...';
      const onKey = (e) => {
        e.preventDefault();
        bindings[name] = e.code;
        saveBindings();
        rebuildCodeMap();
        renderSettings();
        window.removeEventListener('keydown', onKey, true);
      };
      window.addEventListener('keydown', onKey, true);
    };
    settingsEl.appendChild(btn);
  }
}
renderSettings();
}  // startStream
</script>
</body></html>
)HTML";

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
