// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Lobby/VirtualNet.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"

#include "Core/IOS/Network/Socket.h"
#include "Core/Lobby/LobbyNet.h"
#include "Core/Lobby/NetTrace.h"

namespace IOS::HLE::VirtualNet
{
namespace
{
using Lobby::Trace::FormatEndpoint;
using Lobby::Trace::FormatIP;

constexpr u8 PROTO_ICMP = 1;
constexpr u8 PROTO_TCP = 6;
constexpr u8 PROTO_UDP = 17;

// Control bits on a stream segment. There are no sequence numbers: the lobby
// link delivers reliably and in order between any two members, so a stream only
// needs the connection state machine, not a retransmit one.
enum : u8
{
  FLAG_SYN = 0x01,
  FLAG_ACK = 0x02,
  FLAG_FIN = 0x04,
  FLAG_RST = 0x08,
  FLAG_DATA = 0x10,
};

// An ICMP echo reply is an echo request with FLAG_ACK set. src_port carries the
// identifier and dst_port the sequence number, mirroring the fields the console
// puts in a real echo.
constexpr u8 FLAG_ECHO_REPLY = FLAG_ACK;

#pragma pack(push, 1)
struct Segment
{
  u8 protocol;
  u8 flags;
  u16 src_port;
  u16 dst_port;
  u16 length;
};
#pragma pack(pop)
static_assert(sizeof(Segment) == 8);

constexpr u16 EPHEMERAL_FIRST = 49152;
constexpr u16 EPHEMERAL_LAST = 65535;

// Beyond this a stream socket stops accepting more until the console reads. The
// console's own buffers are far smaller; this only exists so a peer that never
// reads cannot grow our memory without bound.
constexpr std::size_t MAX_STREAM_BYTES = 512 * 1024;
constexpr std::size_t MAX_QUEUED_DATAGRAMS = 512;

enum class TcpState
{
  Closed,
  SynSent,
  Listening,
  Established,
  // Local end has sent FIN; the peer may still be sending.
  FinSent,
};

const char* StateName(TcpState state)
{
  switch (state)
  {
  case TcpState::Closed:
    return "closed";
  case TcpState::SynSent:
    return "syn-sent";
  case TcpState::Listening:
    return "listening";
  case TcpState::Established:
    return "established";
  case TcpState::FinSent:
    return "fin-sent";
  }
  return "?";
}

std::string FlagNames(u8 flags)
{
  std::string result;
  const auto add = [&](u8 bit, const char* name) {
    if ((flags & bit) == 0)
      return;
    if (!result.empty())
      result += '|';
    result += name;
  };
  add(FLAG_SYN, "SYN");
  add(FLAG_ACK, "ACK");
  add(FLAG_FIN, "FIN");
  add(FLAG_RST, "RST");
  add(FLAG_DATA, "DATA");
  return result.empty() ? "-" : result;
}

const char* ProtocolName(u8 protocol)
{
  switch (protocol)
  {
  case PROTO_ICMP:
    return "icmp";
  case PROTO_TCP:
    return "tcp";
  case PROTO_UDP:
    return "udp";
  default:
    return "raw";
  }
}

struct Datagram
{
  u32 src_ip;
  u16 src_port;
  std::vector<u8> data;
};
}  // namespace

class Socket
{
public:
  s32 type = WII_SOCK_DGRAM;
  s32 protocol = 0;
  int id = 0;

  // 0 means "any", exactly as INADDR_ANY does.
  u32 local_ip = 0;
  u16 local_port = 0;
  u32 peer_ip = 0;
  u16 peer_port = 0;

  bool bound = false;
  // A datagram socket that has had connect() called on it, which filters what
  // it receives and lets send() be used without an address.
  bool has_peer = false;
  bool closed = false;
  bool shutdown_read = false;
  bool shutdown_write = false;
  // The peer sent FIN; reads drain what is left and then return 0.
  bool peer_finished = false;

  TcpState tcp = TcpState::Closed;
  std::size_t backlog = 0;

  // Reported once by SO_ERROR or by the operation that trips over it.
  s32 pending_error = 0;

  std::deque<Datagram> datagrams;  // datagram and raw sockets
  std::vector<u8> stream;          // stream sockets
  std::deque<SocketPtr> pending_accepts;

  // Remembered so a getsockopt after a setsockopt gives back what was set.
  // None of them change how a virtual link behaves.
  std::map<u64, std::vector<u8>> options;

  u64 bytes_in = 0;
  u64 bytes_out = 0;
  u64 packets_in = 0;
  u64 packets_out = 0;
  u64 drops = 0;

  std::string Describe() const
  {
    std::string text = fmt::format("#{} {} {}", id, ProtocolName(WireProtocol()),
                                   FormatEndpoint(local_ip, local_port));
    if (has_peer || tcp != TcpState::Closed)
      text += fmt::format(" -> {}", FormatEndpoint(peer_ip, peer_port));
    if (type == WII_SOCK_STREAM)
      text += fmt::format(" [{}]", StateName(tcp));
    return text;
  }

  u8 WireProtocol() const
  {
    switch (type)
    {
    case WII_SOCK_STREAM:
      return PROTO_TCP;
    case WII_SOCK_DGRAM:
      return PROTO_UDP;
    default:
      return protocol != 0 ? static_cast<u8>(protocol) : PROTO_ICMP;
    }
  }
};

namespace
{
struct PendingPing
{
  u32 peer_ip;
  u16 identifier;
  bool answered = false;
  std::vector<u8> reply;
};

struct NetState
{
  // Recursive because delivering a frame to a socket on this console can make
  // the stack answer immediately - a SYN produces a SYN|ACK, an echo request
  // produces a reply - and that answer runs through the same send path that is
  // already holding the lock.
  std::recursive_mutex mutex;

