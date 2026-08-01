// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The sampler. Reads every watch in the registry once per sample and publishes
// the result as an immutable snapshot that anything can pick up.
//
// Sampling happens on the CPU thread, from the same field-boundary callback the
// emulator already runs, for two reasons. It is the only thread on which guest
// memory is guaranteed coherent - a pointer chain chased while the CPU is
// mid-write can dereference a half-updated pointer - and it costs nothing,
// where reading from another thread would mean pausing the core with a
// CPUThreadGuard twenty times a second.
//
// Consumers never touch guest memory. They take a copy of the last snapshot,
// which is a plain value type, and are free to hold it for as long as they
// like. Writes go the other way: they are queued from whatever thread asked for
// them and applied at the top of the next frame.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/MemInspect/Items.h"
#include "Core/MemInspect/Racers.h"
#include "Core/MemInspect/Reader.h"
#include "Core/MemInspect/Registry.h"
#include "Core/MemInspect/States.h"

namespace Core
{
class System;
}

namespace MemInspect
{
struct WatchValue
{
  std::string key;
  // The pointer chain reached an address. False means a link was null, which is
  // the normal state for race objects while the game sits in menus.
  bool resolved = false;
  u32 address = 0;
  Reading reading;
};

struct Snapshot
{
  // A game is loaded and its memory has been read at least once.
  bool hooked = false;
  // The 6-character title id at 0x80000000. Compared against GAME_ID by the
  // dashboard, because every address here is build-specific.
  std::string game;
  // Observed sample rate, smoothed.
  double hz = 0.0;
  // Milliseconds since the emulator started sampling. Monotonic.
  u64 timestamp_ms = 0;
  // Bumped once per sample. A consumer that has already sent this sequence has
  // nothing new to send.
  u64 sequence = 0;

  // One entry per watch, in registry order, user watches last.
  std::vector<WatchValue> values;
  // Registry order. No value means the inputs were unreadable, which is
  // different from the predicate being false.
  std::vector<std::pair<std::string, std::optional<bool>>> derived;

  // All three are indexed by the same racer k used everywhere else, and all
  // three are EMPTY whenever there is no race - not merely when no game is
  // loaded. Every singleton they depend on is null outside a race, so a
  // consumer must handle the empty case rather than assuming twelve entries.
  std::vector<RacerIdentity> racers;
  std::vector<RacerState> states;
  std::vector<ItemSlot> items;
};

class Inspector
{
public:
  static Inspector& GetInstance();

  // Called once per emulated field, on the CPU thread. Drains queued writes
  // every time and takes a sample at the configured rate.
  void OnFrame(Core::System& system);
  // Forget everything. Called when emulation stops so a stale snapshot is not
  // served against the next game.
  void Reset();

  Snapshot GetSnapshot() const;
  // Just the sequence number. A consumer polling for "is there anything new"
  // wants this rather than GetSnapshot, which copies every vector in the
  // snapshot to answer one question.
  u64 GetSequence() const;

  // The registry plus whatever the dashboard has added, in the order the
  // snapshot's `values` uses.
  std::vector<Watch> GetWatches() const;

  // Add an ad-hoc watch. `kind` is any spelling ParseKind accepts. Returns
  // false and fills `error` on a bad address, a bad kind, or a duplicate.
  bool AddUserWatch(u32 address, std::string_view kind, std::string_view label, bool hex,
                    std::string* error);
  bool RemoveUserWatch(std::string_view key);

  // Writes. These are queued rather than applied immediately, because the
  // caller is usually a network thread and guest memory belongs to the CPU
  // thread. They take effect within one frame.
  void QueueGiveItem(int racer, int id, std::optional<int> count, bool force);
  void QueueSetState(int racer, std::string name, std::optional<int> frames, bool on);
  void QueueClearStates(int racer);

private:
  Inspector();

  void LoadUserWatches();
  void SaveUserWatches() const;
  void Sample(Core::System& system);
  void ApplyPendingWrites(Core::System& system);

  enum class WriteKind
  {
    GiveItem,
    SetState,
    ClearStates,
  };

  struct PendingWrite
  {
    WriteKind kind = WriteKind::GiveItem;
    int racer = 0;
    int id = 0;
    std::optional<int> count;
    std::string state;
    std::optional<int> frames;
    bool on = true;
    bool force = false;
  };

  mutable std::mutex m_snapshot_mutex;
  Snapshot m_snapshot;

  mutable std::mutex m_watch_mutex;
  std::vector<Watch> m_user_watches;

  std::mutex m_write_mutex;
  std::vector<PendingWrite> m_pending;

  // Sampling cadence, in milliseconds. The game runs at 60 fields a second; a
  // 50 ms gate means every third field, which is well above what a human can
  // read and far below what would show up as a frame time.
  static constexpr u64 SAMPLE_INTERVAL_MS = 50;
  u64 m_last_sample_ms = 0;
  u64 m_sequence = 0;
  double m_hz = 0.0;
};
}  // namespace MemInspect
