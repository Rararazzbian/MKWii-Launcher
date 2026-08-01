// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Read and inject the item held in a racer's slot.
//
//   [0x809BEE20]          ItemDirector   (static, null outside a race)
//   director + 0x14       KartItem array base
//   array + 0x248*k       racer k's KartItem   (same k as everywhere else)
//   kartitem + 0x8C       u32  current item id, 20 = empty
//   kartitem + 0x90       u32  count remaining (3 for a triple)
//
// The two field offsets come from the published WiiRD "WiFi Item Hack", whose
// whole payload is four instructions writing an id to 0x8C and a count to 0x90
// of a KartItem in r3. The chain to reach one was confirmed live on this build:
// the director's only other pointer field that survived a stride scan (+0x4C)
// reads zeros for every racer, while +0x14 with stride 0x248 gave 20 (empty)
// across all twelve racers at the start line.
//
// One write is enough - the game reads the slot when the item button is pressed
// rather than caching it, so there is no need to hold the value every frame the
// way the Gecko codes do.
//
// This is the only genuinely useful WRITE in the inspector. Everything else is
// read-only, so nothing else can corrupt a race.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/MemInspect/Reader.h"

namespace Memory
{
class MemoryManager;
}

namespace MemInspect
{
// The item table is CTGP Revolution's, not stock. 14 is a second Mega Mushroom
// where stock has the Thunder Cloud, 19 is a bugged unused Triple Bananas, and
// 21 is one past the end of the table and HARD-CRASHES THE GAME.
constexpr int ITEM_NONE = 20;
constexpr int ITEM_MAX_ID = 20;
constexpr int ITEM_CRASH_ID = 21;

struct ItemSlot
{
  int index = 0;
  u32 id = 0;
  u32 count = 0;
  std::string name;
  bool empty = false;
};

// Address of racer k's KartItem, or 0 outside a race.
u32 ItemSlotAddress(const Reader& reader, int k);
// Racer count from the kart manager, or 0 outside a race.
int KartCount(const Reader& reader);

std::optional<ItemSlot> ReadItem(const Reader& reader, int k);
std::vector<ItemSlot> ReadAllItems(const Reader& reader);

// An item id from a name, an alias or a decimal number. Empty if unrecognised.
std::optional<int> LookupItem(std::string_view text);
std::string ItemName(int id);
// Why an id is dangerous, or empty when it is fine to hand out.
std::string_view ItemRisk(int id);

// Put an item in racer k's slot. Ids above ITEM_MAX_ID are refused unless
// `force` is set: 21 is one past the end of the item table and is confirmed to
// crash the game, and anything above it reads past the same table end.
//
// With no count, triples get 3 and everything else gets 1, because handing out
// a triple with count 1 works but wastes the item.
bool GiveItem(Memory::MemoryManager& memory, int k, int id, std::optional<int> count,
              bool force = false);
}  // namespace MemInspect
