// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/MemInspect/States.h"

#include <algorithm>
#include <array>

#include "Core/HW/Memmap.h"
#include "Core/MemInspect/Registry.h"

namespace MemInspect
{
namespace
{
constexpr int BIT_STAR = 31;      // bitfield1
constexpr int BIT_SHOCKED = 7;    // bitfield2 from here down
constexpr int BIT_MEGA = 15;
constexpr int BIT_CRUSHED = 16;
constexpr int BIT_STOPPED = 18;
constexpr int BIT_VANISHED = 19;
constexpr int BIT_BULLET = 27;
constexpr int BIT_INKED = 28;
constexpr int BIT_HAS_TC = 29;
constexpr int BIT_CPU = 0;        // bitfield4 from here down
constexpr int BIT_REAL_LOCAL = 1;
constexpr int BIT_LOCAL = 2;
constexpr int BIT_REMOTE = 3;

bool Test(u32 field, int bit)
{
  return (field & (1u << bit)) != 0;
}

// Each settable state is a (bitfield offset, bit, timer offset) triple. A timer
// offset of 0 means the state has no timer: `bullet` is a flag only.
struct Settable
{
  const char* name;
  u32 bitfield;
  int bit;
  u32 timer;
  int default_frames;
};

constexpr std::array<Settable, 6> SETTABLE = {{
    {"star", BITFIELD1, BIT_STAR, T_STAR, 450},
    {"shocked", BITFIELD2, BIT_SHOCKED, T_SHOCK, 240},
    {"mega", BITFIELD2, BIT_MEGA, T_MEGA, 500},
    {"crushed", BITFIELD2, BIT_CRUSHED, T_CRUSH, 180},
    {"inked", BITFIELD2, BIT_INKED, T_INK, 300},
    {"bullet", BITFIELD2, BIT_BULLET, 0, 0},
}};
}  // namespace

int RacerCount(const Reader& reader)
{
  const auto holder = reader.Ptr(PLAYER_HOLDER_PTR);
  if (!holder)
    return 0;
  const auto count = reader.U8(*holder + PH_COUNT_OFF);
  if (!count || *count < 1 || *count > MAX_RACERS)
    return 0;
  return *count;
}

std::pair<u32, u32> PlayerObjects(const Reader& reader, int k)
{
  if (k < 0 || k >= MAX_RACERS)
    return {0, 0};

  const auto holder = reader.Ptr(PLAYER_HOLDER_PTR);
  if (!holder)
    return {0, 0};
  const auto players = reader.Ptr(*holder + PH_PLAYERS_OFF);
  if (!players)
    return {0, 0};
  const auto player = reader.Ptr(*players + 4 * static_cast<u32>(k));
  if (!player)
    return {0, 0};

  // Inline instance - do NOT dereference.
  const u32 pointers = *player + PLAYER_POINTERS_OFF;
  const auto sub1c = reader.Ptr(pointers + PP_SUB1C_OFF);
  const auto sub10 = reader.Ptr(pointers + PP_SUB10_OFF);
  return {sub1c.value_or(0), sub10.value_or(0)};
}

std::optional<RacerState> ReadState(const Reader& reader, int k)
{
  const auto [sub1c, sub10] = PlayerObjects(reader, k);
  if (sub1c == 0 || sub10 == 0)
    return std::nullopt;

  const auto b1 = reader.U32(sub1c + BITFIELD1);
  const auto b2 = reader.U32(sub1c + BITFIELD2);
  const auto b4 = reader.U32(sub1c + BITFIELD4);
  if (!b1 || !b2 || !b4)
    return std::nullopt;

  const auto star_timer = reader.S16(sub10 + T_STAR);
  const auto shock_timer = reader.S16(sub10 + T_SHOCK);
  const auto crush_timer = reader.S16(sub10 + T_CRUSH);
  const auto mega_timer = reader.S16(sub10 + T_MEGA);
  const auto ink_timer = reader.S16(sub10 + T_INK);
  if (!star_timer || !shock_timer || !crush_timer || !mega_timer || !ink_timer)
    return std::nullopt;

  RacerState state;
  state.index = k;
  state.star = Test(*b1, BIT_STAR);
  state.shocked = Test(*b2, BIT_SHOCKED);
  state.mega = Test(*b2, BIT_MEGA);
  state.crushed = Test(*b2, BIT_CRUSHED);
  state.bullet = Test(*b2, BIT_BULLET);
  state.inked = Test(*b2, BIT_INKED);
  state.has_tc = Test(*b2, BIT_HAS_TC);
  state.stopped = Test(*b2, BIT_STOPPED);
  state.vanished = Test(*b2, BIT_VANISHED);

  state.cpu = Test(*b4, BIT_CPU);
  state.real_local = Test(*b4, BIT_REAL_LOCAL);
  state.remote = Test(*b4, BIT_REMOTE);
  // The player at the controller has bit 1 ("real local") set and bit 2 CLEAR,
  // so testing bit 2 alone reports them as a CPU. Bit 2 appears to mean
  // local-but-not-you, i.e. a splitscreen guest, which is untested here.
  state.local = Test(*b4, BIT_REAL_LOCAL) || Test(*b4, BIT_LOCAL);

  state.star_timer = *star_timer;
  state.shock_timer = *shock_timer;
  state.crush_timer = *crush_timer;
  state.mega_timer = *mega_timer;
  state.ink_timer = *ink_timer;

  // Every pair here has both a bit and a timer, so each checks the other.
  // `bullet` is absent deliberately: it is a flag with no timer.
  const std::pair<bool, s16> pairs[] = {
      {state.star, state.star_timer},   {state.shocked, state.shock_timer},
      {state.crushed, state.crush_timer}, {state.mega, state.mega_timer},
      {state.inked, state.ink_timer},
  };
  state.consistent = std::ranges::all_of(
      pairs, [](const auto& pair) { return pair.first == (pair.second > 0); });

  // Display order: the strongest, most visually obvious states first, so the
  // map's halo picks the one a viewer would name.
  const std::pair<const char*, bool> display[] = {
      {"star", state.star},       {"mega", state.mega},         {"bullet", state.bullet},
      {"shocked", state.shocked}, {"crushed", state.crushed},   {"inked", state.inked},
      {"has_tc", state.has_tc},   {"vanished", state.vanished}, {"stopped", state.stopped},
  };
  for (const auto& [name, on] : display)
  {
    if (on)
      state.active.emplace_back(name);
  }

  return state;
}

std::vector<RacerState> ReadAllStates(const Reader& reader)
{
  std::vector<RacerState> out;
  const int count = RacerCount(reader);
  out.reserve(static_cast<std::size_t>(count));
  for (int k = 0; k < count; ++k)
  {
    if (auto state = ReadState(reader, k))
      out.push_back(std::move(*state));
  }
  return out;
}

const std::vector<std::string>& SettableStates()
{
  static const std::vector<std::string> names = [] {
    std::vector<std::string> out;
    for (const Settable& entry : SETTABLE)
      out.emplace_back(entry.name);
    return out;
  }();
  return names;
}

bool SetState(Memory::MemoryManager& memory, int k, std::string_view name,
              std::optional<int> frames, bool on)
{
  const auto match = std::ranges::find_if(
      SETTABLE, [&](const Settable& entry) { return name == entry.name; });
  if (match == SETTABLE.end())
    return false;

  const Reader reader(memory);
  const auto [sub1c, sub10] = PlayerObjects(reader, k);
  if (sub1c == 0 || sub10 == 0)
    return false;

  const int wanted = frames.value_or(match->default_frames);
  // A state with a timer is only really "on" if the timer is running, so
  // frames = 0 clears it even when `on` was asked for.
  const bool assert_bit = on && (match->timer == 0 || wanted > 0);

  if (match->timer != 0)
  {
    const s16 clamped = static_cast<s16>(std::clamp(wanted, 0, 0x7FFF));
    if (!WriteS16(memory, sub10 + match->timer, clamped))
      return false;
  }

  const auto field = reader.U32(sub1c + match->bitfield);
  if (!field)
    return false;
  const u32 updated =
      assert_bit ? (*field | (1u << match->bit)) : (*field & ~(1u << match->bit));
  return WriteU32(memory, sub1c + match->bitfield, updated);
}

void ClearStates(Memory::MemoryManager& memory, int k)
{
  for (const Settable& entry : SETTABLE)
    SetState(memory, k, entry.name, 0, false);
}
}  // namespace MemInspect
