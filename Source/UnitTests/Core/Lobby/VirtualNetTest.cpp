// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the virtual Wii network's stack.
//
// A second console is not needed to exercise most of it. Anything a console
// addresses to itself, and its own copy of anything it broadcasts, never
// reaches the wire - the stack loops it back, exactly as a real machine on a
// real segment would. So a single instance hosting an empty lobby can drive
// sockets against themselves and cover the datagram path, the stream handshake,
// broadcast, port allocation, readiness and the isolation rules.
//
// What this cannot cover is routing between two lobby members, which needs two
// processes.

#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Core/IOS/Network/Socket.h"
#include "Core/Lobby/LobbyNet.h"
#include "Core/Lobby/VirtualNet.h"

namespace
{
using namespace IOS::HLE;

// A lobby hosting on an unused port. The host is given 10.13.37.1 the moment it
// starts listening, so there is no handshake to wait for.
class VirtualNetTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    ASSERT_TRUE(Lobby::Start(Lobby::Role::Host, "", TEST_PORT, "TestHost"));
    ASSERT_TRUE(Lobby::WaitForAddress(1000));
    VirtualNet::Initialize();
    ASSERT_TRUE(VirtualNet::IsActive());
    ASSERT_EQ(Lobby::HOST_IP, VirtualNet::LocalIP());
  }

  void TearDown() override
  {
    VirtualNet::Shutdown();
    Lobby::Stop();
  }

  // Loopback happens inside the send call, so anything addressed to this
  // console is already queued by the time SendTo returns. Nothing here needs to
  // wait on the lobby thread.
  static constexpr u16 TEST_PORT = 32741;
};

std::vector<u8> Bytes(std::string_view text)
{
  return std::vector<u8>(text.begin(), text.end());
}
}  // namespace

TEST_F(VirtualNetTest, InterfaceIsThePrivateSubnet)
{
  EXPECT_EQ(0x0A0D2501u, VirtualNet::LocalIP());     // 10.13.37.1
  EXPECT_EQ(0xFFFFFF00u, VirtualNet::Netmask());     // 255.255.255.0
  EXPECT_EQ(0x0A0D25FFu, VirtualNet::BroadcastIP());  // 10.13.37.255
  EXPECT_EQ(0x0A0D2501u, VirtualNet::Gateway());
}

TEST_F(VirtualNetTest, DatagramToSelf)
{
  auto receiver = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  auto sender = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  ASSERT_TRUE(receiver);
  ASSERT_TRUE(sender);

  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(receiver, 0, 27900));

  const std::vector<u8> payload = Bytes("hello");
  EXPECT_EQ(5, VirtualNet::SendTo(sender, payload.data(), 5, true, VirtualNet::LocalIP(), 27900));

  u8 buffer[64]{};
  u32 from_ip = 0;
  u16 from_port = 0;
  EXPECT_EQ(5, VirtualNet::RecvFrom(receiver, buffer, sizeof(buffer), false, true, &from_ip,
                                    &from_port));
  EXPECT_EQ(0, std::memcmp(buffer, "hello", 5));
  EXPECT_EQ(VirtualNet::LocalIP(), from_ip);
  EXPECT_NE(0, from_port);  // the sender was bound implicitly

  // Nothing left.
  EXPECT_EQ(-SO_EAGAIN,
            VirtualNet::RecvFrom(receiver, buffer, sizeof(buffer), false, true, nullptr, nullptr));
}

TEST_F(VirtualNetTest, BroadcastIsHeardByTheSender)
{
  // This is the behaviour peer discovery depends on. Software that finds other
  // consoles by broadcasting filters its own packets out by source address; it
  // can only do that if they arrive at all.
  auto listener = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(listener, 0, 27901));

  const std::vector<u8> payload = Bytes("discover");
  EXPECT_EQ(8, VirtualNet::SendTo(listener, payload.data(), 8, true, VirtualNet::BroadcastIP(),
                                  27901));

  u8 buffer[64]{};
  u32 from_ip = 0;
  EXPECT_EQ(8,
            VirtualNet::RecvFrom(listener, buffer, sizeof(buffer), false, true, &from_ip, nullptr));
  EXPECT_EQ(VirtualNet::LocalIP(), from_ip);
}