  std::vector<std::weak_ptr<Socket>> sockets;
  std::map<u16, PendingPing> pings;

  int next_socket_id = 1;
  // Read without the lock by IsActive(), which every integration point calls on
  // paths that must not block.
  std::atomic<bool> attached{false};

  // Counters for the trace, so a session can be summarised without reading
  // every line of it.
  u64 frames_in = 0;
  u64 frames_out = 0;
  u64 frames_dropped = 0;
};

NetState s_stack;

std::vector<SocketPtr> LiveSockets()
{
  std::vector<SocketPtr> live;
  live.reserve(s_stack.sockets.size());

  std::erase_if(s_stack.sockets, [&live](const std::weak_ptr<Socket>& weak) {
    if (SocketPtr socket = weak.lock())
    {
      live.push_back(std::move(socket));
      return false;
    }
    return true;
  });

  return live;
}

bool PortInUse(u8 protocol, u16 port)
{
  for (const SocketPtr& socket : LiveSockets())
  {
    if (socket->WireProtocol() == protocol && socket->local_port == port && !socket->closed)
      return true;
  }
  return false;
}

u16 AllocateEphemeralPort(u8 protocol)
{
  for (u32 port = EPHEMERAL_FIRST; port <= EPHEMERAL_LAST; ++port)
  {
    if (!PortInUse(protocol, static_cast<u16>(port)))
      return static_cast<u16>(port);
  }
  return 0;
}

// Gives a socket a source port if it does not have one, the way sendto() and
// connect() implicitly bind on a real stack.
bool EnsureBound(const SocketPtr& socket)
{
  if (socket->local_port != 0)
    return true;

  const u16 port = AllocateEphemeralPort(socket->WireProtocol());
  if (port == 0)
  {
    VNET_TRACE(Error, "{}: no ephemeral port left", socket->Describe());
    return false;
  }

  socket->local_port = port;
  socket->bound = true;
  VNET_TRACE(Socket, "{}: implicitly bound to port {}", socket->Describe(), port);
  return true;
}

void DeliverSegment(u32 src_ip, u32 dst_ip, const u8* frame, std::size_t length);

// Everything that means "everyone on this segment".
//
// Only the subnet-directed form, 10.13.37.255, is an address the lobby knows
// how to route. The others have to be recognised here or they would be looked
// up as ordinary destinations, belong to nobody, and be dropped - which is
// exactly what a peer-discovery broadcast looks like when it silently fails.
//
// 255.255.255.255 is the one that matters in practice: software looking for
// neighbours on an unknown network reaches for the limited broadcast rather
// than working out the subnet-directed address for the interface it is on.
bool IsSegmentWide(u32 ip)
{
  constexpr u32 LIMITED_BROADCAST = 0xFFFFFFFF;

  if (ip == LIMITED_BROADCAST || ip == Lobby::BROADCAST_IP)
    return true;
  // 224.0.0.0/4. Nothing on this subnet is going to do multicast routing, so
  // the honest approximation of a multicast group is everyone.
  if ((ip & 0xF0000000) == 0xE0000000)
    return true;
  // A subnet-directed broadcast for a netmask the console decided on itself.
  return (ip & Lobby::NETMASK) == Lobby::SUBNET && (ip & ~Lobby::NETMASK) == ~Lobby::NETMASK;
}

// Puts a segment on the wire, on the loopback path, or both. Caller holds the
// lock.
bool Transmit(u8 protocol, u8 flags, u16 src_port, u32 dst_ip, u16 dst_port, const u8* data,
              u32 length)
{
  const u32 src_ip = LocalIP();
  if (src_ip == 0)
  {
    VNET_TRACE(Error, "cannot send: this console has no address");
    ++s_stack.frames_dropped;
    return false;
  }

  std::vector<u8> frame(sizeof(Segment) + length);
  Segment segment{};
  segment.protocol = protocol;
  segment.flags = flags;
  segment.src_port = Common::swap16(src_port);
  segment.dst_port = Common::swap16(dst_port);
  segment.length = Common::swap16(static_cast<u16>(length));
  std::memcpy(frame.data(), &segment, sizeof(segment));
  if (length != 0)
    std::memcpy(frame.data() + sizeof(segment), data, length);

  // The lobby only routes to addresses it has handed out, plus the one
  // broadcast address it recognises. Every other way of saying "everyone"
  // becomes that one here.
  const bool to_broadcast = IsSegmentWide(dst_ip);
  const u32 routed_ip = to_broadcast ? Lobby::BROADCAST_IP : dst_ip;
  const bool to_self = routed_ip == src_ip;

  VNET_TRACE(Tx, "{} {} -> {} {} {} bytes", ProtocolName(protocol),
             FormatEndpoint(src_ip, src_port), FormatEndpoint(dst_ip, dst_port), FlagNames(flags),
             length);
  if (to_broadcast && dst_ip != Lobby::BROADCAST_IP)
    VNET_TRACE(Route, "{} means everyone; routing as {}", FormatIP(dst_ip),
               FormatIP(Lobby::BROADCAST_IP));

  bool sent = false;
  if (!to_self)
  {
    sent = Lobby::SendPayload(routed_ip, frame.data(), frame.size());
    if (sent)
      ++s_stack.frames_out;
    else
      ++s_stack.frames_dropped;
  }

  // A real console on a real segment hears its own broadcasts, and anything it
  // addresses to itself never reaches the wire at all. Both are reproduced
  // here, because software that discovers peers by broadcasting relies on
  // filtering itself out by source address - and it can only do that if the
  // packet arrives.
  if (to_self || to_broadcast)
  {
    VNET_TRACE(Route, "looping back {} -> {}", FormatIP(src_ip), FormatIP(routed_ip));
    DeliverSegment(src_ip, routed_ip, frame.data(), frame.size());
    sent = true;
  }

  return sent;
}

bool AcceptsAddress(const SocketPtr& socket, u32 dst_ip)
{
  if (socket->local_ip == 0)
    return true;
  if (socket->local_ip == dst_ip)
    return true;
  // Bound to a specific address, but a broadcast is for everyone on the subnet.
  return dst_ip == Lobby::BROADCAST_IP;
}

SocketPtr FindStream(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port)
{
  for (const SocketPtr& socket : LiveSockets())
  {
    if (socket->type != WII_SOCK_STREAM || socket->closed)
      continue;
    if (socket->local_port != dst_port || !AcceptsAddress(socket, dst_ip))
      continue;
    if (socket->tcp == TcpState::Listening)
      continue;
    if (socket->peer_ip == src_ip && socket->peer_port == src_port)
      return socket;
  }
  return nullptr;
}

SocketPtr FindListener(u32 dst_ip, u16 dst_port)
{
  for (const SocketPtr& socket : LiveSockets())
  {
    if (socket->type != WII_SOCK_STREAM || socket->closed)
      continue;
    if (socket->tcp != TcpState::Listening)
      continue;
    if (socket->local_port == dst_port && AcceptsAddress(socket, dst_ip))
      return socket;
  }
  return nullptr;
}

std::vector<SocketPtr> FindDatagramReceivers(u8 protocol, u32 src_ip, u16 src_port, u32 dst_ip,
                                             u16 dst_port)
{
  std::vector<SocketPtr> matches;
  for (const SocketPtr& socket : LiveSockets())
  {
    if (socket->closed || socket->type == WII_SOCK_STREAM)
      continue;
    if (socket->WireProtocol() != protocol)
      continue;
    // A raw socket takes everything of its protocol regardless of port.
    if (socket->type != WII_SOCK_RAW && socket->local_port != dst_port)
      continue;
    if (!AcceptsAddress(socket, dst_ip))
      continue;
    // connect() on a datagram socket narrows it to one correspondent.
    if (socket->has_peer && (socket->peer_ip != src_ip || socket->peer_port != src_port))
      continue;
    if (socket->shutdown_read)
      continue;
    matches.push_back(socket);
  }
  return matches;
}

void SendReset(u32 dst_ip, u16 dst_port, u16 src_port)
{
  VNET_TRACE(Stack, "resetting {}", FormatEndpoint(dst_ip, dst_port));
  Transmit(PROTO_TCP, FLAG_RST, src_port, dst_ip, dst_port, nullptr, 0);
}

void HandleStreamSegment(u32 src_ip, u32 dst_ip, const Segment& segment, const u8* payload,
                         std::size_t length)
{
  const u16 src_port = Common::swap16(segment.src_port);
  const u16 dst_port = Common::swap16(segment.dst_port);
  const u8 flags = segment.flags;

  // A connection request looks for something listening; everything else must
  // match a connection that already exists.
  if ((flags & FLAG_SYN) != 0 && (flags & FLAG_ACK) == 0)
  {
    SocketPtr listener = FindListener(dst_ip, dst_port);
    if (!listener)
    {
      VNET_TRACE(Stack, "no listener on port {}; refusing {}", dst_port,
                 FormatEndpoint(src_ip, src_port));
      SendReset(src_ip, src_port, dst_port);
      ++s_stack.frames_dropped;
      return;
    }
    if (listener->pending_accepts.size() >= listener->backlog)
    {
      VNET_TRACE(Stack, "{}: backlog of {} is full; refusing {}", listener->Describe(),
                 listener->backlog, FormatEndpoint(src_ip, src_port));
      SendReset(src_ip, src_port, dst_port);
      ++s_stack.frames_dropped;
      return;
    }

    auto accepted = std::make_shared<Socket>();
    accepted->id = s_stack.next_socket_id++;
    accepted->type = WII_SOCK_STREAM;
    accepted->local_ip = LocalIP();
    accepted->local_port = dst_port;
    accepted->peer_ip = src_ip;
    accepted->peer_port = src_port;
    accepted->bound = true;
    accepted->has_peer = true;
    accepted->tcp = TcpState::Established;
    s_stack.sockets.push_back(accepted);
    listener->pending_accepts.push_back(accepted);

    VNET_TRACE(Stack, "{}: accepted {} onto {}", listener->Describe(),
               FormatEndpoint(src_ip, src_port), accepted->Describe());
    Transmit(PROTO_TCP, FLAG_SYN | FLAG_ACK, dst_port, src_ip, src_port, nullptr, 0);
    return;
  }

  SocketPtr socket = FindStream(src_ip, src_port, dst_ip, dst_port);
  if (!socket)
  {
    VNET_TRACE(Stack, "{} for an unknown connection {} -> {}; ignored", FlagNames(flags),
               FormatEndpoint(src_ip, src_port), FormatEndpoint(dst_ip, dst_port));
    ++s_stack.frames_dropped;
    // Answering a stray RST with another RST is how loops start.
    if ((flags & FLAG_RST) == 0)
      SendReset(src_ip, src_port, dst_port);
    return;
  }

  if ((flags & FLAG_RST) != 0)
  {
    VNET_TRACE(Stack, "{}: reset by the peer", socket->Describe());
    socket->pending_error = socket->tcp == TcpState::SynSent ? SO_ECONNREFUSED : SO_ECONNRESET;
    socket->tcp = TcpState::Closed;
    socket->peer_finished = true;
    return;
  }

  if ((flags & (FLAG_SYN | FLAG_ACK)) == (FLAG_SYN | FLAG_ACK))
  {
    if (socket->tcp != TcpState::SynSent)
    {
      VNET_TRACE(Stack, "{}: unexpected SYN|ACK while {}", socket->Describe(),
                 StateName(socket->tcp));
      return;
    }
    socket->tcp = TcpState::Established;
    VNET_TRACE(Stack, "{}: connected", socket->Describe());
    return;
  }

  if ((flags & FLAG_DATA) != 0 && length != 0)
  {
    if (socket->shutdown_read)
    {
      VNET_TRACE(Stack, "{}: {} bytes discarded, reading is shut down", socket->Describe(),
                 length);
      ++socket->drops;
      return;
    }
    if (socket->stream.size() + length > MAX_STREAM_BYTES)
    {
      VNET_TRACE(Error, "{}: receive buffer full at {} bytes; dropping {}", socket->Describe(),
                 socket->stream.size(), length);
      ++socket->drops;
      ++s_stack.frames_dropped;
      return;
    }
    socket->stream.insert(socket->stream.end(), payload, payload + length);
    socket->bytes_in += length;
    ++socket->packets_in;
    VNET_TRACE(Stack, "{}: {} bytes in, {} buffered", socket->Describe(), length,
               socket->stream.size());
  }

  if ((flags & FLAG_FIN) != 0)
  {
    socket->peer_finished = true;
    VNET_TRACE(Stack, "{}: peer finished sending", socket->Describe());
  }
}

void HandleIcmpSegment(u32 src_ip, u32 dst_ip, const Segment& segment, const u8* payload,
                       std::size_t length)
{
  const u16 identifier = Common::swap16(segment.src_port);
  const u16 sequence = Common::swap16(segment.dst_port);

  if ((segment.flags & FLAG_ECHO_REPLY) != 0)
  {
    const auto it = s_stack.pings.find(identifier);
    if (it == s_stack.pings.end())
    {
      VNET_TRACE(Stack, "echo reply id {} from {} matches no outstanding ping", identifier,
                 FormatIP(src_ip));
      return;
    }
    it->second.answered = true;
    it->second.reply.assign(payload, payload + length);
    VNET_TRACE(Stack, "echo reply id {} seq {} from {}", identifier, sequence, FormatIP(src_ip));
    return;
  }

  VNET_TRACE(Stack, "echo request id {} seq {} from {}; replying", identifier, sequence,
             FormatIP(src_ip));
  Transmit(PROTO_ICMP, FLAG_ECHO_REPLY, identifier, src_ip, sequence, payload,
           static_cast<u32>(length));

  // A raw socket, if the console has one open, still sees the request.
  for (const SocketPtr& socket : FindDatagramReceivers(PROTO_ICMP, src_ip, identifier, dst_ip,
                                                       sequence))
  {
    socket->datagrams.push_back(Datagram{src_ip, identifier, {payload, payload + length}});
    ++socket->packets_in;
    socket->bytes_in += length;
  }
}

void HandleDatagramSegment(u32 src_ip, u32 dst_ip, const Segment& segment, const u8* payload,
                           std::size_t length)
{
  const u16 src_port = Common::swap16(segment.src_port);
  const u16 dst_port = Common::swap16(segment.dst_port);

  const std::vector<SocketPtr> receivers =
      FindDatagramReceivers(segment.protocol, src_ip, src_port, dst_ip, dst_port);

  if (receivers.empty())
  {
    VNET_TRACE(Route, "drop {} {} -> {}: nothing is bound to that port",
               ProtocolName(segment.protocol), FormatEndpoint(src_ip, src_port),
               FormatEndpoint(dst_ip, dst_port));
    ++s_stack.frames_dropped;
    return;
  }

  for (const SocketPtr& socket : receivers)
  {
    if (socket->datagrams.size() >= MAX_QUEUED_DATAGRAMS)
    {
      VNET_TRACE(Error, "{}: {} datagrams queued; dropping the oldest", socket->Describe(),
                 socket->datagrams.size());
      socket->datagrams.pop_front();
      ++socket->drops;
    }

    socket->datagrams.push_back(Datagram{src_ip, src_port, {payload, payload + length}});
    ++socket->packets_in;
    socket->bytes_in += length;
    VNET_TRACE(Route, "{}: queued {} bytes from {}, {} waiting", socket->Describe(), length,
               FormatEndpoint(src_ip, src_port), socket->datagrams.size());
  }
}

void DeliverSegment(u32 src_ip, u32 dst_ip, const u8* frame, std::size_t size)
{
  if (size < sizeof(Segment))
  {
    VNET_TRACE(Error, "runt segment of {} bytes from {}", size, FormatIP(src_ip));
    ++s_stack.frames_dropped;
    return;
  }

  Segment segment{};
  std::memcpy(&segment, frame, sizeof(segment));

  const std::size_t claimed = Common::swap16(segment.length);
  if (claimed + sizeof(Segment) > size)
  {
    VNET_TRACE(Error, "segment from {} claims {} payload bytes but carries {}", FormatIP(src_ip),
               claimed, size - sizeof(Segment));
    ++s_stack.frames_dropped;
    return;
  }

  const u8* const payload = frame + sizeof(Segment);
  ++s_stack.frames_in;

  VNET_TRACE_BYTES(Rx, payload, claimed, "{} {} -> {} {}", ProtocolName(segment.protocol),
                   FormatEndpoint(src_ip, Common::swap16(segment.src_port)),
                   FormatEndpoint(dst_ip, Common::swap16(segment.dst_port)),
                   FlagNames(segment.flags));

  switch (segment.protocol)
  {
  case PROTO_TCP:
    HandleStreamSegment(src_ip, dst_ip, segment, payload, claimed);
    break;
  case PROTO_ICMP:
    HandleIcmpSegment(src_ip, dst_ip, segment, payload, claimed);
    break;
  default:
    // UDP and anything else the console decides to send. Unrecognised
    // protocols are carried rather than refused, so this stack is not a
    // whitelist of what one game happens to use.
    HandleDatagramSegment(src_ip, dst_ip, segment, payload, claimed);
    break;
  }
}

void OnLobbyPayload(u32 src_ip, u32 dst_ip, const u8* data, std::size_t length)
{
  std::lock_guard lock{s_stack.mutex};
  DeliverSegment(src_ip, dst_ip, data, length);
}
}  // namespace

void Initialize()
{
  std::lock_guard lock{s_stack.mutex};
  if (s_stack.attached.load())
    return;

  s_stack.sockets.clear();
  s_stack.pings.clear();
  s_stack.frames_in = 0;
  s_stack.frames_out = 0;
  s_stack.frames_dropped = 0;
  s_stack.attached.store(true);

  Lobby::SetPayloadHandler(&OnLobbyPayload);
  VNET_TRACE(Iface, "virtual network up: {}", DescribeInterface());
}

void Shutdown()
{
  Lobby::SetPayloadHandler(nullptr);

  std::lock_guard lock{s_stack.mutex};
  if (!s_stack.attached.exchange(false))
    return;

  VNET_TRACE(Iface, "virtual network down after {} frames in, {} out, {} dropped", s_stack.frames_in,
             s_stack.frames_out, s_stack.frames_dropped);

  for (const SocketPtr& socket : LiveSockets())
  {
    socket->closed = true;
    socket->datagrams.clear();
    socket->stream.clear();
    socket->pending_accepts.clear();
  }
  s_stack.sockets.clear();
  s_stack.pings.clear();
}

bool IsActive()
{
  // All three matter. Without an address there is nothing to put in a source
  // field; without the handler attached, anything sent would be answered by
  // nobody and anything arriving would be discarded before it reached a socket.
  return s_stack.attached.load() && Lobby::IsActive() && Lobby::GetVirtualIP() != 0;
}

u32 LocalIP()
{
  return Lobby::GetVirtualIP();
}

u32 Netmask()
{
  return Lobby::NETMASK;
}

u32 BroadcastIP()
{
  return Lobby::BROADCAST_IP;
}

u32 Gateway()
{
  return Lobby::HOST_IP;
}

std::string DescribeInterface()
{
  return fmt::format("{}/{} broadcast {} gateway {}", FormatIP(LocalIP()), FormatIP(Netmask()),
                     FormatIP(BroadcastIP()), FormatIP(Gateway()));
}

SocketPtr Create(s32 type, s32 protocol)
{
  if (type != WII_SOCK_STREAM && type != WII_SOCK_DGRAM && type != WII_SOCK_RAW)
  {
    VNET_TRACE(Error, "socket type {} is not supported", type);
    return nullptr;
  }

  std::lock_guard lock{s_stack.mutex};

  auto socket = std::make_shared<Socket>();
  socket->id = s_stack.next_socket_id++;
  socket->type = type;
  socket->protocol = protocol;
  s_stack.sockets.push_back(socket);

  VNET_TRACE(Socket, "{}: opened", socket->Describe());
  return socket;
}

s32 Bind(const SocketPtr& socket, u32 ip, u16 port)
{
  if (!socket)
    return -SO_EBADF;

  std::lock_guard lock{s_stack.mutex};

  if (socket->bound)
  {
    VNET_TRACE(Error, "{}: already bound", socket->Describe());
    return -SO_EINVAL;
  }

  // The console may bind to its own address, to any, or to a broadcast address;
  // anything else belongs to somebody on the subnet and is not ours to answer
  // for.
  if (ip != 0 && ip != LocalIP() && !IsSegmentWide(ip))
  {
    VNET_TRACE(Error, "{}: cannot bind to {}, this console is {}", socket->Describe(),
               FormatIP(ip), FormatIP(LocalIP()));
    return -SO_EADDRNOTAVAIL;
  }

  if (port == 0)
  {
    port = AllocateEphemeralPort(socket->WireProtocol());
    if (port == 0)
      return -SO_EADDRINUSE;
  }
  else if (PortInUse(socket->WireProtocol(), port))
  {
    // SO_REUSEADDR is honoured because the console sets it on its discovery
    // socket, and refusing would break rebinding after a reconnect.
    const bool reuse = socket->options.contains(0xFFFF00000004ULL);
    if (!reuse)
    {
      VNET_TRACE(Error, "{}: port {} is already in use", socket->Describe(), port);
      return -SO_EADDRINUSE;
    }
    VNET_TRACE(Socket, "{}: port {} reused (SO_REUSEADDR)", socket->Describe(), port);
  }

  socket->local_ip = ip;
  socket->local_port = port;
  socket->bound = true;

  VNET_TRACE(Socket, "{}: bound", socket->Describe());
  return SO_SUCCESS;
}

s32 Connect(const SocketPtr& socket, u32 ip, u16 port)
{
  if (!socket)
    return -SO_EBADF;

  std::lock_guard lock{s_stack.mutex};

  if (socket->type != WII_SOCK_STREAM)
  {
    // On a datagram socket connect() only records the correspondent.
    socket->peer_ip = ip;
    socket->peer_port = port;
    socket->has_peer = true;
    if (!EnsureBound(socket))
      return -SO_EADDRINUSE;
    VNET_TRACE(Socket, "{}: correspondent set", socket->Describe());
    return SO_SUCCESS;
  }

  switch (socket->tcp)
  {
  case TcpState::Established:
    // IOS retries connect() until it stops saying "in progress"; EISCONN is
    // how it learns the handshake finished.
    return -SO_EISCONN;

  case TcpState::SynSent:
    if (socket->pending_error != 0)
    {
      const s32 error = socket->pending_error;
      socket->pending_error = 0;
      socket->tcp = TcpState::Closed;
      VNET_TRACE(Socket, "{}: connect failed with {}", socket->Describe(), error);
      return -error;
    }
    return -SO_EALREADY;

  case TcpState::Listening:
    return -SO_EINVAL;

  case TcpState::Closed:
    break;

  case TcpState::FinSent:
    return -SO_ENOTCONN;
  }

  if (socket->pending_error != 0)
  {
    const s32 error = socket->pending_error;
    socket->pending_error = 0;
    return -error;
  }

  if (!EnsureBound(socket))
    return -SO_EADDRINUSE;

  socket->peer_ip = ip;
  socket->peer_port = port;
  socket->has_peer = true;
  socket->tcp = TcpState::SynSent;

  VNET_TRACE(Socket, "{}: connecting", socket->Describe());
  if (!Transmit(PROTO_TCP, FLAG_SYN, socket->local_port, ip, port, nullptr, 0))
  {
    socket->tcp = TcpState::Closed;
    return -SO_EHOSTUNREACH;
  }
  return -SO_EINPROGRESS;
}

s32 Listen(const SocketPtr& socket, s32 backlog)
{
  if (!socket)
    return -SO_EBADF;

  std::lock_guard lock{s_stack.mutex};

  if (socket->type != WII_SOCK_STREAM)
    return -SO_EOPNOTSUPP;
  if (!EnsureBound(socket))
    return -SO_EADDRINUSE;

  socket->tcp = TcpState::Listening;
  socket->backlog = backlog <= 0 ? 1 : static_cast<std::size_t>(backlog);

  VNET_TRACE(Socket, "{}: listening, backlog {}", socket->Describe(), socket->backlog);
  return SO_SUCCESS;
}

SocketPtr Accept(const SocketPtr& socket, u32* from_ip, u16* from_port, s32* error)
{
  if (!socket)
  {
    *error = -SO_EBADF;
    return nullptr;
  }

  std::lock_guard lock{s_stack.mutex};

  if (socket->tcp != TcpState::Listening)
  {
    *error = -SO_EINVAL;
    return nullptr;
  }
  if (socket->pending_accepts.empty())
  {
    *error = -SO_EAGAIN;
    return nullptr;
  }

  SocketPtr accepted = socket->pending_accepts.front();
  socket->pending_accepts.pop_front();

  if (from_ip)
    *from_ip = accepted->peer_ip;
  if (from_port)
    *from_port = accepted->peer_port;
  *error = SO_SUCCESS;

  VNET_TRACE(Socket, "{}: handed to the console", accepted->Describe());
  return accepted;
}

s32 SendTo(const SocketPtr& socket, const u8* data, u32 length, bool has_destination, u32 ip,
           u16 port)
{
  if (!socket)
    return -SO_EBADF;

  std::lock_guard lock{s_stack.mutex};

  if (socket->closed)
    return -SO_EBADF;
  if (socket->shutdown_write)
    return -SO_ENOTCONN;

  if (socket->pending_error != 0)
  {
    const s32 error = socket->pending_error;
    socket->pending_error = 0;
    return -error;
  }

  u32 dst_ip = ip;
  u16 dst_port = port;
  if (!has_destination)
  {
    if (!socket->has_peer)
    {
      VNET_TRACE(Error, "{}: send with no destination and no correspondent", socket->Describe());
      return -SO_EDESTADDRREQ;
    }
    dst_ip = socket->peer_ip;
    dst_port = socket->peer_port;
  }

  if (socket->type == WII_SOCK_STREAM)
  {
    if (socket->tcp == TcpState::SynSent)
      return -SO_EAGAIN;
    if (socket->tcp != TcpState::Established)
      return -SO_ENOTCONN;
    dst_ip = socket->peer_ip;
    dst_port = socket->peer_port;
  }
  else if (!EnsureBound(socket))
  {
    return -SO_EADDRINUSE;
  }

  if (length > Lobby::MAX_PAYLOAD - sizeof(Segment))
  {
    VNET_TRACE(Error, "{}: {} bytes is over the {} byte limit", socket->Describe(), length,
               Lobby::MAX_PAYLOAD - sizeof(Segment));
    return -SO_EMSGSIZE;
  }

  const u8 flags = socket->type == WII_SOCK_STREAM ? FLAG_DATA : 0;
  if (!Transmit(socket->WireProtocol(), flags, socket->local_port, dst_ip, dst_port, data, length))
  {
    ++socket->drops;
    return -SO_EHOSTUNREACH;
  }

  socket->bytes_out += length;
  ++socket->packets_out;
  return static_cast<s32>(length);
}

s32 RecvFrom(const SocketPtr& socket, u8* data, u32 length, bool peek, bool want_source,
             u32* from_ip, u16* from_port)
{
  if (!socket)
    return -SO_EBADF;

  std::lock_guard lock{s_stack.mutex};

  if (socket->closed)
    return -SO_EBADF;

  if (socket->type == WII_SOCK_STREAM)
  {
    if (socket->tcp == TcpState::SynSent)
      return -SO_EAGAIN;

    if (socket->stream.empty())
    {
      if (socket->pending_error != 0)
      {
        const s32 error = socket->pending_error;
        socket->pending_error = 0;
        return -error;
      }
      // A clean end of stream is zero bytes, not an error.
      if (socket->peer_finished || socket->shutdown_read)
        return 0;
      if (socket->tcp != TcpState::Established)
        return -SO_ENOTCONN;
      return -SO_EAGAIN;
    }

    const u32 taken = std::min<u32>(length, static_cast<u32>(socket->stream.size()));
    if (data != nullptr && taken != 0)
      std::memcpy(data, socket->stream.data(), taken);
    if (!peek)
      socket->stream.erase(socket->stream.begin(), socket->stream.begin() + taken);

    if (want_source)
    {
      if (from_ip)
        *from_ip = socket->peer_ip;
      if (from_port)
        *from_port = socket->peer_port;
    }

    VNET_TRACE(Stack, "{}: {} bytes to the console{}, {} left", socket->Describe(), taken,
               peek ? " (peek)" : "", socket->stream.size());
    return static_cast<s32>(taken);
  }

  if (socket->datagrams.empty())
  {
    if (socket->pending_error != 0)
    {
      const s32 error = socket->pending_error;
      socket->pending_error = 0;
      return -error;
    }
    if (socket->shutdown_read)
      return 0;
    return -SO_EAGAIN;
  }

  const Datagram& datagram = socket->datagrams.front();
  // Datagrams are not streams: whatever does not fit is discarded with the
  // rest of the packet, as a real recvfrom would.
  const u32 taken = std::min<u32>(length, static_cast<u32>(datagram.data.size()));
  if (data != nullptr && taken != 0)
    std::memcpy(data, datagram.data.data(), taken);

  if (want_source)
  {
    if (from_ip)
      *from_ip = datagram.src_ip;
    if (from_port)
      *from_port = datagram.src_port;
  }

  VNET_TRACE(Route, "{}: {} of {} bytes to the console from {}{}", socket->Describe(), taken,
             datagram.data.size(), FormatEndpoint(datagram.src_ip, datagram.src_port),
             peek ? " (peek)" : "");

  if (!peek)
    socket->datagrams.pop_front();
  return static_cast<s32>(taken);
}

s32 Shutdown(const SocketPtr& socket, u32 how)
{
  if (!socket)
    return -SO_EBADF;
  if (how > 2)
    return -SO_EINVAL;

  std::lock_guard lock{s_stack.mutex};

  const bool stop_read = how == 0 || how == 2;
  const bool stop_write = how == 1 || how == 2;

  // The console does nothing and returns success for datagram sockets, and
  // this matches that.
  if (socket->type != WII_SOCK_STREAM)
    return SO_SUCCESS;

  if (stop_read)
    socket->shutdown_read = true;

  if (stop_write && !socket->shutdown_write)
  {
    socket->shutdown_write = true;
    if (socket->tcp == TcpState::Established)
    {
      Transmit(PROTO_TCP, FLAG_FIN, socket->local_port, socket->peer_ip, socket->peer_port, nullptr,
               0);
      socket->tcp = TcpState::FinSent;
    }
  }

  VNET_TRACE(Socket, "{}: shutdown({})", socket->Describe(), how);
  return SO_SUCCESS;
}

s32 Close(const SocketPtr& socket)
{
  if (!socket)
    return -SO_EBADF;

  std::lock_guard lock{s_stack.mutex};

  if (socket->closed)
    return -SO_EBADF;

  if (socket->type == WII_SOCK_STREAM && socket->tcp == TcpState::Established)
  {
    Transmit(PROTO_TCP, FLAG_FIN, socket->local_port, socket->peer_ip, socket->peer_port, nullptr,
             0);
  }

  // Anything accepted but never handed to the console is refused rather than
  // left hanging on the peer.
  for (const SocketPtr& pending : socket->pending_accepts)
  {
    Transmit(PROTO_TCP, FLAG_RST, pending->local_port, pending->peer_ip, pending->peer_port,
             nullptr, 0);
    pending->closed = true;
  }
  socket->pending_accepts.clear();

  VNET_TRACE(Socket, "{}: closed after {} in / {} out over {} / {} packets, {} dropped",
             socket->Describe(), socket->bytes_in, socket->bytes_out, socket->packets_in,
             socket->packets_out, socket->drops);

  socket->closed = true;
  socket->tcp = TcpState::Closed;
  socket->datagrams.clear();
  socket->stream.clear();
  return SO_SUCCESS;
}

s32 GetSockName(const SocketPtr& socket, u32* ip, u16* port)
{
  if (!socket)
    return -SO_EBADF;

  std::lock_guard lock{s_stack.mutex};
  // An unbound or wildcard-bound socket still reports this console's address,
  // which is what the console expects to see and what its peers will use.
  if (ip)
    *ip = socket->local_ip != 0 ? socket->local_ip : LocalIP();
  if (port)
    *port = socket->local_port;
  return SO_SUCCESS;
}

s32 GetPeerName(const SocketPtr& socket, u32* ip, u16* port)
{
  if (!socket)
    return -SO_EBADF;

  std::lock_guard lock{s_stack.mutex};
  if (!socket->has_peer)
    return -SO_ENOTCONN;
  if (socket->type == WII_SOCK_STREAM && socket->tcp != TcpState::Established)
    return -SO_ENOTCONN;
  if (ip)
    *ip = socket->peer_ip;
  if (port)
    *port = socket->peer_port;
  return SO_SUCCESS;
}

namespace
{
constexpr u64 OptionKey(u32 level, u32 optname)
{
  return (static_cast<u64>(level) << 32) | optname;
}

// The Wii's SO_TYPE and SO_ERROR, which have to be answered from real state
// rather than from whatever was last set.
constexpr u32 WII_SOL_SOCKET = 0xFFFF;
constexpr u32 WII_SO_TYPE = 0x1008;
constexpr u32 WII_SO_ERROR = 0x1009;
}  // namespace

s32 GetSockOpt(const SocketPtr& socket, u32 level, u32 optname, u8* value, u32* length)
{
  if (!socket)
    return -SO_EBADF;

  std::lock_guard lock{s_stack.mutex};

  const auto write_u32 = [&](u32 result) {
    const u32 swapped = Common::swap32(result);
    const u32 room = std::min<u32>(*length, sizeof(swapped));
    std::memcpy(value, &swapped, room);
    *length = sizeof(swapped);
  };

  if (level == WII_SOL_SOCKET && optname == WII_SO_TYPE)
  {
    write_u32(static_cast<u32>(socket->type));
    return SO_SUCCESS;
  }

  if (level == WII_SOL_SOCKET && optname == WII_SO_ERROR)
  {
    // Negated, matching the sign every other SO_* value crosses this boundary
    // with. Reading it clears it, as on a real stack.
    write_u32(static_cast<u32>(-socket->pending_error));
    socket->pending_error = 0;
    return SO_SUCCESS;
  }

  const auto it = socket->options.find(OptionKey(level, optname));
  if (it == socket->options.end())
  {
    // Not previously set. Zero is what an untouched option reads as.
    write_u32(0);
    return SO_SUCCESS;
  }

  const u32 room = std::min<u32>(*length, static_cast<u32>(it->second.size()));
  std::memcpy(value, it->second.data(), room);
  *length = room;
  return SO_SUCCESS;
}

s32 SetSockOpt(const SocketPtr& socket, u32 level, u32 optname, const u8* value, u32 length)
{
  if (!socket)
    return -SO_EBADF;

  std::lock_guard lock{s_stack.mutex};
  socket->options[OptionKey(level, optname)].assign(value, value + length);

  // Nothing here changes behaviour: buffer sizes, keepalives and Nagle have no
  // meaning on a link that is already reliable and in order. They are recorded
  // so a read-back matches, and traced so an unexpected one is visible.
  VNET_TRACE(Socket, "{}: setsockopt level {:#x} option {:#x}, {} bytes (recorded, no effect)",
             socket->Describe(), level, optname, length);
  return SO_SUCCESS;
}

void GetReadiness(const SocketPtr& socket, bool* readable, bool* writable, bool* exceptional)
{
  bool r = false;
  bool w = false;
  bool e = false;

  if (socket)
  {
    std::lock_guard lock{s_stack.mutex};

    if (socket->closed)
    {
      e = true;
    }
    else if (socket->type == WII_SOCK_STREAM)
    {
      // Listening sockets report a waiting connection as readable, which is
      // what makes poll() usable before accept().
      r = socket->tcp == TcpState::Listening ?
              !socket->pending_accepts.empty() :
              !socket->stream.empty() || socket->peer_finished || socket->pending_error != 0;
      w = socket->tcp == TcpState::Established && !socket->shutdown_write;
      e = socket->pending_error != 0;
    }
    else
    {
      r = !socket->datagrams.empty() || socket->pending_error != 0;
      // Nothing queues on send, so a datagram socket is always writable.
      w = !socket->shutdown_write;
      e = socket->pending_error != 0;
    }
  }

  if (readable)
    *readable = r;
  if (writable)
    *writable = w;
  if (exceptional)
    *exceptional = e;
}

bool IsStream(const SocketPtr& socket)
{
  return socket && socket->type == WII_SOCK_STREAM;
}

bool IsConnecting(const SocketPtr& socket)
{
  if (!socket)
    return false;
  std::lock_guard lock{s_stack.mutex};
  return socket->tcp == TcpState::SynSent;
}

bool IsConnected(const SocketPtr& socket)
{
  if (!socket)
    return false;
  std::lock_guard lock{s_stack.mutex};
  return socket->tcp == TcpState::Established;
}

s32 Ping(u32 ip, u16 id, const u8* payload, u32 length, u32 timeout_ms)
{
  {
    std::lock_guard lock{s_stack.mutex};
    s_stack.pings[id] = PendingPing{ip, id, false, {}};
    VNET_TRACE(Stack, "echo request id {} to {}", id, FormatIP(ip));
    if (!Transmit(PROTO_ICMP, 0, id, ip, 0, payload, length))
    {
      s_stack.pings.erase(id);
      return -SO_EHOSTUNREACH;
    }
  }

  // Blocking, as the console's own ICMP ping is. The wait is bounded and the
  // caller already expects it to take as long as the timeout it asked for.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline)
  {
    {
      std::lock_guard lock{s_stack.mutex};
      const auto it = s_stack.pings.find(id);
      if (it != s_stack.pings.end() && it->second.answered)
      {
        const auto size = static_cast<s32>(it->second.reply.size());
        s_stack.pings.erase(it);
        return size;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::lock_guard lock{s_stack.mutex};
  s_stack.pings.erase(id);
  VNET_TRACE(Stack, "echo request id {} to {} timed out", id, FormatIP(ip));
  return -SO_ETIMEDOUT;
}
}  // namespace IOS::HLE::VirtualNet
