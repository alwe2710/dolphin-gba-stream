// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAS_LIBMGBA

#include "Core/HW/GBAStreamLobby.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <SFML/Network/SocketSelector.hpp>
#include <SFML/Network/TcpListener.hpp>
#include <SFML/Network/TcpSocket.hpp>
#include <SFML/System/Time.hpp>

#include "Common/Logging/Log.h"

#include "Core/Config/MainSettings.h"
#include "Core/HW/GBAStreamBeacon.h"
#include "Core/HW/GBAStreamHandshake.h"
#include "Core/HW/GBAStreamHost.h"
#include "Core/HW/GBAStreamNetUtil.h"
#include "Core/HW/GBAStreamWebSocket.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"

namespace HW::GBA
{
namespace
{
// How long the lobby waits for hello_ack once it's sent `hello`, same
// reasoning and value as GBAStreamHost's PerformAppHandshake.
constexpr std::chrono::milliseconds APP_HANDSHAKE_TIMEOUT{3000};

// Every GC port currently configured as GBA (Client-Stream) -- i.e. every
// device_number a GBAStreamHost instance exists for right now. Mirrors
// GBAStreamHost::CheckPortsInUse()'s own device-number loop.
std::vector<int> ConfiguredDevices()
{
  std::vector<int> devices;
  for (int device_number = 0; device_number < SerialInterface::MAX_SI_CHANNELS; ++device_number)
  {
    if (Config::Get(Config::GetInfoForSIDevice(device_number)) ==
        SerialInterface::SIDEVICE_GC_GBA_STREAM)
    {
      devices.push_back(device_number);
    }
  }
  return devices;
}

// The Host header's hostname/IP, minus any :port suffix -- what the client
// itself dialed to reach this lobby, so it's guaranteed reachable for the
// reconnect a redirect asks for. Same source GBAStreamHost::PerformHandshake
// already uses for its own lobby-redirect-on-plain-GET fallback.
std::string RequestHost(const HttpRequest& request)
{
  std::string host = request.headers.count("host") ? request.headers.at("host") : "localhost";
  const auto colon = host.find(':');
  if (colon != std::string::npos)
    host.resize(colon);
  return host;
}

// Runs the app-level handshake (GBAStreamHandshake.h) on an already
// WebSocket-upgraded `socket`: advertises every configured slot, waits for
// hello_ack, and either redirects to the requested slot's player port or
// replies handshake_error. The lobby never streams Video/Audio/Input itself
// -- GBAStreamHost redoes this same hello/hello_ack exchange on the target
// port and performs the real negotiation there (see GBAStreamHost.cpp's
// PerformAppHandshake), since it alone knows that slot's live native audio
// rate/channels and occupancy at the moment the client actually arrives.
void PerformHandshakeAndRedirect(sf::TcpSocket& socket, const HttpRequest& request,
                                  const std::atomic_bool& stop_flag)
{
  const std::vector<int> configured_devices = ConfiguredDevices();
  if (configured_devices.empty())
  {
    // Shouldn't happen -- the lobby only runs while at least one port is
    // configured -- but a client connecting in the narrow window right as
    // the last one is being reconfigured away shouldn't hang waiting for a
    // hello that would otherwise claim zero slots that all get rejected.
    SendWebSocketTextFrame(
        socket,
        BuildHandshakeErrorMessage(HandshakeErrorCode::SlotUnavailable,
                                   "Aktuell ist kein GC-Port als GBA (Client-Stream) konfiguriert."),
        stop_flag);
    return;
  }

  HandshakeOffer offer;
  offer.stream_type = kStreamTypeGcGbaLink;
  offer.slot_list.reserve(configured_devices.size());
  for (const int device_number : configured_devices)
  {
    offer.slot_list.push_back(HandshakeSlot{device_number, GBAStreamHost::GetSlotLabel(device_number),
                                        GBAStreamHost::IsSlotOccupied(device_number)});
  }
  offer.video_width = GBA_NATIVE_WIDTH;
  offer.video_height = GBA_NATIVE_HEIGHT;
  offer.video_fps = GBA_NATIVE_FPS;
  offer.audio = GBAStreamHost::GetNativeAudioInfo(configured_devices.front());
  offer.input_encoding = "gba_buttons";

  if (!SendWebSocketTextFrame(socket, BuildHelloMessage(offer), stop_flag))
    return;

  const auto frame = ReceiveOneWebSocketFrame(socket, stop_flag, APP_HANDSHAKE_TIMEOUT);
  if (!frame || frame->opcode != WS_OPCODE_TEXT)
    return;  // Timed out, disconnected, or not the expected single JSON text
              // frame -- nothing meaningful to reply an error to.

  const auto ack = ParseHelloAck(frame->payload);
  if (!ack)
  {
    SendWebSocketTextFrame(socket,
                           BuildHandshakeErrorMessage(HandshakeErrorCode::MalformedRequest,
                                                       "hello_ack konnte nicht gelesen werden."),
                           stop_flag);
    return;
  }
  if (ack->protocol_version != GBA_STREAM_PROTOCOL_VERSION)
  {
    SendWebSocketTextFrame(
        socket,
        BuildHandshakeErrorMessage(
            HandshakeErrorCode::VersionMismatch,
            "Server spricht Protokollversion " + std::to_string(GBA_STREAM_PROTOCOL_VERSION) +
                ", Client meldet " + std::to_string(ack->protocol_version) + "."),
        stop_flag);
    return;
  }

  const bool slot_configured = std::find(configured_devices.begin(), configured_devices.end(),
                                         ack->requested_slot) != configured_devices.end();
  if (!slot_configured)
  {
    SendWebSocketTextFrame(
        socket,
        BuildHandshakeErrorMessage(HandshakeErrorCode::MalformedRequest,
                                   "Unbekannter oder nicht konfigurierter Slot."),
        stop_flag);
    return;
  }
  if (GBAStreamHost::IsSlotOccupied(ack->requested_slot))
  {
    SendWebSocketTextFrame(
        socket,
        BuildHandshakeErrorMessage(HandshakeErrorCode::SlotUnavailable,
                                   "Slot " + GBAStreamHost::GetSlotLabel(ack->requested_slot) +
                                       " wurde inzwischen von einem anderen Client belegt."),
        stop_flag);
    return;
  }

  // Nothing here is the final negotiation (GBAStreamHost does that once the
  // client reconnects to the target port) -- `video`/`audio`/`video_mode`
  // are filled with placeholder values only because BuildSessionReadyMessage's
  // shape requires something; the `redirect` field is the only part of this
  // reply the client actually needs to act on. The real, authoritative
  // video_mode arrives in the second session_ready, after the redirect.
  const NegotiatedVideo native_video{GBA_NATIVE_WIDTH, GBA_NATIVE_HEIGHT, GBA_NATIVE_FPS};
  const HandshakeRedirect redirect{
      RequestHost(request),
      static_cast<u16>(GBA_STREAM_PLAYER_BASE_PORT + ack->requested_slot)};
  SendWebSocketTextFrame(
      socket, BuildSessionReadyMessage(ack->requested_slot, native_video, std::nullopt, redirect,
                                        "tiles" /* placeholder, see comment above */),
      stop_flag);
}

class LobbyServer
{
public:
  void Start()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_ref_count;
    // Separate from the refcount check: if an earlier AddRef's listen() call
    // failed (e.g. the port was transiently still held by a just-exited
    // previous Dolphin process), every later AddRef used to see refcount > 0
    // and assume the lobby was already up, silently leaving it dead for the
    // rest of the session. Retrying here instead means the *next* GBA slot
    // to start gets another chance once the transient conflict has cleared.
    if (m_running)
      return;

