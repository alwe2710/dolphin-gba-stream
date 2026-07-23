// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAS_LIBMGBA

#include "Core/HW/GBAStreamLobby.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <SFML/Network/SocketSelector.hpp>
#include <SFML/Network/TcpListener.hpp>
#include <SFML/Network/TcpSocket.hpp>
#include <SFML/System/Time.hpp>

#include "Common/Logging/Log.h"

#include "Core/HW/GBAStreamClientPage.h"
#include "Core/HW/GBAStreamNetUtil.h"

namespace HW::GBA
{
namespace
{
class LobbyServer
{
public:
  void Start()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_ref_count;
    // Separate from the refcount check: if an earlier AddRef's listen() call
    // failed (e.g. the port was transiently still held by a just-exited
    // previous Dolphin process), every later AddRef used to see refcount > 0
    // and assume the lobby was already up, silently leaving it dead for the
    // rest of the session. Retrying here instead means the *next* GBA slot
    // to start gets another chance once the transient conflict has cleared.
    if (m_running)
      return;

    m_stop = false;
    const auto status = m_listener.listen(kGBAStreamLobbyPort);
    if (status != sf::Socket::Status::Done)
    {
      ERROR_LOG_FMT(SERIALINTERFACE, "GBAStreamLobby: failed to listen on port {}",
                   kGBAStreamLobbyPort);
      return;
    }
    m_running = true;
    m_thread = std::thread([this] { AcceptLoop(); });
  }

  void Stop()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (--m_ref_count > 0)
      return;

    if (!m_running)
      return;
    m_running = false;
    m_stop = true;
    m_listener.close();
    if (m_thread.joinable())
      m_thread.join();
  }

private:
  void AcceptLoop()
  {
    sf::SocketSelector selector;
    selector.add(m_listener);
    while (!m_stop)
    {
      if (!selector.wait(sf::milliseconds(100)))
        continue;
      sf::TcpSocket socket;
      if (m_listener.accept(socket) != sf::Socket::Status::Done)
        continue;
      HandleConnection(socket);
    }
  }

  void HandleConnection(sf::TcpSocket& socket)
  {
    socket.setBlocking(false);
    // This server only ever serves one static page regardless of request
    // path or method, so there's no need to actually parse the request --
    // just drain whatever the browser sends (bounded, so a slow/silent
    // client can't stall the accept loop) and respond. Every successful read
    // is followed by another attempt instead of stopping right away: a real
    // request (extra headers, cookies) can take more than one recv() to
    // fully drain, and leaving any of it unread in the kernel's receive
    // queue when this socket later closes makes Linux send an RST instead of
    // a graceful FIN -- exactly the truncation-risk failure mode
    // CloseGracefully() below exists to avoid. Once reads have gone quiet
    // for a short grace period, the request is assumed fully buffered.
    std::array<char, 1024> buf{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    constexpr auto kQuietGrace = std::chrono::milliseconds(20);
    auto quiet_since = std::chrono::steady_clock::now();
    while (!m_stop && std::chrono::steady_clock::now() < deadline)
    {
      size_t received = 0;
      const auto status = socket.receive(buf.data(), buf.size(), received);
      if (status == sf::Socket::Status::Done)
      {
        quiet_since = std::chrono::steady_clock::now();
        continue;
      }
      if (status == sf::Socket::Status::NotReady)
      {
        if (std::chrono::steady_clock::now() - quiet_since > kQuietGrace)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      break;
    }

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: text/html; charset=utf-8\r\n"
             << "Content-Length: " << kGBAStreamClientHtml.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << kGBAStreamClientHtml;
    const std::string response_str = response.str();
    if (SendAllBytes(socket, response_str.data(), response_str.size(), m_stop))
      CloseGracefully(socket, m_stop);
  }

  std::mutex m_mutex;
  int m_ref_count = 0;
  bool m_running = false;
  sf::TcpListener m_listener;
  std::thread m_thread;
  std::atomic_bool m_stop{false};
};

LobbyServer& GetLobbyServer()
{
  static LobbyServer server;
  return server;
}

}  // namespace

void GBAStreamLobby::AddRef()
{
  GetLobbyServer().Start();
}

void GBAStreamLobby::Release()
{
  GetLobbyServer().Stop();
}

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