TEST_F(VirtualNetTest, LimitedBroadcastIsTreatedAsSegmentWide)
{
  // 255.255.255.255 is what software looking for neighbours on an unknown
  // network actually sends to, rather than working out the subnet-directed
  // address. It belongs to no lobby member, so without being recognised here it
  // is dropped as unroutable and discovery silently finds nobody.
  auto listener = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(listener, 0, 27920));

  const std::vector<u8> payload = Bytes("who is there");
  EXPECT_EQ(12, VirtualNet::SendTo(listener, payload.data(), 12, true, 0xFFFFFFFF, 27920));

  u8 buffer[64]{};
  u32 from_ip = 0;
  EXPECT_EQ(12,
            VirtualNet::RecvFrom(listener, buffer, sizeof(buffer), false, true, &from_ip, nullptr));
  EXPECT_EQ(VirtualNet::LocalIP(), from_ip);
}

TEST_F(VirtualNetTest, MulticastIsTreatedAsSegmentWide)
{
  auto listener = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(listener, 0, 27921));

  const std::vector<u8> payload = Bytes("group");
  // 239.255.0.1, an ordinary administratively-scoped group.
  EXPECT_EQ(5, VirtualNet::SendTo(listener, payload.data(), 5, true, 0xEFFF0001, 27921));

  u8 buffer[64]{};
  EXPECT_EQ(5, VirtualNet::RecvFrom(listener, buffer, sizeof(buffer), false, false, nullptr,
                                    nullptr));
}

TEST_F(VirtualNetTest, PeekLeavesTheDatagramQueued)
{
  auto socket = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(socket, 0, 27902));

  const std::vector<u8> payload = Bytes("abcd");
  ASSERT_EQ(4, VirtualNet::SendTo(socket, payload.data(), 4, true, VirtualNet::LocalIP(), 27902));

  u8 buffer[16]{};
  EXPECT_EQ(4, VirtualNet::RecvFrom(socket, buffer, sizeof(buffer), true, false, nullptr, nullptr));
  EXPECT_EQ(4, VirtualNet::RecvFrom(socket, buffer, sizeof(buffer), false, false, nullptr, nullptr));
  EXPECT_EQ(-SO_EAGAIN,
            VirtualNet::RecvFrom(socket, buffer, sizeof(buffer), false, false, nullptr, nullptr));
}

TEST_F(VirtualNetTest, OversizedReadTruncatesRatherThanSplitting)
{
  // A datagram is not a stream: what does not fit goes with the rest of the
  // packet.
  auto socket = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(socket, 0, 27903));

  const std::vector<u8> payload(64, 0xAB);
  ASSERT_EQ(64, VirtualNet::SendTo(socket, payload.data(), 64, true, VirtualNet::LocalIP(), 27903));

  u8 buffer[16]{};
  EXPECT_EQ(16, VirtualNet::RecvFrom(socket, buffer, 16, false, false, nullptr, nullptr));
  EXPECT_EQ(-SO_EAGAIN, VirtualNet::RecvFrom(socket, buffer, 16, false, false, nullptr, nullptr));
}

TEST_F(VirtualNetTest, ConnectedDatagramSocketRejectsOtherSenders)
{
  auto receiver = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  auto expected = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  auto stranger = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);

  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(receiver, 0, 27904));
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(expected, 0, 27905));
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(stranger, 0, 27906));
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Connect(receiver, VirtualNet::LocalIP(), 27905));

  const std::vector<u8> payload = Bytes("x");
  ASSERT_EQ(1, VirtualNet::SendTo(stranger, payload.data(), 1, true, VirtualNet::LocalIP(), 27904));

  u8 buffer[8]{};
  EXPECT_EQ(-SO_EAGAIN,
            VirtualNet::RecvFrom(receiver, buffer, sizeof(buffer), false, false, nullptr, nullptr));

  ASSERT_EQ(1, VirtualNet::SendTo(expected, payload.data(), 1, true, VirtualNet::LocalIP(), 27904));
  EXPECT_EQ(1, VirtualNet::RecvFrom(receiver, buffer, sizeof(buffer), false, false, nullptr,
                                    nullptr));
}

TEST_F(VirtualNetTest, BindRefusesAPortTwiceWithoutReuse)
{
  auto first = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  auto second = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);

  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(first, 0, 27907));
  EXPECT_EQ(-SO_EADDRINUSE, VirtualNet::Bind(second, 0, 27907));

  // SO_REUSEADDR, at the Wii's own level and option numbers.
  const u32 enable = 1;
  auto third = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::SetSockOpt(third, 0xFFFF, 0x0004,
                                               reinterpret_cast<const u8*>(&enable), sizeof(enable)));
  EXPECT_EQ(SO_SUCCESS, VirtualNet::Bind(third, 0, 27907));
}

TEST_F(VirtualNetTest, BindRefusesAnAddressThisConsoleDoesNotHold)
{
  auto socket = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  EXPECT_EQ(-SO_EADDRNOTAVAIL, VirtualNet::Bind(socket, 0x0A0D2502, 27908));  // 10.13.37.2
  EXPECT_EQ(SO_SUCCESS, VirtualNet::Bind(socket, VirtualNet::LocalIP(), 27908));
}

