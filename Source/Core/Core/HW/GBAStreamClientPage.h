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
// input message whenever the locally-held button state changes.
//
// On a touch-capable device (phones/tablets, which have no physical
// keyboard) the video goes fullscreen with a mobile-emulator-style D-pad and
// button overlay drawn directly on top of it, the desktop keyboard-rebind
// panel is hidden, and a small centered hamburger button opens a menu for
// binding a connected game controller (Gamepad API) and for hiding the
// overlay for players who'd rather use a controller alone.
//
// Visual language ("Handheld Link"): a warm dark plum theme referencing the
// GC-GBA link cable this feature emulates -- rounded "cartridge" tiles for
// the player picker, joined by a thin connecting line, and a periwinkle
// accent used only for the primary/active affordance (the A button, primary
// actions) so it doesn't compete with itself. Chosen from a set of three
// design proposals shown to and picked by the project owner.
inline constexpr std::string_view kGBAStreamClientHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Dolphin GBA Stream</title>
<style>
  :root{
    --bg:#241b2f;--surface:#33253f;--surface-2:#3e2d4d;
    --ink:#f7eefc;--muted:#b79fc7;--accent:#7c5cff;--accent-ink:#fbfaff;
    --ok:#7be0b0;--radius:14px;
    --font-display:"Trebuchet MS","Century Gothic",system-ui,sans-serif;
    --font-body:system-ui,"Segoe UI",sans-serif;
    --border:1px solid rgba(255,255,255,0.12);
  }
  html,body{margin:0;background:var(--bg);color:var(--ink);font-family:var(--font-body);
            height:100%;display:flex;flex-direction:column;align-items:center;
            justify-content:center;overscroll-behavior:none}

  /* The video always fills as much of the browser window as the GBA's 3:2
     aspect ratio allows (desktop: JS sizes the canvas element itself so
     #videoWrap -- and therefore the gear button pinned to its corner --
     shrink-wraps to exactly the visible image, not the letterboxed
     viewport; mobile: plain object-fit:contain, no JS sizing needed since
     its overlay controls intentionally span the whole screen instead of
     hugging the image). */
  #game{display:none;position:fixed;inset:0;width:100vw;height:100vh;background:#000;
        align-items:center;justify-content:center}
  #status{position:absolute;top:8px;left:8px;margin:0;z-index:5;font-size:11px;
          color:var(--ink);background:rgba(0,0,0,0.4);padding:2px 8px;border-radius:20px}
  #videoWrap{position:relative;line-height:0}
  canvas{image-rendering:pixelated;display:block;border:var(--border);border-radius:12px}

  /* ---------- Lobby: rounded "cartridge" tiles joined by a thin link-cable
     line, referencing the physical GC-GBA link cable this feature emulates. ---------- */
  #lobby{text-align:center;max-width:360px;padding:0 20px;box-sizing:border-box}
  .lobby-brand{margin-bottom:22px}
  .lobby-brand .mark{font-family:var(--font-display);font-weight:700;font-size:13px;
                      letter-spacing:0.08em;text-transform:uppercase;color:var(--accent)}
  .lobby-brand h1{font-family:var(--font-display);margin:6px 0 0;font-size:24px;
                  letter-spacing:-0.01em;text-wrap:balance}
  .lobby-brand p{margin:8px 0 0;font-size:13.5px;color:var(--muted)}
  #lobbyButtons{display:flex;flex-direction:column;margin-top:8px;text-align:left}
  .slot{display:flex;align-items:center;gap:12px;width:100%;font:inherit;color:var(--ink);
        text-align:left;cursor:pointer;background:var(--surface);border:var(--border);
        border-radius:var(--radius);padding:12px 16px;margin:0 0 10px;position:relative}
  .slot:not(:last-child)::after{content:"";position:absolute;left:31px;bottom:-10px;
                                width:2px;height:10px;
                                background:linear-gradient(var(--accent),transparent)}
  .slot .tag{width:38px;height:38px;border-radius:12px;flex-shrink:0;display:flex;
            align-items:center;justify-content:center;font-family:var(--font-display);
            font-weight:700;font-size:14px;background:var(--accent);color:var(--accent-ink)}
  .slot:disabled .tag{background:var(--surface-2);color:var(--muted)}
  .slot .info{flex:1;min-width:0;display:flex;flex-direction:column;gap:1px}
  .slot .name{font-size:14px;font-weight:600}
  .slot .sub{font-size:11.5px;color:var(--muted)}
  .slot .dot{width:8px;height:8px;border-radius:50%;background:var(--ok);flex-shrink:0}
  .slot:disabled .dot{background:var(--muted)}
  .slot:disabled{cursor:not-allowed;opacity:0.7}

  /* Mobile: the D-pad/buttons overlay is drawn on top of the video like a
     typical mobile emulator and intentionally spans the whole screen
     (rather than hugging the image like the desktop gear button does), so
     #videoWrap just needs to fill #game and let object-fit do the letterbox
     -- no JS sizing needed, unlike desktop. */
  body.mobile{overflow:hidden}
  body.mobile #videoWrap{width:100%;height:100%}
  body.mobile canvas{width:100%;height:100%;object-fit:contain;border:none;border-radius:0}

  /* Absolutely positioned within the fixed full-viewport container, laid out
     like a typical mobile emulator: shoulder buttons in the top corners,
     D-pad bottom-left, Select/Start (small) stacked above the big A/B
     buttons bottom-right -- keeps the bottom-right cluster narrow enough to
     never overflow off-screen on narrow phones, unlike a single wide row. */
  #touchControls{position:fixed;inset:0;display:none;pointer-events:none;z-index:10}
  .tshoulder{position:absolute;top:14px;width:52px;height:36px;font-size:13px}
  #touchL{left:14px}
  #touchR{right:14px}
  #touchDpad{position:absolute;left:20px;bottom:24px;
             display:grid;grid-template-columns:repeat(3, 48px);grid-template-rows:repeat(3, 48px);
             grid-template-areas:". u ." "l . r" ". d ."}
  #touchRight{position:absolute;right:20px;bottom:24px;
              display:flex;flex-direction:column;align-items:flex-end;gap:12px}
  #touchStartSelect{display:flex;gap:8px}
  /* A (last child) sits higher than B, like the real GBA's staggered layout. */
  #touchAB{display:flex;align-items:flex-end;gap:10px}
  #touchAB button:last-child{margin-bottom:20px}
  .tbtn{font-family:var(--font-display);font-weight:600;font-size:15px;border-radius:var(--radius);
        border:var(--border);background:var(--surface);color:var(--ink);user-select:none;
        -webkit-user-select:none;touch-action:none;pointer-events:auto}
  .tbtn.round{border-radius:50%;width:48px;height:48px}
  .tbtn.big{width:60px;height:60px;font-size:19px}
  .tbtn.small{width:44px;height:30px;font-size:10.5px;border-radius:999px}
  /* Darkened while held, so a press is visually obvious with no haptic
     feedback to rely on. */
  .tbtn.pressed{background:var(--surface-2)}
  /* A is the primary action, so it alone carries the accent -- a soft glow
     rather than the flat swap the other buttons get when pressed. */
  [data-name="A"].tbtn{background:var(--accent);color:var(--accent-ink);border-color:transparent;
                        box-shadow:0 0 0 5px rgba(124,92,255,0.25)}
  [data-name="A"].tbtn.pressed{box-shadow:0 0 0 3px rgba(124,92,255,0.4)}

  #menuButton{display:none;position:fixed;top:14px;left:50%;transform:translateX(-50%);
              width:44px;height:44px;border-radius:50%;background:rgba(20,15,26,0.7);
              color:var(--ink);border:var(--border);font-size:18px;
              padding:0;z-index:20;align-items:center;justify-content:center}
  /* justify-content:flex-start (not center) so a panel taller than the
     viewport in landscape scrolls from the top instead of centering and
     clipping its first item above the visible area. */
  #menuPanel{display:none;position:fixed;inset:0;background:rgba(20,15,26,0.94);color:var(--ink);
             font-family:var(--font-body);z-index:30;flex-direction:column;align-items:center;
             justify-content:flex-start;gap:10px;padding:20px 18px;
             padding-top:max(20px, env(safe-area-inset-top));
             box-sizing:border-box;overflow-y:auto;text-align:center}
  #menuPanel h3{font-family:var(--font-display);margin:4px 0 4px;font-size:18px}
  .menu-label{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:0.06em;
              margin-top:8px}
  .menu-row{display:flex;align-items:center;justify-content:space-between;gap:10px;width:100%;
            max-width:320px;background:var(--surface);border:var(--border);
            border-radius:var(--radius);padding:10px 14px;font-size:13px;box-sizing:border-box;
            cursor:default}
  #gamepadBindingsList{display:flex;flex-direction:column;gap:8px;width:100%;align-items:center}
  #gamepadBindingsList .hint{font-size:12px;color:var(--muted);max-width:280px}
  .pill{font-family:var(--font-body);font-size:11.5px;font-weight:700;border:none;cursor:pointer;
        padding:7px 14px;border-radius:999px;background:var(--accent);color:var(--accent-ink)}
  input.switch{appearance:none;-webkit-appearance:none;width:34px;height:20px;border-radius:999px;
               background:var(--surface-2);position:relative;cursor:pointer;margin:0;
               flex-shrink:0}
  input.switch::after{content:"";position:absolute;top:2px;left:2px;width:16px;height:16px;
                      border-radius:50%;background:var(--muted)}
  input.switch:checked::after{left:16px;background:var(--accent)}
  #closeMenu{margin-top:10px;background:transparent;border:var(--border);color:var(--muted);
             font-family:var(--font-body);font-size:12.5px;font-weight:600;
             padding:9px 20px;border-radius:999px;cursor:pointer}

  /* ---------- Desktop keyboard-rebind: a gear button bottom-right opens a
     table of button->key assignments, mirroring the mobile hamburger menu's
     role but as a table since there's no touch overlay to keep in view. ---------- */
  /* Positioned absolute within #videoWrap (not fixed to the viewport) so it
     sits right on the corner of the visible image itself, letterboxing or
     not. */
  #settingsButton{display:none;position:absolute;right:8px;bottom:8px;
                  width:42px;height:42px;border-radius:50%;background:var(--surface);
                  color:var(--ink);border:var(--border);font-size:18px;
                  padding:0;z-index:20;align-items:center;justify-content:center;cursor:pointer}
  body.mobile #settingsButton{display:none !important}
  #settingsPanel{display:none;position:fixed;inset:0;background:rgba(20,15,26,0.85);
                 color:var(--ink);font-family:var(--font-body);z-index:30;
                 flex-direction:column;align-items:center;justify-content:center;
                 gap:14px;padding:20px;box-sizing:border-box}
  #settingsPanel h3{font-family:var(--font-display);margin:0;font-size:19px}
  #settingsTable{border-collapse:collapse;background:var(--surface);border:var(--border);
                 border-radius:var(--radius);overflow:hidden;font-size:13.5px}
  #settingsTable th,#settingsTable td{padding:9px 16px;text-align:left}
  #settingsTable th{color:var(--muted);font-size:11px;text-transform:uppercase;
                    letter-spacing:0.05em;font-weight:600;border-bottom:var(--border)}
  #settingsTable tr:not(:last-child) td{border-bottom:1px solid rgba(255,255,255,0.06)}
  #settingsTable td:first-child{font-weight:600}
  #settingsTable td:nth-child(2){color:var(--muted);font-variant-numeric:tabular-nums}
  #settingsTable button{font-family:var(--font-body);font-size:11.5px;font-weight:700;
                        border:none;cursor:pointer;padding:6px 12px;border-radius:999px;
                        background:var(--accent);color:var(--accent-ink)}
  #closeSettings{background:transparent;border:var(--border);color:var(--muted);
                 font-family:var(--font-body);font-size:12.5px;font-weight:600;
                 padding:9px 20px;border-radius:999px;cursor:pointer}
