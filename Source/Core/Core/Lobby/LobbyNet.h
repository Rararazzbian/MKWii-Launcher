// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The link between Dolphin instances that a lobby's virtual Wii network runs
// over.
//
// One instance hosts and listens on a port; the others connect to it. Every
// participant is given an address on a private 10.13.37.0/24 range - the host
// is .1, clients get .2 upward - and from then on this layer will carry an
// opaque frame from any address on that range to any other, or to the broadcast
// address.
//
// The topology is a star: clients only ever have a connection to the host, and
// the host forwards anything addressed elsewhere. That is invisible above this
// layer, which sees a flat subnet.
//
// Nothing here knows what a frame contains. See VirtualNet for the stack that
// puts sockets on top of it.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Lobby
{
// 10.13.37.0/24, in host byte order throughout this interface.
constexpr u32 SUBNET = 0x0A0D2500;
constexpr u32 NETMASK = 0xFFFFFF00;
constexpr u32 HOST_IP = SUBNET | 0x01;
constexpr u32 BROADCAST_IP = SUBNET | 0xFF;

// Larger than any datagram the console will produce, and small enough that a
// malformed length cannot be used to make us allocate wildly.
constexpr std::size_t MAX_PAYLOAD = 16 * 1024;

enum class Role
{
  Host,
  Client,
};

enum class Status
{
  Inactive,
  Connecting,
  Connected,
  // Was connected and is not any more. Distinct from Failed: this is the state
  // a reconnect can recover from, and the one it retries from automatically.
  Disconnected,
  Failed,
};

struct Peer
{
  u32 ip;
  std::string name;
  // False for the entry describing this console itself.
  bool is_remote;
};

// Invoked on the lobby thread, once per frame addressed to this console.
using PayloadHandler = std::function<void(u32 src_ip, u32 dst_ip, const u8* data, std::size_t len)>;

// Voice rides the same link but is not the console's traffic - it is between
// Dolphin instances, and is delivered whether or not the virtual network is up.
// `audio` separates encoded speech from the occasional control message.
using VoiceHandler = std::function<void(u32 src_ip, bool audio, const u8* data, std::size_t len)>;

// Starts hosting on `port`, or joins `address` ("host:port"). Returns false
// only if the socket could not be created; a client that cannot reach its host
// starts successfully and reports Status::Failed once it gives up.
bool Start(Role role, const std::string& address, u16 port, const std::string& nickname);
void Stop();

// Drops the current connection and dials the host again, keeping the emulated
// console running. The address previously assigned is asked for again, so
// sockets already bound to it keep working if the host can still grant it.
//
// Returns false when there is nothing to reconnect to - when hosting, or before
// Start(). Safe to call at any time; the work happens on the lobby thread.
bool Reconnect();
// Whether Reconnect() would do anything, for greying out a menu entry.
bool CanReconnect();

// Automatic retry after an unexpected disconnect. On by default.
void SetAutoReconnect(bool enabled);
bool GetAutoReconnect();

// Blocks until this machine has been given an address, or the timeout expires.
bool WaitForAddress(int timeout_ms);

bool IsActive();
Status GetStatus();
// Human-readable, for the UI and for error dialogs.
std::string GetStatusText();
Role GetRole();

// This machine's address on the lobby's private range. Zero until a client has
// been assigned one by the host.
u32 GetVirtualIP();

// Everyone currently in the lobby, this console included. Ordered by address.
std::vector<Peer> GetPeers();

// Hands a frame to the lobby for delivery to `dst_ip`, which may be
// BROADCAST_IP. Returns false when there is no route - not yet connected, an
// address that belongs to nobody, or a payload over MAX_PAYLOAD.
//
// Delivery to this console's own address, and the local copy of a broadcast,
// are the caller's business: this only puts frames on the wire.
bool SendPayload(u32 dst_ip, const u8* data, std::size_t length);

// Replaces the handler invoked for incoming frames. Pass nullptr to detach.
void SetPayloadHandler(PayloadHandler handler);

// Sends on the voice channel. `audio` true is an encoded frame, sent unreliably
// and unsequenced on a channel of its own - a retransmitted 20 ms frame arrives
// after the moment it belonged to, and holding later audio behind it turns one
// lost packet into an audible stall. `audio` false is a control message and
// travels reliably alongside the lobby's own control traffic.
bool SendVoice(u32 dst_ip, bool audio, const u8* data, std::size_t length);
void SetVoiceHandler(VoiceHandler handler);
}  // namespace Lobby