TEST_F(VirtualNetTest, StreamHandshakeAndTransfer)
{
  auto listener = VirtualNet::Create(VirtualNet::WII_SOCK_STREAM, 0);
  auto client = VirtualNet::Create(VirtualNet::WII_SOCK_STREAM, 0);

  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(listener, 0, 27910));
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Listen(listener, 4));

  // The SYN loops back, so the SYN|ACK is already back by the time this
  // returns and the connection is established in one call.
  EXPECT_EQ(-SO_EINPROGRESS, VirtualNet::Connect(client, VirtualNet::LocalIP(), 27910));
  EXPECT_EQ(-SO_EISCONN, VirtualNet::Connect(client, VirtualNet::LocalIP(), 27910));
  EXPECT_TRUE(VirtualNet::IsConnected(client));

  u32 from_ip = 0;
  u16 from_port = 0;
  s32 error = 0;
  auto accepted = VirtualNet::Accept(listener, &from_ip, &from_port, &error);
  ASSERT_TRUE(accepted);
  EXPECT_EQ(SO_SUCCESS, error);
  EXPECT_EQ(VirtualNet::LocalIP(), from_ip);

  const std::vector<u8> payload = Bytes("stream data");
  EXPECT_EQ(11, VirtualNet::SendTo(client, payload.data(), 11, false, 0, 0));

  u8 buffer[64]{};
  EXPECT_EQ(11, VirtualNet::RecvFrom(accepted, buffer, sizeof(buffer), false, false, nullptr,
                                     nullptr));
  EXPECT_EQ(0, std::memcmp(buffer, "stream data", 11));

  // And back the other way.
  EXPECT_EQ(2, VirtualNet::SendTo(accepted, payload.data(), 2, false, 0, 0));
  EXPECT_EQ(2, VirtualNet::RecvFrom(client, buffer, sizeof(buffer), false, false, nullptr, nullptr));
}

TEST_F(VirtualNetTest, StreamReadsCoalesce)
{
  auto listener = VirtualNet::Create(VirtualNet::WII_SOCK_STREAM, 0);
  auto client = VirtualNet::Create(VirtualNet::WII_SOCK_STREAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(listener, 0, 27911));
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Listen(listener, 1));
  ASSERT_EQ(-SO_EINPROGRESS, VirtualNet::Connect(client, VirtualNet::LocalIP(), 27911));

  s32 error = 0;
  auto accepted = VirtualNet::Accept(listener, nullptr, nullptr, &error);
  ASSERT_TRUE(accepted);

  const std::vector<u8> a = Bytes("aaa");
  const std::vector<u8> b = Bytes("bbb");
  ASSERT_EQ(3, VirtualNet::SendTo(client, a.data(), 3, false, 0, 0));
  ASSERT_EQ(3, VirtualNet::SendTo(client, b.data(), 3, false, 0, 0));

  // Two sends, one read: a stream has no packet boundaries.
  u8 buffer[16]{};
  EXPECT_EQ(6, VirtualNet::RecvFrom(accepted, buffer, sizeof(buffer), false, false, nullptr,
                                    nullptr));
  EXPECT_EQ(0, std::memcmp(buffer, "aaabbb", 6));
}

TEST_F(VirtualNetTest, ConnectingToNothingIsRefused)
{
  auto client = VirtualNet::Create(VirtualNet::WII_SOCK_STREAM, 0);
  // The RST comes straight back through loopback, so the refusal is already
  // recorded and the retry reports it.
  ASSERT_EQ(-SO_EINPROGRESS, VirtualNet::Connect(client, VirtualNet::LocalIP(), 27912));
  EXPECT_EQ(-SO_ECONNREFUSED, VirtualNet::Connect(client, VirtualNet::LocalIP(), 27912));
}

TEST_F(VirtualNetTest, ClosingAStreamEndsThePeersReads)
{
  auto listener = VirtualNet::Create(VirtualNet::WII_SOCK_STREAM, 0);
  auto client = VirtualNet::Create(VirtualNet::WII_SOCK_STREAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(listener, 0, 27913));
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Listen(listener, 1));
  ASSERT_EQ(-SO_EINPROGRESS, VirtualNet::Connect(client, VirtualNet::LocalIP(), 27913));

  s32 error = 0;
  auto accepted = VirtualNet::Accept(listener, nullptr, nullptr, &error);
  ASSERT_TRUE(accepted);

  u8 buffer[16]{};
  EXPECT_EQ(-SO_EAGAIN,
            VirtualNet::RecvFrom(accepted, buffer, sizeof(buffer), false, false, nullptr, nullptr));

  ASSERT_EQ(SO_SUCCESS, VirtualNet::Close(client));

  // End of stream is zero bytes, not an error.
  EXPECT_EQ(0, VirtualNet::RecvFrom(accepted, buffer, sizeof(buffer), false, false, nullptr,
                                    nullptr));
}

