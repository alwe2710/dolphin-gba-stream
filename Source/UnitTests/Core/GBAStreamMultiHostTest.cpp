// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Real integration tests for the "multi-host" test category (finlink's
// test-categorization project notes): does connecting multiple simultaneous
// clients to different GBAStreamHost slots actually work, and does the
// slot-occupancy state GBAStreamLobby relies on (IsSlotOccupied/
// GetSlotLabel) correctly reflect real, live connections?
//
// Unlike the modern-host mic/touch passthrough tests (deferred to a
// separate ROM-boot initiative, see finlink's own memory notes),
// GBAStreamHost's constructor takes only a device_number -- no GBA core
// reference at all (confirmed by reading GBAStreamHost.cpp: no m_core/
// mCore member anywhere) -- so real GBAStreamHost instances, real TCP
// sockets, and a real WebSocket + app-level handshake can all run here
// without booting any ROM. The `tests` binary already links `core` (which
// GBAStreamHost.cpp/GBAStreamLobby.cpp are part of, see
// UnitTests/CMakeLists.txt), so no new linking was needed for that part.
//
// The test client's own WS/handshake handling deliberately goes through
// finlink_core (Externals/finlink/core) rather than hand-rolling another
// copy of the WebSocket/JSON logic GBAStreamHandshake.cpp already has --
// this doubles as a real cross-check that GBAStreamHost's hand-rolled
// picojson-based protocol implementation actually produces/accepts what
// the documented finlink wire protocol (docs/protocol.md) says, the same
// spirit as the "generell: Protokoll-Implementierung einheitlich" test
// category, as a side effect of just needing *a* client.
//
// A note for anyone re-running this binary repeatedly by hand while
// iterating locally (not a concern for real CI, see below): SFML's
// sf::TcpListener doesn't set SO_REUSEADDR, so a port these tests just
// used stays in the kernel's TCP TIME_WAIT state for a while (~60s on
// Linux) after the process exits -- re-running the binary again
// immediately can make GBAStreamHost's own m_listener.listen() fail for
// that reason alone (logged as "Failed to bind listener socket to port
// N"), which surfaces here as PerformClientHandshake() returning nullopt,
// not as a real logic failure. Confirmed by hand: waiting out that window
// (or polling until the ports are actually bindable again) makes a
// previously-"failing" re-run pass cleanly with zero code changes. Doesn't
// affect a real CI run, which never has a prior invocation's ports still
// draining -- only back-to-back manual re-runs on the same machine.

#ifdef HAS_LIBMGBA

#include <gtest/gtest.h>

#include <SFML/Network/TcpSocket.hpp>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/ConfigManager.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GBAStreamHost.h"
#include "Core/HW/GBAStreamNetUtil.h"
#include "UICommon/UICommon.h"

extern "C"
{
#include <finlink/handshake.h>
#include <finlink/websocket.h>
}

