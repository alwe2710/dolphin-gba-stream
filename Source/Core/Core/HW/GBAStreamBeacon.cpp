// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAS_LIBMGBA

#include "Core/HW/GBAStreamBeacon.h"

#include <chrono>
#include <string>

#include <picojson.h>

#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/UdpSocket.hpp>

#include "Core/ConfigManager.h"
#include "Core/HW/GBAStreamHandshake.h"
#include "Core/HW/GBAStreamNetUtil.h"

namespace HW::GBA
{
namespace
{
std::string BuildBeaconMessage()
{
  picojson::object obj;
  obj.emplace("type", std::string("finlink_beacon"));
  obj.emplace("protocol_version", static_cast<double>(GBA_STREAM_PROTOCOL_VERSION));
  obj.emplace("emulator_identifier", std::string("Dolphin"));
  // Same source DiscordPresence.cpp / Core.cpp use for "what's currently
  // running" -- see docs/protocol.md's note that this is the GC game (the
  // one actually loaded), not the GBA content on the link-cable side, since
  // that's what a human picking a server from a list wants to recognize.
  obj.emplace("game_title", SConfig::GetInstance().GetTitleDescription());
  obj.emplace("stream_type", std::string(kStreamTypeGcGbaLink));
  const auto local_address = sf::IpAddress::getLocalAddress();
  obj.emplace("host", local_address ? local_address->toString() : std::string());
  obj.emplace("handshake_port", static_cast<double>(GBA_STREAM_LOBBY_PORT));
  return picojson::value(obj).serialize();
}

}  // namespace

GBAStreamBeacon::~GBAStreamBeacon()
{
  Stop();
}

void GBAStreamBeacon::Start()
{
  m_stop = false;
  m_thread = std::thread([this] { Run(); });
}

void GBAStreamBeacon::Stop()
{
  m_stop = true;
  if (m_thread.joinable())
    m_thread.join();
}

void GBAStreamBeacon::Run()
{
  // Own socket for this thread's lifetime; UDP send doesn't need a bound
  // local port (the OS assigns an ephemeral one on first send), and SFML
  // enables SO_BROADCAST by default for every UDP socket it creates.
  sf::UdpSocket socket;
  while (!m_stop)
  {
    const std::string message = BuildBeaconMessage();
    // Best-effort: a dropped/failed broadcast just means this tick's beacon
    // didn't go out, no different from ordinary UDP loss -- the next tick
    // two seconds later covers for it, so the [[nodiscard]] status is
    // deliberately ignored here rather than logged on every failure.
    (void)socket.send(message.data(), message.size(), sf::IpAddress::Broadcast,
                       GBA_STREAM_BEACON_PORT);

    // Polls m_stop every 100ms instead of sleeping the full interval in one
    // call, so Stop() (called when the last GC port configured as GBA
    // (Client-Stream) goes away) doesn't have to wait out an in-progress
    // interval.
    for (auto waited = std::chrono::milliseconds::zero();
         waited < GBA_STREAM_BEACON_INTERVAL && !m_stop;
         waited += std::chrono::milliseconds(100))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