TEST_F(VirtualNetTest, ReadinessDrivesPoll)
{
  auto socket = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(socket, 0, 27914));

  bool readable = true;
  bool writable = false;
  bool exceptional = true;
  VirtualNet::GetReadiness(socket, &readable, &writable, &exceptional);
  EXPECT_FALSE(readable);
  EXPECT_TRUE(writable);  // nothing queues on send
  EXPECT_FALSE(exceptional);

  const std::vector<u8> payload = Bytes("!");
  ASSERT_EQ(1, VirtualNet::SendTo(socket, payload.data(), 1, true, VirtualNet::LocalIP(), 27914));

  VirtualNet::GetReadiness(socket, &readable, &writable, &exceptional);
  EXPECT_TRUE(readable);
}

TEST_F(VirtualNetTest, ListeningSocketBecomesReadableOnAPendingConnection)
{
  auto listener = VirtualNet::Create(VirtualNet::WII_SOCK_STREAM, 0);
  auto client = VirtualNet::Create(VirtualNet::WII_SOCK_STREAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(listener, 0, 27915));
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Listen(listener, 1));

  bool readable = true;
  VirtualNet::GetReadiness(listener, &readable, nullptr, nullptr);
  EXPECT_FALSE(readable);

  ASSERT_EQ(-SO_EINPROGRESS, VirtualNet::Connect(client, VirtualNet::LocalIP(), 27915));

  VirtualNet::GetReadiness(listener, &readable, nullptr, nullptr);
  EXPECT_TRUE(readable);
}

TEST_F(VirtualNetTest, SocketNamesReportTheVirtualAddress)
{
  auto socket = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  ASSERT_EQ(SO_SUCCESS, VirtualNet::Bind(socket, 0, 27916));

  u32 ip = 0;
  u16 port = 0;
  EXPECT_EQ(SO_SUCCESS, VirtualNet::GetSockName(socket, &ip, &port));
  // Bound to the wildcard, but the console must still be told an address its
  // peers can reach it on.
  EXPECT_EQ(VirtualNet::LocalIP(), ip);
  EXPECT_EQ(27916, port);

  EXPECT_EQ(-SO_ENOTCONN, VirtualNet::GetPeerName(socket, &ip, &port));
}

TEST_F(VirtualNetTest, SocketTypeIsAnsweredFromRealState)
{
  auto stream = VirtualNet::Create(VirtualNet::WII_SOCK_STREAM, 0);
  auto datagram = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);

  const auto read_type = [](const VirtualNet::SocketPtr& socket) {
    u8 value[20]{};
    u32 length = sizeof(value);
    EXPECT_EQ(SO_SUCCESS, VirtualNet::GetSockOpt(socket, 0xFFFF, 0x1008, value, &length));
    EXPECT_EQ(4u, length);
    // Written big-endian, because the console reads it straight out of memory.
    return (u32(value[0]) << 24) | (u32(value[1]) << 16) | (u32(value[2]) << 8) | value[3];
  };

  EXPECT_EQ(u32(VirtualNet::WII_SOCK_STREAM), read_type(stream));
  EXPECT_EQ(u32(VirtualNet::WII_SOCK_DGRAM), read_type(datagram));
}

TEST_F(VirtualNetTest, EchoRequestsAreAnswered)
{
  const std::vector<u8> payload = Bytes("ping");
  // Addressed to this console, so its own stack produces the reply.
  EXPECT_EQ(4, VirtualNet::Ping(VirtualNet::LocalIP(), 0x1234, payload.data(), 4, 500));
}

TEST_F(VirtualNetTest, SendingToNobodyFails)
{
  auto socket = VirtualNet::Create(VirtualNet::WII_SOCK_DGRAM, 0);
  const std::vector<u8> payload = Bytes("x");
  // 10.13.37.9 is on the subnet but nobody holds it.
  EXPECT_EQ(-SO_EHOSTUNREACH,
            VirtualNet::SendTo(socket, payload.data(), 1, true, 0x0A0D2509, 1234));
}

TEST_F(VirtualNetTest, StackIsInactiveOnceShutDown)
{
  VirtualNet::Shutdown();
  EXPECT_FALSE(VirtualNet::IsActive());
  // Brought back so TearDown has something consistent to take apart.
  VirtualNet::Initialize();
}
