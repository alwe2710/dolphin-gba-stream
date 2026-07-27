// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAS_LIBMGBA

#include "Core/HW/GBAStreamHost.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include <zlib.h>

#include <SFML/Network/SocketSelector.hpp>
#include <SFML/System/Time.hpp>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#include "Core/Config/MainSettings.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GBAPadEmu.h"
#include "Core/HW/GBAStreamHandshake.h"
#include "Core/HW/GBAStreamLobby.h"
#include "Core/HW/GBAStreamNetUtil.h"
#include "Core/HW/GBAStreamWebSocket.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"

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

constexpr u8 MSG_TYPE_VIDEO_FRAME = 0x01;
constexpr u8 MSG_TYPE_INPUT = 0x02;
constexpr u8 MSG_TYPE_AUDIO = 0x03;
// Client sends an 8-byte opaque timestamp (its own clock, meaningless to us)
// once a second; we echo it back verbatim as soon as it's parsed, from
// within the receive loop rather than waiting for the next Send*IfPending
// pass, so the round-trip the client measures reflects real network +
// dispatch latency instead of being inflated by our own poll interval. Lets
// the client's latency-monitoring UI show a real ping instead of a guess.
constexpr u8 MSG_TYPE_PING = 0x04;
constexpr u8 MSG_TYPE_PONG = 0x05;

// Video frame sub-format, sent as one extra byte right after width/height
// (see SendVideoFrameIfPending). Two independent bits:
//  - VIDEO_FORMAT_INDEXED: pixels are a per-frame palette (<=256 entries)
//    plus one index byte each, instead of raw RGB565; falls back to unset
//    (raw) whenever a frame/tile-set actually uses more than 256 colors.
//  - VIDEO_FORMAT_TILES: only pixels belonging to 8x8 tiles that changed
//    since the last *sent* frame are included, each prefixed by which tile
//    it is; unset means every pixel is present (the first frame after
//    connect always is, since there's nothing yet to diff against).
// Payload layout once decompressed:
//   [ if TILES: u16 tile_count, tile_count * u16 tile_index ]
//   [ if INDEXED: u16 palette_count, palette_count * u16 rgb565 ]
//   pixel data: (tile_count * TILE_SIZE * TILE_SIZE) or (width * height)
//               pixels, each one u8 palette index (INDEXED) or u16 rgb565
//               (not INDEXED); tile pixels are row-major within each tile,
//               tiles appear in the same order as their indices above.
constexpr u8 VIDEO_FORMAT_RAW = 0x00;
constexpr u8 VIDEO_FORMAT_INDEXED = 0x01;
constexpr u8 VIDEO_FORMAT_TILES = 0x02;
constexpr size_t MAX_PALETTE_COLORS = 256;

// 8x8 evenly divides the fixed 240x160 GBA screen (30x20 tiles) with no
// remainder, so tile position math never needs to handle a partial edge tile.
constexpr u32 TILE_SIZE = 8;

// Remote key bitmask layout (client->server). Chosen to mirror the bit order
// SI_DeviceGBAEmu::GetData() already uses for the internal GBA keypad word,
// purely for readability -- this is our own wire protocol, not mGBA's ABI.
constexpr u16 KEY_A = 1 << 0;
constexpr u16 KEY_B = 1 << 1;
constexpr u16 KEY_SELECT = 1 << 2;
constexpr u16 KEY_START = 1 << 3;
constexpr u16 KEY_RIGHT = 1 << 4;
constexpr u16 KEY_LEFT = 1 << 5;
constexpr u16 KEY_UP = 1 << 6;
constexpr u16 KEY_DOWN = 1 << 7;
constexpr u16 KEY_R = 1 << 8;
constexpr u16 KEY_L = 1 << 9;

void AppendU16LE(std::vector<u8>* out, u16 value)
{
  out->push_back(static_cast<u8>(value & 0xFF));
  out->push_back(static_cast<u8>((value >> 8) & 0xFF));
}

void AppendU32LE(std::vector<u8>* out, u32 value)
{
  out->push_back(static_cast<u8>(value & 0xFF));
  out->push_back(static_cast<u8>((value >> 8) & 0xFF));
  out->push_back(static_cast<u8>((value >> 16) & 0xFF));
  out->push_back(static_cast<u8>((value >> 24) & 0xFF));
}

struct EncodedPixels
{
  bool indexed;
  std::vector<u8> payload;
};

