// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef HAS_LIBMGBA

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace HW::GBA
{
// App-level handshake (hello / hello_ack / session_ready / handshake_error),
// exchanged as WebSocket text frames before any Video/Audio/Input binary
// frame, per Unison's docs/protocol.md ("Verbindungsaufbau: Handshake").
// Shared between GBAStreamLobby (port 6800, always the first hop, sends the
// redirect for multi-slot stream types) and GBAStreamHost (player ports,
// 6801-6804, the terminal hop for GC_GBA_LINK). Pure message (de)serialization
// and negotiation math -- no socket I/O, mirroring how Unison's own core/
// separates protocol logic from transport.

constexpr char kStreamTypeGcGbaLink[] = "GC_GBA_LINK";

struct HandshakeSlot
{
  int index;
  std::string label;
  bool occupied;
};

struct HandshakeAudioInfo
{
  u32 sample_rate;
  u8 channels;
};

// What the server advertises in `hello`. `audio` is nullopt for stream types
// without audio -- not applicable to GC_GBA_LINK, which always has it, but
// the type stays shared so a future N3DS_BOTTOM_SCREEN implementation reuses
// it verbatim.
struct HandshakeOffer
{
  std::string stream_type;
  // Named slot_list, not "slots" -- Qt's <QObject> headers #define slots as a
  // macro (Q_SLOTS), which silently mangles a field of that exact name into
  // a syntax error the moment any translation unit that includes this header
  // also includes Qt (e.g. DolphinQt/MainWindow.cpp). The wire field is still
  // "slots" (see BuildHelloMessage) -- that's a JSON string literal, entirely
  // unaffected by C++ identifier macros.
  std::vector<HandshakeSlot> slot_list;
  u32 video_width;
  u32 video_height;
  double video_fps;
  std::optional<HandshakeAudioInfo> audio;
  std::string input_encoding;
};

struct VideoLimits
{
  u32 max_width;
  u32 max_height;
  double max_fps;
};

struct AudioLimits
{
  u32 max_sample_rate;
  u8 max_channels;
};

// What the client sends back in `hello_ack`. video_mode (Unison's
// protocol.md "tiles"/"legacy"/"h264"/"h265", empty if unset/unrecognized):
// "h264"/"h265" get a real SoftwareVideoEncoder (see GBAStreamHost.cpp's
// SendVideoFrameIfPending); anything else falls back to this stream type's
// existing tile-vs-raw choice (`use_tiles`, a per-frame adaptive heuristic
// that was never driven by this field and still isn't) -- there's no
// "legacy" mode here, this stream type has always been at least
// tiles-capable, unlike azahar/melonDS's plain-raw-frame-only fallback.
struct HandshakeAck
{
  int protocol_version;
  int requested_slot;
  VideoLimits video_limits;
  std::optional<AudioLimits> audio_limits;
  std::string video_mode;
};

struct NegotiatedVideo
{
  u32 width;
  u32 height;
  double fps;
};

struct NegotiatedAudio
{
  u32 sample_rate;
  u8 channels;
};

struct HandshakeRedirect
{
  std::string host;
  u16 port;
};

enum class HandshakeErrorCode
{
  VersionMismatch,
  SlotUnavailable,
  MalformedRequest,
};

// Serializes a `hello` message body (the JSON text frame payload -- callers
// send it via SendWebSocketTextFrame, this doesn't touch the socket).
std::string BuildHelloMessage(const HandshakeOffer& offer);

// Parses a `hello_ack` text frame payload. Returns nullopt if the JSON is
// malformed or missing required fields -- callers should treat that as
// HandshakeErrorCode::MalformedRequest.
std::optional<HandshakeAck> ParseHelloAck(const std::vector<u8>& payload);

// video_mode: GBAStreamLobby.cpp's redirect-hop placeholder reply always
// passes the fixed "tiles" (it never actually streams video, see its own
// call site) -- GBAStreamHost.cpp's real player-port session now passes
// whatever PerformAppHandshake() decided ("h264"/"h265" if requested and
// SoftwareVideoEncoder construction is attempted, "tiles" otherwise; see
// SendVideoFrameIfPending()'s own h264/h265 branch and HandshakeAck::
// video_mode's own comment for how that decision is made). Either way this
// function itself doesn't decide anything, it only reports what the caller
// already chose.
std::string BuildSessionReadyMessage(int slot, const NegotiatedVideo& video,
                                      const std::optional<NegotiatedAudio>& audio,
                                      const std::optional<HandshakeRedirect>& redirect,
                                      const std::string& video_mode);

std::string BuildHandshakeErrorMessage(HandshakeErrorCode code, const std::string& detail);

// Native-priority downscaling: the native stream is used as-is whenever the
// client's limits already cover it; otherwise both dimensions are scaled down
// together (by the more restrictive of the two ratios) and rounded down to
// the nearest multiple of 8 -- matching TILE_SIZE in GBAStreamHost.cpp's
// existing (untouched) tile-diff video encoding, which relies on width/height
// dividing evenly by 8. Never scales up, and never returns less than 8x8.
NegotiatedVideo NegotiateVideo(u32 native_width, u32 native_height, double native_fps,
                                const VideoLimits& limits);

// Native-priority audio negotiation. Downsampling the actual sample rate is
// not implemented (see docs/protocol.md's known limitations) -- if the
// client's max_sample_rate is below native, audio is left at native anyway
// rather than silently claiming a rate that isn't actually produced;
// channel count *is* honored (stereo can be downmixed to mono by the caller).
NegotiatedAudio NegotiateAudio(u32 native_sample_rate, u8 native_channels,
                                const AudioLimits& limits);

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
