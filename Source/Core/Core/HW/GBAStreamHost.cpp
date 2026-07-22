// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAS_LIBMGBA

#include "Core/HW/GBAStreamHost.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <zlib.h>

#include <mbedtls/base64.h>

#include <SFML/Network/SocketSelector.hpp>
#include <SFML/System/Time.hpp>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Logging/Log.h"

#include "Core/HW/GBAPad.h"
#include "Core/HW/GBAPadEmu.h"

#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/InputConfig.h"

namespace HW::GBA
{
namespace
{
// The integrated GBA core is always 240x160 for real GC-port SI slots (the
// only slots SIDEVICE_GC_GBA_STREAM can be selected for); the Game Boy Player
// HSP slot uses a different code path entirely and never constructs this class.
constexpr u32 GBA_STREAM_WIDTH = 240;
constexpr u32 GBA_STREAM_HEIGHT = 160;

constexpr u16 GBA_STREAM_BASE_PORT = 6800;

constexpr u8 kMsgTypeVideoFrame = 0x01;
constexpr u8 kMsgTypeInput = 0x02;

// Remote key bitmask layout (client->server). Chosen to mirror the bit order
// SI_DeviceGBAEmu::GetData() already uses for the internal GBA keypad word,
// purely for readability -- this is our own wire protocol, not mGBA's ABI.
constexpr u16 kKeyA = 1 << 0;
constexpr u16 kKeyB = 1 << 1;
constexpr u16 kKeySelect = 1 << 2;
constexpr u16 kKeyStart = 1 << 3;
constexpr u16 kKeyRight = 1 << 4;
constexpr u16 kKeyLeft = 1 << 5;
constexpr u16 kKeyUp = 1 << 6;
constexpr u16 kKeyDown = 1 << 7;
constexpr u16 kKeyR = 1 << 8;
constexpr u16 kKeyL = 1 << 9;

void AppendU32LE(std::vector<u8>* out, u32 value)
{
  out->push_back(static_cast<u8>(value & 0xFF));
  out->push_back(static_cast<u8>((value >> 8) & 0xFF));
  out->push_back(static_cast<u8>((value >> 16) & 0xFF));
  out->push_back(static_cast<u8>((value >> 24) & 0xFF));
}

std::vector<u8> DeflateRaw(const std::vector<u8>& input)
{
  z_stream strm{};
  // windowBits = -15 requests headerless "raw deflate", which is exactly what
  // the browser's DecompressionStream('deflate-raw') expects.
  if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    return {};

  std::vector<u8> out(deflateBound(&strm, static_cast<uLong>(input.size())));
  strm.next_in = const_cast<Bytef*>(input.data());
  strm.avail_in = static_cast<uInt>(input.size());
  strm.next_out = out.data();
  strm.avail_out = static_cast<uInt>(out.size());

  deflate(&strm, Z_FINISH);
  out.resize(strm.total_out);
  deflateEnd(&strm);
  return out;
}

struct WebSocketFrame
{
  u8 opcode;
  std::vector<u8> payload;
  size_t consumed;
};

constexpr u8 kOpcodeBinary = 0x2;
constexpr u8 kOpcodeClose = 0x8;

// Parses at most one client->server (masked) WebSocket frame from the front
// of `buf`. Returns nullopt if `buf` doesn't yet contain a full frame -- the
// caller should wait for more data and retry. Fragmented frames (FIN=0) are
// not supported: our client never sends them, so treat one as a protocol
// error (handled the same as a close frame by the caller).
std::optional<WebSocketFrame> TryParseWebSocketFrame(const std::vector<u8>& buf)
{
  if (buf.size() < 2)
    return std::nullopt;

  const u8 b0 = buf[0];
  const u8 b1 = buf[1];
  const u8 opcode = b0 & 0x0F;
  const bool masked = (b1 & 0x80) != 0;
  u64 len = b1 & 0x7F;
  size_t pos = 2;

  if (len == 126)
  {
    if (buf.size() < 4)
      return std::nullopt;
    len = (static_cast<u64>(buf[2]) << 8) | buf[3];
    pos = 4;
  }
  else if (len == 127)
  {
    if (buf.size() < 10)
      return std::nullopt;
    len = 0;
    for (int i = 0; i < 8; ++i)
      len = (len << 8) | buf[2 + i];
    pos = 10;
  }

  std::array<u8, 4> mask_key{};
  if (masked)
  {
    if (buf.size() < pos + 4)
      return std::nullopt;
    std::copy_n(buf.begin() + pos, 4, mask_key.begin());
    pos += 4;
  }

  if (buf.size() < pos + len)
    return std::nullopt;

  WebSocketFrame frame;
  frame.opcode = opcode;
  frame.payload.resize(len);
  for (u64 i = 0; i < len; ++i)
    frame.payload[i] = buf[pos + i] ^ (masked ? mask_key[i % 4] : u8{0});
  frame.consumed = pos + len;
  return frame;
}

bool SendWebSocketBinaryFrame(sf::TcpSocket& socket, const std::vector<u8>& payload)
{
  std::vector<u8> frame;
  frame.reserve(payload.size() + 10);
  frame.push_back(0x82);  // FIN=1, opcode=2 (binary). Server frames are unmasked.

  const size_t len = payload.size();
  if (len < 126)
  {
    frame.push_back(static_cast<u8>(len));
  }
  else if (len <= 0xFFFF)
  {
    frame.push_back(126);
    frame.push_back(static_cast<u8>((len >> 8) & 0xFF));
    frame.push_back(static_cast<u8>(len & 0xFF));
  }
  else
  {
    frame.push_back(127);
    for (int shift = 56; shift >= 0; shift -= 8)
      frame.push_back(static_cast<u8>((static_cast<u64>(len) >> shift) & 0xFF));
  }
  frame.insert(frame.end(), payload.begin(), payload.end());

  size_t sent_total = 0;
  while (sent_total < frame.size())
  {
    size_t sent = 0;
    const auto status =
        socket.send(frame.data() + sent_total, frame.size() - sent_total, sent);
    if (status != sf::Socket::Status::Done && status != sf::Socket::Status::Partial)
      return false;
    sent_total += sent;
  }
  return true;
}

// Single-page client. Landing view is a P1-P4 lobby: for each of the four
// possible GC ports it probes http://<host>:(6800+n)/status (see the
// "/status" branch in PerformHandshake) to find out which ports currently
// have a GBAStreamHost running at all (i.e. are configured as GBA
// (Client-Stream)) and whether each is already occupied by another client,
// then shows a picker with unavailable slots grayed out. Picking a slot opens
// a WebSocket to that port and switches to the canvas+input view, which
// decodes raw-deflate RGB565 frames and sends a 3-byte input message whenever
// the locally-held button state changes. Served directly over plain HTTP GET
// from the same port as the WebSocket endpoint, so no separate web
// server/hosting is needed.
constexpr std::string_view kClientHtml = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Dolphin GBA Stream</title>
<style>
  html,body{margin:0;background:#111;color:#ddd;font-family:sans-serif;height:100%;
            display:flex;flex-direction:column;align-items:center;justify-content:center}
  canvas{image-rendering:pixelated;width:min(96vw,720px);height:auto;border:1px solid #444}
  #status{margin:8px;font-size:14px}
  #settings{margin-top:8px;font-size:13px}
  #settings button{margin:2px;min-width:80px}
  #game{display:none;flex-direction:column;align-items:center}
  #lobbyButtons{display:flex;gap:10px;margin-top:12px}
  #lobbyButtons button{font-size:20px;min-width:64px;min-height:64px;cursor:pointer}
  #lobbyButtons button:disabled{opacity:0.35;cursor:not-allowed}
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
</div>
<script>
const BASE_PORT = 6800;
const lobbyEl = document.getElementById('lobby');
const lobbyStatusEl = document.getElementById('lobbyStatus');
const lobbyButtonsEl = document.getElementById('lobbyButtons');
const gameEl = document.getElementById('game');

async function checkSlot(n) {
  const port = BASE_PORT + n;
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
      startStream(BASE_PORT + i);
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

const ws = new WebSocket('ws://' + location.hostname + ':' + port + '/');
ws.binaryType = 'arraybuffer';
ws.onopen = () => statusEl.textContent = 'connected';
ws.onclose = () => statusEl.textContent = 'disconnected';
ws.onerror = () => statusEl.textContent = 'error';

ws.onmessage = async (ev) => {
  const view = new DataView(ev.data);
  if (view.getUint8(0) !== 1) return;
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

}  // namespace

GBAStreamHost::GBAStreamHost(int device_number) : m_device_number(device_number)
{
  const auto port = static_cast<unsigned short>(GBA_STREAM_BASE_PORT + device_number);
  const auto status = m_listener.listen(port);
  if (status != sf::Socket::Status::Done)
  {
    ERROR_LOG_FMT(SERIALINTERFACE, "GBAStreamHost: failed to listen on port {}", port);
    return;
  }
  NOTICE_LOG_FMT(SERIALINTERFACE, "GBAStreamHost: serving GBA {} on ws://<host>:{}/",
                 device_number + 1, port);
  m_accept_thread = std::thread([this] { AcceptLoop(); });
}

GBAStreamHost::~GBAStreamHost()
{
  m_stop = true;
  m_listener.close();
  if (m_accept_thread.joinable())
    m_accept_thread.join();
  DetachInputOverride();
}

void GBAStreamHost::GameChanged()
{
}

void GBAStreamHost::FrameEnded(std::span<const u32> video_buffer)
{
  std::lock_guard<std::mutex> lock(m_frame_mutex);
  m_pending_frame.assign(video_buffer.begin(), video_buffer.end());
  m_frame_width = GBA_STREAM_WIDTH;
  m_frame_height = GBA_STREAM_HEIGHT;
  ++m_frame_id;
}

void GBAStreamHost::AcceptLoop()
{
  while (!m_stop)
  {
    sf::TcpSocket socket;
    if (m_listener.accept(socket) != sf::Socket::Status::Done)
      continue;
    ServeConnection(socket);
  }
}

void GBAStreamHost::ServeConnection(sf::TcpSocket& socket)
{
  bool is_websocket = false;
  if (!PerformHandshake(socket, &is_websocket) || !is_websocket)
    return;

  AttachInputOverride();
  RunWebSocketSession(socket);
  DetachInputOverride();
}

bool GBAStreamHost::PerformHandshake(sf::TcpSocket& socket, bool* is_websocket)
{
  *is_websocket = false;

  std::string request;
  std::array<char, 4096> buf{};
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384)
  {
    size_t received = 0;
    const auto status = socket.receive(buf.data(), buf.size(), received);
    if (status != sf::Socket::Status::Done || received == 0)
      return false;
    request.append(buf.data(), received);
  }
  if (request.find("\r\n\r\n") == std::string::npos)
    return false;

  std::string path;
  std::map<std::string, std::string> headers;
  {
    std::istringstream stream(request);
    std::string request_line;
    std::getline(stream, request_line);
    {
      const auto first_space = request_line.find(' ');
      const auto second_space =
          first_space == std::string::npos ? std::string::npos : request_line.find(' ', first_space + 1);
      if (first_space != std::string::npos && second_space != std::string::npos)
        path = request_line.substr(first_space + 1, second_space - first_space - 1);
    }
    std::string line;
    while (std::getline(stream, line) && line != "\r" && !line.empty())
    {
      const auto colon = line.find(':');
      if (colon == std::string::npos)
        continue;
      std::string key = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      while (!value.empty() && value.front() == ' ')
        value.erase(value.begin());
      while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
        value.pop_back();
      std::transform(key.begin(), key.end(), key.begin(),
                      [](unsigned char c) { return std::tolower(c); });
      headers[key] = value;
    }
  }

  std::string upgrade = headers.count("upgrade") ? headers["upgrade"] : "";
  std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (path == "/status")
  {
    // Queried cross-port by the lobby screen (see kClientHtml) to find out which
    // GC ports are currently configured as GBA (Client-Stream) and whether each
    // one already has a client attached, so it can show a P1-P4 picker with
    // taken slots grayed out. CORS is required since the lobby page is loaded
    // from one port but fetch()es the status of the other three.
    const std::string body =
        std::string("{\"occupied\":") + (m_client_connected ? "true" : "false") + "}";
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << body;
    const std::string response_str = response.str();
    std::size_t sent = 0;
    [[maybe_unused]] const auto send_status =
        socket.send(response_str.data(), response_str.size(), sent);
    return true;
  }

  if (upgrade != "websocket" || !headers.count("sec-websocket-key"))
  {
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: text/html; charset=utf-8\r\n"
             << "Content-Length: " << kClientHtml.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << kClientHtml;
    const std::string response_str = response.str();
    std::size_t sent = 0;
    [[maybe_unused]] const auto send_status =
        socket.send(response_str.data(), response_str.size(), sent);
    return true;
  }

  const std::string concatenated = headers["sec-websocket-key"] +
                                    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const auto digest = Common::SHA1::CalculateDigest(concatenated);
  std::array<unsigned char, 64> b64{};
  size_t b64_len = 0;
  mbedtls_base64_encode(b64.data(), b64.size(), &b64_len, digest.data(), digest.size());

  std::ostringstream response;
  response << "HTTP/1.1 101 Switching Protocols\r\n"
           << "Upgrade: websocket\r\n"
           << "Connection: Upgrade\r\n"
           << "Sec-WebSocket-Accept: " << std::string(reinterpret_cast<char*>(b64.data()), b64_len)
           << "\r\n\r\n";
  const std::string response_str = response.str();
  if (socket.send(response_str.data(), response_str.size()) != sf::Socket::Status::Done)
    return false;

  *is_websocket = true;
  return true;
}

void GBAStreamHost::RunWebSocketSession(sf::TcpSocket& socket)
{
  sf::SocketSelector selector;
  selector.add(socket);

  std::vector<u8> recv_buffer;
  std::array<u8, 4096> read_buf{};
  u64 last_sent_frame_id = 0;
  std::vector<u8> previous_rgb565;

  m_remote_keys = 0;
  m_client_connected = true;

  while (!m_stop)
  {
    if (selector.wait(sf::milliseconds(4)))
    {
      size_t received = 0;
      const auto status = socket.receive(read_buf.data(), read_buf.size(), received);
      if (status == sf::Socket::Status::Disconnected || status == sf::Socket::Status::Error)
        break;

      if (status == sf::Socket::Status::Done && received > 0)
      {
        recv_buffer.insert(recv_buffer.end(), read_buf.begin(), read_buf.begin() + received);

        bool disconnect_requested = false;
        for (;;)
        {
          const auto frame = TryParseWebSocketFrame(recv_buffer);
          if (!frame)
            break;
          recv_buffer.erase(recv_buffer.begin(),
                            recv_buffer.begin() + static_cast<ptrdiff_t>(frame->consumed));

          if (frame->opcode == kOpcodeClose)
          {
            disconnect_requested = true;
            break;
          }
          if (frame->opcode == kOpcodeBinary && frame->payload.size() == 3 &&
              frame->payload[0] == kMsgTypeInput)
          {
            const u16 keys = static_cast<u16>(frame->payload[1]) |
                             (static_cast<u16>(frame->payload[2]) << 8);
            m_remote_keys.store(keys);
          }
        }
        if (disconnect_requested)
          break;
      }
    }

    SendVideoFrameIfPending(socket, &last_sent_frame_id, &previous_rgb565);
  }

  m_client_connected = false;
  m_remote_keys = 0;
}

void GBAStreamHost::SendVideoFrameIfPending(sf::TcpSocket& socket, u64* last_sent_frame_id,
                                            std::vector<u8>* previous_rgb565)
{
  std::vector<u32> frame;
  u32 width = 0;
  u32 height = 0;
  {
    std::lock_guard<std::mutex> lock(m_frame_mutex);
    if (m_frame_id == *last_sent_frame_id || m_pending_frame.empty())
      return;
    frame = m_pending_frame;
    width = m_frame_width;
    height = m_frame_height;
    *last_sent_frame_id = m_frame_id;
  }

  std::vector<u8> rgb565(static_cast<size_t>(width) * height * 2);
  for (size_t i = 0; i < frame.size(); ++i)
  {
    // mGBA's native pixel format here is 32bpp with byte0=R, byte1=G, byte2=B
    // (byte3 unused) -- see GBAWidget::SetVideoBuffer for the equivalent Qt
    // conversion this must match (ARGB32 interpretation + convertToFormat(RGB32)
    // + rgbSwapped() nets the same R/G/B extraction done here directly).
    const u32 pixel = frame[i];
    const u8 r = static_cast<u8>(pixel & 0xFF);
    const u8 g = static_cast<u8>((pixel >> 8) & 0xFF);
    const u8 b = static_cast<u8>((pixel >> 16) & 0xFF);
    const u16 packed = static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    rgb565[i * 2 + 0] = static_cast<u8>(packed & 0xFF);
    rgb565[i * 2 + 1] = static_cast<u8>(packed >> 8);
  }

  if (rgb565 == *previous_rgb565)
    return;

  const std::vector<u8> compressed = DeflateRaw(rgb565);

  std::vector<u8> message;
  message.reserve(9 + compressed.size());
  message.push_back(kMsgTypeVideoFrame);
  AppendU32LE(&message, width);
  AppendU32LE(&message, height);
  message.insert(message.end(), compressed.begin(), compressed.end());

  if (SendWebSocketBinaryFrame(socket, message))
    *previous_rgb565 = std::move(rgb565);
}

void GBAStreamHost::AttachInputOverride()
{
  auto* controller = Pad::GetGBAConfig()->GetController(m_device_number);
  controller->SetInputOverrideFunction(
      [this](std::string_view group, std::string_view control,
             ControlState state) -> std::optional<ControlState> {
        static constexpr std::array<std::pair<const char*, u16>, 6> buttons{{
            {GBAPad::A_BUTTON, kKeyA},
            {GBAPad::B_BUTTON, kKeyB},
            {GBAPad::SELECT_BUTTON, kKeySelect},
            {GBAPad::START_BUTTON, kKeyStart},
            {GBAPad::L_BUTTON, kKeyL},
            {GBAPad::R_BUTTON, kKeyR},
        }};
        static constexpr std::array<std::pair<const char*, u16>, 4> dpad{{
            {DIRECTION_UP, kKeyUp},
            {DIRECTION_DOWN, kKeyDown},
            {DIRECTION_LEFT, kKeyLeft},
            {DIRECTION_RIGHT, kKeyRight},
        }};

        if (!m_client_connected)
          return std::nullopt;

        const u16 keys = m_remote_keys.load();
        if (group == GBAPad::BUTTONS_GROUP)
        {
          for (const auto& [name, bit] : buttons)
            if (control == name)
              return (keys & bit) ? 1.0 : 0.0;
        }
        else if (group == GBAPad::DPAD_GROUP)
        {
          for (const auto& [name, bit] : dpad)
            if (control == name)
              return (keys & bit) ? 1.0 : 0.0;
        }
        (void)state;
        return std::nullopt;
      });
}

void GBAStreamHost::DetachInputOverride()
{
  Pad::GetGBAConfig()->GetController(m_device_number)->ClearInputOverrideFunction();
}

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