</style></head>
<body>
<div id="lobby">
  <div class="lobby-brand">
    <div class="mark">Link Cable</div>
    <h1>Wer spielt mit?</h1>
    <p id="lobbyStatus">Suche nach aktiven GBA-Slots...</p>
  </div>
  <div id="lobbyButtons"></div>
</div>
<div id="game">
<div id="status">connecting...</div>
<div id="videoWrap">
  <canvas id="screen" width="240" height="160"></canvas>
  <button id="settingsButton" title="Tastenbelegung">&#9881;</button>
</div>
<div id="settingsPanel">
  <h3>Tastenbelegung</h3>
  <table id="settingsTable">
    <thead><tr><th>Taste</th><th>Belegung</th><th></th></tr></thead>
    <tbody id="settingsTableBody"></tbody>
  </table>
  <button id="closeSettings">Schließen</button>
</div>
<div id="touchControls">
  <button class="tbtn tshoulder" id="touchL" data-name="L">L</button>
  <button class="tbtn tshoulder" id="touchR" data-name="R">R</button>
  <div id="touchDpad">
    <button class="tbtn" data-name="Up" style="grid-area:u">▲</button>
    <button class="tbtn" data-name="Left" style="grid-area:l">◀︎</button>
    <button class="tbtn" data-name="Right" style="grid-area:r">▶︎</button>
    <button class="tbtn" data-name="Down" style="grid-area:d">▼</button>
  </div>
  <div id="touchRight">
    <div id="touchStartSelect">
      <button class="tbtn small" data-name="Select">SEL</button>
      <button class="tbtn small" data-name="Start">STA</button>
    </div>
    <div id="touchAB">
      <button class="tbtn round big" data-name="B">B</button>
      <button class="tbtn round big" data-name="A">A</button>
    </div>
  </div>
