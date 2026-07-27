// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef HAS_LIBMGBA

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <SFML/Network/TcpListener.hpp>
#include <SFML/Network/TcpSocket.hpp>

#include "Common/CommonTypes.h"
#include "Core/HW/GBAStreamHandshake.h"
#include "Core/HW/GBAStreamNetUtil.h"
#include "Core/Host.h"

namespace HW::GBA
{
// GBAHostInterface implementation backing the "GBA (Client-Stream)" SI device.
//
// One instance per GC port configured as GBA (Client-Stream), each listening
// on its own player port (6801-6804) for a single browser client: video
// frames and audio go server->client, button state comes back client->server,
// all over one small custom WebSocket-based protocol (see GBAStreamHost.cpp
// for the wire format). Received input is fed into the GBA pad via
// ControllerEmu's input-override mechanism (the same mechanism the GBA TAS
// input dialog uses). The human-facing P1-P4 picker page itself is served
// separately by GBAStreamLobby on a fixed port (6800); this class only
// answers a tiny /status JSON query for it. This class never touches
// VideoBackends or GBAWidget.
class GBAStreamHost final : public GBAHostInterface
{
public:
  explicit GBAStreamHost(int device_number);
  GBAStreamHost(const GBAStreamHost&) = delete;
  GBAStreamHost& operator=(const GBAStreamHost&) = delete;
  ~GBAStreamHost() override;

  // Pre-flight check for the frontend to call before booting: for every GC
  // port currently configured as GBA (Client-Stream), probes whether its
  // player port (and the lobby port, if any such port exists at all) can
  // actually be bound, without constructing any real server yet. Returns the
  // list of ports that are already taken by something else (empty if
  // everything the boot will need is free). Exists so a busy port produces a
  // clear "can't start" message instead of a GBA slot silently booting with
  // no working stream.
  static std::vector<int> CheckPortsInUse();

  // Whether the GC port `device_number` (0-3) currently has a client
  // attached -- used by GBAStreamLobby to build the `slots` list in its own
  // `hello` (see GBAStreamHandshake.h) without reaching into this class's
  // streaming internals. false for a device_number with no GBAStreamHost
  // instance at all (not configured as GBA (Client-Stream)), same as an
  // unoccupied one -- callers that need to tell the two apart already know
  // which device numbers are configured, the same way CheckPortsInUse()'s
  // caller does.
  static bool IsSlotOccupied(int device_number);
  // "P1".."P4", matching the existing picker page's labels.
  static std::string GetSlotLabel(int device_number);
  // Current sample_rate/channels for `device_number`'s instance, or the
  // m_audio_sample_rate/m_audio_channels default initializers below if no
  // instance is registered for it. Used by GBAStreamLobby's own `hello`,
  // which -- unlike GBAStreamHost's -- is sent before any particular slot is
  // chosen, so it reports whichever configured slot answers first rather
  // than one specific slot's value (in practice identical across all four,
  // since they're all the same GBA core type).
  static HandshakeAudioInfo GetNativeAudioInfo(int device_number);

  void GameChanged() override;
  void FrameEnded(std::span<const u32> video_buffer) override;
  void AudioRateChanged(u32 sample_rate) override;
  bool ForwardAudioSamples(std::span<const s16> samples, u32 channels) override;

private:
  void AcceptLoop();
  void ServeConnection(sf::TcpSocket& socket);
  bool PerformHandshake(sf::TcpSocket& socket, bool* is_websocket);
  // Runs the app-level handshake (GBAStreamHandshake.h) on an already
  // WebSocket-upgraded `socket`: sends `hello`, waits for `hello_ack`,
  // rejects on version mismatch / wrong or already-occupied slot / malformed
  // reply, otherwise negotiates and replies `session_ready`. On success,
  // writes the negotiated parameters into m_negotiated_* (read by
  // SendVideoFrameIfPending/SendAudioIfPending) before returning true; the
  // caller (ServeConnection) only proceeds to RunWebSocketSession on true.
  bool PerformAppHandshake(sf::TcpSocket& socket);
  void RunWebSocketSession(sf::TcpSocket& socket);
  void SendVideoFrameIfPending(sf::TcpSocket& socket, u64* last_sent_frame_id,
                               std::vector<u8>* previous_rgb565);
  void SendAudioIfPending(sf::TcpSocket& socket);

  void AttachInputOverride();
  void DetachInputOverride();

  static std::mutex s_registry_mutex;
  // Guarded by s_registry_mutex; index = device_number. Registered/
  // unregistered around this object's lifetime (constructor/destructor) so
  // IsSlotOccupied() can never observe a partially-destroyed instance --
  // the destructor unregisters under the same mutex before tearing down
  // anything else, and IsSlotOccupied() holds it for the whole lookup+read.
  static std::array<GBAStreamHost*, 4> s_instances;

  const int m_device_number;

  sf::TcpListener m_listener;
  std::thread m_accept_thread;
  std::atomic_bool m_stop{false};

  // Latest raw GBA framebuffer handed off from the GBA core thread (FrameEnded)
  // to the connection-serving thread. Single slot by design: real-time video
  // favors showing the newest frame over queueing stale ones.
  std::mutex m_frame_mutex;
  std::condition_variable m_frame_cv;
  std::vector<u32> m_pending_frame;
  u32 m_frame_width = 0;
  u32 m_frame_height = 0;
  u64 m_frame_id = 0;

  // Current remote button state, written by the WS session thread as input
  // messages arrive, read by the input-override lambda (invoked on the CPU/
  // emulation thread via GBAPad::GetInput()). Bit layout documented in the .cpp.
  std::atomic<u16> m_remote_keys{0};
  std::atomic_bool m_client_connected{false};

  // PCM audio handed off from the GBA core thread (ForwardAudioSamples) to the
  // connection-serving thread. Unlike video this is a queue, not a single
  // slot: dropping stale audio would be audible as glitches, so every sample
  // must eventually be sent (bounded by RunWebSocketSession's ~4ms poll,
  // comfortably faster than mGBA's audio buffer callback interval).
  std::mutex m_audio_mutex;
  std::vector<s16> m_pending_audio;
  u32 m_audio_channels = 2;
  std::atomic<u32> m_audio_sample_rate{32768};

  // Result of the current session's app-level handshake, written by
  // PerformAppHandshake right before ServeConnection calls RunWebSocketSession
  // (never during it), read by SendVideoFrameIfPending/SendAudioIfPending to
  // decide whether to downscale before running the existing (untouched)
  // encode pipeline. Default-initialized to native/enabled here purely as a
  // safe fallback; every real session's values come from that handshake, not
  // from these initializers, since RunWebSocketSession is unreachable without
  // a successful one first.
  u32 m_negotiated_width = GBA_NATIVE_WIDTH;
  u32 m_negotiated_height = GBA_NATIVE_HEIGHT;
  double m_negotiated_fps = GBA_NATIVE_FPS;
  bool m_audio_enabled = true;
  u8 m_negotiated_audio_channels = 2;
  std::chrono::steady_clock::time_point m_last_video_send_time{};
};

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
