// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Lobby/LobbyNet.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include "Common/TraversalClient.h"

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Common/Thread.h"

#include "Core/Config/MainSettings.h"
#include "Core/Lobby/NetTrace.h"

#include "VideoCommon/OnScreenDisplay.h"

namespace Lobby
{
namespace
{
using Trace::Cat;
using Trace::FormatIP;

// Control traffic and virtual network frames are separated so a burst of game
// traffic cannot delay an address assignment or a roster update behind it.
// Voice gets a third channel for the same reason in reverse: it is sent
// unreliably, and must not be able to hold up anything that is not.
constexpr u8 CHANNEL_CONTROL = 0;
constexpr u8 CHANNEL_DATA = 1;
constexpr u8 CHANNEL_VOICE = 2;
constexpr std::size_t CHANNEL_COUNT = 3;

// How a frame travels. Control and virtual network frames are reliable and
// ordered, because the game's stack is built on the assumption that they are.
//
// Voice audio is neither. A retransmitted 20 ms frame arrives long after the
// moment it belonged to, and holding later audio behind it to preserve order
// turns one lost packet into an audible stall - which is worse than the gap it
// was trying to fill, and worse than what the decoder can conceal on its own.
enum class Wire
{
  Control,
  Data,
  VoiceAudio,
  VoiceControl,
};

u8 WireChannel(Wire wire)
{
  switch (wire)
  {
  case Wire::Data:
    return CHANNEL_DATA;
  case Wire::VoiceAudio:
    return CHANNEL_VOICE;
  case Wire::Control:
  case Wire::VoiceControl:
  default:
    return CHANNEL_CONTROL;
  }
}

u32 WireFlags(Wire wire)
{
  // UNSEQUENCED as well as unreliable: without it ENet still refuses to deliver
  // a packet older than one already seen, which for audio means silently
  // dropping a frame that arrived late but is still perfectly usable.
  return wire == Wire::VoiceAudio ? ENET_PACKET_FLAG_UNSEQUENCED : ENET_PACKET_FLAG_RELIABLE;
}

constexpr std::size_t MAX_CLIENTS = 15;  // .2 through .16; the range allows more

// How often the lobby thread wakes. Also the worst-case delay between a frame
// being handed to SendPayload and reaching the wire, so it is kept short.
constexpr u32 SERVICE_TIMEOUT_MS = 1;

// Reconnect backoff, in seconds, then the last value repeats.
constexpr int RETRY_BACKOFF[] = {2, 3, 5, 8, 15};

// How long to leave between hanging up and dialling again, so the host has
// processed the disconnect and freed this console's address before it is asked
// for back. See BeginDial.
constexpr int DIAL_DELAY_MS = 250;

#pragma pack(push, 1)
struct Header
{
  u8 magic;
  u8 version;
  u8 type;
  u8 reserved;
  u32 src_ip;
  u32 dst_ip;
  u32 length;  // payload bytes following the header
};
#pragma pack(pop)
static_assert(sizeof(Header) == 16);

constexpr u8 MAGIC = 'L';
constexpr u8 VERSION = 1;

enum : u8
{
  // Client to host: u32 requested address (0 for "any"), then the nickname.
  TYPE_HELLO = 1,
  // Host to client: dst_ip is the address granted.
  TYPE_ASSIGN = 2,
  // Host to everyone: repeated { u32 ip, u8 name length, name }.
  TYPE_ROSTER = 3,
  // Anyone to anyone: an opaque virtual network frame.
  TYPE_DATA = 4,
  // Host to client, refusing the join. Payload is the reason.
  TYPE_REJECT = 5,
  // Anyone to anyone: an encoded voice frame. Unreliable. See Wire.
  TYPE_VOICE = 6,
  // Anyone to anyone: who this console is, what the host wants the bitrate to
  // be, and whether this console is racing. Reliable, and rare.
  TYPE_VOICE_CONTROL = 7,
};

const char* TypeName(u8 type)
{
  switch (type)
  {
  case TYPE_HELLO:
    return "HELLO";
  case TYPE_ASSIGN:
    return "ASSIGN";
  case TYPE_ROSTER:
    return "ROSTER";
  case TYPE_DATA:
    return "DATA";
  case TYPE_REJECT:
    return "REJECT";
  case TYPE_VOICE:
    return "VOICE";
  case TYPE_VOICE_CONTROL:
    return "VOICECTL";
  default:
    return "?";
  }
}

struct Outgoing
{
  u32 dst_ip;
  Wire wire;
  std::vector<u8> frame;
};

struct State
{
  std::atomic<bool> active{false};
  std::atomic<Status> status{Status::Inactive};
  std::atomic<u32> virtual_ip{0};
  std::atomic<bool> auto_reconnect{true};
  std::atomic<bool> reconnect_requested{false};
  std::atomic<bool> stop_requested{false};
  // Written by the lobby thread, read by whichever thread calls SendPayload,
  // so it cannot live behind the mutex the send path would otherwise contend.
  std::atomic<ENetPeer*> server_peer{nullptr};

  // Set once by Start(), before the thread exists.
  Role role = Role::Host;

  // Lobby thread only.
  ENetHost* host = nullptr;
  // False when the host belongs to the traversal client, which owns and reuses
  // one across sessions. Destroying it from here would take theirs with it.
  bool owns_host = true;
  bool use_traversal = false;
  // The host's code, once the traversal server has issued one. Empty otherwise,
  // including for clients, which only ever dial someone else's.
  std::string host_code;
  ENetAddress server_address{};
  std::chrono::steady_clock::time_point next_retry{};
  // Set while a dial is pending; see BeginDial.
  std::optional<std::chrono::steady_clock::time_point> dial_at;
  int retry_index = 0;

  std::thread thread;

  std::mutex mutex;  // guards everything below
  std::map<ENetPeer*, u32> peer_ips;
  std::map<u32, ENetPeer*> ip_peers;
  std::map<u32, std::string> names;  // includes this console
  std::deque<Outgoing> outgoing;
  std::string nickname;
  std::string join_address;
  std::string status_text = "Inactive";