</div>
<button id="menuButton">&#9776;</button>
<div id="menuPanel">
  <h3>Menü</h3>
  <label class="menu-row" for="toggleOverlay">
    <span>Touch-Overlay anzeigen</span>
    <input type="checkbox" class="switch" id="toggleOverlay">
  </label>
  <div class="menu-label">Gamecontroller-Belegung</div>
  <div id="gamepadBindingsList"></div>
  <button id="closeMenu">Schließen</button>
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
    const port = PLAYER_BASE_PORT + i;
    const btn = document.createElement('button');
    btn.className = 'slot';
    btn.disabled = r.occupied;
    btn.title = r.occupied ? 'Bereits verbunden' : 'Verbinden';
    btn.innerHTML =
        '<span class="tag">P' + (i + 1) + '</span>' +
        '<span class="info"><span class="name">Port ' + (i + 1) + ' · ' +
        (r.occupied ? 'belegt' : 'frei') + '</span>' +
        '<span class="sub">ws · ' + port + '</span></span>' +
        '<span class="dot"></span>';
    btn.onclick = () => {
      lobbyEl.style.display = 'none';
      gameEl.style.display = 'flex';
      startStream(port);
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

// Any touch-capable device is treated as mobile: fullscreen video, on-screen
// overlay instead of the keyboard-rebind panel, and the hamburger menu for
// optional gamepad binding.
const isMobile = ('ontouchstart' in window) || navigator.maxTouchPoints > 0;
if (isMobile) document.body.classList.add('mobile');

let keyState = 0;

const statusEl = document.getElementById('status');
const canvas = document.getElementById('screen');
const ctx = canvas.getContext('2d');
let imageData = ctx.createImageData(240, 160);

// Desktop only: mobile lets object-fit:contain (see CSS) do the letterboxing
// responsively for free. Here we size the canvas element itself in exact
// pixels so #videoWrap -- an inline-block that shrink-wraps to its only
// normal-flow child, the canvas -- ends up exactly matching the visible
// image, with nothing left over for the settings gear (absolutely
// positioned inside #videoWrap) to float in.
function resizeDesktopCanvas() {
  if (isMobile) return;
  const aspect = canvas.width / canvas.height;
  let w = window.innerWidth * 0.96;
  let h = w / aspect;
  if (h > window.innerHeight * 0.96) {
    h = window.innerHeight * 0.96;
    w = h * aspect;
  }
  canvas.style.width = w + 'px';
  canvas.style.height = h + 'px';
}
window.addEventListener('resize', resizeDesktopCanvas);
resizeDesktopCanvas();

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
    resizeDesktopCanvas();
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

// --- Physical keyboard (desktop, or a Bluetooth keyboard paired to a phone) ---
const stored = JSON.parse(localStorage.getItem('gbaStreamBindings') || '{}');
const bindings = {};
for (const [name, , def] of BUTTONS) bindings[name] = stored[name] || def;
function saveBindings() { localStorage.setItem('gbaStreamBindings', JSON.stringify(bindings)); }
const codeToButton = {};
function rebuildCodeMap() {
  for (const k in codeToButton) delete codeToButton[k];
  for (const [name, bit] of BUTTONS) codeToButton[bindings[name]] = bit;
}
rebuildCodeMap();
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

// --- Mobile-only: on-screen touch overlay (drawn on top of the fullscreen
// video like a typical mobile emulator) and optional game-controller
// binding via the hamburger menu. Both are irrelevant on desktop, which
// already has the keyboard-rebind panel below. ---
const touchControlsEl = document.getElementById('touchControls');
if (isMobile) {
  const GAMEPAD_DEFAULTS = {A: 0, B: 1, Select: 8, Start: 9, Up: 12, Down: 13, Left: 14,
                            Right: 15, L: 4, R: 5};
  const gamepadBindings =
      JSON.parse(localStorage.getItem('gbaStreamGamepadBindings') || 'null') ||
      Object.assign({}, GAMEPAD_DEFAULTS);
  function saveGamepadBindings() {
    localStorage.setItem('gbaStreamGamepadBindings', JSON.stringify(gamepadBindings));
  }
  let activeGamepadIndex = null;
  window.addEventListener('gamepadconnected', (e) => { activeGamepadIndex = e.gamepad.index; });
  window.addEventListener('gamepaddisconnected', (e) => {
    if (activeGamepadIndex === e.gamepad.index) activeGamepadIndex = null;
  });
  let lastGamepadState = 0;
  function pollGamepad() {
    if (activeGamepadIndex !== null) {
      const gp = navigator.getGamepads()[activeGamepadIndex];
      if (gp) {
        let newState = 0;
        for (const [name, bit] of BUTTONS) {
          const idx = gamepadBindings[name];
          if (idx !== undefined && gp.buttons[idx] && gp.buttons[idx].pressed) newState |= bit;
        }
        if (newState !== lastGamepadState) {
          // Merge: clear bits the gamepad used to hold, set the ones it
          // holds now, leave bits held by keyboard/touch alone.
          keyState = (keyState & ~lastGamepadState) | newState;
          lastGamepadState = newState;
          sendKeys();
        }
      }
    }
    requestAnimationFrame(pollGamepad);
  }
  requestAnimationFrame(pollGamepad);

  function rebindGamepadButton(name, onDone) {
    if (activeGamepadIndex === null) { onDone(); return; }
    const baseline = navigator.getGamepads()[activeGamepadIndex].buttons.map((b) => b.pressed);
    function check() {
      const gp = navigator.getGamepads()[activeGamepadIndex];
      if (gp) {
        for (let i = 0; i < gp.buttons.length; i++) {
          if (gp.buttons[i].pressed && !baseline[i]) {
            gamepadBindings[name] = i;
            saveGamepadBindings();
            onDone();
            return;
          }
        }
      }
      requestAnimationFrame(check);
    }
    requestAnimationFrame(check);
  }

  touchControlsEl.querySelectorAll('.tbtn').forEach((btn) => {
    const bit = nameToBit[btn.dataset.name];
    const press = (e) => {
      e.preventDefault();
      btn.classList.add('pressed');
      if (!(keyState & bit)) { keyState |= bit; sendKeys(); }
    };
    const release = (e) => {
      e.preventDefault();
      btn.classList.remove('pressed');
      if (keyState & bit) { keyState &= ~bit; sendKeys(); }
    };
    btn.addEventListener('touchstart', press, {passive: false});
    btn.addEventListener('touchend', release, {passive: false});
    btn.addEventListener('touchcancel', release, {passive: false});
  });

  const overlayStored = localStorage.getItem('gbaStreamShowOverlay');
  const showOverlay = overlayStored === null ? true : overlayStored === 'true';
  touchControlsEl.style.display = showOverlay ? 'block' : 'none';

  // Best-effort true fullscreen (hides browser chrome too); harmless if the
  // browser blocks it since the CSS-driven fullscreen layout still applies.
  document.documentElement.requestFullscreen?.().catch(() => {});

  const menuButton = document.getElementById('menuButton');
  const menuPanel = document.getElementById('menuPanel');
  const overlayToggle = document.getElementById('toggleOverlay');
  const gamepadListEl = document.getElementById('gamepadBindingsList');
  overlayToggle.checked = showOverlay;
  overlayToggle.onchange = () => {
    touchControlsEl.style.display = overlayToggle.checked ? 'block' : 'none';
    localStorage.setItem('gbaStreamShowOverlay', overlayToggle.checked);
  };

  function renderGamepadBindingsList() {
    gamepadListEl.innerHTML = '';
    if (activeGamepadIndex === null) {
      const hint = document.createElement('div');
      hint.className = 'hint';
      hint.textContent = 'Kein Controller erkannt -- bitte verbinden und einen Knopf drücken.';
      gamepadListEl.appendChild(hint);
    }
    for (const [name] of BUTTONS) {
      const row = document.createElement('div');
      row.className = 'menu-row';
      const label = document.createElement('span');
      label.textContent = name + ': ' +
          (gamepadBindings[name] !== undefined ? ('Knopf ' + gamepadBindings[name]) :
                                                  'nicht belegt');
      const btn = document.createElement('button');
      btn.className = 'pill';
      btn.textContent = 'Belegen';
      btn.onclick = () => {
        btn.textContent = 'Drücke einen Knopf...';
        rebindGamepadButton(name, renderGamepadBindingsList);
      };
      row.appendChild(label);
      row.appendChild(btn);
      gamepadListEl.appendChild(row);
    }
  }

  menuButton.style.display = 'flex';
  menuButton.onclick = () => {
    menuPanel.style.display = 'flex';
    renderGamepadBindingsList();
  };
  document.getElementById('closeMenu').onclick = () => { menuPanel.style.display = 'none'; };
}

// The desktop keyboard-rebind table has no purpose on a touch device -- it
// shows the mobile overlay/gamepad menu instead (see above).
if (!isMobile) {
  const settingsButton = document.getElementById('settingsButton');
  const settingsPanel = document.getElementById('settingsPanel');
  const settingsTableBody = document.getElementById('settingsTableBody');

  function renderSettings() {
    settingsTableBody.innerHTML = '';
    for (const [name, bit] of BUTTONS) {
      const row = document.createElement('tr');
      const nameCell = document.createElement('td');
      nameCell.textContent = name;
      const keyCell = document.createElement('td');
      keyCell.textContent = bindings[name];
      const actionCell = document.createElement('td');
      const btn = document.createElement('button');
      btn.textContent = 'Belegen';
      btn.onclick = () => {
        btn.textContent = 'Taste drücken...';
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
      actionCell.appendChild(btn);
      row.appendChild(nameCell);
      row.appendChild(keyCell);
      row.appendChild(actionCell);
      settingsTableBody.appendChild(row);
    }
  }
  renderSettings();

  settingsButton.style.display = 'flex';
  settingsButton.onclick = () => { settingsPanel.style.display = 'flex'; };
  document.getElementById('closeSettings').onclick = () => { settingsPanel.style.display = 'none'; };
}
}  // startStream
</script>
</body></html>
)HTML";

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
