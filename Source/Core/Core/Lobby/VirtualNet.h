// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A network stack for the emulated console that exists entirely inside
// Dolphin.
//
// Dolphin does not emulate a Wii NIC. IOS's socket API is normally a thin shim
// over host sockets, so the console inherits every adapter the PC has, talks on
// whichever one the OS picks, and reports an address that may belong to none of
// them. For two consoles that are meant to see each other as neighbours on one
// quiet LAN, that is the wrong shape entirely.
//
// This replaces the shim. While a lobby is up, a socket the console opens is
// backed by nothing on the host at all: no descriptor, no bind, no packet
// leaving through a real adapter. Datagrams and streams are carried between
// lobby members over the lobby's own link, and the console is told it lives on
// 10.13.37.0/24 with an address the lobby assigned it.
//
// What the console can do on that subnet is deliberately not restricted to what
// Mario Kart happens to need: UDP, TCP, broadcast, ICMP echo and unrecognised
// protocols are all carried, and anything addressed to a member of the lobby
// arrives there.
//
// Everything is traced. See NetTrace.h for why that is unconditional.

#pragma once

#include <memory>
#include <string>

#include "Common/CommonTypes.h"

namespace IOS::HLE::VirtualNet
{
class Socket;
using SocketPtr = std::shared_ptr<Socket>;

// Wii socket type values, as IOS passes them to SO_SOCKET. Native SOCK_*
// constants differ per platform and are not used here.
constexpr s32 WII_SOCK_STREAM = 1;
constexpr s32 WII_SOCK_DGRAM = 2;
// Not something IOS's SO_SOCKET accepts, but SO_ICMPSOCKET asks for it.
constexpr s32 WII_SOCK_RAW = 3;

// Attaches to the lobby and starts accepting frames. Called when a console
// boots into a lobby; safe to call repeatedly.
void Initialize();
// Detaches, closes every virtual socket and discards queued traffic.
void Shutdown();

// True when the console's sockets should be served from here rather than from
// the host. False the moment the lobby is not usable, which is what every
// integration point keys off.
bool IsActive();

// What the console is told about its own interface. Host byte order.
u32 LocalIP();
u32 Netmask();
u32 BroadcastIP();
// The lobby host doubles as the gateway. Nothing is routed through it, but the
// console expects a default route to exist.
u32 Gateway();

// A one-line summary for the UI and for the OSD.
std::string DescribeInterface();

// Opens a socket. `type` is one of the WII_SOCK_* values above. Returns null
// on an unsupported combination, with the reason traced.
SocketPtr Create(s32 type, s32 protocol);

// The calls below mirror the BSD names IOS exposes, and return either a
// non-negative result or a negated SO_* error, ready to be handed back to the
// console as-is.
s32 Bind(const SocketPtr& socket, u32 ip, u16 port);
s32 Connect(const SocketPtr& socket, u32 ip, u16 port);
s32 Listen(const SocketPtr& socket, s32 backlog);
// On success returns the accepted socket and fills `from`. On failure returns
// null and sets `error`.
SocketPtr Accept(const SocketPtr& socket, u32* from_ip, u16* from_port, s32* error);
s32 SendTo(const SocketPtr& socket, const u8* data, u32 length, bool has_destination, u32 ip,
           u16 port);
s32 RecvFrom(const SocketPtr& socket, u8* data, u32 length, bool peek, bool want_source,
             u32* from_ip, u16* from_port);
s32 Shutdown(const SocketPtr& socket, u32 how);
s32 Close(const SocketPtr& socket);
s32 GetSockName(const SocketPtr& socket, u32* ip, u16* port);
s32 GetPeerName(const SocketPtr& socket, u32* ip, u16* port);
s32 GetSockOpt(const SocketPtr& socket, u32 level, u32 optname, u8* value, u32* length);
s32 SetSockOpt(const SocketPtr& socket, u32 level, u32 optname, const u8* value, u32 length);

// What a poll or select over this socket would report right now.
void GetReadiness(const SocketPtr& socket, bool* readable, bool* writable, bool* exceptional);

bool IsStream(const SocketPtr& socket);
// Whether a connect() is still in progress, which IOS asks about separately.
bool IsConnecting(const SocketPtr& socket);
bool IsConnected(const SocketPtr& socket);

// Sends an ICMP echo request and waits up to `timeout_ms` for the reply, which
// the peer's stack generates. Returns the reply size, or a negated SO_* error.
s32 Ping(u32 ip, u16 id, const u8* payload, u32 length, u32 timeout_ms);
}  // namespace IOS::HLE::VirtualNet