// Builds the pixel portion of a video frame message (see the VIDEO_FORMAT_*
// comment above) from a run of `pixel_count` RGB565 pixels -- either the
// whole frame or just the gathered pixels of its changed tiles, the caller
// doesn't need to differ here. Tries a per-frame palette first since GBA
// tile-mode content rarely uses more than a handful of colors at once; falls
// back to the raw bytes untouched if this particular pixel run happens to
// use more than 256 distinct colors (bitmap modes, heavy blending, or -- in
// the tiles case -- an unusually large, colorful set of changed tiles).
EncodedPixels EncodePixelsWithOptionalPalette(const std::vector<u8>& rgb565, size_t pixel_count)
{
  std::vector<u16> palette;
  std::vector<u8> indices(pixel_count);
  std::unordered_map<u16, u8> color_to_index;
  bool use_palette = true;
  for (size_t i = 0; i < pixel_count && use_palette; ++i)
  {
    const u16 color = static_cast<u16>(rgb565[i * 2]) | (static_cast<u16>(rgb565[i * 2 + 1]) << 8);
    auto [it, inserted] = color_to_index.try_emplace(color, 0);
    if (inserted)
    {
      if (palette.size() >= MAX_PALETTE_COLORS)
      {
        use_palette = false;
        break;
      }
      it->second = static_cast<u8>(palette.size());
      palette.push_back(color);
    }
    indices[i] = it->second;
  }

  EncodedPixels result;
  result.indexed = use_palette;
  if (use_palette)
  {
    result.payload.reserve(2 + palette.size() * 2 + indices.size());
    AppendU16LE(&result.payload, static_cast<u16>(palette.size()));
    for (const u16 color : palette)
      AppendU16LE(&result.payload, color);
    result.payload.insert(result.payload.end(), indices.begin(), indices.end());
  }
  else
  {
    result.payload.assign(rgb565.begin(), rgb565.begin() + static_cast<ptrdiff_t>(pixel_count * 2));
  }
  return result;
}

// Nearest-neighbor downscale from (src_width, src_height) to (dst_width,
// dst_height), both RGB565 row-major. Only used when a client's negotiated
// resolution (GBAStreamHandshake.h's NegotiateVideo) is below native -- GBA
// content is small pixel art to begin with, so a cheap point-sample resize is
// a fine tradeoff against a real filter's extra CPU time on every frame.
std::vector<u8> DownscaleRgb565(const std::vector<u8>& src, u32 src_width, u32 src_height,
                                u32 dst_width, u32 dst_height)
{
  std::vector<u8> dst(static_cast<size_t>(dst_width) * dst_height * 2);
  for (u32 y = 0; y < dst_height; ++y)
  {
    const u32 src_y = std::min(src_height - 1, y * src_height / dst_height);
    for (u32 x = 0; x < dst_width; ++x)
    {
      const u32 src_x = std::min(src_width - 1, x * src_width / dst_width);
      const size_t src_index = (static_cast<size_t>(src_y) * src_width + src_x) * 2;
      const size_t dst_index = (static_cast<size_t>(y) * dst_width + x) * 2;
      dst[dst_index] = src[src_index];
      dst[dst_index + 1] = src[src_index + 1];
    }
  }
  return dst;
}