  std::mutex handler_mutex;
  PayloadHandler handler;
  VoiceHandler voice_handler;
};

State s_state;

// Every address that means "everyone on this segment".
//
// The stack above normalises these before handing a frame down, but the
// transport recognises them too, and deliberately so. A peer running a build
// that does not normalise still gets its broadcasts delivered rather than
// having them routed as a unicast to an address nobody holds - which is a
// silent, total loss of peer discovery in one direction only, and looks from
// the game like the other console simply is not there.
bool MeansEveryone(u32 ip)
{
  constexpr u32 LIMITED_BROADCAST = 0xFFFFFFFF;

  return ip == LIMITED_BROADCAST || ip == BROADCAST_IP ||
         (ip & 0xF0000000) == 0xE0000000 ||  // 224.0.0.0/4
         ((ip & NETMASK) == SUBNET && (ip & ~NETMASK) == ~NETMASK);
}

void SetStatus(Status status, std::string text)
{
  s_state.status.store(status);
  VNET_TRACE(Lobby, "status: {}", text);
  std::lock_guard lock{s_state.mutex};
  s_state.status_text = std::move(text);
}

std::vector<u8> BuildFrame(u8 type, u32 src_ip, u32 dst_ip, const void* data, std::size_t length)
{
  std::vector<u8> frame(sizeof(Header) + length);
  Header header{};
  header.magic = MAGIC;
  header.version = VERSION;
  header.type = type;
  header.src_ip = Common::swap32(src_ip);
  header.dst_ip = Common::swap32(dst_ip);
  header.length = Common::swap32(static_cast<u32>(length));
  std::memcpy(frame.data(), &header, sizeof(header));
  if (length != 0)
    std::memcpy(frame.data() + sizeof(header), data, length);
  return frame;
}

// Lobby thread only.
void SendRaw(ENetPeer* peer, const std::vector<u8>& frame, Wire wire)
{
  if (!peer)
    return;

  ENetPacket* packet = enet_packet_create(frame.data(), frame.size(), WireFlags(wire));
  if (!packet)
  {
    VNET_TRACE(Error, "enet_packet_create failed for a {} byte frame", frame.size());
    return;
  }

  const u8 channel = WireChannel(wire);
  if (enet_peer_send(peer, channel, packet) != 0)
  {
    VNET_TRACE(Error, "enet_peer_send failed on channel {} for a {} byte frame", channel,
               frame.size());
    enet_packet_destroy(packet);
  }
}

// Caller holds the mutex.
void QueueLocked(u32 dst_ip, Wire wire, std::vector<u8> frame)
{
  s_state.outgoing.push_back(Outgoing{dst_ip, wire, std::move(frame)});
}

void Queue(u32 dst_ip, Wire wire, std::vector<u8> frame)
{
  std::lock_guard lock{s_state.mutex};
  QueueLocked(dst_ip, wire, std::move(frame));
}

void Deliver(u32 src_ip, u32 dst_ip, const u8* data, std::size_t length)
{
  std::lock_guard lock{s_state.handler_mutex};
  if (!s_state.handler)
  {
    VNET_TRACE(Route, "drop {} -> {}: nothing is listening on this console yet", FormatIP(src_ip),
               FormatIP(dst_ip));
    return;
  }
  s_state.handler(src_ip, dst_ip, data, length);
}

void DeliverVoice(u32 src_ip, bool audio, const u8* data, std::size_t length)
{
  std::lock_guard lock{s_state.handler_mutex};
  if (!s_state.voice_handler)
    return;  // voice chat is not running on this console; not worth a trace line
  s_state.voice_handler(src_ip, audio, data, length);
}

// Lobby thread only. Sends `frame` to the peer owning `dst_ip`, or to every
// peer when it is the broadcast address. `except` is skipped, which is how a
// forwarded broadcast avoids going back where it came from.
void RouteToPeers(u32 dst_ip, Wire wire, const std::vector<u8>& frame, ENetPeer* except)
{
  std::lock_guard lock{s_state.mutex};

  if (s_state.role == Role::Client)
  {
    // Clients have exactly one link. The host does the routing.
    ENetPeer* const server = s_state.server_peer.load();
    if (!server)
    {
      VNET_TRACE(Route, "drop -> {}: not connected to the host", FormatIP(dst_ip));
      return;
    }
    SendRaw(server, frame, wire);
    return;
  }

  if (MeansEveryone(dst_ip))
  {
    int sent = 0;
    for (const auto& [peer, ip] : s_state.peer_ips)
    {
      if (peer == except)
        continue;
      SendRaw(peer, frame, wire);
      ++sent;
    }
    VNET_TRACE(Route, "broadcast fanned out to {} peer(s)", sent);
    return;
  }

  const auto it = s_state.ip_peers.find(dst_ip);
  if (it == s_state.ip_peers.end())
  {
    VNET_TRACE(Route, "drop -> {}: no peer holds that address", FormatIP(dst_ip));
    return;
  }
  SendRaw(it->second, frame, wire);
}

// Host only, caller holds the mutex. Prefers `requested` so a client that comes
// back after a disconnect keeps the address its sockets are already bound to.
u32 AllocateVirtualIP(u32 requested)
{
  const auto taken = [](u32 candidate) { return s_state.ip_peers.contains(candidate); };

  if (requested != 0 && (requested & NETMASK) == SUBNET && requested != HOST_IP &&
      requested != BROADCAST_IP && !taken(requested))
  {
    return requested;
  }

  for (u32 candidate = SUBNET + 2; candidate < BROADCAST_IP; ++candidate)
  {
    if (!taken(candidate))
      return candidate;
  }
  return 0;
}

// Host only. Tells everyone who is in the lobby.
void BroadcastRoster()
{
  std::vector<u8> body;
  {
    std::lock_guard lock{s_state.mutex};
    for (const auto& [ip, name] : s_state.names)
    {
      const u32 be_ip = Common::swap32(ip);
      const u8 length = static_cast<u8>(std::min<std::size_t>(name.size(), 255));
      body.insert(body.end(), reinterpret_cast<const u8*>(&be_ip),
                  reinterpret_cast<const u8*>(&be_ip) + sizeof(be_ip));
      body.push_back(length);
      body.insert(body.end(), name.begin(), name.begin() + length);
    }
  }

  Queue(BROADCAST_IP, Wire::Control, BuildFrame(TYPE_ROSTER, HOST_IP, BROADCAST_IP, body.data(),
                                       body.size()));
  VNET_TRACE(Lobby, "roster broadcast, {} bytes", body.size());
}

void ApplyRoster(const u8* data, std::size_t length)
{
  std::map<u32, std::string> names;
  std::size_t offset = 0;
  while (offset + 5 <= length)
  {
    u32 be_ip;
    std::memcpy(&be_ip, data + offset, sizeof(be_ip));
    offset += sizeof(be_ip);
    const u8 name_length = data[offset++];
    if (offset + name_length > length)
      break;
    names[Common::swap32(be_ip)] =
        std::string(reinterpret_cast<const char*>(data + offset), name_length);
    offset += name_length;
  }

  {
    std::lock_guard lock{s_state.mutex};
    s_state.names = std::move(names);
  }

  for (const Peer& peer : GetPeers())
    VNET_TRACE(Lobby, "  in lobby: {} {}", FormatIP(peer.ip), peer.name);
}

void SendHello()
{
  std::string name;
  u32 requested;
  {
    std::lock_guard lock{s_state.mutex};
    name = s_state.nickname;
  }
  requested = s_state.virtual_ip.load();

  std::vector<u8> body(sizeof(u32) + name.size());
  const u32 be_requested = Common::swap32(requested);
  std::memcpy(body.data(), &be_requested, sizeof(be_requested));
  std::memcpy(body.data() + sizeof(be_requested), name.data(), name.size());

  VNET_TRACE(Lobby, "hello as \"{}\", asking for {}", name,
             requested ? FormatIP(requested) : "any address");
  Queue(HOST_IP, Wire::Control, BuildFrame(TYPE_HELLO, requested, HOST_IP, body.data(), body.size()));
}

// Host side of a join.
void HandleHello(ENetPeer* from, const u8* data, std::size_t length)
{
  u32 requested = 0;
  std::string name;
  if (length >= sizeof(u32))
  {
    u32 be_requested;
    std::memcpy(&be_requested, data, sizeof(be_requested));
    requested = Common::swap32(be_requested);
    name.assign(reinterpret_cast<const char*>(data + sizeof(u32)), length - sizeof(u32));
  }
  if (name.empty())
    name = "A player";

  u32 assigned;
  {
    std::lock_guard lock{s_state.mutex};

    // A second HELLO on a live connection is a client repeating itself; keep
    // the address it already has rather than handing out a new one.
    const auto existing = s_state.peer_ips.find(from);
    if (existing != s_state.peer_ips.end())
    {
      assigned = existing->second;
    }
    else if (s_state.peer_ips.size() >= MAX_CLIENTS)
    {
      assigned = 0;
    }
    else
    {
      assigned = AllocateVirtualIP(requested);
    }

    if (assigned != 0)
    {
      s_state.peer_ips[from] = assigned;
      s_state.ip_peers[assigned] = from;
      s_state.names[assigned] = name;
    }
  }

  if (assigned == 0)
  {
    const std::string reason = "The lobby is full";
    VNET_TRACE(Error, "refused {}: {}", name, reason);
    SendRaw(from, BuildFrame(TYPE_REJECT, HOST_IP, 0, reason.data(), reason.size()),
            Wire::Control);
    return;
  }

  VNET_TRACE(Lobby, "{} joined as {}{}", name, FormatIP(assigned),
             assigned == requested ? " (the address it asked for)" : "");
  SendRaw(from, BuildFrame(TYPE_ASSIGN, HOST_IP, assigned, nullptr, 0), Wire::Control);
  OSD::AddMessage(name + " connected", OSD::Duration::NORMAL, OSD::Color::GREEN);

  {
    std::lock_guard lock{s_state.mutex};
    s_state.status_text = "Hosting, " + std::to_string(s_state.peer_ips.size()) + " connected";
  }
  s_state.status.store(Status::Connected);
  BroadcastRoster();
}

// Client side of a join.
void HandleAssign(u32 assigned)
{
  const u32 previous = s_state.virtual_ip.exchange(assigned);
  s_state.retry_index = 0;

  if (previous != 0 && previous != assigned)
  {
    // Worth shouting about: anything already bound to the old address stops
    // receiving, and that looks exactly like a network fault from the game.
    VNET_TRACE(Error, "address changed from {} to {} across a reconnect; sockets bound to the old "
                      "address will no longer receive",
               FormatIP(previous), FormatIP(assigned));
    OSD::AddMessage("Lobby address changed to " + FormatIP(assigned), OSD::Duration::VERY_LONG,
                    OSD::Color::YELLOW);
  }

  SetStatus(Status::Connected, "Connected as " + FormatIP(assigned));
  OSD::AddMessage(previous == assigned && previous != 0 ?
                      "Reconnected to host as " + FormatIP(assigned) :
                      "Connected to host as " + FormatIP(assigned),
                  OSD::Duration::NORMAL, OSD::Color::GREEN);
}

void HandleData(ENetPeer* from, const Header& header, const u8* payload, std::size_t length)
{
  u32 src_ip = Common::swap32(header.src_ip);
  const u32 claimed_dst = Common::swap32(header.dst_ip);

  // Normalised here rather than trusted as sent, so a peer that addressed its
  // broadcast some other way is still understood.
  const bool to_everyone = MeansEveryone(claimed_dst);
  const u32 dst_ip = to_everyone ? BROADCAST_IP : claimed_dst;
  if (to_everyone && claimed_dst != BROADCAST_IP)
  {
    VNET_TRACE(Route, "{} from {} means everyone; handling as {}", FormatIP(claimed_dst),
               FormatIP(src_ip), FormatIP(BROADCAST_IP));
  }

  if (s_state.role == Role::Host)
  {
    // Take the source address from the connection rather than the frame. A
    // peer cannot then claim to be someone else, and a stale address after a
    // reconnect is corrected rather than confusing the receiver.
    u32 peer_ip = 0;
    {
      std::lock_guard lock{s_state.mutex};
      const auto it = s_state.peer_ips.find(from);
      if (it != s_state.peer_ips.end())
        peer_ip = it->second;
    }

    if (peer_ip == 0)
    {
      VNET_TRACE(Route, "drop DATA from a peer with no address yet");
      return;
    }
    if (peer_ip != src_ip)
    {
      VNET_TRACE(Error, "frame claimed source {} but arrived from {}; rewriting", FormatIP(src_ip),
                 FormatIP(peer_ip));
      src_ip = peer_ip;
    }

    if (dst_ip == BROADCAST_IP)
    {
      Deliver(src_ip, dst_ip, payload, length);
      RouteToPeers(dst_ip, Wire::Data, BuildFrame(TYPE_DATA, src_ip, dst_ip, payload, length),
                   from);
      return;
    }
    if (dst_ip == HOST_IP)
    {
      Deliver(src_ip, dst_ip, payload, length);
      return;
    }

    VNET_TRACE(Route, "forwarding {} -> {}, {} bytes", FormatIP(src_ip), FormatIP(dst_ip), length);
    RouteToPeers(dst_ip, Wire::Data, BuildFrame(TYPE_DATA, src_ip, dst_ip, payload, length),
                 from);
    return;
  }

  const u32 local = s_state.virtual_ip.load();
  if (dst_ip != BROADCAST_IP && dst_ip != local)
  {
    VNET_TRACE(Route, "drop {} -> {}: this console is {}", FormatIP(src_ip), FormatIP(dst_ip),
               FormatIP(local));
    return;
  }
  Deliver(src_ip, dst_ip, payload, length);
}

// Voice takes the same star route as the virtual network - a client sends to
// the host, the host fans out - but it is kept separate rather than folded into
// HandleData, because a voice frame that arrives while the console is not on
// the virtual network yet is still perfectly deliverable. Voice is between
// Dolphin instances; the emulated console is not involved.
void HandleVoice(ENetPeer* from, const Header& header, bool audio, const u8* payload,
                 std::size_t length)
{
  u32 src_ip = Common::swap32(header.src_ip);
  const u32 claimed_dst = Common::swap32(header.dst_ip);
  const bool to_everyone = MeansEveryone(claimed_dst);
  const u32 dst_ip = to_everyone ? BROADCAST_IP : claimed_dst;

  if (s_state.role == Role::Host)
  {
    u32 peer_ip = 0;
    {
      std::lock_guard lock{s_state.mutex};
      const auto it = s_state.peer_ips.find(from);
      if (it != s_state.peer_ips.end())
        peer_ip = it->second;
    }
    if (peer_ip == 0)
      return;
    // Same reasoning as HandleData: the connection is the authority on who sent
    // this, not the frame. Per-peer volume is keyed off the source, so letting
    // a frame claim someone else's address would let it borrow their volume.
    src_ip = peer_ip;

    const u8 type = audio ? TYPE_VOICE : TYPE_VOICE_CONTROL;
    const Wire wire = audio ? Wire::VoiceAudio : Wire::VoiceControl;

    if (dst_ip == BROADCAST_IP)
    {
      DeliverVoice(src_ip, audio, payload, length);
      RouteToPeers(dst_ip, wire, BuildFrame(type, src_ip, dst_ip, payload, length), from);
      return;
    }
    if (dst_ip == HOST_IP)
    {
      DeliverVoice(src_ip, audio, payload, length);
      return;
    }
    RouteToPeers(dst_ip, wire, BuildFrame(type, src_ip, dst_ip, payload, length), from);
    return;
  }

  const u32 local = s_state.virtual_ip.load();
  if (dst_ip != BROADCAST_IP && dst_ip != local)
    return;
  DeliverVoice(src_ip, audio, payload, length);
}

void HandleFrame(ENetPeer* from, const u8* data, std::size_t size)
{
  if (size < sizeof(Header))
  {
    VNET_TRACE(Error, "runt frame of {} bytes discarded", size);
    return;
  }

  Header header{};
  std::memcpy(&header, data, sizeof(header));

  if (header.magic != MAGIC || header.version != VERSION)
  {
    VNET_TRACE(Error, "frame with magic {:#04x} version {} discarded; the other end is running a "
                      "different build",
               header.magic, header.version);
    return;
  }

  const std::size_t length = Common::swap32(header.length);
  if (length + sizeof(Header) > size)
  {
    VNET_TRACE(Error, "frame claims a {} byte payload but only {} bytes arrived", length,
               size - sizeof(Header));
    return;
  }

  const u8* const payload = data + sizeof(Header);
  const u32 src_ip = Common::swap32(header.src_ip);
  const u32 dst_ip = Common::swap32(header.dst_ip);

  if (header.type == TYPE_DATA)
  {
    VNET_TRACE_BYTES(Rx, payload, length, "DATA {} -> {}", FormatIP(src_ip), FormatIP(dst_ip));
  }
  // Voice audio is deliberately not traced. It arrives fifty times a second per
  // talking peer, and a line each would bury everything the trace exists for
  // under audio that says nothing except that someone is speaking.
  else if (header.type != TYPE_VOICE)
  {
    VNET_TRACE(Rx, "{} {} -> {}, {} bytes", TypeName(header.type), FormatIP(src_ip),
               FormatIP(dst_ip), length);
  }

  switch (header.type)
  {
  case TYPE_HELLO:
    if (s_state.role == Role::Host)
      HandleHello(from, payload, length);
    break;

  case TYPE_ASSIGN:
    if (s_state.role == Role::Client)
      HandleAssign(dst_ip);
    break;

  case TYPE_ROSTER:
    if (s_state.role == Role::Client)
      ApplyRoster(payload, length);
    break;

  case TYPE_DATA:
    HandleData(from, header, payload, length);
    break;

  case TYPE_VOICE:
    HandleVoice(from, header, true, payload, length);
    break;

  case TYPE_VOICE_CONTROL:
    HandleVoice(from, header, false, payload, length);
    break;

  case TYPE_REJECT:
  {
    const std::string reason(reinterpret_cast<const char*>(payload), length);
    SetStatus(Status::Failed, reason.empty() ? "The host refused the connection" : reason);
    OSD::AddMessage("Lobby: " + reason, OSD::Duration::VERY_LONG, OSD::Color::RED);
    break;
  }

  default:
    VNET_TRACE(Error, "unknown frame type {}", header.type);
    break;
  }
}

// Lobby thread only. Hangs up, if there is anything to hang up on, and arranges
// for the dial to happen shortly afterwards.
//
// The gap is the point. A reset alone tells the host nothing, so it would go on
// holding the address this console is about to ask for and hand out a different
// one instead - which looks to the game like every socket it had suddenly
// stopped receiving. Disconnecting properly and letting the host act on it
// first is what makes the address come back.
void BeginDial(const char* why)
{
  if (ENetPeer* const stale = s_state.server_peer.exchange(nullptr))
  {
    VNET_TRACE(Lobby, "hanging up so the host releases {}", FormatIP(s_state.virtual_ip.load()));
    enet_peer_disconnect_now(stale, 0);
    enet_host_flush(s_state.host);
  }

  {
    std::lock_guard lock{s_state.mutex};
    // The old link is gone; anything queued for it would be sent to nobody.
    s_state.outgoing.clear();
  }

  VNET_TRACE(Lobby, "will dial in {} ms ({})", DIAL_DELAY_MS, why);
  s_state.dial_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(DIAL_DELAY_MS);
}

// The traversal server's side of the conversation.
//
// Every one of these arrives on the lobby thread, because that is where
// enet_host_service and HandleResends are called from, so they can touch lobby
// state directly rather than hopping threads.
class TraversalHandler final : public Common::TraversalClientClient
{
public:
  void OnTraversalStateChanged() override;
  void OnConnectReady(ENetAddress addr) override;
  void OnConnectFailed(Common::TraversalConnectFailedReason reason) override;
  void OnTtlDetermined(u8 ttl) override {}
};

TraversalHandler s_traversal_handler;

// Lobby thread only.
void Dial()
{
  std::string address;
  {
    std::lock_guard lock{s_state.mutex};
    address = s_state.join_address;
  }

  VNET_TRACE(Lobby, "dialling {}", address);

  if (s_state.use_traversal)
  {
    // Nothing to connect to yet: the traversal server has to introduce the two
    // ends first. The actual enet_host_connect happens in OnConnectReady.
    if (!Common::g_TraversalClient)
    {
      SetStatus(Status::Failed, "Traversal client went away");
      return;
    }
    if (Common::g_TraversalClient->HasFailed())
      Common::g_TraversalClient->ReconnectToServer();
    Common::g_TraversalClient->ConnectToClient(address);
    SetStatus(Status::Connecting, "Looking up room " + address);
    return;
  }

  ENetPeer* const peer =
      enet_host_connect(s_state.host, &s_state.server_address, CHANNEL_COUNT, 0);
  s_state.server_peer.store(peer);
  if (!peer)
  {
    SetStatus(Status::Failed, "Could not reach " + address);
    return;
  }
  SetStatus(Status::Connecting, "Connecting to " + address);
}

void TraversalHandler::OnTraversalStateChanged()
{
  if (!Common::g_TraversalClient)
    return;

  switch (Common::g_TraversalClient->GetState())
  {
  case Common::TraversalClient::State::Connecting:
    SetStatus(Status::Connecting, "Contacting the traversal server");
    break;

  case Common::TraversalClient::State::Connected:
  {
    if (s_state.role == Role::Host)
    {
      const auto id = Common::g_TraversalClient->GetHostID();
      const std::string code(id.data(), id.size());
      {
        std::lock_guard lock{s_state.mutex};
        s_state.host_code = code;
      }
      SetStatus(Status::Connected, "Hosting, room code " + code);
      VNET_TRACE(Lobby, "traversal gave us room code {}", code);
    }
    else
    {
      // The server is up but has not been asked for anyone yet. Dial now that
      // there is something to dial through.
      std::string address;
      {
        std::lock_guard lock{s_state.mutex};
        address = s_state.join_address;
      }
      Common::g_TraversalClient->ConnectToClient(address);
      SetStatus(Status::Connecting, "Looking up room " + address);
    }
    break;
  }

  case Common::TraversalClient::State::Failure:
    SetStatus(Status::Failed, "Could not reach the traversal server");
    VNET_TRACE(Lobby, "traversal server unreachable");
    break;
  }
}

void TraversalHandler::OnConnectReady(ENetAddress addr)
{
  // The hole is punched; this is an ordinary ENet connect from here on, and
  // everything above this layer is none the wiser.
  VNET_TRACE(Lobby, "traversal introduced us to {}:{}", addr.host, addr.port);
  s_state.server_address = addr;
  ENetPeer* const peer = enet_host_connect(s_state.host, &addr, CHANNEL_COUNT, 0);
  s_state.server_peer.store(peer);
  if (!peer)
  {
    SetStatus(Status::Failed, "Could not open a connection");
    return;
  }
  SetStatus(Status::Connecting, "Connecting");
}

void TraversalHandler::OnConnectFailed(Common::TraversalConnectFailedReason reason)
{
  using Reason = Common::TraversalConnectFailedReason;
  switch (reason)
  {
  case Reason::ClientDidntRespond:
    SetStatus(Status::Failed, "The host did not respond");
    break;
  case Reason::ClientFailure:
    SetStatus(Status::Failed, "The traversal server rejected the connection");
    break;
  case Reason::NoSuchClient:
    SetStatus(Status::Failed, "No room with that code");
    break;
  default:
    SetStatus(Status::Failed, "Could not connect through the traversal server");
    break;
  }
  VNET_TRACE(Lobby, "traversal connect failed ({})", static_cast<int>(reason));
}

// Lobby thread only. Moves anything SendPayload queued onto the wire.
void DrainOutgoing()
{
  std::deque<Outgoing> batch;
  {
    std::lock_guard lock{s_state.mutex};
    batch.swap(s_state.outgoing);
  }

  for (const Outgoing& out : batch)
    RouteToPeers(out.dst_ip, out.wire, out.frame, nullptr);
}

void HandleDisconnect(ENetPeer* peer)
{
  if (s_state.role == Role::Host)
  {
    std::string name = "A player";
    {
      std::lock_guard lock{s_state.mutex};
      const auto it = s_state.peer_ips.find(peer);
      if (it != s_state.peer_ips.end())
      {
        const u32 ip = it->second;
        const auto name_it = s_state.names.find(ip);
        if (name_it != s_state.names.end())
        {
          name = name_it->second;
          s_state.names.erase(name_it);
        }
        s_state.ip_peers.erase(ip);
        s_state.peer_ips.erase(it);
        VNET_TRACE(Lobby, "{} left, {} is free again", name, FormatIP(ip));
      }
      s_state.status_text = "Hosting, " + std::to_string(s_state.peer_ips.size()) + " connected";
    }

    OSD::AddMessage(name + " disconnected", OSD::Duration::NORMAL, OSD::Color::YELLOW);
    BroadcastRoster();
    return;
  }

  s_state.server_peer.store(nullptr);
  {
    std::lock_guard lock{s_state.mutex};
    s_state.outgoing.clear();
  }

  // The address is deliberately kept. Sockets are bound to it, and the host is
  // asked for it back on the next attempt.
  const u32 held = s_state.virtual_ip.load();
  VNET_TRACE(Lobby, "lost the host; holding {} for a reconnect", FormatIP(held));

  SetStatus(Status::Disconnected, "Disconnected from host");
  OSD::AddMessage(s_state.auto_reconnect.load() ? "Disconnected from host - reconnecting" :
                                                  "Disconnected from host",
                  OSD::Duration::VERY_LONG, OSD::Color::RED);

  // The backoff index is deliberately left alone. A host that stays down should
  // be dialled less and less often, and only a successful join resets it.
  s_state.next_retry = std::chrono::steady_clock::now() +
                       std::chrono::seconds(RETRY_BACKOFF[0]);
}

void ServiceReconnect()
{
  if (s_state.role != Role::Client)
    return;

  // Waits longer after each failure, up to the last value in the table. Reset
  // by a successful address assignment, not by the disconnect that caused the
  // retry - otherwise a host that is down would be dialled every two seconds
  // forever.
  const auto schedule_next = [] {
    const auto last = static_cast<int>(std::size(RETRY_BACKOFF)) - 1;
    const int wait = RETRY_BACKOFF[std::min(s_state.retry_index, last)];
    s_state.retry_index = std::min(s_state.retry_index + 1, last);
    s_state.next_retry = std::chrono::steady_clock::now() + std::chrono::seconds(wait);
  };

  const auto now = std::chrono::steady_clock::now();

  // A dial that BeginDial scheduled comes first: until it has happened there is
  // nothing to reconnect, and starting another would leave the first orphaned.
  if (s_state.dial_at)
  {
    if (now >= *s_state.dial_at)
    {
      s_state.dial_at.reset();
      Dial();
    }
    return;
  }

  const Status status = s_state.status.load();

  if (s_state.reconnect_requested.exchange(false))
  {
    // A manual retry means the wait so far was wrong, so it starts over.
    s_state.retry_index = 0;
    OSD::AddMessage("Reconnecting to the lobby", OSD::Duration::NORMAL, OSD::Color::YELLOW);
    SetStatus(Status::Connecting, "Reconnecting");
    BeginDial(status == Status::Connected ? "asked for by hand while connected" :
                                            "asked for by hand");
    schedule_next();
    return;
  }

  if (status != Status::Disconnected || !s_state.auto_reconnect.load())
    return;
  if (now < s_state.next_retry)
    return;

  BeginDial("automatic retry");
  schedule_next();
}

void ThreadFunc()
{
  Common::SetCurrentThreadName("LobbyNet");
  VNET_TRACE(Lobby, "lobby thread started as {}",
             s_state.role == Role::Host ? "host" : "client");

  while (!s_state.stop_requested.load())
  {
    DrainOutgoing();

    // Traversal packets ride the same socket and are pulled out by the intercept
    // that TraversalClient installed, but its own retransmits are on us to pump.
    if (s_state.use_traversal && Common::g_TraversalClient)
      Common::g_TraversalClient->HandleResends();

    ENetEvent event;
    while (enet_host_service(s_state.host, &event, SERVICE_TIMEOUT_MS) > 0)
    {
      switch (event.type)
      {
      case ENET_EVENT_TYPE_CONNECT:
        if (s_state.role == Role::Host)
        {
          VNET_TRACE(Lobby, "a peer connected; waiting for its hello");
        }
        else
        {
          s_state.server_peer.store(event.peer);
          SetStatus(Status::Connecting, "Connected, waiting for an address");
          SendHello();
        }
        break;

      case ENET_EVENT_TYPE_RECEIVE:
        HandleFrame(event.peer, event.packet->data, event.packet->dataLength);
        enet_packet_destroy(event.packet);
        break;

      case ENET_EVENT_TYPE_DISCONNECT:
        HandleDisconnect(event.peer);
        break;

      default:
        break;
      }
    }

    ServiceReconnect();
  }

  VNET_TRACE(Lobby, "lobby thread stopping");
}
}  // namespace

// Brings up the traversal client and borrows its ENetHost. Returns false with
// the status already set on failure.
//
// listen_port 0 lets the OS pick, which is the point: the whole reason to use a
// traversal server is not having to own a particular port on the router.
bool StartTraversal(u16 listen_port)
{
  const std::string server = Config::Get(Config::MAIN_LOBBY_TRAVERSAL_SERVER);
  const u16 server_port = Config::Get(Config::MAIN_LOBBY_TRAVERSAL_PORT);
  const u16 server_port_alt = Config::Get(Config::MAIN_LOBBY_TRAVERSAL_PORT_ALT);

  if (!Common::EnsureTraversalClient(server, server_port, server_port_alt, listen_port))
  {
    SetStatus(Status::Failed, "Could not reach the traversal server " + server);
    return false;
  }

  s_state.host = Common::g_MainNetHost.get();
  s_state.owns_host = false;
  if (!s_state.host)
  {
    SetStatus(Status::Failed, "Traversal client produced no socket");
    return false;
  }

  Common::g_TraversalClient->m_Client = &s_traversal_handler;
  if (Common::g_TraversalClient->HasFailed())
    Common::g_TraversalClient->ReconnectToServer();
  return true;
}

bool Start(Role role, const std::string& address, u16 port, const std::string& nickname)
{
  Stop();

  Trace::SetEnabled(Config::Get(Config::MAIN_LOBBY_NET_TRACE));
  Trace::SetPayloadDumpEnabled(Config::Get(Config::MAIN_LOBBY_NET_TRACE_PAYLOADS));
  Trace::Open();

  if (enet_initialize() != 0)
  {
    SetStatus(Status::Failed, "Could not initialise networking");
    return false;
  }

  {
    std::lock_guard lock{s_state.mutex};
    s_state.nickname = nickname;
    s_state.names.clear();
    s_state.peer_ips.clear();
    s_state.ip_peers.clear();
    s_state.outgoing.clear();
    s_state.join_address = address;
  }
  s_state.role = role;
  s_state.use_traversal = Config::Get(Config::MAIN_LOBBY_USE_TRAVERSAL);
  s_state.stop_requested.store(false);
  s_state.reconnect_requested.store(false);
  s_state.virtual_ip.store(0);
  s_state.server_peer.store(nullptr);
  s_state.retry_index = 0;
  s_state.dial_at.reset();

  if (role == Role::Host)
  {
    if (s_state.use_traversal)
    {
      // The traversal client owns the socket, and installs an intercept on it so
      // its own protocol is filtered out before ENet ever sees it. Everything
      // else - the lobby's traffic - reaches us untouched.
      if (!StartTraversal(0))
        return false;
    }
    else
    {
      ENetAddress listen{};
      listen.host = ENET_HOST_ANY;
      listen.port = port;
      s_state.host = enet_host_create(&listen, MAX_CLIENTS, CHANNEL_COUNT, 0, 0);
      if (!s_state.host)
      {
        enet_deinitialize();
        SetStatus(Status::Failed, "Port " + std::to_string(port) + " is already in use");
        return false;
      }
    }

    s_state.virtual_ip.store(HOST_IP);
    {
      std::lock_guard lock{s_state.mutex};
      s_state.names[HOST_IP] = nickname.empty() ? "Host" : nickname;
    }

    if (s_state.use_traversal)
    {
      // Connected once the server hands over a room code, not before, so there
      // is something to tell people to dial.
      SetStatus(Status::Connecting, "Getting a room code");
      VNET_TRACE(Lobby, "hosting via traversal as {}", FormatIP(HOST_IP));
    }
    else
    {
      SetStatus(Status::Connected, "Hosting on port " + std::to_string(port));
      VNET_TRACE(Lobby, "hosting on port {} as {}", port, FormatIP(HOST_IP));
    }
  }
  else
  {
    if (s_state.use_traversal)
    {
      if (!StartTraversal(0))
        return false;

      // Dialling waits for the traversal server to answer; see
      // TraversalHandler::OnTraversalStateChanged.
      SetStatus(Status::Connecting, "Contacting the traversal server");
      VNET_TRACE(Lobby, "joining room {}", address);

      s_state.active.store(true);
      s_state.thread = std::thread(ThreadFunc);
      return true;
    }

    s_state.host = enet_host_create(nullptr, 1, CHANNEL_COUNT, 0, 0);
    if (!s_state.host)
    {
      enet_deinitialize();
      SetStatus(Status::Failed, "Could not create socket");
      return false;
    }

    const std::size_t colon = address.rfind(':');
    const std::string host_part = colon == std::string::npos ? address : address.substr(0, colon);
    const u16 host_port =
        colon == std::string::npos ?
            port :
            static_cast<u16>(std::strtoul(address.c_str() + colon + 1, nullptr, 10));

    if (enet_address_set_host(&s_state.server_address, host_part.c_str()) != 0)
    {
      enet_host_destroy(s_state.host);
      s_state.host = nullptr;
      enet_deinitialize();
      SetStatus(Status::Failed, "Could not resolve " + host_part);
      return false;
    }
    s_state.server_address.port = host_port;

    s_state.server_peer.store(
        enet_host_connect(s_state.host, &s_state.server_address, CHANNEL_COUNT, 0));
    if (!s_state.server_peer.load())
    {
      enet_host_destroy(s_state.host);
      s_state.host = nullptr;
      enet_deinitialize();
      SetStatus(Status::Failed, "Could not reach " + address);
      return false;
    }
    SetStatus(Status::Connecting, "Connecting to " + address);
    VNET_TRACE(Lobby, "joining {}", address);
  }

  s_state.active.store(true);
  s_state.thread = std::thread(ThreadFunc);
  return true;
}

void Stop()
{
  if (!s_state.active.exchange(false))
    return;

  s_state.stop_requested.store(true);
  if (s_state.thread.joinable())
    s_state.thread.join();

  if (s_state.host)
  {
    if (s_state.owns_host)
    {
      enet_host_destroy(s_state.host);
      enet_deinitialize();
    }
    else
    {
      // Borrowed from the traversal client, which owns it and reuses it across
      // sessions. Detach from it first: its callbacks reach into lobby state
      // that is about to be cleared.
      if (Common::g_TraversalClient)
        Common::g_TraversalClient->m_Client = nullptr;
      Common::ReleaseTraversalClient();
    }
    s_state.host = nullptr;
    s_state.owns_host = true;
  }
  {
    std::lock_guard lock{s_state.mutex};
    s_state.host_code.clear();
  }
  s_state.server_peer.store(nullptr);

  {
    std::lock_guard lock{s_state.handler_mutex};
    s_state.handler = nullptr;
  }

  {
    std::lock_guard lock{s_state.mutex};
    s_state.peer_ips.clear();
    s_state.ip_peers.clear();
    s_state.names.clear();
    s_state.outgoing.clear();
    s_state.status_text = "Inactive";
  }
  s_state.virtual_ip.store(0);
  s_state.status.store(Status::Inactive);

  VNET_TRACE(Lobby, "lobby stopped");
  Trace::Close();
}

bool Reconnect()
{
  if (!CanReconnect())
    return false;
  VNET_TRACE(Lobby, "reconnect requested");
  s_state.reconnect_requested.store(true);
  return true;
}

bool CanReconnect()
{
  return s_state.active.load() && s_state.role == Role::Client;
}

void SetAutoReconnect(bool enabled)
{
  s_state.auto_reconnect.store(enabled);
}

bool GetAutoReconnect()
{
  return s_state.auto_reconnect.load();
}

bool WaitForAddress(int timeout_ms)
{
  // Polled rather than condition-variable signalled: this runs once per boot,
  // for at most a few seconds, and a poll cannot miss a wakeup.
  for (int elapsed = 0; elapsed < timeout_ms; elapsed += 25)
  {
    if (s_state.virtual_ip.load() != 0)
      return true;
    if (s_state.status.load() == Status::Failed)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return s_state.virtual_ip.load() != 0;
}

bool IsActive()
{
  return s_state.active.load();
}

Status GetStatus()
{
  return s_state.status.load();
}

std::string GetHostCode()
{
  std::lock_guard lock{s_state.mutex};
  return s_state.host_code;
}

std::string GetStatusText()
{
  std::lock_guard lock{s_state.mutex};
  return s_state.status_text;
}

Role GetRole()
{
  return s_state.role;
}

u32 GetVirtualIP()
{
  return s_state.virtual_ip.load();
}

std::vector<Peer> GetPeers()
{
  const u32 local = s_state.virtual_ip.load();

  std::lock_guard lock{s_state.mutex};
  std::vector<Peer> peers;
  peers.reserve(s_state.names.size());
  for (const auto& [ip, name] : s_state.names)
    peers.push_back(Peer{ip, name, ip != local});
  return peers;
}

bool SendPayload(u32 dst_ip, const u8* data, std::size_t length)
{
  if (!s_state.active.load())
  {
    VNET_TRACE(Route, "drop -> {}: the lobby is not running", FormatIP(dst_ip));
    return false;
  }

  if (length > MAX_PAYLOAD)
  {
    VNET_TRACE(Error, "drop -> {}: {} byte payload is over the {} byte limit", FormatIP(dst_ip),
               length, MAX_PAYLOAD);
    return false;
  }

  const u32 src_ip = s_state.virtual_ip.load();
  if (src_ip == 0)
  {
    VNET_TRACE(Route, "drop -> {}: this console has no address yet", FormatIP(dst_ip));
    return false;
  }

  if (MeansEveryone(dst_ip))
    dst_ip = BROADCAST_IP;

  {
    std::lock_guard lock{s_state.mutex};

    // A client hands everything to the host, which knows the whole subnet. A
    // host can check the destination itself and say so now rather than having
    // the frame vanish on the lobby thread.
    if (s_state.role == Role::Host && dst_ip != BROADCAST_IP &&
        !s_state.ip_peers.contains(dst_ip))
    {
      VNET_TRACE(Route, "drop -> {}: nobody in the lobby holds that address", FormatIP(dst_ip));
      return false;
    }
    if (s_state.role == Role::Client && !s_state.server_peer.load())
    {
      VNET_TRACE(Route, "drop -> {}: not connected to the host", FormatIP(dst_ip));
      return false;
    }

    QueueLocked(dst_ip, Wire::Data, BuildFrame(TYPE_DATA, src_ip, dst_ip, data, length));
  }

  VNET_TRACE_BYTES(Tx, data, length, "DATA {} -> {}", FormatIP(src_ip), FormatIP(dst_ip));
  return true;
}

bool SendVoice(u32 dst_ip, bool audio, const u8* data, std::size_t length)
{
  if (!s_state.active.load() || length > MAX_PAYLOAD)
    return false;

  const u32 src_ip = s_state.virtual_ip.load();
  if (src_ip == 0)
    return false;

  if (MeansEveryone(dst_ip))
    dst_ip = BROADCAST_IP;

  std::lock_guard lock{s_state.mutex};
  if (s_state.role == Role::Host && dst_ip != BROADCAST_IP && !s_state.ip_peers.contains(dst_ip))
    return false;
  if (s_state.role == Role::Client && !s_state.server_peer.load())
    return false;

  const u8 type = audio ? TYPE_VOICE : TYPE_VOICE_CONTROL;
  QueueLocked(dst_ip, audio ? Wire::VoiceAudio : Wire::VoiceControl,
              BuildFrame(type, src_ip, dst_ip, data, length));
  return true;
}

void SetVoiceHandler(VoiceHandler handler)
{
  std::lock_guard lock{s_state.handler_mutex};
  s_state.voice_handler = std::move(handler);
  VNET_TRACE(Lobby, "voice handler {}", s_state.voice_handler ? "attached" : "detached");
}

void SetPayloadHandler(PayloadHandler handler)
{
  std::lock_guard lock{s_state.handler_mutex};
  s_state.handler = std::move(handler);
  VNET_TRACE(Lobby, "payload handler {}", s_state.handler ? "attached" : "detached");
}
}  // namespace Lobby
