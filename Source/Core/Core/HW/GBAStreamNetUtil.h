// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef HAS_LIBMGBA

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

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

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
