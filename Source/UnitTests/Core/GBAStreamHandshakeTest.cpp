// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Core/HW/GBAStreamHandshake.{h,cpp} had zero test coverage before this
// file, despite being exactly where hello_ack.video_mode gets parsed and
// session_ready.video_mode gets reported -- the two fields the "Video-mode
// fallback" negotiation feature (unison/docs/protocol.md) actually runs
// on. Verified via picojson (this file's own JSON library, already linked
// into `core` -- see GBAStreamHandshake.cpp's own includes), not
// unison_core: unlike Cemu/melonDS/azahar, this fork's WebSocket/JSON
// handling predates unison_core's extraction as a shared library and was
// never migrated onto it (Externals/unison is checked out but genuinely
// unused anywhere in Source/, a separate, already-flagged migration this
// test doesn't attempt as a side effect).

#ifdef HAS_LIBMGBA

#include <gtest/gtest.h>

#include <picojson.h>

#include "Core/HW/GBAStreamHandshake.h"
#include "Core/HW/GBAStreamNetUtil.h"

namespace HW::GBA
{
namespace
{
picojson::object ParseObject(const std::string& json)
{
  picojson::value value;
  const std::string error = picojson::parse(value, json);
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_TRUE(value.is<picojson::object>());
  return value.get<picojson::object>();
}

std::vector<u8> ToBytes(const std::string& s)
{
  return std::vector<u8>(s.begin(), s.end());
}
}  // namespace

TEST(GBAStreamHandshake, BuildHelloMessage)
{
  HandshakeOffer offer;
  offer.stream_type = kStreamTypeGcGbaLink;
  offer.slot_list = {{0, "P1", false}, {1, "P2", true}};
  offer.video_width = 240;
  offer.video_height = 160;
  offer.video_fps = 59.7275;
  offer.audio = HandshakeAudioInfo{32768, 2};
  offer.input_encoding = "gba_buttons";

  const auto obj = ParseObject(BuildHelloMessage(offer));
  EXPECT_EQ(obj.at("message").to_str(), "hello");
  EXPECT_EQ(obj.at("protocol_version").get<double>(), GBA_STREAM_PROTOCOL_VERSION);
  EXPECT_EQ(obj.at("stream_type").to_str(), kStreamTypeGcGbaLink);
  ASSERT_TRUE(obj.at("slots").is<picojson::array>());
  EXPECT_EQ(obj.at("slots").get<picojson::array>().size(), 2u);
  EXPECT_EQ(obj.at("video").get<picojson::object>().at("width").get<double>(), 240.0);
  EXPECT_EQ(obj.at("audio").get<picojson::object>().at("channels").get<double>(), 2.0);
  EXPECT_EQ(obj.at("input_encoding").to_str(), "gba_buttons");
}

TEST(GBAStreamHandshake, BuildHelloMessageOmitsAudioWhenAbsent)
{
  // offer.audio left as std::nullopt (its default) -- e.g. a stream type
  // without audio would never set it. Confirms the "audio" member is
  // actually skipped, not written as a null/zeroed placeholder.
  HandshakeOffer offer;
  offer.stream_type = "SOME_AUDIOLESS_TYPE";
  offer.video_width = 320;
  offer.video_height = 240;
  offer.video_fps = 60.0;
  offer.input_encoding = "touch_and_buttons";

  const auto obj = ParseObject(BuildHelloMessage(offer));
  EXPECT_EQ(obj.count("audio"), 0u);
}

TEST(GBAStreamHandshake, ParseHelloAckVideoMode)
{
  // No video_mode field at all -- must stay empty (HandshakeAck::video_mode
  // is only ever meant to carry "what was actually requested, if
  // anything" through to the honest fallback report).
  const auto no_mode = ParseHelloAck(ToBytes(
      R"({"message":"hello_ack","protocol_version":2,"requested_slot":0,)"
      R"("video_limits":{"max_width":240,"max_height":160,"max_fps":60}})"));
  ASSERT_TRUE(no_mode.has_value());
  EXPECT_EQ(no_mode->protocol_version, 2);
  EXPECT_EQ(no_mode->requested_slot, 0);
  EXPECT_TRUE(no_mode->video_mode.empty());

  // This stream type's tile-vs-raw choice is a per-frame adaptive
  // heuristic (SendVideoFrameIfPending's use_tiles), never driven by this
  // value -- ParseHelloAck() only carries it through for the fallback
  // report, so (unlike Cemu's stricter whitelist) it accepts and stores
  // whatever string was actually sent, verbatim.
  for (const std::string mode : {"tiles", "legacy", "h264", "h265", "vp9"})
  {
    const std::string json =
        R"({"message":"hello_ack","protocol_version":2,"requested_slot":0,)"
        R"("video_limits":{"max_width":240,"max_height":160,"max_fps":60},)"
        R"("video_mode":")" +
        mode + "\"}";
    const auto ack = ParseHelloAck(ToBytes(json));
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->video_mode, mode);
  }
}

