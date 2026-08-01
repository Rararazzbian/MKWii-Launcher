// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Resolve a display name for every racer, online or offline.
//
// There is no single table mapping a race slot to a player name, so this has to
// be worked out:
//
//   * The ROSTER at [0x809BD958] holds every player's Mii name, including
//     remote players. Its order is NOT the race slot order, and it is sparse.
//   * RACECONFIG slots are in race order and carry a Mii ID, but online the
//     local player's Mii block is left EMPTY, and other slots can hold a stale
//     Mii left over from a previous race.
//
// So: anchor the local player first (matched to the licence's Mii), then match
// the remaining slots by Mii ID against roster entries not already claimed,
// then fill anything left by elimination. The elimination step is what corrects
// a stale slot.
//
// Verified against a 3-player online race in which RaceConfig slot 1 still held
// the local player's Mii from an earlier offline race.

#pragma once

#include <string>
#include <vector>

#include "Core/MemInspect/Reader.h"

namespace MemInspect
{
struct RacerIdentity
{
  int index = 0;
  std::string name;
  // How the name was resolved: license, raceconfig, miiid, elimination or cpu.
  // Useful when a name looks wrong.
  std::string source;
  bool local = false;
};

// One entry per racer in the current race, or empty outside a race.
std::vector<RacerIdentity> ResolveRacers(const Reader& reader);
}  // namespace MemInspect
