// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The half of the virtual network that a single process cannot reach: traffic
// that actually crosses the lobby link between two consoles.
//
// This needs two processes, so it is driven by an environment variable and
// skipped otherwise. Source/UnitTests/Core/Lobby/run-peer-test.sh starts both
// halves and reports the result.
//
//   VNET_PEER_ROLE=host   VNET_PEER_PORT=nnnnn  tests --gtest_filter=VirtualNetPeer.*
//   VNET_PEER_ROLE=client VNET_PEER_PORT=nnnnn  tests --gtest_filter=VirtualNetPeer.*
//
// The host half serves whatever the client asks of it for a fixed window and
// then exits; the client half is the one that asserts.

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Core/IOS/Network/Socket.h"
#include "Core/Lobby/LobbyNet.h"
#include "Core/Lobby/VirtualNet.h"

namespace
{
using namespace IOS::HLE;

constexpr u16 ECHO_PORT = 27900;
constexpr u16 STREAM_PORT = 27910;
constexpr auto RUN_TIME = std::chrono::seconds(20);

std::string EnvOr(const char* name, const char* fallback)
{
  const char* const value = std::getenv(name);
  return value ? value : fallback;
}

std::string Role()
{
  return EnvOr("VNET_PEER_ROLE", "");
}

u16 Port()
{
  return static_cast<u16>(std::strtoul(EnvOr("VNET_PEER_PORT", "38921").c_str(), nullptr, 10));
}

// Frames cross a real socket and a worker thread, so nothing arrives within the
// same call that sent it. Everything that waits for a peer goes through here.
template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

std::vector<u8> Bytes(std::string_view text)
{
  return std::vector<u8>(text.begin(), text.end());
}
}  // namespace

