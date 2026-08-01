// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Per-racer status: star, shocked, crushed, mega, bullet, inked and the rest.
//
// Every state except `bullet` is available twice - as a bit in PlayerSub1c and
// as a countdown in frames at 60 fps in PlayerSub10. Reading both is a free
// consistency check, and a disagreement means the pointer chain has drifted
// rather than that the racer is in an odd state.
//
// bitfield4 also carries a `local` bit, which is a much more reliable "which
// racer am I" test than anything in RaceConfig.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/MemInspect/Reader.h"

namespace Memory
{
class MemoryManager;
}

namespace MemInspect
{
struct RacerState
{
  int index = 0;

  bool star = false;
  bool shocked = false;
  bool mega = false;
  bool crushed = false;
  bool bullet = false;
  bool inked = false;
  bool has_tc = false;
  bool stopped = false;
  bool vanished = false;

  // Who this racer is, from bitfield4.
  bool cpu = false;
  bool local = false;
  bool real_local = false;
  bool remote = false;

  // Frames at 60 fps. There is no bullet timer - riding a Bullet Bill is a flag
  // question, not a duration one.
  s16 star_timer = 0;
  s16 shock_timer = 0;
  s16 crush_timer = 0;
  s16 mega_timer = 0;
  s16 ink_timer = 0;

  // The bit and the timer are independent reads of one state. False here is a
  // fault indicator, not an unusual racer.
  bool consistent = true;

  // The states that are on, in display order - the convenient form.
  std::vector<std::string> active;
};

// Number of racers, or 0 outside a race.
int RacerCount(const Reader& reader);

// (PlayerSub1c, PlayerSub10) for racer k, or (0, 0) when the objects are gone.
// PlayerPointers at player+0x1C is INLINE, so this adds 0x1C+0x04 in one step
// rather than dereferencing twice.
std::pair<u32, u32> PlayerObjects(const Reader& reader, int k);

std::optional<RacerState> ReadState(const Reader& reader, int k);
std::vector<RacerState> ReadAllStates(const Reader& reader);

// Names accepted by SetState, for validation and for the dashboard's buttons.
const std::vector<std::string>& SettableStates();

// Force a state on racer k, writing the timer and the bit together the way the
// game's own activation routine does. Pass frames = 0 or on = false to clear.
//
// WRITING ONLY GENUINELY WORKS FOR STAR. Setting shocked or mega sets the flag
// and the timer, but the kart never changes size, because the resize is done by
// the activation routine rather than derived from the timer. Star is different
// because invincibility is read from the flag on every collision. Reading is
// correct for every state either way.
bool SetState(Memory::MemoryManager& memory, int k, std::string_view name,
              std::optional<int> frames, bool on);
void ClearStates(Memory::MemoryManager& memory, int k);
}  // namespace MemInspect
