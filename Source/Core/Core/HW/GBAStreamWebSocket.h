// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef HAS_LIBMGBA

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <mbedtls/base64.h>

#include <SFML/Network/TcpSocket.hpp>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"

#include "Core/HW/GBAStreamNetUtil.h"

namespace HW::GBA
{
// RFC6455 WebSocket transport shared between GBAStreamHost (player ports,
// 6801-6804) and GBAStreamLobby (handshake port, 6800): reading/parsing the
// plain-HTTP upgrade request, computing Sec-WebSocket-Accept, parsing framed
// client->server messages, and building server->client frames. Deliberately
// only the transport -- neither the app-level handshake (GBAStreamHandshake.h)
// nor the existing Video/Audio/Input binary message formats (GBAStreamHost.cpp)
// live here.

struct HttpRequest
{
  std::string path;
  std::map<std::string, std::string> headers;  // keys lowercased
};

// Reads and minimally parses one HTTP request (request line + headers) from
// `socket`, which must already be non-blocking. Returns nullopt on timeout,
// a malformed request, or if `stop_flag` is set while waiting -- callers
// should treat that identically to a failed connection attempt.
inline std::optional<HttpRequest> ReadHttpRequest(sf::TcpSocket& socket,
                                                   const std::atomic_bool& stop_flag)
{
  std::string request;
  std::array<char, 4096> buf{};
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384)
  {
    if (stop_flag)
      return std::nullopt;
    size_t received = 0;
    const auto status = socket.receive(buf.data(), buf.size(), received);
    if (status == sf::Socket::Status::NotReady)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    if (status != sf::Socket::Status::Done || received == 0)
      return std::nullopt;
    request.append(buf.data(), received);
  }
  if (request.find("\r\n\r\n") == std::string::npos)
    return std::nullopt;