namespace HW::GBA
{
namespace
{
// GBAStreamHost's constructor unconditionally starts GBAStreamLobby (via
// AddRef()), whose Beacon thread calls SConfig::GetInstance() to build its
// UDP announcement -- SConfig::Init() (and Config::Init() underneath it)
// is required before constructing any GBAStreamHost, or that thread
// segfaults dereferencing SConfig's never-initialized singleton pointer
// (confirmed the hard way: a first version of this test crashed inside
// GBAStreamBeacon::Run() -> SConfig::GetTitleDescription() without this).
// Same pattern CoreTimingTest.cpp/PageTableHostMappingTest.cpp already use
// for their own SConfig-touching code, not improvised here. A temp
// UserDirectory keeps this from touching (or depending on) the real
// developer/CI machine's actual Dolphin config.
//
// Pad::InitializeGBA() is the second, separately-discovered dependency:
// GBAStreamHost::AttachInputOverride() (entered once a session's app
// handshake succeeds) calls Pad::GetGBAConfig()->GetController(device_number),
// which throws std::out_of_range if the 4 GBA pad slots were never created
// (again confirmed the hard way -- a second crash, this time a std::terminate
// from InputConfig::GetController(), without this). Unlike the modern-host
// mic/touch passthrough tests (deferred to a separate ROM-boot initiative,
// see finlink's own memory notes), this is real, lightweight, self-contained
// controller-config bring-up -- no GBA core, no ROM, no GPU renderer needed,
// same reasoning that made this whole test feasible in the first place.
class ScopedDolphinConfig
{
public:
  ScopedDolphinConfig() : m_profile_path(File::CreateTempDir())
  {
    UICommon::SetUserDirectory(m_profile_path);
    Config::Init();
    SConfig::Init();
    Pad::InitializeGBA();
  }
  ~ScopedDolphinConfig()
  {
    Pad::ShutdownGBA();
    SConfig::Shutdown();
    Config::Shutdown();
    File::DeleteDirRecursively(m_profile_path);
  }
  ScopedDolphinConfig(const ScopedDolphinConfig&) = delete;
  ScopedDolphinConfig& operator=(const ScopedDolphinConfig&) = delete;

private:
  std::string m_profile_path;
};

// Real, minimal WebSocket + finlink app-handshake client -- plays the same
// role a browser tab would, using finlink_core directly (see this file's
// own top comment). Blocking sockets with a generous fixed deadline: these
// tests only ever talk to a real, already-listening GBAStreamHost on
// localhost, never a flaky network.
class TestClient
{
public:
  // Connects and completes the RFC6455 opening handshake. Returns false if
  // either step fails -- callers that expect a working connection should
  // ASSERT_TRUE this before going any further.
  bool Connect(unsigned short port)
  {
    if (socket.connect(sf::IpAddress::LocalHost, port, sf::seconds(2)) != sf::Socket::Status::Done)
      return false;
    socket.setBlocking(true);

    uint8_t random_bytes[16];
    for (int i = 0; i < 16; i++)
      random_bytes[i] = static_cast<uint8_t>(i * 17 + port);  // port folded in so parallel clients differ
    char key[FINLINK_WS_KEY_BUF_LEN];
    finlink_ws_generate_key(random_bytes, key);
    std::memcpy(sent_key, key, FINLINK_WS_KEY_LEN);

    char request[512];
    const size_t request_len =
        finlink_ws_build_handshake_request("127.0.0.1", "/", key, request, sizeof(request));
    if (request_len == 0 || !SendAll(request, request_len))
      return false;

    // Accumulate bytes until finlink_ws_parse_handshake_response sees a
    // complete header; anything past it in the same read is already frame
    // data and must be kept, not discarded (see that function's own doc).
    for (;;)
    {
      if (!ReadMore())
        return false;
      size_t header_len = 0;
      const auto status =
          finlink_ws_parse_handshake_response(buffer.data(), buffer.size(), sent_key, &header_len);
      if (status == FINLINK_WS_HANDSHAKE_OK)
      {
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(header_len));
        return true;
      }
      if (status == FINLINK_WS_HANDSHAKE_ERR)
        return false;
      // INCOMPLETE: loop and read more.
    }
  }

  // Reads exactly one text frame's payload, blocking until it arrives (or
  // the connection drops). Returns nullopt on any failure/disconnect.
  std::optional<std::string> ReceiveTextFrame()
  {
    for (;;)
    {
      finlink_ws_frame frame;
      const auto status = finlink_ws_parse_frame(buffer.data(), buffer.size(), &frame);
      if (status == FINLINK_WS_FRAME_OK)
      {
        if (frame.opcode != FINLINK_WS_OPCODE_TEXT)
          return std::nullopt;
        std::string text(reinterpret_cast<char*>(frame.payload), frame.payload_size);
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(frame.frame_size));
        return text;
      }
      if (status == FINLINK_WS_FRAME_ERR)
        return std::nullopt;
      if (!ReadMore())
        return std::nullopt;
    }
  }

  bool SendTextFrame(const std::string& payload)
  {
    const uint8_t mask_key[4] = {0x11, 0x22, 0x33, 0x44};
    const size_t max_size = finlink_ws_build_frame_max_size(payload.size());
    std::vector<uint8_t> out(max_size);
    const size_t written =
        finlink_ws_build_frame(FINLINK_WS_OPCODE_TEXT, reinterpret_cast<const uint8_t*>(payload.data()),
                                payload.size(), mask_key, out.data(), out.size());
    if (written == 0)
      return false;
    return SendAll(reinterpret_cast<const char*>(out.data()), written);
  }

private:
  bool SendAll(const char* data, size_t size)
  {
    size_t sent_total = 0;
    while (sent_total < size)
    {
      size_t sent = 0;
      if (socket.send(data + sent_total, size - sent_total, sent) != sf::Socket::Status::Done)
        return false;
      sent_total += sent;
    }
    return true;
  }