    m_stop = false;
    const auto status = m_listener.listen(GBA_STREAM_LOBBY_PORT);
    if (status != sf::Socket::Status::Done)
    {
      ERROR_LOG_FMT(SERIALINTERFACE, "GBAStreamLobby: failed to listen on port {}",
                    GBA_STREAM_LOBBY_PORT);
      return;
    }
    m_running = true;
    m_thread = std::thread([this] { AcceptLoop(); });
    m_beacon.Start();
  }

  void Stop()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (--m_ref_count > 0)
      return;

    if (!m_running)
      return;
    m_running = false;
    m_stop = true;
    m_beacon.Stop();
    m_listener.close();
    if (m_thread.joinable())
      m_thread.join();
  }

private:
  void AcceptLoop()
  {
    sf::SocketSelector selector;
    selector.add(m_listener);
    while (!m_stop)
    {
      if (!selector.wait(sf::milliseconds(100)))
        continue;
      auto socket = std::make_shared<sf::TcpSocket>();
      if (m_listener.accept(*socket) != sf::Socket::Status::Done)
        continue;

      // Dispatched to its own thread rather than served inline here: for a
      // WS-upgraded connection, HandleConnection() runs the app handshake,
      // which waits up to APP_HANDSHAKE_TIMEOUT for hello_ack -- handling
      // that inline on the same thread that also accept()s would block
      // every other prospective client for that whole duration. Same fix
      // and reasoning as GBAStreamHost::ServeConnection (see its header
      // comment); threads aren't reaped as they finish, only joined in bulk
      // once this loop exits, for the same reasoning given there.
      std::lock_guard<std::mutex> lock(m_connection_threads_mutex);
      m_connection_threads.emplace_back([this, socket] { HandleConnection(*socket); });
    }

    std::lock_guard<std::mutex> lock(m_connection_threads_mutex);
    for (std::thread& t : m_connection_threads)
    {
      if (t.joinable())
        t.join();
    }
  }

