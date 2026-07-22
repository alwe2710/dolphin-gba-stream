// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef HAS_LIBMGBA

namespace HW::GBA
{
// Reference-counted, process-wide server for the P1-P4 lobby page, always
// listening on a single fixed port (6800) for as long as at least one GC
// port is configured as GBA (Client-Stream) -- independent of *which*
// port(s) those are, so the lobby URL stays stable even if e.g. only GC
// port 3 is actually streaming. Player ports (6801-6804) are each a separate
// GBAStreamHost/WebSocket server. Every GBAStreamHost calls AddRef()/Release()
// around its own lifetime; the underlying listener thread is started on the
// first reference and stopped on the last.
class GBAStreamLobby
{
public:
  static void AddRef();
  static void Release();
};

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