std::vector<u8> DeflateRaw(const std::vector<u8>& input)
{
  z_stream strm{};
  // windowBits = -15 requests headerless "raw deflate", which is exactly what
  // the browser's DecompressionStream('deflate-raw') expects. Z_BEST_SPEED
  // over the default level trades a slightly larger payload for noticeably
  // less CPU time per frame -- on a LAN the extra bytes are negligible, but
  // the encode time is pure added latency for a real-time stream, and GBA
  // frames (paletted pixel art) compress well even at the fastest level.
  if (deflateInit2(&strm, Z_BEST_SPEED, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
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

// WebSocketFrame, TryParseWebSocketFrame, SendWebSocketBinaryFrame and the
// OPCODE_* constants used to live here; they moved to GBAStreamWebSocket.h
// (as WS_OPCODE_*) once GBAStreamLobby needed the same WebSocket transport
// for the app-level handshake, rather than duplicating them there.

}  // namespace

std::vector<int> GBAStreamHost::CheckPortsInUse()
{
  std::vector<int> busy_ports;
  bool any_stream_port_configured = false;

  for (int device_number = 0; device_number < SerialInterface::MAX_SI_CHANNELS; ++device_number)
  {
    if (Config::Get(Config::GetInfoForSIDevice(device_number)) !=
        SerialInterface::SIDEVICE_GC_GBA_STREAM)
    {
      continue;
    }
    any_stream_port_configured = true;

    const auto port = static_cast<unsigned short>(GBA_STREAM_PLAYER_BASE_PORT + device_number);
    sf::TcpListener probe;
    if (probe.listen(port) != sf::Socket::Status::Done)
      busy_ports.push_back(port);
  }

  if (any_stream_port_configured)
  {
    sf::TcpListener probe;
    if (probe.listen(GBA_STREAM_LOBBY_PORT) != sf::Socket::Status::Done)
      busy_ports.push_back(GBA_STREAM_LOBBY_PORT);
  }

  return busy_ports;
}

std::mutex GBAStreamHost::s_registry_mutex;
std::array<GBAStreamHost*, 4> GBAStreamHost::s_instances{};

bool GBAStreamHost::IsSlotOccupied(int device_number)
{
  std::lock_guard<std::mutex> lock(s_registry_mutex);
  if (device_number < 0 || device_number >= static_cast<int>(s_instances.size()))
    return false;
  const GBAStreamHost* host = s_instances[static_cast<size_t>(device_number)];
  return host != nullptr && host->m_client_connected.load();
}

std::string GBAStreamHost::GetSlotLabel(int device_number)
{
  return "P" + std::to_string(device_number + 1);
}

HandshakeAudioInfo GBAStreamHost::GetNativeAudioInfo(int device_number)
{
  std::lock_guard<std::mutex> registry_lock(s_registry_mutex);
  if (device_number < 0 || device_number >= static_cast<int>(s_instances.size()))
    return HandshakeAudioInfo{32768, 2};
  GBAStreamHost* host = s_instances[static_cast<size_t>(device_number)];
  if (!host)
    return HandshakeAudioInfo{32768, 2};
  std::lock_guard<std::mutex> audio_lock(host->m_audio_mutex);
  return HandshakeAudioInfo{host->m_audio_sample_rate.load(), static_cast<u8>(host->m_audio_channels)};
}

GBAStreamHost::GBAStreamHost(int device_number) : m_device_number(device_number)
{
  {
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    s_instances[static_cast<size_t>(device_number)] = this;
  }

  // Keeps the always-on lobby page (fixed port 6800) running for as long as
  // at least one GC port is configured as GBA (Client-Stream), regardless of
  // which port(s) those are.
  GBAStreamLobby::AddRef();

  const auto port = static_cast<unsigned short>(GBA_STREAM_PLAYER_BASE_PORT + device_number);
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
  // Unregister first, under s_registry_mutex, before anything below starts
  // tearing down this instance's own state -- see the s_instances comment in
  // the header for why this ordering is what makes IsSlotOccupied() safe to
  // call concurrently from another thread (GBAStreamLobby's) throughout the
  // rest of this destructor.
  {
    std::lock_guard<std::mutex> lock(s_registry_mutex);
    s_instances[static_cast<size_t>(m_device_number)] = nullptr;
  }

  m_stop = true;
  m_listener.close();
  if (m_accept_thread.joinable())
    m_accept_thread.join();
  DetachInputOverride();
  GBAStreamLobby::Release();
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
  // A plain blocking m_listener.accept() would only return once a connection
  // arrives; closing the listener from the destructor's thread while this
  // thread is parked inside accept() is not guaranteed to unblock it on
  // Linux, which previously made the destructor's join() -- and therefore
  // stopping emulation -- hang whenever no client was currently connected.
  // Polling through a selector instead bounds every iteration to 100ms so
  // m_stop is always checked promptly.
  sf::SocketSelector selector;
  selector.add(m_listener);
  while (!m_stop)
  {
    if (!selector.wait(sf::milliseconds(100)))
      continue;
    auto socket = std::make_shared<sf::TcpSocket>();
    if (m_listener.accept(*socket) != sf::Socket::Status::Done)
      continue;

    // Dispatched to its own thread rather than served inline here (see
    // ServeConnection()'s header comment for why) -- `socket` is kept alive
    // for that thread's lifetime by the shared_ptr the lambda captures.
    // Threads aren't reaped as they finish, only joined in bulk once this
    // loop itself exits below: a /status probe's thread lives at most a
    // few hundred milliseconds, and this project's Dolphin sessions don't
    // run long/busy enough for the resulting handful-to-low-hundreds of
    // joinable-but-finished std::thread objects to matter -- simpler than
    // adding a way to detect "already finished" without joining.
    std::lock_guard<std::mutex> lock(m_connection_threads_mutex);
    m_connection_threads.emplace_back([this, socket] { ServeConnection(*socket); });
  }

  // m_stop is set: every ServeConnection() thread's own blocking calls
  // already unwind promptly once they observe it (same stop_flag pattern as
  // this loop's own selector.wait() above), so this join only has to wait
  // out that same ~100ms budget per thread, not an active session's full
  // remaining duration. Must happen here, before returning -- ~GBAStreamHost()
  // (the only caller that sets m_stop and then joins m_accept_thread, which
  // runs this function) starts destroying this object's members right after
  // that join, and every still-running ServeConnection() touches them.
  std::lock_guard<std::mutex> lock(m_connection_threads_mutex);
  for (std::thread& t : m_connection_threads)
  {
    if (t.joinable())
      t.join();
  }
}

void GBAStreamHost::ServeConnection(sf::TcpSocket& socket)
{
  // Non-blocking for the connection's whole lifetime: every read/send below is
  // written to tolerate NotReady and re-check m_stop, so a stalled peer (dead
  // network, frozen tab) can never block this thread -- and therefore never
  // block stopping emulation -- indefinitely.
  socket.setBlocking(false);
  SetNoDelay(socket);

  bool is_websocket = false;
  if (!PerformHandshake(socket, &is_websocket) || !is_websocket)
    return;

  // App-level handshake (GBAStreamHandshake.h): must succeed -- version match,
  // this slot requested and free -- before any Video/Audio/Input binary frame
  // is allowed on this connection. Populates m_negotiated_* on success.
  if (!PerformAppHandshake(socket))
    return;

  AttachInputOverride();
  RunWebSocketSession(socket);
  DetachInputOverride();
}

bool GBAStreamHost::PerformHandshake(sf::TcpSocket& socket, bool* is_websocket)
{
  *is_websocket = false;

  const auto request_opt = ReadHttpRequest(socket, m_stop);
  if (!request_opt)
    return false;
  const HttpRequest& request = *request_opt;
  const std::string& path = request.path;
  const std::map<std::string, std::string>& headers = request.headers;

  if (path == "/status")
  {
    // Queried cross-port by the lobby page (GBAStreamLobby, GBAStreamClientPage.h)
    // to find out which GC ports are currently configured as GBA (Client-Stream)
    // and whether each one already has a client attached, so it can show a
    // P1-P4 picker with taken slots grayed out. CORS is required since the
    // lobby always lives on a different port (6800) than this one.
    const std::string body =
        std::string("{\"occupied\":") + (m_client_connected ? "true" : "false") + "}";
    std::ostringstream response;
    // std::to_string(), not body.size() streamed directly: an ostringstream
    // formats integers per the process's current locale, which (unlike the
    // "C" locale streams default to) may insert thousands-grouping
    // punctuation into what must be a plain decimal Content-Length --
    // std::to_string() is always locale-independent. See GBAStreamLobby.cpp's
    // identical fix for where this was actually observed (this body is
    // always under 1000 bytes, too small to trigger grouping, but the bug
    // is the same).
    response << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Content-Length: " << std::to_string(body.size()) << "\r\n"
             << "Connection: close\r\n\r\n"
             << body;
    const std::string response_str = response.str();
    if (SendAllBytes(socket, response_str.data(), response_str.size(), m_stop))
      CloseGracefully(socket, m_stop);
    return true;
  }

  if (!IsWebSocketUpgradeRequest(request))
  {
    // Player ports are API-only (status + WebSocket); send anyone who
    // navigates here directly (e.g. an old bookmark) to the lobby (fixed
    // port 6800) instead of duplicating its page here -- a plain GET there
    // once again serves the (now WASM-based) web client page, see
    // GBAStreamLobby.cpp/GBAStreamClientPage.h.
    std::string host = headers.count("host") ? headers.at("host") : "localhost";
    const auto colon = host.find(':');
    if (colon != std::string::npos)
      host.resize(colon);

    std::ostringstream response;
    response << "HTTP/1.1 302 Found\r\n"
             << "Location: http://" << host << ":6800/\r\n"
             << "Connection: close\r\n\r\n";
    const std::string response_str = response.str();
    if (SendAllBytes(socket, response_str.data(), response_str.size(), m_stop))
      CloseGracefully(socket, m_stop);
    return true;
  }

  if (!SendWebSocketUpgradeResponse(socket, request, m_stop))
    return false;

  *is_websocket = true;
  return true;
}

namespace
{
// How long PerformAppHandshake waits for hello_ack before giving up on this
// connection -- generous for a LAN round-trip, short enough that a client
// that connected but never speaks the handshake (e.g. an old, pre-protocol-2
// client, or a stray non-finlink connection) doesn't tie up this slot.
constexpr std::chrono::milliseconds APP_HANDSHAKE_TIMEOUT{3000};
}  // namespace

bool GBAStreamHost::PerformAppHandshake(sf::TcpSocket& socket)
{
  u32 native_sample_rate;
  u8 native_channels;
  {
    std::lock_guard<std::mutex> lock(m_audio_mutex);
    native_sample_rate = m_audio_sample_rate.load();
    native_channels = static_cast<u8>(m_audio_channels);
  }

  HandshakeOffer offer;
  offer.stream_type = kStreamTypeGcGbaLink;
  offer.slot_list = {HandshakeSlot{m_device_number, GetSlotLabel(m_device_number),
                               m_client_connected.load()}};
  offer.video_width = GBA_NATIVE_WIDTH;
  offer.video_height = GBA_NATIVE_HEIGHT;
  offer.video_fps = GBA_NATIVE_FPS;
  offer.audio = HandshakeAudioInfo{native_sample_rate, native_channels};
  offer.input_encoding = "gba_buttons";

  if (!SendWebSocketTextFrame(socket, BuildHelloMessage(offer), m_stop))
    return false;

  const auto frame = ReceiveOneWebSocketFrame(socket, m_stop, APP_HANDSHAKE_TIMEOUT);
  if (!frame || frame->opcode != WS_OPCODE_TEXT)
    return false;  // Timed out, disconnected, or sent something other than
                    // the expected single JSON text frame -- nothing
                    // meaningful to reply an error to at this point.

  const auto ack = ParseHelloAck(frame->payload);
  if (!ack)
  {
    SendWebSocketTextFrame(
        socket, BuildHandshakeErrorMessage(HandshakeErrorCode::MalformedRequest,
                                           "hello_ack konnte nicht gelesen werden."),
        m_stop);
    return false;
  }
  if (ack->protocol_version != GBA_STREAM_PROTOCOL_VERSION)
  {
    SendWebSocketTextFrame(
        socket,
        BuildHandshakeErrorMessage(
            HandshakeErrorCode::VersionMismatch,
            "Server spricht Protokollversion " + std::to_string(GBA_STREAM_PROTOCOL_VERSION) +
                ", Client meldet " + std::to_string(ack->protocol_version) + "."),
        m_stop);
    return false;
  }
  if (ack->requested_slot != m_device_number)
  {
    SendWebSocketTextFrame(
        socket,
        BuildHandshakeErrorMessage(HandshakeErrorCode::MalformedRequest,
                                   "Dieser Port bedient nur Slot " +
                                       std::to_string(m_device_number) + "."),
        m_stop);
    return false;
  }
  if (m_client_connected.load())
  {
    SendWebSocketTextFrame(
        socket,
        BuildHandshakeErrorMessage(HandshakeErrorCode::SlotUnavailable,
                                   "Slot " + GetSlotLabel(m_device_number) +
                                       " wurde inzwischen von einem anderen Client belegt."),
        m_stop);
    return false;
  }

  const NegotiatedVideo negotiated_video =
      NegotiateVideo(GBA_NATIVE_WIDTH, GBA_NATIVE_HEIGHT, GBA_NATIVE_FPS, ack->video_limits);
  std::optional<NegotiatedAudio> negotiated_audio;
  if (ack->audio_limits)
    negotiated_audio = NegotiateAudio(native_sample_rate, native_channels, *ack->audio_limits);

  if (!SendWebSocketTextFrame(
          socket, BuildSessionReadyMessage(m_device_number, negotiated_video, negotiated_audio,
                                           std::nullopt /* redirect: this is the terminal port */),
          m_stop))
  {
    return false;
  }

  m_negotiated_width = negotiated_video.width;
  m_negotiated_height = negotiated_video.height;
  m_negotiated_fps = negotiated_video.fps;
  m_audio_enabled = negotiated_audio.has_value();
  m_negotiated_audio_channels = negotiated_audio ? negotiated_audio->channels : native_channels;
  m_last_video_send_time = std::chrono::steady_clock::time_point{};
  return true;
}

void GBAStreamHost::RunWebSocketSession(sf::TcpSocket& socket)
{
  sf::SocketSelector selector;
  selector.add(socket);

  std::vector<u8> recv_buffer;
  std::array<u8, 4096> read_buf{};

  m_remote_keys = 0;
  {
    std::lock_guard<std::mutex> lock(m_audio_mutex);
    m_pending_audio.clear();
  }
  m_client_connected = true;

  // Guards every send on this socket (video/audio from the writer thread
  // below, plus this thread's own inline pong replies) so the two threads can
  // never interleave bytes mid-frame on the wire -- SendAllBytes' retry loop
  // assumes it's the sole writer for the duration of one logical message.
  // Deliberately does NOT guard receive(): reading and applying fresh input
  // must never wait on a stuck send, which is the whole point of this split.
  // Safe to use alongside concurrent receive() on the same socket only
  // because this feature never enables TLS (plain ws://, never wss://) --
  // SFML's raw send()/receive() overloads touch no shared instance state in
  // that case. Revisit this if wss:// support is ever added.
  std::mutex send_mutex;
  std::atomic_bool session_stop{false};

  // Dedicated to draining pending video/audio so a send stalled by a
  // congested/lossy link (bounded by SendAllBytes' own 3s deadline) can never
  // delay reading/applying new input or replying to pings on the thread
  // below -- previously both were serialized on one thread, so a slow frame
  // send doubled as input lag. Poll cadence matches the original combined
  // loop's 4ms.
  std::thread writer_thread([this, &socket, &send_mutex, &session_stop] {
    u64 last_sent_frame_id = 0;
    std::vector<u8> previous_rgb565;
    while (!m_stop && !session_stop)
    {
      {
        std::lock_guard<std::mutex> lock(send_mutex);
        SendVideoFrameIfPending(socket, &last_sent_frame_id, &previous_rgb565);
        SendAudioIfPending(socket);
      }
      if (m_stop || session_stop)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
  });

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
        while (true)
        {
          const auto frame = TryParseWebSocketFrame(recv_buffer);
          if (!frame)
            break;
          recv_buffer.erase(recv_buffer.begin(),
                            recv_buffer.begin() + static_cast<ptrdiff_t>(frame->consumed));

          if (frame->opcode == WS_OPCODE_CLOSE)
          {
            disconnect_requested = true;
            break;
          }
          if (frame->opcode == WS_OPCODE_BINARY && frame->payload.size() == 3 &&
              frame->payload[0] == MSG_TYPE_INPUT)
          {
            const u16 keys =
                static_cast<u16>(frame->payload[1]) | (static_cast<u16>(frame->payload[2]) << 8);
            m_remote_keys.store(keys);
          }
          else if (frame->opcode == WS_OPCODE_BINARY && frame->payload.size() == 9 &&
                   frame->payload[0] == MSG_TYPE_PING)
          {
            std::vector<u8> pong;
            pong.reserve(9);
            pong.push_back(MSG_TYPE_PONG);
            pong.insert(pong.end(), frame->payload.begin() + 1, frame->payload.end());
            std::lock_guard<std::mutex> lock(send_mutex);
            SendWebSocketBinaryFrame(socket, pong, m_stop);
          }
        }
        if (disconnect_requested)
          break;
      }
    }
  }

  session_stop = true;
  writer_thread.join();

  CloseGracefully(socket, m_stop);
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

  // Frame-rate cap negotiated in PerformAppHandshake (m_negotiated_fps <
  // native = client asked for less than the GBA's native ~59.7275 Hz).
  // Independent of the pixel-diff dedup further down: this gates purely on
  // wall-clock time since the last frame actually *sent*, which also
  // naturally saves the pixel-conversion/diff work below for a throttled
  // session. Skipped entirely -- zero added latency -- when native FPS was
  // negotiated, the overwhelmingly common case.
  if (m_negotiated_fps < GBA_NATIVE_FPS)
  {
    const auto min_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / m_negotiated_fps));
    const bool sent_before = m_last_video_send_time.time_since_epoch().count() != 0;
    if (sent_before && std::chrono::steady_clock::now() - m_last_video_send_time < min_interval)
      return;
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

  // Downscaling negotiated in PerformAppHandshake: replaces the native buffer
  // (and the width/height the rest of this function -- tile math, message
  // header -- works off) with a smaller one *before* any of the existing
  // diff/encode logic below runs, so that logic itself needs no knowledge of
  // negotiation at all. NegotiateVideo() only ever returns native dimensions
  // or ones divisible by 8, so the tile math's "8x8 evenly divides
  // width/height" assumption keeps holding here exactly as it did before
  // downscaling existed.
  if (m_negotiated_width != width || m_negotiated_height != height)
  {
    rgb565 = DownscaleRgb565(rgb565, width, height, m_negotiated_width, m_negotiated_height);
    width = m_negotiated_width;
    height = m_negotiated_height;
  }

  if (rgb565 == *previous_rgb565)
    return;

  // GBA content typically only changes a fraction of the screen per frame
  // (HUD/background stay put while sprites move or a small area scrolls), so
  // resending all 240x160 pixels every time this differs from the last
  // *sent* frame at all wastes most of the message on pixels the client
  // already has correctly on screen. Diffing in 8x8 tiles against
  // *previous_rgb565 finds just the changed ones; `have_previous` is false
  // only right after connect (previous_rgb565 starts empty), which is
  // exactly when a full frame is needed anyway to give the client something
  // to diff against in the first place.
  const bool have_previous = previous_rgb565->size() == rgb565.size();
  const u32 tiles_x = width / TILE_SIZE;
  const u32 tiles_y = height / TILE_SIZE;

  std::vector<u16> changed_tiles;
  std::vector<u8> tile_pixels;
  if (have_previous)
  {
    for (u32 ty = 0; ty < tiles_y; ++ty)
    {
      for (u32 tx = 0; tx < tiles_x; ++tx)
      {
        bool tile_changed = false;
        for (u32 row = 0; row < TILE_SIZE; ++row)
        {
          const auto row_offset = static_cast<ptrdiff_t>(
              (static_cast<size_t>(ty * TILE_SIZE + row) * width + tx * TILE_SIZE) * 2);
          if (!std::equal(rgb565.begin() + row_offset,
                          rgb565.begin() + row_offset + TILE_SIZE * 2,
                          previous_rgb565->begin() + row_offset))
          {
            tile_changed = true;
            break;
          }
        }
        if (tile_changed)
          changed_tiles.push_back(static_cast<u16>(ty * tiles_x + tx));
      }
    }

    if (changed_tiles.empty())
      return;  // Bit-identical to the last sent frame (handled per-tile) -- nothing to send.

    tile_pixels.resize(changed_tiles.size() * TILE_SIZE * TILE_SIZE * 2);
    size_t out = 0;
    for (const u16 tile_index : changed_tiles)
    {
      const u32 tx = tile_index % tiles_x;
      const u32 ty = tile_index / tiles_x;
      for (u32 row = 0; row < TILE_SIZE; ++row)
      {
        const auto row_offset = static_cast<ptrdiff_t>(
            (static_cast<size_t>(ty * TILE_SIZE + row) * width + tx * TILE_SIZE) * 2);
        std::copy(rgb565.begin() + row_offset, rgb565.begin() + row_offset + TILE_SIZE * 2,
                  tile_pixels.begin() + static_cast<ptrdiff_t>(out));
        out += TILE_SIZE * 2;
      }
    }
  }

  // Sending only changed tiles costs 2 extra header bytes per tile for its
  // index -- worth it unless enough of the screen changed at once (e.g. a
  // full-screen fade) that the overhead exceeds just resending everything.
  const bool use_tiles =
      have_previous && changed_tiles.size() * (TILE_SIZE * TILE_SIZE * 2 + 2) < rgb565.size();

  const EncodedPixels encoded =
      use_tiles ?
          EncodePixelsWithOptionalPalette(tile_pixels,
                                          changed_tiles.size() * TILE_SIZE * TILE_SIZE) :
          EncodePixelsWithOptionalPalette(rgb565, static_cast<size_t>(width) * height);

  std::vector<u8> payload;
  if (use_tiles)
  {
    payload.reserve(2 + changed_tiles.size() * 2 + encoded.payload.size());
    AppendU16LE(&payload, static_cast<u16>(changed_tiles.size()));
    for (const u16 tile_index : changed_tiles)
      AppendU16LE(&payload, tile_index);
  }
  payload.insert(payload.end(), encoded.payload.begin(), encoded.payload.end());

  const u8 format = static_cast<u8>((encoded.indexed ? VIDEO_FORMAT_INDEXED : VIDEO_FORMAT_RAW) |
                                    (use_tiles ? VIDEO_FORMAT_TILES : 0));

  const std::vector<u8> compressed = DeflateRaw(payload);

  std::vector<u8> message;
  message.reserve(10 + compressed.size());
  message.push_back(MSG_TYPE_VIDEO_FRAME);
  AppendU32LE(&message, width);
  AppendU32LE(&message, height);
  message.push_back(format);
  message.insert(message.end(), compressed.begin(), compressed.end());

  if (SendWebSocketBinaryFrame(socket, message, m_stop))
  {
    *previous_rgb565 = std::move(rgb565);
    m_last_video_send_time = std::chrono::steady_clock::now();
  }
}