TEST(GBAStreamHandshake, ParseHelloAckAudioLimitsOptional)
{
  // audio_limits is genuinely optional (a client that doesn't want audio
  // omits it) -- must come back nullopt, not a zeroed struct.
  const auto without_audio = ParseHelloAck(ToBytes(
      R"({"message":"hello_ack","protocol_version":2,"requested_slot":0,)"
      R"("video_limits":{"max_width":240,"max_height":160,"max_fps":60}})"));
  ASSERT_TRUE(without_audio.has_value());
  EXPECT_FALSE(without_audio->audio_limits.has_value());

  const auto with_audio = ParseHelloAck(ToBytes(
      R"({"message":"hello_ack","protocol_version":2,"requested_slot":0,)"
      R"("video_limits":{"max_width":240,"max_height":160,"max_fps":60},)"
      R"("audio_limits":{"max_sample_rate":48000,"max_channels":2}})"));
  ASSERT_TRUE(with_audio.has_value());
  ASSERT_TRUE(with_audio->audio_limits.has_value());
  EXPECT_EQ(with_audio->audio_limits->max_sample_rate, 48000u);
  EXPECT_EQ(with_audio->audio_limits->max_channels, 2);
}

TEST(GBAStreamHandshake, ParseHelloAckRejectsMalformed)
{
  EXPECT_FALSE(ParseHelloAck({}).has_value());
  EXPECT_FALSE(ParseHelloAck(ToBytes(R"({"message":"session_ready"})")).has_value());
  // video_limits is required -- absent entirely must fail, not default to
  // some placeholder size.
  EXPECT_FALSE(ParseHelloAck(ToBytes(
                                 R"({"message":"hello_ack","protocol_version":2,"requested_slot":0})"))
                   .has_value());
}

TEST(GBAStreamHandshake, BuildSessionReadyMessageVideoMode)
{
  // Both real call sites (GBAStreamHost.cpp's terminal session,
  // GBAStreamLobby.cpp's redirect placeholder) always pass "tiles" --
  // confirm the parameter actually flows through to the JSON for any
  // value, not hardcoded past this function's own boundary.
  for (const std::string mode : {"tiles", "legacy", "h264", "h265"})
  {
    const auto obj = ParseObject(
        BuildSessionReadyMessage(0, NegotiatedVideo{240, 160, 59.7275}, std::nullopt, std::nullopt, mode));
    EXPECT_EQ(obj.at("message").to_str(), "session_ready");
    EXPECT_EQ(obj.at("video_mode").to_str(), mode);
    EXPECT_EQ(obj.at("video").get<picojson::object>().at("width").get<double>(), 240.0);
    EXPECT_EQ(obj.count("audio"), 0u);
    EXPECT_EQ(obj.count("redirect"), 0u);
  }
}

TEST(GBAStreamHandshake, BuildSessionReadyMessageWithAudioAndRedirect)
{
  const auto obj = ParseObject(BuildSessionReadyMessage(
      2, NegotiatedVideo{240, 160, 59.7275}, NegotiatedAudio{32768, 1},
      HandshakeRedirect{"192.168.1.42", 6803}, "tiles"));
  EXPECT_EQ(obj.at("slot").get<double>(), 2.0);
  ASSERT_EQ(obj.count("audio"), 1u);
  EXPECT_EQ(obj.at("audio").get<picojson::object>().at("channels").get<double>(), 1.0);
  ASSERT_EQ(obj.count("redirect"), 1u);
  const auto& redirect_obj = obj.at("redirect").get<picojson::object>();
  EXPECT_EQ(redirect_obj.at("host").to_str(), "192.168.1.42");
  EXPECT_EQ(redirect_obj.at("port").get<double>(), 6803.0);
}

TEST(GBAStreamHandshake, BuildHandshakeErrorMessage)
{
  const auto obj = ParseObject(
      BuildHandshakeErrorMessage(HandshakeErrorCode::SlotUnavailable, "Slot P2 was taken."));
  EXPECT_EQ(obj.at("message").to_str(), "handshake_error");
  EXPECT_EQ(obj.at("code").to_str(), "slot_unavailable");
  EXPECT_EQ(obj.at("detail").to_str(), "Slot P2 was taken.");
}

TEST(GBAStreamHandshake, NegotiateVideoPrefersNativeWhenWithinLimits)
{
  const auto result = NegotiateVideo(240, 160, 59.7275, VideoLimits{320, 240, 60.0});
  EXPECT_EQ(result.width, 240u);
  EXPECT_EQ(result.height, 160u);
}

TEST(GBAStreamHandshake, NegotiateVideoDownscalesToMultipleOf8)
{
  // Client only accepts up to half native width -- both dimensions scale
  // together, rounded down to a multiple of 8 (the tile size
  // GBAStreamHost.cpp's own tile-diff encoding relies on).
  const auto result = NegotiateVideo(240, 160, 59.7275, VideoLimits{120, 240, 60.0});
  EXPECT_LE(result.width, 120u);
  EXPECT_EQ(result.width % 8, 0u);
  EXPECT_EQ(result.height % 8, 0u);
  EXPECT_GE(result.width, 8u);
  EXPECT_GE(result.height, 8u);
}

TEST(GBAStreamHandshake, NegotiateAudioNeverDownsamplesRate)
{
  // Sample-rate downsampling isn't implemented (see NegotiateAudio's own
  // comment) -- the rate must always stay native, regardless of a lower
  // max_sample_rate.
  const auto result = NegotiateAudio(32768, 2, AudioLimits{8000, 2});
  EXPECT_EQ(result.sample_rate, 32768u);
  EXPECT_EQ(result.channels, 2);
}

TEST(GBAStreamHandshake, NegotiateAudioHonorsChannelCount)
{
  const auto mono = NegotiateAudio(32768, 2, AudioLimits{32768, 1});
  EXPECT_EQ(mono.channels, 1);
}

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
