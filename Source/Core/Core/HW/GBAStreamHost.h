// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef HAS_LIBMGBA

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <SFML/Network/TcpListener.hpp>
#include <SFML/Network/TcpSocket.hpp>

#include "Common/CommonTypes.h"
#include "Core/Host.h"

namespace HW::GBA
{
// GBAHostInterface implementation backing the "GBA (Client-Stream)" SI device.
//
// Serves the integrated GBA's video output to a single browser client over a
// small custom protocol (one WebSocket connection per GC port, video frames
// server->client, button state client->server), and feeds received button
// presses into the GBA pad via ControllerEmu's input-override mechanism (the
// same mechanism the GBA TAS input dialog uses). See GBAStreamHost.cpp for the
// wire format. This class never touches VideoBackends or GBAWidget.
class GBAStreamHost final : public GBAHostInterface
{
public:
  explicit GBAStreamHost(int device_number);
  GBAStreamHost(const GBAStreamHost&) = delete;
  GBAStreamHost& operator=(const GBAStreamHost&) = delete;
  ~GBAStreamHost() override;

  void GameChanged() override;
  void FrameEnded(std::span<const u32> video_buffer) override;
  void AudioRateChanged(u32 sample_rate) override;
  bool ForwardAudioSamples(std::span<const s16> samples, u32 channels) override;

private:
  void AcceptLoop();
  void ServeConnection(sf::TcpSocket& socket);
  bool PerformHandshake(sf::TcpSocket& socket, bool* is_websocket);
  void RunWebSocketSession(sf::TcpSocket& socket);
  void SendVideoFrameIfPending(sf::TcpSocket& socket, u64* last_sent_frame_id,
                               std::vector<u8>* previous_rgb565);
  void SendAudioIfPending(sf::TcpSocket& socket);

  void AttachInputOverride();
  void DetachInputOverride();

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
};

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
