// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tracing for the virtual Wii network.
//
// This deliberately does not go through DEBUG_LOG_FMT. Release builds cap
// logging at LINFO, so anything written at debug level vanishes from exactly
// the build people actually play on - and a network bug that only appears with
// two consoles talking to each other is not reproducible in a Debug build.
//
// Everything here is compiled in unconditionally and on by default. It writes
// to User\Logs\VirtualNet.log, which is truncated each time a lobby starts, so
// a session's trace is self-contained and can be handed over as-is.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "Common/CommonTypes.h"

namespace Lobby::Trace
{
// Categories are a fixed set rather than free text so a trace can be filtered
// with a plain substring search and nothing is missed to a typo.
enum class Cat
{
  Lobby,    // transport: connect, disconnect, address assignment, retries
  Socket,   // socket lifecycle: create, bind, connect, listen, accept, close
  Tx,       // frames leaving this console
  Rx,       // frames arriving at this console
  Route,    // delivery decisions, including every drop and why
  Stack,    // TCP state machine, port allocation, queue depths
  Iface,    // what the console is told about its own interface
  Error,    // anything that should not have happened
};

// Truncates the log and starts a session. Safe to call when already open; the
// previous session is closed first.
void Open();
void Close();

bool IsEnabled();
void SetEnabled(bool enabled);

// Per-frame payload hex dumps. Separate from IsEnabled() because they are by
// far the bulk of the output and are the first thing worth turning off if the
// trace is slowing a session down.
bool IsPayloadDumpEnabled();
void SetPayloadDumpEnabled(bool enabled);

void WriteLine(Cat category, std::string message);
void WriteBytes(Cat category, std::string message, const void* data, std::size_t length);

// Reads back the tail of the current session, for showing in the UI without
// making the user go and find the file.
std::string GetRecentLines(std::size_t count);

// "10.13.37.2", for messages. Takes host byte order.
std::string FormatIP(u32 ip);
// "10.13.37.2:27900".
std::string FormatEndpoint(u32 ip, u16 port);
}  // namespace Lobby::Trace

// fmt-style. The arguments are only formatted when tracing is on.
#define VNET_TRACE(category, ...)                                                                  \
  do                                                                                               \
  {                                                                                                \
    if (::Lobby::Trace::IsEnabled())                                                                \
      ::Lobby::Trace::WriteLine(::Lobby::Trace::Cat::category, fmt::format(__VA_ARGS__));           \
  } while (0)

// As above, followed by a hex dump of `len` bytes at `ptr`.
#define VNET_TRACE_BYTES(category, ptr, len, ...)                                                  \
  do                                                                                               \
  {                                                                                                \
    if (::Lobby::Trace::IsEnabled())                                                                \
    {                                                                                              \
      ::Lobby::Trace::WriteBytes(::Lobby::Trace::Cat::category, fmt::format(__VA_ARGS__), (ptr),    \
                                 (len));                                                           \
    }                                                                                              \
  } while (0)
