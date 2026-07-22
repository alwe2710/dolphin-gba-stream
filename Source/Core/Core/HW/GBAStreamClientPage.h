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

  /* Mobile: fullscreen video with the D-pad/buttons drawn on top of it,
     like a typical mobile emulator, instead of the desktop's bordered,
     centered canvas with controls below it. */
  body.mobile{overflow:hidden}
  body.mobile #game{position:fixed;inset:0;width:100vw;height:100vh;background:#000}
  body.mobile canvas{position:absolute;top:0;left:0;width:100%;height:100%;
                      object-fit:contain;border:none}
  body.mobile #settings{display:none}
  body.mobile #status{position:fixed;top:6px;left:6px;margin:0;z-index:5;font-size:11px;
                       background:rgba(0,0,0,0.4);padding:2px 6px;border-radius:4px}

  /* Absolutely positioned within the fixed full-viewport container, laid out
     like a typical mobile emulator: shoulder buttons in the top corners,
     D-pad bottom-left, Select/Start (small) stacked above the big A/B
     buttons bottom-right -- keeps the bottom-right cluster narrow enough to
     never overflow off-screen on narrow phones, unlike a single wide row. */
  #touchControls{position:fixed;inset:0;display:none;pointer-events:none;z-index:10}
  .tshoulder{position:absolute;top:14px;width:56px;height:40px;font-size:14px}
  #touchL{left:14px}
  #touchR{right:14px}
  #touchDpad{position:absolute;left:20px;bottom:24px;
             display:grid;grid-template-columns:repeat(3, 52px);grid-template-rows:repeat(3, 52px);
             grid-template-areas:". u ." "l . r" ". d ."}
  #touchRight{position:absolute;right:20px;bottom:24px;
              display:flex;flex-direction:column;align-items:flex-end;gap:12px}
  #touchStartSelect{display:flex;gap:8px}
  #touchAB{display:flex;gap:12px}
  .tbtn{font-size:16px;border-radius:8px;border:1px solid rgba(255,255,255,0.5);
        background:rgba(255,255,255,0.15);color:#fff;user-select:none;
        -webkit-user-select:none;touch-action:none;pointer-events:auto}
  .tbtn.round{border-radius:50%;width:52px;height:52px}
  .tbtn.big{width:64px;height:64px;font-size:20px}
  .tbtn.small{width:48px;height:32px;font-size:11px;border-radius:6px}

  #menuButton{display:none;position:fixed;top:14px;left:50%;transform:translateX(-50%);
              width:44px;height:44px;border-radius:50%;background:rgba(0,0,0,0.45);
              color:#fff;border:1px solid rgba(255,255,255,0.5);font-size:20px;
              padding:0;z-index:20;align-items:center;justify-content:center}
  /* justify-content:flex-start (not center) so a panel taller than the
     viewport in landscape scrolls from the top instead of centering and
     clipping its first item above the visible area. */
  #menuPanel{display:none;position:fixed;inset:0;background:rgba(0,0,0,0.9);color:#fff;
             z-index:30;flex-direction:column;align-items:center;justify-content:flex-start;
             gap:8px;padding:16px;padding-top:max(16px, env(safe-area-inset-top));
             box-sizing:border-box;overflow-y:auto;text-align:center}
  #menuPanel button{margin:2px;min-width:100px}
  #gamepadBindingsList{display:flex;flex-direction:column;gap:4px;margin:8px 0}
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
  <button class="tbtn tshoulder" id="touchL" data-name="L">L</button>
  <button class="tbtn tshoulder" id="touchR" data-name="R">R</button>
  <div id="touchDpad">
    <button class="tbtn" data-name="Up" style="grid-area:u">▲</button>
    <button class="tbtn" data-name="Left" style="grid-area:l">◀</button>
    <button class="tbtn" data-name="Right" style="grid-area:r">▶</button>
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
  <h3 style="margin:0">Menü</h3>
  <div>
    <label><input type="checkbox" id="toggleOverlay"> Touch-Overlay anzeigen</label>
  </div>
  <div>Gamecontroller-Belegung:</div>
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
    const press = (e) => { e.preventDefault(); if (!(keyState & bit)) { keyState |= bit; sendKeys(); } };
    const release = (e) => { e.preventDefault(); if (keyState & bit) { keyState &= ~bit; sendKeys(); } };
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
      hint.textContent = 'Kein Controller erkannt -- bitte verbinden und einen Knopf drücken.';
      gamepadListEl.appendChild(hint);
    }
    for (const [name] of BUTTONS) {
      const row = document.createElement('div');
      const label = document.createElement('span');
      label.textContent = name + ': ' +
          (gamepadBindings[name] !== undefined ? ('Knopf ' + gamepadBindings[name]) :
                                                  'nicht belegt') + '  ';
      const btn = document.createElement('button');
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

// The desktop keyboard-rebind panel has no purpose on a touch device -- it
// shows the mobile overlay/gamepad menu instead (see above).
if (!isMobile) {
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
}
}  // startStream
</script>
</body></html>
)HTML";

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
