// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/MemInspect/Racers.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include "Common/StringUtil.h"
#include "Core/MemInspect/Registry.h"

namespace MemInspect
{
namespace
{
using MiiId = std::array<u8, MIIID_SIZE>;
constexpr MiiId NULL_MII = {};

struct Slot
{
  int index = 0;
  s32 controller = 0;
  MiiId mii{};
  std::string config_name;
};

struct RosterEntry
{
  std::string name;
  MiiId mii{};
};

std::optional<MiiId> ReadMiiId(const Reader& reader, u32 address)
{
  MiiId id{};
  if (!reader.Bytes(address, id.data(), static_cast<u32>(id.size())))
    return std::nullopt;
  return id;
}

std::string ReadName(const Reader& reader, u32 address)
{
  const auto name = reader.Str16(address, 10);
  if (!name)
    return {};
  return std::string(StripWhitespace(*name));
}
}  // namespace

std::vector<RacerIdentity> ResolveRacers(const Reader& reader)
{
  const auto manager = reader.Ptr(KART_MGR_PTR);
  const auto config = reader.Ptr(RACE_CFG_PTR);
  if (!manager || !config)
    return {};

  const auto count = reader.U32(*manager + KART_COUNT_OFF);
  if (!count || *count < 1 || *count > static_cast<u32>(MAX_RACERS))
    return {};
  const int racers = static_cast<int>(*count);

  // Records start at config + 0xF0*k. Using 0xF0*(k+1) reads one record too
  // late and rotates every racer's identity by one - verified against a
  // ground-truth run in which each player drove in turn.
  std::vector<Slot> slots;
  slots.reserve(static_cast<std::size_t>(racers));
  for (int k = 0; k < racers; ++k)
  {
    const u32 base = *config + SLOT_STRIDE * static_cast<u32>(k);
    Slot slot;
    slot.index = k;
    slot.controller = reader.S32(base + SLOT_CONTROLLER).value_or(0);
    slot.mii = ReadMiiId(reader, base + SLOT_MIIID).value_or(NULL_MII);
    slot.config_name = ReadName(reader, base + SLOT_MIINAME);
    slots.push_back(std::move(slot));
  }

  // The roster can be SPARSE: with 3 players the entries were seen at indices
  // 0, 1 and 3 with 2 blank. Scan the full table and skip blanks rather than
  // reading only `racers` entries.
  std::vector<RosterEntry> roster;
  if (const auto roster_base = reader.Ptr(ROSTER_PTR))
  {
    std::vector<MiiId> seen;
    for (int i = 0; i < MAX_RACERS; ++i)
    {
      const u32 base = *roster_base + ROSTER_STRIDE * static_cast<u32>(i);
      const std::string name = ReadName(reader, base + ROSTER_NAME);
      const auto mii = ReadMiiId(reader, base + ROSTER_MIIID);
      if (name.empty() || !mii || *mii == NULL_MII)
        continue;
      if (std::ranges::find(seen, *mii) != seen.end())
        continue;
      seen.push_back(*mii);
      roster.push_back({name, *mii});
    }
  }

  // The local player's own name and Mii come from the save file - online their
  // RaceConfig Mii block is empty.
  std::string license_name;
  MiiId license_mii = NULL_MII;
  if (const auto license = reader.Ptr(LICENSE_PTR))
  {
    const auto active = reader.U32(*license + LICENSE_ACTIVE_OFF);
    if (active && *active < 4)
    {
      const u32 block = *license + LICENSE_NAME_OFF + LICENSE_STRIDE * *active;
      license_name = ReadName(reader, block);
      license_mii = ReadMiiId(reader, block + LICENSE_MIIID_OFF).value_or(NULL_MII);
    }
  }

  // key: racer index -> (name, source)
  std::vector<std::optional<std::pair<std::string, std::string>>> resolved(
      static_cast<std::size_t>(racers));
  std::vector<bool> claimed(roster.size(), false);

  // Exactly ONE slot is the local player. `controller == 1` is not enough on
  // its own: after a host change two slots were seen holding 1, which made
  // every one of them claim the licence name. Prefer an exact match on the
  // licence's Mii ID, which is unambiguous.
  std::optional<int> local;
  if (license_mii != NULL_MII)
  {
    for (const Slot& slot : slots)
    {
      if (slot.mii == license_mii)
      {
        local = slot.index;
        break;
      }
    }
  }
  if (!local)
  {
    std::vector<int> ones;
    for (const Slot& slot : slots)
    {
      if (slot.controller == 1)
        ones.push_back(slot.index);
    }
    if (ones.size() == 1)
      local = ones.front();
  }
  if (!local)
  {
    // Last resort: mgr+0x54 is the local player's kart, so find which array
    // entry it repeats.
    if (const auto mine = reader.U32(*manager + KART_LOCAL_OFF))
    {
      for (int k = 0; k < racers; ++k)
      {
        const auto entry = reader.U32(*manager + KART_LIST_OFF + 4 * static_cast<u32>(k));
        if (entry && *entry == *mine)
        {
          local = k;
          break;
        }
      }
    }
  }

  if (local)
  {
    resolved[static_cast<std::size_t>(*local)] = {
        {license_name.empty() ? "YOU" : license_name, "license"}};
    for (std::size_t i = 0; i < roster.size(); ++i)
    {
      if ((license_mii != NULL_MII && roster[i].mii == license_mii) ||
          roster[i].name == license_name)
      {
        claimed[i] = true;
        break;
      }
    }
  }

  // A slot's Mii block is only trustworthy if that Mii is in the LIVE roster.
  // RaceConfig keeps stale Mii blocks after a session ends: back offline, slots
  // 1 and 2 still held the two remote players from the previous online race, so
  // CPUs were being labelled with real names. Offline the roster is empty, so
  // this correctly names nobody but the local player.
  for (const Slot& slot : slots)
  {
    auto& entry = resolved[static_cast<std::size_t>(slot.index)];
    if (entry || slot.mii == NULL_MII)
      continue;
    const bool live = std::ranges::any_of(
        roster, [&](const RosterEntry& r) { return r.mii == slot.mii; });
    if (!live)
      continue;  // stale leftover - not a racer in this race

    if (!slot.config_name.empty())
    {
      entry = {{slot.config_name, "raceconfig"}};
      for (std::size_t i = 0; i < roster.size(); ++i)
      {
        if (!claimed[i] && roster[i].mii == slot.mii)
        {
          claimed[i] = true;
          break;
        }
      }
      continue;
    }
    for (std::size_t i = 0; i < roster.size(); ++i)
    {
      if (!claimed[i] && roster[i].mii == slot.mii)
      {
        entry = {{roster[i].name, "miiid"}};
        claimed[i] = true;
        break;
      }
    }
  }

  // Elimination only makes sense in an online room, where every slot is a real
  // player. Offline the other slots are CPUs and must not inherit a leftover
  // name.
  if (reader.U32(ONLINE_ADDR).value_or(0) == 1)
  {
    std::size_t next = 0;
    for (const Slot& slot : slots)
    {
      auto& entry = resolved[static_cast<std::size_t>(slot.index)];
      if (entry)
        continue;
      while (next < roster.size() && claimed[next])
        ++next;
      if (next >= roster.size())
        break;
      entry = {{roster[next].name, "elimination"}};
      claimed[next] = true;
    }
  }

  std::vector<RacerIdentity> out;
  out.reserve(static_cast<std::size_t>(racers));
  for (const Slot& slot : slots)
  {
    const auto& entry = resolved[static_cast<std::size_t>(slot.index)];
    RacerIdentity identity;
    identity.index = slot.index;
    identity.name = entry ? entry->first : "CPU " + std::to_string(slot.index);
    identity.source = entry ? entry->second : "cpu";
    identity.local = local && *local == slot.index;
    out.push_back(std::move(identity));
  }
  return out;
}
}  // namespace MemInspect