  HttpRequest result;
  std::istringstream stream(request);
  std::string request_line;
  std::getline(stream, request_line);
  {
    const auto first_space = request_line.find(' ');
    const auto second_space = first_space == std::string::npos ?
                                   std::string::npos :
                                   request_line.find(' ', first_space + 1);
    if (first_space != std::string::npos && second_space != std::string::npos)
      result.path = request_line.substr(first_space + 1, second_space - first_space - 1);
  }
  std::string line;
  while (std::getline(stream, line) && line != "\r" && !line.empty())
  {
    const auto colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ')
      value.erase(value.begin());
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
      value.pop_back();
    std::transform(key.begin(), key.end(), key.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    result.headers[key] = value;
  }
  return result;
}

inline bool IsWebSocketUpgradeRequest(const HttpRequest& request)
{
  auto it = request.headers.find("upgrade");
  if (it == request.headers.end() || !request.headers.count("sec-websocket-key"))
    return false;
  std::string upgrade = it->second;
  std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return upgrade == "websocket";
}

// Computes and sends the 101 Switching Protocols response. `request` must
// satisfy IsWebSocketUpgradeRequest(). Returns false if the write failed
// (dead peer) -- the caller should give up on the connection in that case,
// same as any other send failure in this feature.
inline bool SendWebSocketUpgradeResponse(sf::TcpSocket& socket, const HttpRequest& request,
                                          const std::atomic_bool& stop_flag)
{
  const std::string concatenated =
      request.headers.at("sec-websocket-key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const auto digest = Common::SHA1::CalculateDigest(concatenated);
  std::array<unsigned char, 64> b64{};
  size_t b64_len = 0;
  mbedtls_base64_encode(b64.data(), b64.size(), &b64_len, digest.data(), digest.size());

  std::ostringstream response;
  response << "HTTP/1.1 101 Switching Protocols\r\n"
           << "Upgrade: websocket\r\n"
           << "Connection: Upgrade\r\n"
           << "Sec-WebSocket-Accept: " << std::string(reinterpret_cast<char*>(b64.data()), b64_len)
           << "\r\n\r\n";
  const std::string response_str = response.str();
  return SendAllBytes(socket, response_str.data(), response_str.size(), stop_flag);
}

struct WebSocketFrame
{
  u8 opcode;
  std::vector<u8> payload;
  size_t consumed;
};

constexpr u8 WS_OPCODE_TEXT = 0x1;
constexpr u8 WS_OPCODE_BINARY = 0x2;
constexpr u8 WS_OPCODE_CLOSE = 0x8;

// Our own clients never send anything bigger than a handful of bytes (a
// binary input message, or a small JSON handshake reply), so this is
// generous headroom, not a real limit -- it exists purely so a malformed or
// hostile peer can't claim an absurd 64-bit length. Without a cap, `pos + len`
// below (size_t arithmetic) can overflow and wrap back into a small value,
// making the "is the full frame buffered yet" check pass despite `buf`
// actually holding far fewer bytes than claimed -- the unmasking loop then
// reads out of bounds, and even if it didn't, `frame.payload.resize(len)`
// would attempt an unbounded allocation.
constexpr u64 MAX_WEBSOCKET_FRAME_PAYLOAD = 1 << 20;  // 1 MiB

// Parses at most one client->server (masked) WebSocket frame from the front
// of `buf`. Returns nullopt if `buf` doesn't yet contain a full frame -- the
// caller should wait for more data and retry. Fragmented frames (FIN=0) are
// not supported: our clients never send them, so treat one as a protocol
// error (handled the same as a close frame by the caller). An oversized
// declared length is treated the same way (see MAX_WEBSOCKET_FRAME_PAYLOAD).
inline std::optional<WebSocketFrame> TryParseWebSocketFrame(const std::vector<u8>& buf)
{
  if (buf.size() < 2)
    return std::nullopt;

  const u8 b0 = buf[0];
  const u8 b1 = buf[1];
  const u8 opcode = b0 & 0x0F;
  const bool masked = (b1 & 0x80) != 0;
  u64 len = b1 & 0x7F;
  size_t pos = 2;

  if (len == 126)
  {
    if (buf.size() < 4)
      return std::nullopt;
    len = (static_cast<u64>(buf[2]) << 8) | buf[3];
    pos = 4;
  }
  else if (len == 127)
  {
    if (buf.size() < 10)
      return std::nullopt;
    len = 0;
    for (int i = 0; i < 8; ++i)
      len = (len << 8) | buf[2 + i];
    pos = 10;
  }

  if (len > MAX_WEBSOCKET_FRAME_PAYLOAD)
  {
    WebSocketFrame frame;
    frame.opcode = WS_OPCODE_CLOSE;
    frame.consumed = buf.size();
    return frame;
  }

  std::array<u8, 4> mask_key{};
  if (masked)
  {
    if (buf.size() < pos + 4)
      return std::nullopt;
    std::copy_n(buf.begin() + pos, 4, mask_key.begin());
    pos += 4;
  }

  if (buf.size() < pos + len)
    return std::nullopt;

  WebSocketFrame frame;
  frame.opcode = opcode;
  frame.payload.resize(len);
  for (u64 i = 0; i < len; ++i)
    frame.payload[i] = buf[pos + i] ^ (masked ? mask_key[i % 4] : u8{0});
  frame.consumed = pos + len;
  return frame;
}

// Sends one unmasked, unfragmented server->client frame with the given
// opcode (WS_OPCODE_BINARY for Video/Audio, WS_OPCODE_TEXT for handshake JSON).
inline bool SendWebSocketFrame(sf::TcpSocket& socket, u8 opcode, const std::vector<u8>& payload,
                                const std::atomic_bool& stop_flag)
{
  std::vector<u8> frame;
  frame.reserve(payload.size() + 10);
  frame.push_back(static_cast<u8>(0x80 | (opcode & 0x0F)));  // FIN=1, given opcode.

  const size_t len = payload.size();
  if (len < 126)
  {
    frame.push_back(static_cast<u8>(len));
  }
  else if (len <= 0xFFFF)
  {
    frame.push_back(126);
    frame.push_back(static_cast<u8>((len >> 8) & 0xFF));
    frame.push_back(static_cast<u8>(len & 0xFF));
  }
  else
  {
    frame.push_back(127);
    for (int shift = 56; shift >= 0; shift -= 8)
      frame.push_back(static_cast<u8>((static_cast<u64>(len) >> shift) & 0xFF));
  }
  frame.insert(frame.end(), payload.begin(), payload.end());

  return SendAllBytes(socket, frame.data(), frame.size(), stop_flag);
}

inline bool SendWebSocketBinaryFrame(sf::TcpSocket& socket, const std::vector<u8>& payload,
                                      const std::atomic_bool& stop_flag)
{
  return SendWebSocketFrame(socket, WS_OPCODE_BINARY, payload, stop_flag);
}

inline bool SendWebSocketTextFrame(sf::TcpSocket& socket, const std::string& payload,
                                    const std::atomic_bool& stop_flag)
{
  return SendWebSocketFrame(socket, WS_OPCODE_TEXT, std::vector<u8>(payload.begin(), payload.end()),
                             stop_flag);
}

// Reads off `socket` (already upgraded, non-blocking) until one full
// WebSocket frame has been received or `timeout` elapses. Used for the
// app-level handshake (GBAStreamHandshake.h), where exactly one text frame
// (hello_ack) is expected before any Video/Audio/Input binary frame -- not
// used once a session's binary streaming loop has started, which has its own
// receive loop with different (indefinite, disconnect-driven) semantics.
// Returns nullopt on timeout, disconnect/error, or if `stop_flag` becomes set
// while waiting.
inline std::optional<WebSocketFrame> ReceiveOneWebSocketFrame(sf::TcpSocket& socket,
                                                               const std::atomic_bool& stop_flag,
                                                               std::chrono::milliseconds timeout)
{
  std::vector<u8> recv_buffer;
  std::array<u8, 4096> read_buf{};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (stop_flag)
      return std::nullopt;
    size_t received = 0;
    const auto status = socket.receive(read_buf.data(), read_buf.size(), received);
    if (status == sf::Socket::Status::Disconnected || status == sf::Socket::Status::Error)
      return std::nullopt;
    if (status == sf::Socket::Status::NotReady)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    if (status == sf::Socket::Status::Done && received > 0)
    {
      recv_buffer.insert(recv_buffer.end(), read_buf.begin(), read_buf.begin() + received);
      auto frame = TryParseWebSocketFrame(recv_buffer);
      if (frame)
        return frame;
    }
  }
  return std::nullopt;
}

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