  void HandleConnection(sf::TcpSocket& socket)
  {
    socket.setBlocking(false);
    SetNoDelay(socket);

    const auto request = ReadHttpRequest(socket, m_stop);
    if (!request)
      return;

    if (!IsWebSocketUpgradeRequest(*request))
    {
      // A plain (non-WS-upgrade) GET here is a human opening this URL in a
      // browser out of habit -- there's no page to serve any more (the web
      // client used to be embedded and served from here; it's now
      // finlink/clients/web, a standalone client decoupled from Dolphin the
      // same way Android/3DS/Switch/NDS always were, connecting to
      // whichever finlink server -- Dolphin included -- the user points it
      // at). Just says so, rather than a bare/confusing connection reset.
      static constexpr std::string_view body =
          "This is a finlink server (GBAStreamLobby), not a web page.\n"
          "Connect with a finlink client instead -- e.g. finlink/clients/web "
          "(github.com/alwe2710/finlink), pointed at this host and port.\n";
      std::ostringstream response;
      // std::to_string(), not body.size() streamed directly: an
      // ostringstream formats integers per the process's current locale
      // (Qt sets this from the system locale at startup), which can insert
      // thousands-grouping punctuation into what must be a plain decimal
      // Content-Length -- see GBAStreamHost.cpp's /status body for the
      // identical fix (this body is short enough to normally stay under the
      // ~1000-byte threshold where grouping kicks in, but there's no reason
      // to rely on that). std::to_string() is always locale-independent.
      response << "HTTP/1.1 200 OK\r\n"
               << "Content-Type: text/plain; charset=utf-8\r\n"
               << "Content-Length: " << std::to_string(body.size()) << "\r\n"
               << "Connection: close\r\n\r\n"
               << body;
      const std::string response_str = response.str();
      if (SendAllBytes(socket, response_str.data(), response_str.size(), m_stop))
        CloseGracefully(socket, m_stop);
      return;
    }

    if (!SendWebSocketUpgradeResponse(socket, *request, m_stop))
      return;

    PerformHandshakeAndRedirect(socket, *request, m_stop);
    CloseGracefully(socket, m_stop);
  }

  std::mutex m_mutex;
  int m_ref_count = 0;
  bool m_running = false;
  sf::TcpListener m_listener;
  std::thread m_thread;
  std::atomic_bool m_stop{false};
  GBAStreamBeacon m_beacon;

  // One entry per connection HandleConnection() is currently handling (or
  // has handled) -- see AcceptLoop()'s comment. Joined in full at its tail,
  // after m_stop is set, before Stop()'s own m_thread.join() can return.
  std::mutex m_connection_threads_mutex;
  std::vector<std::thread> m_connection_threads;
};

LobbyServer& GetLobbyServer()
{
  static LobbyServer server;
  return server;
}

}  // namespace

void GBAStreamLobby::AddRef()
{
  GetLobbyServer().Start();
}

void GBAStreamLobby::Release()
{
  GetLobbyServer().Stop();
}

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
