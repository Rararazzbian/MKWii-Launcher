// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Lobby/NetTrace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>
#include <string_view>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"


namespace Lobby::Trace
{
namespace
{
// Kept small enough that the UI can show it without paging, large enough to
// cover a whole join attempt.
constexpr std::size_t RECENT_LINES = 400;

constexpr const char* CATEGORY_NAMES[] = {
    "LOBBY", "SOCKET", "TX", "RX", "ROUTE", "STACK", "IFACE", "ERROR",
};

struct TraceState
{
  std::mutex mutex;
  std::ofstream file;
  std::deque<std::string> recent;
  std::chrono::steady_clock::time_point start;
  bool open = false;

  std::atomic<bool> enabled{true};
  std::atomic<bool> payload_dump{true};
};

TraceState s_trace;

// Which thread a line came from. The virtual network is driven from two: the
// lobby's ENet thread and the emulated CPU thread that services IPC. Telling
// them apart is most of what makes an ordering bug readable.
std::string_view ThreadTag()
{
  static std::atomic<unsigned> next_id{0};
  static thread_local const std::string tag = fmt::format("t{}", next_id.fetch_add(1));
  return tag;
}

// Milliseconds since the session started. Wall-clock timestamps are no use for
// reading a packet trace; the gap between two frames is what matters, and two
// machines' clocks do not agree anyway.
std::string Timestamp()
{
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - s_trace.start)
                           .count();
  return fmt::format("{:>8}.{:03}", elapsed / 1000, elapsed % 1000);
}

// Caller holds the lock.
void Emit(std::string line)
{
  if (s_trace.file.is_open())
  {
    s_trace.file << line << '\n';
    // Flushed per line on purpose. A crash or a hang is precisely when the
    // trace matters most, and that is exactly when a buffer would be lost.
    s_trace.file.flush();
  }

  s_trace.recent.push_back(line);
  while (s_trace.recent.size() > RECENT_LINES)
    s_trace.recent.pop_front();
}
}  // namespace

void Open()
{
  Close();

  std::lock_guard lock{s_trace.mutex};
  s_trace.start = std::chrono::steady_clock::now();
  s_trace.recent.clear();

  const std::string path = File::GetUserPath(D_LOGS_IDX) + "VirtualNet.log";
  File::OpenFStream(s_trace.file, path, std::ios::out | std::ios::trunc);
  s_trace.open = true;

  if (!s_trace.file.is_open())
  {
    ERROR_LOG_FMT(VNET, "Could not open the virtual network trace at {}; tracing to the main log "
                        "only",
                  path);
  }

  Emit(fmt::format("# Virtual Wii network trace"));
  Emit(fmt::format("# elapsed ms | category | thread | message"));
  INFO_LOG_FMT(VNET, "Virtual network tracing to {}", path);
}

void Close()
{
  std::lock_guard lock{s_trace.mutex};
  if (!s_trace.open)
    return;
  Emit("# end of trace");
  if (s_trace.file.is_open())
    s_trace.file.close();
  s_trace.open = false;
}

bool IsEnabled()
{
  return s_trace.enabled.load(std::memory_order_relaxed);
}

void SetEnabled(bool enabled)
{
  s_trace.enabled.store(enabled, std::memory_order_relaxed);
}

bool IsPayloadDumpEnabled()
{
  return s_trace.payload_dump.load(std::memory_order_relaxed);
}

void SetPayloadDumpEnabled(bool enabled)
{
  s_trace.payload_dump.store(enabled, std::memory_order_relaxed);
}

void WriteLine(Cat category, std::string message)
{
  const auto index = static_cast<std::size_t>(category);
  const char* const name = CATEGORY_NAMES[index];

  // Errors are worth having in the main log too, where they sit next to the
  // IOS_NET messages that led up to them.
  if (category == Cat::Error)
    ERROR_LOG_FMT(VNET, "{}", message);
  else
    INFO_LOG_FMT(VNET, "[{}] {}", name, message);

  std::lock_guard lock{s_trace.mutex};
  if (!s_trace.open)
    return;
  Emit(fmt::format("{} {:<6} {:<12} {}", Timestamp(), name, ThreadTag(),
                   message));
}

void WriteBytes(Cat category, std::string message, const void* data, std::size_t length)
{
  WriteLine(category, fmt::format("{} [{} bytes]", message, length));

  if (!IsPayloadDumpEnabled() || data == nullptr || length == 0)
    return;

  const auto* const bytes = static_cast<const u8*>(data);

  std::lock_guard lock{s_trace.mutex};
  if (!s_trace.open)
    return;

  for (std::size_t offset = 0; offset < length; offset += 16)
  {
    const std::size_t count = std::min<std::size_t>(16, length - offset);

    std::string hex;
    std::string ascii;
    hex.reserve(16 * 3);
    ascii.reserve(16);
    for (std::size_t i = 0; i < 16; ++i)
    {
      if (i < count)
      {
        const u8 byte = bytes[offset + i];
        hex += fmt::format("{:02x} ", byte);
        ascii += (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
      }
      else
      {
        hex += "   ";
      }
      if (i == 7)
        hex += ' ';
    }

    Emit(fmt::format("{} {:<6} {:<12}   {:04x}  {} |{}|", Timestamp(), "DATA",
                     ThreadTag(), offset, hex, ascii));
  }
}

std::string GetRecentLines(std::size_t count)
{
  std::lock_guard lock{s_trace.mutex};

  std::string result;
  const std::size_t skip = s_trace.recent.size() > count ? s_trace.recent.size() - count : 0;
  for (std::size_t i = skip; i < s_trace.recent.size(); ++i)
  {
    result += s_trace.recent[i];
    result += '\n';
  }
  return result;
}

std::string FormatIP(u32 ip)
{
  return fmt::format("{}.{}.{}.{}", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF,
                     ip & 0xFF);
}

std::string FormatEndpoint(u32 ip, u16 port)
{
  return fmt::format("{}:{}", FormatIP(ip), port);
}
}  // namespace Lobby::Trace