  bool ReadMore()
  {
    std::array<uint8_t, 4096> chunk{};
    size_t received = 0;
    const auto status = socket.receive(chunk.data(), chunk.size(), received);
    if (status != sf::Socket::Status::Done || received == 0)
      return false;
    buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + static_cast<long>(received));
    return true;
  }

  sf::TcpSocket socket;
  char sent_key[FINLINK_WS_KEY_LEN];
  std::vector<uint8_t> buffer;
};

// The full client side of the app-level handshake (finlink/handshake.h):
// connect, read `hello`, send `hello_ack` requesting `slot`, read the
// server's reply. Returns nullopt on any transport/protocol failure (the
// caller only cares about the two shapes below); on success, exactly one
// of session_ready/handshake_error is populated, matching
// finlink_handshake_message_type.
struct HandshakeOutcome
{
  std::optional<finlink_session_ready> session_ready;
  std::optional<finlink_handshake_error> handshake_error;
};

std::optional<HandshakeOutcome> PerformClientHandshake(TestClient* client, unsigned short port, int slot)
{
  if (!client->Connect(port))
    return std::nullopt;

  const auto hello_text = client->ReceiveTextFrame();
  if (!hello_text)
    return std::nullopt;
  finlink_hello hello;
  if (finlink_parse_hello(reinterpret_cast<const uint8_t*>(hello_text->data()), hello_text->size(),
                           &hello) != FINLINK_HANDSHAKE_OK)
  {
    return std::nullopt;
  }
  // Real cross-check (see this file's top comment): confirms
  // GBAStreamHost::PerformAppHandshake's hand-built `hello` JSON actually
  // parses via finlink_core's own parser, not just GBAStreamHandshake.cpp's
  // matching picojson-based one.
  if (hello.protocol_version != GBA_STREAM_PROTOCOL_VERSION)
    return std::nullopt;

  finlink_hello_ack_request req{};
  req.requested_slot = slot;
  req.max_width = 240;
  req.max_height = 160;
  req.max_fps = 60.0;
  req.wants_audio = 0;
  // No video_mode: this checked-out Externals/finlink pin (tracks finlink's
  // main branch, see finlink-smoke.yml's own freshness-check comment)
  // predates that field entirely -- finlink_hello_ack_request has no such
  // member here. Omitting it is itself valid per docs/protocol.md (an old
  // client that never sets it), not something this test needs to work
  // around.
  char ack_buf[512];
  const size_t ack_len = finlink_build_hello_ack(&req, ack_buf, sizeof(ack_buf));
  if (ack_len == 0 || !client->SendTextFrame(std::string(ack_buf, ack_len)))
    return std::nullopt;

  const auto reply_text = client->ReceiveTextFrame();
  if (!reply_text)
    return std::nullopt;
  const auto* reply_bytes = reinterpret_cast<const uint8_t*>(reply_text->data());
  const auto msg_type = finlink_peek_handshake_message(reply_bytes, reply_text->size());

  HandshakeOutcome outcome;
  if (msg_type == FINLINK_HS_MSG_SESSION_READY)
  {
    finlink_session_ready ready;
    if (finlink_parse_session_ready(reply_bytes, reply_text->size(), &ready) != FINLINK_HANDSHAKE_OK)
      return std::nullopt;
    outcome.session_ready = ready;
  }
  else if (msg_type == FINLINK_HS_MSG_HANDSHAKE_ERROR)
  {
    finlink_handshake_error error;
    if (finlink_parse_handshake_error(reply_bytes, reply_text->size(), &error) != FINLINK_HANDSHAKE_OK)
      return std::nullopt;
    outcome.handshake_error = error;
  }
  else
  {
    return std::nullopt;
  }
  return outcome;
}
}  // namespace