TEST(VirtualNetPeer, Host)
{
  if (Role() != "host")
    GTEST_SKIP() << "not the host half";

  ASSERT_TRUE(Lobby::Start(Lobby::Role::Host, "", Port(), "HostConsole"));
  ASSERT_TRUE(Lobby::WaitForAddress(2000));
  VirtualNet::Initialize();
  ASSERT_EQ(Lobby::HOST_IP, VirtualNet::LocalIP());

  auto echo = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(echo, 0, ECHO_PORT));

  auto listener = VirtualNet::Create(VirtualNet::WII_SOCK_STREAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(listener, 0, STREAM_PORT));
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Listen(listener, 4));

  VirtualNet::SocketPtr accepted;

  // Serve whatever turns up until the window closes. The client is what
  // asserts; this half only has to behave.
  const auto deadline = std::chrono::steady_clock::now() + RUN_TIME;
  while (std::chrono::steady_clock::now() < deadline)
  {
    u8 buffer[512]{};
    u32 from_ip = 0;
    u16 from_port = 0;

    const s32 received =
        VirtualNet::RecvFrom(echo, buffer, sizeof(buffer), false, true, &from_ip, &from_port);
    if (received > 0)
    {
      // Reply to wherever it came from, which is how the client learns the
      // return path works as well as the outbound one.
      VirtualNet::SendTo(echo, buffer, received, true, from_ip, from_port);
    }

    if (!accepted)
    {
      s32 error = 0;
      accepted = VirtualNet::Accept(listener, nullptr, nullptr, &error);
    }
    else
    {
      const s32 streamed =
          VirtualNet::RecvFrom(accepted, buffer, sizeof(buffer), false, false, nullptr, nullptr);
      if (streamed > 0)
        VirtualNet::SendTo(accepted, buffer, streamed, false, 0, 0);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  VirtualNet::Shutdown();
  Lobby::Stop();
}

TEST(VirtualNetPeer, Client)
{
  if (Role() != "client")
    GTEST_SKIP() << "not the client half";

  const std::string address = "127.0.0.1:" + std::to_string(Port());
  ASSERT_TRUE(Lobby::Start(Lobby::Role::Client, address, Port(), "ClientConsole"));
  ASSERT_TRUE(Lobby::WaitForAddress(10000)) << Lobby::GetStatusText();

  VirtualNet::Initialize();

  // The host keeps .1 and hands out .2 upward, so this is what a second console
  // must have been given.
  EXPECT_EQ(Lobby::SUBNET | 0x02, VirtualNet::LocalIP());
  ASSERT_TRUE(VirtualNet::IsActive());

  // Both consoles show up in the roster the host broadcasts.
  EXPECT_TRUE(WaitFor([] { return Lobby::GetPeers().size() == 2; }, std::chrono::seconds(5)))
      << "roster has " << Lobby::GetPeers().size() << " entries";

  auto socket = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(socket, 0, 27950));

  u8 buffer[512]{};
  u32 from_ip = 0;
  u16 from_port = 0;

  // --- unicast, and the reply back ---
  {
    const std::vector<u8> payload = Bytes("unicast");
    ASSERT_EQ(7, VirtualNet::SendTo(socket, payload.data(), 7, true, Lobby::HOST_IP, ECHO_PORT));

    ASSERT_TRUE(WaitFor(
        [&] {
          return VirtualNet::RecvFrom(socket, buffer, sizeof(buffer), false, true, &from_ip,
                                      &from_port) > 0;
        },
        std::chrono::seconds(5)))
        << "no reply to a unicast datagram";
    EXPECT_EQ(0, std::memcmp(buffer, "unicast", 7));
    EXPECT_EQ(Lobby::HOST_IP, from_ip);
    EXPECT_EQ(ECHO_PORT, from_port);
  }

  // --- broadcast reaches the other console ---
  {
    const std::vector<u8> payload = Bytes("broadcast");
    ASSERT_EQ(9, VirtualNet::SendTo(socket, payload.data(), 9, true, Lobby::BROADCAST_IP,
                                    ECHO_PORT));

    // The sender hears its own broadcast too, and that copy arrives first
    // because it never leaves the process. The one that matters is the reply
    // from the other console, which carries its address as the source.
    ASSERT_TRUE(WaitFor(
        [&] {
          const s32 size = VirtualNet::RecvFrom(socket, buffer, sizeof(buffer), false, true,
                                                &from_ip, &from_port);
          return size > 0 && from_ip == Lobby::HOST_IP;
        },
        std::chrono::seconds(5)))
        << "the other console never answered a broadcast";
    EXPECT_EQ(0, std::memcmp(buffer, "broadcast", 9));
  }

  // --- a stream across the link ---
  {
    auto stream = VirtualNet::Create(VirtualNet::WII_SOCK_STREAM, 0);
    ASSERT_EQ(-SO_EINPROGRESS, VirtualNet::Connect(stream, Lobby::HOST_IP, STREAM_PORT));

    ASSERT_TRUE(WaitFor(
        [&] { return VirtualNet::Connect(stream, Lobby::HOST_IP, STREAM_PORT) == -SO_EISCONN; },
        std::chrono::seconds(5)))
        << "the stream never connected";

    const std::vector<u8> payload = Bytes("stream over the lobby");
    ASSERT_EQ(21, VirtualNet::SendTo(stream, payload.data(), 21, false, 0, 0));

    ASSERT_TRUE(WaitFor(
        [&] {
          return VirtualNet::RecvFrom(stream, buffer, sizeof(buffer), false, false, nullptr,
                                      nullptr) == 21;
        },
        std::chrono::seconds(5)))
        << "the stream echo never came back";
    EXPECT_EQ(0, std::memcmp(buffer, "stream over the lobby", 21));
  }

  // --- echo request answered by the other console's stack ---
  {
    const std::vector<u8> payload = Bytes("ping");
    EXPECT_EQ(4, VirtualNet::Ping(Lobby::HOST_IP, 0x4321, payload.data(), 4, 3000));
  }

  // --- reconnecting keeps the address, so bound sockets keep working ---
  {
    const u32 before = VirtualNet::LocalIP();
    ASSERT_TRUE(Lobby::Reconnect());

    // The link has to be seen going down before waiting for it to come back up,
    // or the still-Connected status from a moment ago satisfies the wait and
    // nothing is actually tested.
    ASSERT_TRUE(WaitFor([] { return Lobby::GetStatus() != Lobby::Status::Connected; },
                        std::chrono::seconds(5)))
        << "the reconnect request was never acted on";

    ASSERT_TRUE(WaitFor([] { return Lobby::GetStatus() == Lobby::Status::Connected; },
                        std::chrono::seconds(10)))
        << "never got back in: " << Lobby::GetStatusText();
    EXPECT_EQ(before, VirtualNet::LocalIP()) << "the address was not preserved across a reconnect";

    // The socket bound before the reconnect is still the one that receives.
    const std::vector<u8> payload = Bytes("after");
    ASSERT_TRUE(WaitFor(
        [&] {
          return VirtualNet::SendTo(socket, payload.data(), 5, true, Lobby::HOST_IP, ECHO_PORT) ==
                 5;
        },
        std::chrono::seconds(5)));

    ASSERT_TRUE(WaitFor(
        [&] {
          return VirtualNet::RecvFrom(socket, buffer, sizeof(buffer), false, false, nullptr,
                                      nullptr) == 5;
        },
        std::chrono::seconds(5)))
        << "nothing came back after reconnecting";
    EXPECT_EQ(0, std::memcmp(buffer, "after", 5));
  }

  VirtualNet::Shutdown();
  Lobby::Stop();
}
