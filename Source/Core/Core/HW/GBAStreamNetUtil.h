// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef HAS_LIBMGBA

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include <SFML/Network/TcpSocket.hpp>

#include "Common/CommonTypes.h"

namespace HW::GBA
{
// Fixed ports for the GBA (Client-Stream) feature: one always-on lobby
// (GBAStreamLobby) plus one player port per GC port configured as GBA
// (Client-Stream) (GBAStreamHost, device_number 0-3 -> base port +
// device_number). Shared so GBAStreamHost::CheckPortsInUse() can pre-flight
// exactly the same addresses the real servers will later try to bind.
constexpr unsigned short kGBAStreamLobbyPort = 6800;
constexpr unsigned short kGBAStreamPlayerBasePort = 6801;

// Sends `size` bytes on a non-blocking socket, retrying on NotReady. Bounds
// every wait to a short sleep so a stalled/frozen peer (e.g. a crashed
// browser tab that stops draining its receive buffer) can never block this
// thread forever -- `stop_flag` is checked on every retry so shutdown always
// completes promptly regardless of what the remote end is doing. Shared by
// GBAStreamHost and GBAStreamLobby.
inline bool SendAllBytes(sf::TcpSocket& socket, const void* data, size_t size,
                         const std::atomic_bool& stop_flag)
{
  const auto* bytes = static_cast<const u8*>(data);
  size_t sent_total = 0;
  while (sent_total < size)
  {
    if (stop_flag)
      return false;
    size_t sent = 0;
    const auto status = socket.send(bytes + sent_total, size - sent_total, sent);
    if (status == sf::Socket::Status::Done || status == sf::Socket::Status::Partial)
    {
      sent_total += sent;
      continue;
    }
    if (status == sf::Socket::Status::NotReady)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    return false;
  }
  return true;
}

namespace detail
{
// sf::Socket::getNativeHandle() is protected; this adds no members/behavior
// of its own, just re-exposes it, so casting an existing sf::TcpSocket
// reference to this type is safe (same object layout, no vtable changes).
class TcpSocketHandleAccessor : public sf::TcpSocket
{
public:
  static sf::SocketHandle Get(sf::TcpSocket& socket)
  {
    return static_cast<TcpSocketHandleAccessor&>(socket).getNativeHandle();
  }
};
}  // namespace detail

// Skips TCP's normal TIME_WAIT teardown for this connection by making its
// eventual close send an immediate RST instead of a graceful FIN (SO_LINGER
// with a zero timeout). Without this, once any client has connected, the
// accepted connection's TIME_WAIT state (up to ~60s on Linux) blocks a new
// listen() on this exact port even though nothing shows as LISTENing in
// netstat/ss in the meantime, which otherwise made "stop, then immediately
// restart" intermittently fail with a port that looks free but isn't yet.
// Confirmed empirically that SO_REUSEADDR alone does *not* bypass this on
// Linux; SO_LINGER's abortive close avoids TIME_WAIT from ever occurring at
// all. Safe here: by the time any of our connections are torn down there's
// nothing left worth delivering reliably -- either the peer already
// disconnected, or Dolphin/the SI device is shutting down.
inline void SetAbortiveClose(sf::TcpSocket& socket)
{
  const sf::SocketHandle handle = detail::TcpSocketHandleAccessor::Get(socket);
#ifdef _WIN32
  if (handle == INVALID_SOCKET)
    return;
#else
  if (handle < 0)
    return;
#endif
  linger lg{};
  lg.l_onoff = 1;
  lg.l_linger = 0;
#ifdef _WIN32
  setsockopt(handle, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
#else
  setsockopt(handle, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
#endif
}

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