void GBAStreamHost::SendAudioIfPending(sf::TcpSocket& socket)
{
  std::vector<s16> samples;
  u32 channels;
  {
    std::lock_guard<std::mutex> lock(m_audio_mutex);
    if (m_pending_audio.empty())
      return;
    samples = std::move(m_pending_audio);
    m_pending_audio.clear();
    channels = m_audio_channels;
  }

  // Negotiated in PerformAppHandshake: a stream_type without audio (not
  // GC_GBA_LINK, not reachable in this branch) or a client that declined it
  // disables sending entirely -- the samples above are still drained (so
  // ForwardAudioSamples' queue doesn't grow across an entire muted session)
  // but simply dropped here instead of sent.
  if (!m_audio_enabled)
    return;

  // Sample-rate downsampling isn't implemented (NegotiateAudio() never
  // reports one, see GBAStreamHandshake.cpp); channel count is the one axis
  // actually acted on here, and only for the one real case this core has --
  // stereo down to mono, by averaging each L/R pair -- before the existing
  // message-building loop below, which is otherwise unchanged.
  if (channels == 2 && m_negotiated_audio_channels == 1)
  {
    std::vector<s16> mono(samples.size() / 2);
    for (size_t i = 0; i < mono.size(); ++i)
    {
      const int left = samples[i * 2];
      const int right = samples[i * 2 + 1];
      mono[i] = static_cast<s16>((left + right) / 2);
    }
    samples = std::move(mono);
    channels = 1;
  }

  std::vector<u8> message;
  message.reserve(6 + samples.size() * 2);
  message.push_back(MSG_TYPE_AUDIO);
  AppendU32LE(&message, m_audio_sample_rate.load());
  message.push_back(static_cast<u8>(channels));
  for (const s16 sample : samples)
  {
    message.push_back(static_cast<u8>(sample & 0xFF));
    message.push_back(static_cast<u8>((sample >> 8) & 0xFF));
  }
  SendWebSocketBinaryFrame(socket, message, m_stop);
}

