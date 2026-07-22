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
constexpr unsigned short kLobbyPort = 6800;

class LobbyServer
{
public:
  void Start()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ref_count++ > 0)
      return;

    m_stop = false;
    const auto status = m_listener.listen(kLobbyPort);
    if (status != sf::Socket::Status::Done)
    {
      ERROR_LOG_FMT(SERIALINTERFACE, "GBAStreamLobby: failed to listen on port {}", kLobbyPort);
      return;
    }
    m_thread = std::thread([this] { AcceptLoop(); });
  }

  void Stop()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (--m_ref_count > 0)
      return;

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
    // client can't stall the accept loop) and respond.
    std::array<char, 1024> buf{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (!m_stop && std::chrono::steady_clock::now() < deadline)
    {
      size_t received = 0;
      const auto status = socket.receive(buf.data(), buf.size(), received);
      if (status == sf::Socket::Status::NotReady)
      {
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
    SendAllBytes(socket, response_str.data(), response_str.size(), m_stop);
  }

  std::mutex m_mutex;
  int m_ref_count = 0;
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
