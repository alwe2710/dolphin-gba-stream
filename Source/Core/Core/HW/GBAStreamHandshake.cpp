// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAS_LIBMGBA

#include "Core/HW/GBAStreamHandshake.h"

#include <algorithm>

#include <picojson.h>

#include "Common/JsonUtil.h"

#include "Core/HW/GBAStreamNetUtil.h"

namespace HW::GBA
{
namespace
{
picojson::object VideoToJson(u32 width, u32 height, double fps)
{
  picojson::object obj;
  obj.emplace("width", static_cast<double>(width));
  obj.emplace("height", static_cast<double>(height));
  obj.emplace("fps", fps);
  return obj;
}

picojson::object AudioToJson(const HandshakeAudioInfo& audio)
{
  picojson::object obj;
  obj.emplace("sample_rate", static_cast<double>(audio.sample_rate));
  obj.emplace("channels", static_cast<double>(audio.channels));
  return obj;
}

const char* ErrorCodeToString(HandshakeErrorCode code)
{
  switch (code)
  {
  case HandshakeErrorCode::VersionMismatch:
    return "version_mismatch";
  case HandshakeErrorCode::SlotUnavailable:
    return "slot_unavailable";
  case HandshakeErrorCode::MalformedRequest:
    return "malformed_request";
  }
  return "malformed_request";
}

// Parses payload (raw UTF-8 bytes from a WebSocket text frame) into a
// picojson object. Returns nullopt on any parse error or if the top-level
// value isn't a JSON object -- every message this protocol defines is one.
std::optional<picojson::object> ParseJsonObject(const std::vector<u8>& payload)
{
  picojson::value value;
  const std::string text(payload.begin(), payload.end());
  const std::string error = picojson::parse(value, text);
  if (!error.empty() || !value.is<picojson::object>())
    return std::nullopt;
  return value.get<picojson::object>();
}

}  // namespace

std::string BuildHelloMessage(const HandshakeOffer& offer)
{
  picojson::object obj;
  obj.emplace("message", picojson::value("hello"));
  obj.emplace("protocol_version", static_cast<double>(GBA_STREAM_PROTOCOL_VERSION));
  obj.emplace("stream_type", offer.stream_type);

  picojson::array slots;
  slots.reserve(offer.slot_list.size());
  for (const auto& slot : offer.slot_list)
  {
    picojson::object slot_obj;
    slot_obj.emplace("index", static_cast<double>(slot.index));
    slot_obj.emplace("label", slot.label);
    slot_obj.emplace("occupied", slot.occupied);
    slots.emplace_back(slot_obj);
  }
  obj.emplace("slots", slots);

  obj.emplace("video", VideoToJson(offer.video_width, offer.video_height, offer.video_fps));
  if (offer.audio)
    obj.emplace("audio", AudioToJson(*offer.audio));
  obj.emplace("input_encoding", offer.input_encoding);

  return picojson::value(obj).serialize();
}

std::optional<HandshakeAck> ParseHelloAck(const std::vector<u8>& payload)
{
  const auto obj = ParseJsonObject(payload);
  if (!obj)
    return std::nullopt;

  const auto message = ReadStringFromJson(*obj, "message");
  if (!message || *message != "hello_ack")
    return std::nullopt;

  const auto protocol_version = ReadNumericFromJson<int>(*obj, "protocol_version");
  const auto requested_slot = ReadNumericFromJson<int>(*obj, "requested_slot");
  if (!protocol_version || !requested_slot)
    return std::nullopt;

  const auto video_limits_it = obj->find("video_limits");
  if (video_limits_it == obj->end() || !video_limits_it->second.is<picojson::object>())
    return std::nullopt;
  const auto& video_limits_obj = video_limits_it->second.get<picojson::object>();
  const auto max_width = ReadNumericFromJson<u32>(video_limits_obj, "max_width");
  const auto max_height = ReadNumericFromJson<u32>(video_limits_obj, "max_height");
  const auto max_fps = ReadNumericFromJson<double>(video_limits_obj, "max_fps");
  if (!max_width || !max_height || !max_fps)
    return std::nullopt;

  HandshakeAck ack;
  ack.protocol_version = *protocol_version;
  ack.requested_slot = *requested_slot;
  ack.video_limits = VideoLimits{*max_width, *max_height, *max_fps};

  const auto audio_limits_it = obj->find("audio_limits");
  if (audio_limits_it != obj->end() && audio_limits_it->second.is<picojson::object>())
  {
    const auto& audio_limits_obj = audio_limits_it->second.get<picojson::object>();
    const auto max_sample_rate = ReadNumericFromJson<u32>(audio_limits_obj, "max_sample_rate");
    const auto max_channels = ReadNumericFromJson<u32>(audio_limits_obj, "max_channels");
    if (max_sample_rate && max_channels)
      ack.audio_limits = AudioLimits{*max_sample_rate, static_cast<u8>(*max_channels)};
  }

  const auto video_mode = ReadStringFromJson(*obj, "video_mode");
  if (video_mode)
    ack.video_mode = *video_mode;

  return ack;
}

std::string BuildSessionReadyMessage(int slot, const NegotiatedVideo& video,
                                      const std::optional<NegotiatedAudio>& audio,
                                      const std::optional<HandshakeRedirect>& redirect,
                                      const std::string& video_mode)
{
  picojson::object obj;
  obj.emplace("message", picojson::value("session_ready"));
  obj.emplace("slot", static_cast<double>(slot));
  obj.emplace("video", VideoToJson(video.width, video.height, video.fps));
  if (audio)
  {
    obj.emplace("audio", AudioToJson(HandshakeAudioInfo{audio->sample_rate, audio->channels}));
  }
  if (redirect)
  {
    picojson::object redirect_obj;
    redirect_obj.emplace("host", redirect->host);
    redirect_obj.emplace("port", static_cast<double>(redirect->port));
    obj.emplace("redirect", redirect_obj);
  }
  obj.emplace("video_mode", picojson::value(video_mode));
  return picojson::value(obj).serialize();
}

std::string BuildHandshakeErrorMessage(HandshakeErrorCode code, const std::string& detail)
{
  picojson::object obj;
  obj.emplace("message", picojson::value("handshake_error"));
  obj.emplace("code", std::string(ErrorCodeToString(code)));
  obj.emplace("detail", detail);
  return picojson::value(obj).serialize();
}

NegotiatedVideo NegotiateVideo(u32 native_width, u32 native_height, double native_fps,
                                const VideoLimits& limits)
{
  const double width_ratio =
      limits.max_width >= native_width ? 1.0 : static_cast<double>(limits.max_width) / native_width;
  const double height_ratio = limits.max_height >= native_height ?
                                   1.0 :
                                   static_cast<double>(limits.max_height) / native_height;
  const double scale = std::min({1.0, width_ratio, height_ratio});

  auto round_down_to_multiple_of_8 = [](double value) -> u32 {
    const auto rounded = static_cast<u32>(value) & ~static_cast<u32>(7);
    return std::max<u32>(rounded, 8);
  };

  NegotiatedVideo result;
  result.width = scale >= 1.0 ? native_width : round_down_to_multiple_of_8(native_width * scale);
  result.height = scale >= 1.0 ? native_height : round_down_to_multiple_of_8(native_height * scale);
  result.fps = std::min(native_fps, limits.max_fps > 0 ? limits.max_fps : native_fps);
  return result;
}

NegotiatedAudio NegotiateAudio(u32 native_sample_rate, u8 native_channels,
                                const AudioLimits& limits)
{
  // Sample-rate downsampling isn't implemented (see docs/protocol.md) --
  // reporting anything other than the native rate here would claim a
  // negotiation that didn't actually happen, so the rate always stays
  // native. Channel count is the one axis actually honored: a client that
  // asks for mono gets the existing stereo->mono downmix
  // (GBAStreamHost::SendAudioIfPending).
  NegotiatedAudio result;
  result.sample_rate = native_sample_rate;
  result.channels = std::min(native_channels, std::max<u8>(limits.max_channels, 1));
  return result;
}

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
