// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef HAS_LIBMGBA

#include <array>
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

// Makes this socket's eventual close send an immediate RST instead of a
// graceful FIN (SO_LINGER with a zero timeout), which skips TCP's TIME_WAIT
// teardown entirely instead of leaving it to the usual ~60s. Only safe to
// call once nothing more is going to be sent on this socket AND either the
// peer has already seen everything we wrote or we no longer care --
// otherwise data still sitting in the kernel's send buffer at the moment of
// close can be discarded, truncating whatever we just sent. Use
// CloseGracefully() below rather than calling this directly.
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

// Called right before a socket goes out of scope, once we're done sending on
// it. Prefers to let the *peer* send the first FIN -- once they've read
// everything we wrote, their browser/OS closes its end, which makes them the
// TCP "active closer" and puts TIME_WAIT on their side, not ours, with zero
// risk of truncating data we just sent. Only if the peer doesn't close
// within a short, generous grace period (they may just be slow, or gone
// without a trace) do we fall back to SetAbortiveClose() -- by then anything
// we wrote has long since either been delivered or given up on, so an
// abortive close is no longer a truncation risk, just a bounded worst case
// so this never hangs shutdown.
//
// This -- not an unconditional SetAbortiveClose() at accept time -- is what
// actually fixed a port intermittently refusing to rebind after a quick
// Dolphin restart: that bug came from *us* always being the active closer
// (we write a one-shot HTTP response, then immediately destruct the
// socket), which is exactly the scenario SetAbortiveClose() alone isn't
// safe to use for.
inline void CloseGracefully(sf::TcpSocket& socket, const std::atomic_bool& stop_flag)
{
  socket.setBlocking(false);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  std::array<char, 256> buf{};
  while (!stop_flag && std::chrono::steady_clock::now() < deadline)
  {
    size_t received = 0;
    const auto status = socket.receive(buf.data(), buf.size(), received);
    if (status == sf::Socket::Status::Disconnected)
      return;  // Peer closed first -- nothing more to do, no TIME_WAIT for us.
    if (status != sf::Socket::Status::NotReady)
      return;  // Error, or unexpectedly received more data -- give up cleanly.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  SetAbortiveClose(socket);
}

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
