// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/MemInspect/Inspector.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace MemInspect
{
namespace
{
u64 NowMs()
{
  using namespace std::chrono;
  return static_cast<u64>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string UserWatchPath()
{
  return File::GetUserPath(D_CONFIG_IDX) + "MemInspectWatches.txt";
}

// The key a user watch gets. Address and kind together, so the same address
// watched as two different types is two cards rather than a rejected duplicate.
std::string UserWatchKey(u32 address, const std::string& kind)
{
  std::string sanitised = kind;
  std::ranges::replace(sanitised, ':', '_');
  return fmt::format("user_{:08x}_{}", address, sanitised);
}
}  // namespace

Inspector& Inspector::GetInstance()
{
  static Inspector instance;
  return instance;
}

Inspector::Inspector()
{
  LoadUserWatches();
}

void Inspector::Reset()
{
  {
    std::lock_guard lock(m_snapshot_mutex);
    m_snapshot = {};
  }
  {
    std::lock_guard lock(m_write_mutex);
    m_pending.clear();
  }
  m_last_sample_ms = 0;
  m_hz = 0.0;
}

Snapshot Inspector::GetSnapshot() const
{
  std::lock_guard lock(m_snapshot_mutex);
  return m_snapshot;
}

u64 Inspector::GetSequence() const
{
  std::lock_guard lock(m_snapshot_mutex);
  return m_snapshot.sequence;
}

std::vector<Watch> Inspector::GetWatches() const
{
  std::vector<Watch> out = BuiltinWatches();
  std::lock_guard lock(m_watch_mutex);
  out.insert(out.end(), m_user_watches.begin(), m_user_watches.end());
  return out;
}

// ---------------------------------------------------------------------------
// Ad-hoc watches.
//
// These exist so a candidate address can be tested without editing Registry.h
// and rebuilding. They persist across runs; the good ones are meant to be
// promoted into the registry once they have earned it.

bool Inspector::AddUserWatch(u32 address, std::string_view kind, std::string_view label, bool hex,
                             std::string* error)
{
  Kind parsed{};
  u32 chars = 0;
  if (!ParseKind(kind, &parsed, &chars))
  {
    if (error)
      *error = fmt::format("unknown type {}", kind);
    return false;
  }

  const u32 size = parsed == Kind::Str16 ? chars * 2 : KindSize(parsed);
  if (!IsGuestAddress(address) || !IsGuestAddress(address + size - 1))
  {
    if (error)
    {
      *error = fmt::format("0x{:08X} is outside MEM1 (0x80000000-0x817FFFFF) and MEM2 "
                           "(0x90000000-0x93FFFFFF)",
                           address);
    }
    return false;
  }

  Watch watch;
  watch.key = UserWatchKey(address, KindName(parsed, chars));
  watch.address = address;
  watch.kind = parsed;
  watch.chars = chars;
  watch.label = label.empty() ? fmt::format("0x{:08X}", address) : std::string(label);
  watch.group = "Live Watches";
  watch.hex = hex;
  watch.user = true;

  {
    std::lock_guard lock(m_watch_mutex);
    const bool exists = std::ranges::any_of(
        m_user_watches, [&](const Watch& existing) { return existing.key == watch.key; });
    if (exists)
    {
      if (error)
        *error = "already watching that address as that type";
      return false;
    }
    m_user_watches.push_back(std::move(watch));
  }
  SaveUserWatches();
  return true;
}

bool Inspector::RemoveUserWatch(std::string_view key)
{
  {
    std::lock_guard lock(m_watch_mutex);
    const auto removed = std::erase_if(
        m_user_watches, [&](const Watch& watch) { return watch.key == key; });
    if (removed == 0)
      return false;
  }
  SaveUserWatches();
  return true;
}

void Inspector::LoadUserWatches()
{
  std::ifstream file;
  File::OpenFStream(file, UserWatchPath(), std::ios_base::in);
  if (!file)
    return;

  std::string line;
  while (std::getline(file, line))
  {
    // address <tab> kind <tab> hex flag <tab> label. The label is last so it
    // may contain anything except a newline.
    const std::vector<std::string> fields = SplitString(line, '\t');
    if (fields.size() < 3)
      continue;

    u32 address = 0;
    if (!TryParse("0x" + fields[0], &address))
      continue;
    Kind kind{};
    u32 chars = 0;
    if (!ParseKind(fields[1], &kind, &chars))
      continue;

    Watch watch;
    watch.key = UserWatchKey(address, KindName(kind, chars));
    watch.address = address;
    watch.kind = kind;
    watch.chars = chars;
    watch.label = fields.size() > 3 && !fields[3].empty() ? fields[3] :
                                                            fmt::format("0x{:08X}", address);
    watch.group = "Live Watches";
    watch.hex = fields[2] == "1";
    watch.user = true;
    m_user_watches.push_back(std::move(watch));
  }
}

void Inspector::SaveUserWatches() const
{
  std::vector<Watch> copy;
  {
    std::lock_guard lock(m_watch_mutex);
    copy = m_user_watches;
  }

  std::ofstream file;
  File::OpenFStream(file, UserWatchPath(), std::ios_base::out | std::ios_base::trunc);
  if (!file)
    return;
  for (const Watch& watch : copy)
  {
    file << fmt::format("{:08x}\t{}\t{}\t{}\n", watch.address, KindName(watch.kind, watch.chars),
                        watch.hex ? 1 : 0, watch.label);
  }
}

// ---------------------------------------------------------------------------
// Writes.

void Inspector::QueueGiveItem(int racer, int id, std::optional<int> count, bool force)
{
  std::lock_guard lock(m_write_mutex);
  m_pending.push_back({.kind = WriteKind::GiveItem,
                       .racer = racer,
                       .id = id,
                       .count = count,
                       .force = force});
}

void Inspector::QueueSetState(int racer, std::string name, std::optional<int> frames, bool on)
{
  std::lock_guard lock(m_write_mutex);
  m_pending.push_back({.kind = WriteKind::SetState,
                       .racer = racer,
                       .state = std::move(name),
                       .frames = frames,
                       .on = on});
}

void Inspector::QueueClearStates(int racer)
{
  std::lock_guard lock(m_write_mutex);
  m_pending.push_back({.kind = WriteKind::ClearStates, .racer = racer});
}

void Inspector::ApplyPendingWrites(Core::System& system)
{
  std::vector<PendingWrite> writes;
  {
    std::lock_guard lock(m_write_mutex);
    if (m_pending.empty())
      return;
    writes.swap(m_pending);
  }

  auto& memory = system.GetMemory();
  for (const PendingWrite& write : writes)
  {
    switch (write.kind)
    {
    case WriteKind::GiveItem:
      GiveItem(memory, write.racer, write.id, write.count, write.force);
      break;
    case WriteKind::SetState:
      SetState(memory, write.racer, write.state, write.frames, write.on);
      break;
    case WriteKind::ClearStates:
      ClearStates(memory, write.racer);
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Sampling.

void Inspector::OnFrame(Core::System& system)
{
  // Writes are applied on every field rather than on the sample gate, so an
  // injected item lands within a frame of being asked for.
  ApplyPendingWrites(system);

  const u64 now = NowMs();
  if (m_last_sample_ms != 0 && now - m_last_sample_ms < SAMPLE_INTERVAL_MS)
    return;

  if (m_last_sample_ms != 0)
  {
    const u64 elapsed = now - m_last_sample_ms;
    if (elapsed > 0)
    {
      const double instant = 1000.0 / static_cast<double>(elapsed);
      m_hz = m_hz == 0.0 ? instant : 0.85 * m_hz + 0.15 * instant;
    }
  }
  m_last_sample_ms = now;

  Sample(system);
}

void Inspector::Sample(Core::System& system)
{
  const auto& memory = system.GetMemory();
  if (!memory.IsInitialized())
    return;

  const Reader reader(memory);

  Snapshot snapshot;
  snapshot.hooked = true;
  snapshot.hz = m_hz;
  snapshot.timestamp_ms = m_last_sample_ms;
  snapshot.sequence = ++m_sequence;

  // The title id lives in the disc header copy at the very start of MEM1.
  std::string game(6, '\0');
  if (reader.Bytes(0x80000000, reinterpret_cast<u8*>(game.data()), 6))
  {
    // Anything non-printable means memory is up but no disc header is present
    // yet, which happens for a moment during boot.
    if (std::ranges::all_of(game, [](char c) { return c >= 0x20 && c < 0x7F; }))
      snapshot.game = game;
  }

  const std::vector<Watch> watches = GetWatches();
  snapshot.values.reserve(watches.size());

  // Index by key as we go, so a watch using index_key can read the value of an
  // earlier watch without a second pass.
  std::map<std::string, s64, std::less<>> integers;

  for (const Watch& watch : watches)
  {
    WatchValue value;
    value.key = watch.key;

    auto address = reader.Chain(watch.address, watch.offsets);

    // Array element: step `stride` bytes per unit of another watch's value.
    if (address && !watch.index_key.empty())
    {
      const auto index = integers.find(watch.index_key);
      if (index == integers.end())
        address.reset();
      else
        address = *address + watch.stride * static_cast<u32>(index->second);
    }

    if (address)
    {
      value.resolved = true;
      value.address = *address;
      value.reading = reader.Read(*address, watch.kind, watch.chars);
      if (value.reading.ok && watch.kind != Kind::F32 && watch.kind != Kind::F64 &&
          watch.kind != Kind::Str16)
      {
        integers.emplace(watch.key, value.reading.integer);
      }
    }

    snapshot.values.push_back(std::move(value));
  }

  const IntLookup lookup = [&integers](std::string_view key) -> std::optional<s64> {
    const auto found = integers.find(key);
    if (found == integers.end())
      return std::nullopt;
    return found->second;
  };

  for (const DerivedPredicate& predicate : DerivedPredicates())
    snapshot.derived.emplace_back(predicate.name, predicate.evaluate(lookup));

  // These three cannot be expressed as a Watch. Names need a join between the
  // roster, RaceConfig Mii IDs and the save file; statuses are bits spread
  // across two bitfields plus matching timers; items need the CTGP item table.
  snapshot.racers = ResolveRacers(reader);
  snapshot.states = ReadAllStates(reader);
  snapshot.items = ReadAllItems(reader);

  std::lock_guard lock(m_snapshot_mutex);
  m_snapshot = std::move(snapshot);
}
}  // namespace MemInspect
