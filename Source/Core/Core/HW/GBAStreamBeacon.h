// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef HAS_LIBMGBA

#include <atomic>
#include <string>
#include <thread>

namespace HW::GBA
{
// UDP discovery beacon (Unison docs/protocol.md, "Discovery-Beacon (UDP)"):
// broadcasts protocol_version/emulator_identifier/game_title/host/handshake_port
// on GBA_STREAM_BEACON_PORT every GBA_STREAM_BEACON_INTERVAL, for as long as
// GBAStreamLobby is running (i.e. at least one GC port is configured as GBA
// (Client-Stream)). One instance, owned by GBAStreamLobby's internal
// LobbyServer -- entirely independent of which/how many player ports are
// actually occupied, same as the lobby itself.
class GBAStreamBeacon
{
public:
  ~GBAStreamBeacon();

  void Start();
  void Stop();

private:
  void Run();

  std::atomic_bool m_stop{false};
  std::thread m_thread;
  // Probed once per Start(), not once per beacon tick -- see Run()'s comment
  // on BuildBeaconMessage() for why a per-tick reprobe is actively harmful
  // to discovery clients, not just wasted work.
  std::string m_local_host;
};

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