void GBAStreamHost::AudioRateChanged(u32 sample_rate)
{
  m_audio_sample_rate.store(sample_rate);
}

bool GBAStreamHost::ForwardAudioSamples(std::span<const s16> samples, u32 channels)
{
  // Unlike video/input, audio is never allowed to fall back to the local
  // speakers for a GBA (Client-Stream) port, connected client or not: this
  // slot's audio belongs to whichever remote player it's streaming to, full
  // stop. If nobody is connected the samples are simply dropped (nobody is
  // listening on the network side either), rather than played locally.
  if (m_client_connected)
  {
    std::lock_guard<std::mutex> lock(m_audio_mutex);
    m_pending_audio.insert(m_pending_audio.end(), samples.begin(), samples.end());
    m_audio_channels = channels;

    // Backstop for the time it takes SendAllBytes' own timeout to notice a
    // stalled client: without a cap, a send loop stuck waiting on a wedged
    // peer would let this queue -- fed independently from the GBA audio
    // thread -- grow without bound. ~2s of 48kHz stereo audio is generous
    // enough to never trim during normal playback; if it's ever hit, the
    // oldest samples are dropped since a backlog that size is already
    // inaudibly stale.
    constexpr size_t MAX_PENDING_SAMPLES = 48000 * 2 * 2;
    if (m_pending_audio.size() > MAX_PENDING_SAMPLES)
    {
      m_pending_audio.erase(m_pending_audio.begin(),
                            m_pending_audio.end() - static_cast<ptrdiff_t>(MAX_PENDING_SAMPLES));
    }
  }
  return true;
}

void GBAStreamHost::AttachInputOverride()
{
  auto* controller = Pad::GetGBAConfig()->GetController(m_device_number);
  controller->SetInputOverrideFunction([this](std::string_view group, std::string_view control,
                                              ControlState state) -> std::optional<ControlState> {
    static constexpr std::array<std::pair<const char*, u16>, 6> buttons{{
        {GBAPad::A_BUTTON, KEY_A},
        {GBAPad::B_BUTTON, KEY_B},
        {GBAPad::SELECT_BUTTON, KEY_SELECT},
        {GBAPad::START_BUTTON, KEY_START},
        {GBAPad::L_BUTTON, KEY_L},
        {GBAPad::R_BUTTON, KEY_R},
    }};
    static constexpr std::array<std::pair<const char*, u16>, 4> dpad{{
        {DIRECTION_UP, KEY_UP},
        {DIRECTION_DOWN, KEY_DOWN},
        {DIRECTION_LEFT, KEY_LEFT},
        {DIRECTION_RIGHT, KEY_RIGHT},
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
