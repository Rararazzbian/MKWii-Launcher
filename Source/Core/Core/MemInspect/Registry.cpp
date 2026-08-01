// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/MemInspect/Registry.h"

#include <fmt/format.h>

namespace MemInspect
{
const EnumTable& SceneLabels()
{
  static const EnumTable table = {{1, "MENU"}, {2, "RACE"}, {4, "ONLINE MENU"}};
  return table;
}

const EnumTable& OnlineLabels()
{
  static const EnumTable table = {{0, "OFFLINE"}, {1, "ONLINE"}};
  return table;
}

const EnumTable& ProgressLabels()
{
  static const EnumTable table = {{0, "0 - init"},   {2, "2 - loading"}, {3, "3 - pre-race"},
                                  {4, "4 - lap 1"},  {5, "5 - lap 2"},   {6, "6 - lap 3"},
                                  {7, "7 - FINISHED"}};
  return table;
}

const EnumTable& LicenseLabels()
{
  static const EnumTable table = {
      {0, "License 1"}, {1, "License 2"}, {2, "License 3"}, {3, "License 4"}};
  return table;
}

const EnumTable& ItemLabels()
{
  static const EnumTable table = {
      {0, "Green Shell"},
      {1, "Red Shell"},
      {2, "Banana"},
      {3, "Fake Item Box"},
      {4, "Mushroom"},
      {5, "Triple Mushrooms"},
      {6, "Bob-omb"},
      {7, "Blue Shell"},
      {8, "Lightning"},
      {9, "Star"},
      {10, "Golden Mushroom"},
      {11, "Mega Mushroom"},
      {12, "Blooper"},
      {13, "POW Block"},
      {14, "Mega Mushroom (2nd entry)"},
      {15, "Bullet Bill"},
      {16, "Triple Green Shells"},
      {17, "Triple Red Shells"},
      {18, "Triple Bananas"},
      {19, "Triple Bananas (bugged, unused)"},
      {20, "(empty)"},
      {21, "!! CRASHES THE GAME !!"},
  };
  return table;
}

namespace
{
std::vector<Watch> BuildWatches()
{
  std::vector<Watch> watches;

  watches.push_back({.key = "scene_id",
                     .address = SCENE_ID_ADDR,
                     .kind = Kind::U32,
                     .label = "Scene ID",
                     .group = "Scene",
                     .enum_labels = SceneLabels(),
                     .note = "2 = race scene LOADED, 1 = menus, 4 = online top menu. Stays 2 "
                             "through the results table, podium and the Next Race menu - it is "
                             "not 'actively driving'. It also does NOT distinguish online from "
                             "offline: the online course poll reads 1 and an online race reads "
                             "2, same as offline."});

  watches.push_back({.key = "online",
                     .address = ONLINE_ADDR,
                     .kind = Kind::U32,
                     .label = "Online",
                     .group = "Scene",
                     .enum_labels = OnlineLabels(),
                     .note = "0x8038296C - static. 1 for the ENTIRE online session: top menu, "
                             "room, course poll and race. Resets to 0 on backing out to the main "
                             "menu. For 'in an online race' combine with scene_id == 2."});

  watches.push_back(
      {.key = "race_progress",
       .address = RACE_INFO_PTR,
       .kind = Kind::U32,
       .label = "Race Progress",
       .group = "Race",
       .enum_labels = ProgressLabels(),
       .offsets = {RACE_PROGRESS_OFF},
       .note = fmt::format("[0x809BDBB8]+0x40 - flips to {} exactly on GO, then one per lap, "
                           "reaching {} the moment you cross the finish line. Null in menus, "
                           "which shows as 'unreadable'.",
                           PROGRESS_GO, PROGRESS_FINISHED)});

  watches.push_back({.key = "active_license",
                     .address = LICENSE_PTR,
                     .kind = Kind::U32,
                     .label = "Active License",
                     .group = "Save",
                     .enum_labels = LicenseLabels(),
                     .offsets = {LICENSE_ACTIVE_OFF},
                     .note = "[0x809B8F88]+0x34 - ZERO-indexed: raw 0 = the first license on the "
                             "Select License screen. Set when a license is chosen and holds "
                             "through races."});

  // Must come AFTER active_license - it indexes the array with that value.
  watches.push_back(
      {.key = "license_name",
       .address = LICENSE_PTR,
       .kind = Kind::Str16,
       .chars = 10,
       .label = "License Mii Name",
       .group = "Save",
       .offsets = {LICENSE_NAME_OFF},
       .note = fmt::format("[0x809B8F88]+0x{:X}+0x{:X}*index - the Mii name of the ACTIVE "
                           "license. The same array holds all 4 names; change the index to read "
                           "the others.",
                           LICENSE_NAME_OFF, LICENSE_STRIDE),
       .index_key = "active_license",
       .stride = LICENSE_STRIDE});

  for (int k = 0; k < MAX_RACERS; ++k)
  {
    const std::string group = fmt::format("Racer {}", k);
    const std::vector<u32> kart_chain = {KART_LIST_OFF + 4 * static_cast<u32>(k), KART_NODE_OFF};

    struct Axis
    {
      const char* lower;
      const char* upper;
      u32 offset;
      const char* note;
    };

    const Axis position[] = {
        {"x", "X", MTX_POS_X, "world position, Y is vertical"},
        {"y", "Y", MTX_POS_Y, ""},
        {"z", "Z", MTX_POS_Z, ""},
    };
    for (const Axis& axis : position)
    {
      std::vector<u32> chain = kart_chain;
      chain.push_back(KART_MTX_OFF + axis.offset);
      watches.push_back({.key = fmt::format("r{}_pos_{}", k, axis.lower),
                         .address = KART_MGR_PTR,
                         .kind = Kind::F32,
                         .label = fmt::format("Pos {}", axis.upper),
                         .group = group,
                         .offsets = std::move(chain),
                         .note = axis.note});
    }

    // Heading comes from COLUMN 2 of the rotation - the local +Z axis in world
    // space. Row 2 is the transpose and points sideways or backwards.
    const Axis forward[] = {
        {"x", "X", MTX_FWD_X, "unit forward vector; yaw = atan2(fwd_x, fwd_z)"},
        {"z", "Z", MTX_FWD_Z, ""},
    };
    for (const Axis& axis : forward)
    {
      std::vector<u32> chain = kart_chain;
      chain.push_back(KART_MTX_OFF + axis.offset);
      watches.push_back({.key = fmt::format("r{}_fwd_{}", k, axis.lower),
                         .address = KART_MGR_PTR,
                         .kind = Kind::F32,
                         .label = fmt::format("Fwd {}", axis.upper),
                         .group = group,
                         .offsets = std::move(chain),
                         .note = axis.note});
    }

    watches.push_back({.key = fmt::format("r{}_place", k),
                       .address = PLACE_PTR,
                       .kind = Kind::U8,
                       .label = "Place",
                       .group = group,
                       .offsets = {PLACE_OFF + static_cast<u32>(k)},
                       .note = "live race position, 1-12, updates on every overtake"});

    watches.push_back({.key = fmt::format("r{}_grid", k),
                       .address = RACE_CFG_PTR,
                       .kind = Kind::U8,
                       .label = "Grid order?",
                       .group = group,
                       .offsets = {SLOT_STRIDE * static_cast<u32>(k) + SLOT_RANK},
                       .note = "NOT race position. Fixed for a whole race while the HUD position "
                               "changed - probably starting grid order. Use Place instead."});

    watches.push_back({.key = fmt::format("r{}_controller", k),
                       .address = RACE_CFG_PTR,
                       .kind = Kind::S32,
                       .label = "Controller",
                       .group = group,
                       .offsets = {SLOT_STRIDE * static_cast<u32>(k) + SLOT_CONTROLLER},
                       .note = "unreliable: -1 for CPUs AND remote humans, and two slots have "
                               "read 1 at once. Identify yourself by Mii ID."});

    watches.push_back({.key = fmt::format("r{}_vehicle", k),
                       .address = RACE_CFG_PTR,
                       .kind = Kind::U16,
                       .label = "Vehicle ID",
                       .group = group,
                       .offsets = {SLOT_STRIDE * static_cast<u32>(k) + SLOT_VEHICLE}});

    watches.push_back(
        {.key = fmt::format("r{}_item", k),
         .address = ITEM_DIRECTOR_PTR,
         .kind = Kind::U32,
         .label = "Item",
         .group = group,
         .enum_labels = ItemLabels(),
         .offsets = {ITEM_ARRAY_OFF, ITEM_STRIDE * static_cast<u32>(k) + ITEM_ID_OFF},
         .note = "20 = empty. Writable - the dashboard's item buttons put any item in the slot."});

    watches.push_back(
        {.key = fmt::format("r{}_item_count", k),
         .address = ITEM_DIRECTOR_PTR,
         .kind = Kind::U32,
         .label = "Item Count",
         .group = group,
         .offsets = {ITEM_ARRAY_OFF, ITEM_STRIDE * static_cast<u32>(k) + ITEM_COUNT_OFF},
         .note = "3 for a triple, 0 when the slot is empty"});

    // PlayerPointers at player+0x1C is INLINE, so the chain adds 0x1C+0x04 (or
    // +0x28) in one step rather than dereferencing twice.
    const u32 player_slot = PH_PLAYERS_OFF + 4 * static_cast<u32>(k);
    const std::vector<u32> bits_chain = {player_slot, PLAYER_POINTERS_OFF + PP_SUB1C_OFF};
    const std::vector<u32> timer_chain = {player_slot, PLAYER_POINTERS_OFF + PP_SUB10_OFF};

    auto with = [](std::vector<u32> chain, u32 tail) {
      chain.push_back(tail);
      return chain;
    };

    watches.push_back({.key = fmt::format("r{}_bits_star", k),
                       .address = PLAYER_HOLDER_PTR,
                       .kind = Kind::U32,
                       .label = "Bitfield1",
                       .group = group,
                       .offsets = with(bits_chain, BITFIELD1),
                       .note = "bit 31 = in a star",
                       .hex = true});

    watches.push_back({.key = fmt::format("r{}_bits_state", k),
                       .address = PLAYER_HOLDER_PTR,
                       .kind = Kind::U32,
                       .label = "Bitfield2",
                       .group = group,
                       .offsets = with(bits_chain, BITFIELD2),
                       .note = "bit 7 shocked, 15 mega, 16 crushed, 27 bullet, 28 inked",
                       .hex = true});

    watches.push_back({.key = fmt::format("r{}_star_timer", k),
                       .address = PLAYER_HOLDER_PTR,
                       .kind = Kind::S16,
                       .label = "Star Frames",
                       .group = group,
                       .offsets = with(timer_chain, T_STAR),
                       .note = "frames at 60fps; 449 = 7.5s"});

    watches.push_back({.key = fmt::format("r{}_shock_timer", k),
                       .address = PLAYER_HOLDER_PTR,
                       .kind = Kind::S16,
                       .label = "Shock Frames",
                       .group = group,
                       .offsets = with(timer_chain, T_SHOCK),
                       .note = "Lightning, Thunder Cloud or KC Zappers; length depends on race "
                               "position"});

    watches.push_back({.key = fmt::format("r{}_crush_timer", k),
                       .address = PLAYER_HOLDER_PTR,
                       .kind = Kind::S16,
                       .label = "Crush Frames",
                       .group = group,
                       .offsets = with(timer_chain, T_CRUSH),
                       .note = "Thwomp, Mega or a press"});

    watches.push_back({.key = fmt::format("r{}_mega_timer", k),
                       .address = PLAYER_HOLDER_PTR,
                       .kind = Kind::S16,
                       .label = "Mega Frames",
                       .group = group,
                       .offsets = with(timer_chain, T_MEGA)});
  }

  return watches;
}
}  // namespace

const std::vector<Watch>& BuiltinWatches()
{
  static const std::vector<Watch> watches = BuildWatches();
  return watches;
}

const std::vector<DerivedPredicate>& DerivedPredicates()
{
  static const std::vector<DerivedPredicate> predicates = {
      {"in_race", "Race scene is loaded",
       [](const IntLookup& get) -> std::optional<bool> {
         const auto scene = get("scene_id");
         if (!scene)
           return std::nullopt;
         return *scene == 2;
       }},

      // The player can actually drive: from GO until the finish line.
      {"has_control", "Player can actually drive (GO -> finish line)",
       [](const IntLookup& get) -> std::optional<bool> {
         const auto scene = get("scene_id");
         if (!scene)
           return std::nullopt;
         if (*scene != 2)
           return false;
         const auto progress = get("race_progress");
         if (!progress)
           return std::nullopt;
         return *progress >= PROGRESS_GO && *progress < PROGRESS_FINISHED;
       }},

      // Both terms are needed: `online` alone reads 1 across the online menus
      // too, and scene_id alone reads 2 for online and offline races alike.
      {"online_race", "In a race, and that race is online",
       [](const IntLookup& get) -> std::optional<bool> {
         const auto online = get("online");
         const auto scene = get("scene_id");
         if (!online || !scene)
           return std::nullopt;
         return *online == 1 && *scene == 2;
       }},

      {"offline_race", "In a race, and that race is offline",
       [](const IntLookup& get) -> std::optional<bool> {
         const auto online = get("online");
         const auto scene = get("scene_id");
         if (!online || !scene)
           return std::nullopt;
         return *online == 0 && *scene == 2;
       }},
  };
  return predicates;
}
}  // namespace MemInspect