TEST(GBAStreamMultiHost, TwoDifferentSlotsConnectSimultaneouslyWithoutInterference)
{
  ScopedDolphinConfig config;
  // Two independent GBAStreamHost instances, different device_numbers ->
  // different player ports (GBA_STREAM_PLAYER_BASE_PORT + device_number) --
  // the core "multi-host" property: connecting to one must never block or
  // otherwise affect the other.
  GBAStreamHost host_a(0);
  GBAStreamHost host_b(1);

  TestClient client_a, client_b;
  const auto outcome_a =
      PerformClientHandshake(&client_a, static_cast<unsigned short>(GBA_STREAM_PLAYER_BASE_PORT + 0), 0);
  const auto outcome_b =
      PerformClientHandshake(&client_b, static_cast<unsigned short>(GBA_STREAM_PLAYER_BASE_PORT + 1), 1);

  ASSERT_TRUE(outcome_a.has_value());
  ASSERT_TRUE(outcome_b.has_value());
  EXPECT_TRUE(outcome_a->session_ready.has_value());
  EXPECT_TRUE(outcome_b->session_ready.has_value());
  EXPECT_EQ(outcome_a->session_ready->slot, 0);
  EXPECT_EQ(outcome_b->session_ready->slot, 1);

  // The propagation property GBAStreamLobby's own hello.slots list relies
  // on (see GBAStreamHost.h's own comment on IsSlotOccupied): both slots
  // must now report occupied, reflecting these two real, live sessions.
  EXPECT_TRUE(GBAStreamHost::IsSlotOccupied(0));
  EXPECT_TRUE(GBAStreamHost::IsSlotOccupied(1));
  // Slots 2/3 are configured on neither instance in this test -- must
  // report unoccupied, not crash/throw for an unregistered device_number.
  EXPECT_FALSE(GBAStreamHost::IsSlotOccupied(2));
  EXPECT_FALSE(GBAStreamHost::IsSlotOccupied(3));

  EXPECT_EQ(GBAStreamHost::GetSlotLabel(0), "P1");
  EXPECT_EQ(GBAStreamHost::GetSlotLabel(1), "P2");
}

TEST(GBAStreamMultiHost, SecondConnectionToAlreadyOccupiedSlotIsRejected)
{
  ScopedDolphinConfig config;
  GBAStreamHost host(2);

  TestClient first_client;
  const auto first_outcome =
      PerformClientHandshake(&first_client, static_cast<unsigned short>(GBA_STREAM_PLAYER_BASE_PORT + 2), 2);
  ASSERT_TRUE(first_outcome.has_value());
  ASSERT_TRUE(first_outcome->session_ready.has_value());

  // RunWebSocketSession() (entered by ServeConnection right after a
  // successful PerformAppHandshake) is what actually flips
  // m_client_connected -- give the first session's own thread a moment to
  // reach it before the second connection attempt, so this test reliably
  // exercises the "genuinely occupied" path rather than racing it.
  for (int i = 0; i < 50 && !GBAStreamHost::IsSlotOccupied(2); i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_TRUE(GBAStreamHost::IsSlotOccupied(2));

  TestClient second_client;
  const auto second_outcome =
      PerformClientHandshake(&second_client, static_cast<unsigned short>(GBA_STREAM_PLAYER_BASE_PORT + 2), 2);
  ASSERT_TRUE(second_outcome.has_value());
  ASSERT_TRUE(second_outcome->handshake_error.has_value());
  EXPECT_STREQ(second_outcome->handshake_error->code, "slot_unavailable");
}

TEST(GBAStreamMultiHost, WrongSlotRequestOnAPortIsRejected)
{
  ScopedDolphinConfig config;
  // Each player port only ever serves its own device_number -- requesting
  // a different slot on it (e.g. a stale/mismatched client) must be
  // rejected, not silently granted the port's actual slot.
  GBAStreamHost host(3);

  TestClient client;
  const auto outcome =
      PerformClientHandshake(&client, static_cast<unsigned short>(GBA_STREAM_PLAYER_BASE_PORT + 3), /*slot=*/0);
  ASSERT_TRUE(outcome.has_value());
  ASSERT_TRUE(outcome->handshake_error.has_value());
  EXPECT_STREQ(outcome->handshake_error->code, "malformed_request");
}

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
