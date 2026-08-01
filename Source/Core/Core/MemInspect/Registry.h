// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The map: every address the inspector knows about, and what it means.
//
// ADDING A NEW VALUE = ADD ONE ENTRY TO BuiltinWatches(). Nothing else needs to
// change - the poller reads it, the JSON carries it and the dashboard draws a
// card for it automatically.
//
// These addresses were derived empirically from one specific build: Mario Kart
// Wii RMCE01 running CTGP Revolution, which ships a patched DOL. Published
// stock-MKWii addresses do not necessarily apply and vice versa. The inspector
// reports the running title alongside the values so a mismatch is visible
// rather than silently producing plausible nonsense.
//
// Three of these have an easy-to-get-wrong base, each of which produces output
// that looks fine until it is checked against ground truth:
//
//   * the kart array starts at mgr+0x24, not mgr+0x2C. Reading from +0x2C
//     skips the first two racers - including the human player, who is entry 0 -
//     and shifts every index by two.
//   * RaceConfig records are at +0xF0*k, not +0xF0*(k+1). One record too late
//     attributes every racer's rank, vehicle and Mii to the racer behind them.
//   * the transform's basis vectors are its COLUMNS, not its rows. Row 2 is the
//     transpose and points sideways; column 2 scored 0.906 mean dot against
//     direction of travel over a race where row 2 managed 0.467.

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/MemInspect/Reader.h"

namespace MemInspect
{
// The build this registry was derived from. The snapshot carries the running
// title so the dashboard can warn when they differ.
constexpr std::string_view GAME_ID = "RMCE01";
constexpr std::string_view GAME_NOTE = "CTGP Revolution (custom distribution on RMCE01)";

constexpr int MAX_RACERS = 12;

// --- Scene ------------------------------------------------------------------
// Static, in a scene-manager struct in .data, so the address is fixed across
// reboots. 2 means the race scene is LOADED, which is not the same as the
// player being able to drive - it stays 2 through the results table, the podium
// and the Next Race menu.
constexpr u32 SCENE_ID_ADDR = 0x80444804;
// Static. 1 for the ENTIRE online session - top menu, room, course poll and
// race - not just the race. Resets to 0 on backing out to the main menu.
constexpr u32 ONLINE_ADDR = 0x8038296C;

// --- Race progress ----------------------------------------------------------
// [0x809BDBB8]+0x40. Null in menus, which shows as "unreadable".
constexpr u32 RACE_INFO_PTR = 0x809BDBB8;
constexpr u32 RACE_PROGRESS_OFF = 0x040;
// Lap-indexed: 4 is GO, then one per lap, reaching 7 the moment the finish line
// is crossed on a 3-lap race. A 5-lap race would finish at 9, so both bounds
// are named rather than written inline.
constexpr s64 PROGRESS_GO = 4;
constexpr s64 PROGRESS_FINISHED = 7;

// --- Save data --------------------------------------------------------------
// The license manager holds an array of 4 license records. The active-license
// field is ZERO-indexed, so raw 0 is the first license on the Select License
// screen.
constexpr u32 LICENSE_PTR = 0x809B8F88;
constexpr u32 LICENSE_ACTIVE_OFF = 0x034;
constexpr u32 LICENSE_NAME_OFF = 0x038;  // Mii name, UTF-16BE, 10 chars
constexpr u32 LICENSE_MIIID_OFF = 0x016;  // relative to the name field
constexpr u32 LICENSE_STRIDE = 0x93F0;

// --- Karts ------------------------------------------------------------------
//   [0x809BE398]        kart manager    (static, null outside a race)
//   mgr + 0x14          racer count
//   mgr + 0x24 + 4*k    racer k's kart object
//   mgr + 0x54          the local player's kart (repeats one array entry, so a
//                       naive scan of the array looks like 13 karts, not 12)
//   kart + 0x08         its transform node
//   node + 0x1C         a row-major Mtx34: 3x3 rotation, translation in col 3
constexpr u32 KART_MGR_PTR = 0x809BE398;
constexpr u32 KART_COUNT_OFF = 0x014;
constexpr u32 KART_LIST_OFF = 0x024;
constexpr u32 KART_LOCAL_OFF = 0x054;
constexpr u32 KART_NODE_OFF = 0x008;
constexpr u32 KART_MTX_OFF = 0x01C;
// X, Y, Z: the translation column. The three components are 0x10 APART, not
// contiguous - reading 12 bytes from the X offset gives a rotation row instead.
constexpr u32 MTX_POS_X = 0x0C, MTX_POS_Y = 0x1C, MTX_POS_Z = 0x2C;
// m02, m12, m22: the local +Z axis in world space. See the header comment.
constexpr u32 MTX_FWD_X = 0x08, MTX_FWD_Z = 0x28;

// --- RaceConfig: who each racer is ------------------------------------------
constexpr u32 RACE_CFG_PTR = 0x809B8F68;
constexpr u32 SLOT_STRIDE = 0x0F0;
constexpr u32 SLOT_CONTROLLER = 0x008;  // s32: 1 = human, -1 = CPU or remote
constexpr u32 SLOT_VEHICLE = 0x010;     // u16
constexpr u32 SLOT_RANK = 0x018;        // u8 - NOT race position
constexpr u32 SLOT_MIINAME = 0x0A4;     // UTF-16BE, 10 chars
constexpr u32 SLOT_MIIID = 0x0D0;       // 8 bytes

// --- Roster: every player's Mii, remote players included --------------------
// Order is NOT race-slot order, and the table is SPARSE: with 3 players the
// entries were seen at indices 0, 1 and 3 with 2 blank.
constexpr u32 ROSTER_PTR = 0x809BD958;
constexpr u32 ROSTER_STRIDE = 0x0C0;
constexpr u32 ROSTER_NAME = 0x012;
constexpr u32 ROSTER_MIIID = 0x028;
constexpr u32 MIIID_SIZE = 8;

// --- Live race position -----------------------------------------------------
// [0x809BE740] + 0x161 + k = racer k's place, 1..12. Indexed BY RACER (not "who
// is in place p"), 1-indexed, and it really does update on every overtake.
constexpr u32 PLACE_PTR = 0x809BE740;
constexpr u32 PLACE_OFF = 0x161;

// --- Item slot --------------------------------------------------------------
// [[0x809BEE20]+0x14] + 0x248*k. Writable - see Items.h.
constexpr u32 ITEM_DIRECTOR_PTR = 0x809BEE20;
constexpr u32 ITEM_ARRAY_OFF = 0x014;
constexpr u32 ITEM_STRIDE = 0x248;
constexpr u32 ITEM_ID_OFF = 0x08C;
constexpr u32 ITEM_COUNT_OFF = 0x090;

// --- Per-racer status -------------------------------------------------------
//   [0x809BD110]          PlayerHolder    (static, null outside a race)
//     + 0x24              u8 player count
//     + 0x20              Player **players
//   players[k] + 0x1C     PlayerPointers  INLINE, not a pointer
//     + 0x04              PlayerSub1c *   bitfields
//     + 0x28              PlayerSub10 *   timers
//
// Layout from SeekyCt/mkw-structures (player.h). Only the static instance had
// to be found for this build, by scanning .data/.bss for a pointer to an object
// whose u8 at +0x24 equals the racer count and whose +0x20 points at that many
// valid Player pointers. Exactly one address matched.
constexpr u32 PLAYER_HOLDER_PTR = 0x809BD110;
constexpr u32 PH_PLAYERS_OFF = 0x020;
constexpr u32 PH_COUNT_OFF = 0x024;
constexpr u32 PLAYER_POINTERS_OFF = 0x01C;
constexpr u32 PP_SUB1C_OFF = 0x004;
constexpr u32 PP_SUB10_OFF = 0x028;

constexpr u32 BITFIELD1 = 0x008;  // bit 31 star
constexpr u32 BITFIELD2 = 0x00C;  // bit 7 shocked, 15 mega, 16 crushed, ...
constexpr u32 BITFIELD4 = 0x014;  // bit 0 cpu, 1 real local, 2 local, 3 remote

constexpr u32 T_STAR = 0x18A;
constexpr u32 T_SHOCK = 0x18C;
constexpr u32 T_INK = 0x18E;
constexpr u32 T_CRUSH = 0x192;
constexpr u32 T_MEGA = 0x194;

// --- Value tables -----------------------------------------------------------
using EnumTable = std::map<s64, std::string>;

const EnumTable& SceneLabels();
const EnumTable& OnlineLabels();
const EnumTable& ProgressLabels();
const EnumTable& LicenseLabels();
// RMCE01 + CTGP Revolution, NOT the stock table: 14 is a second Mega Mushroom
// where stock has the Thunder Cloud, 19 is a bugged unused entry, and 21 is one
// past the end of the table and crashes the game.
const EnumTable& ItemLabels();

// --- Watches ----------------------------------------------------------------
struct Watch
{
  // Unique id. Used as the JSON key and in the page's DOM.
  std::string key;
  // A static address, or the address of a pointer when `offsets` is set.
  u32 address = 0;
  Kind kind = Kind::U32;
  u32 chars = 0;  // Str16 only
  std::string label;
  std::string group;
  // Value -> display name.
  EnumTable enum_labels;
  // Pointer chain from `address`. See Reader::Chain.
  std::vector<u32> offsets;
  // Shown under the value on the dashboard.
  std::string note;
  // Display the raw as hex.
  bool hex = false;
  // Array element: after resolving, add `stride` * the value of the watch named
  // by `index_key`. That watch must appear EARLIER in the list.
  std::string index_key;
  u32 stride = 0;
  // Added from the dashboard rather than from this file.
  bool user = false;
};

// The permanent registry, built once and cached.
const std::vector<Watch>& BuiltinWatches();

// --- Derived predicates -----------------------------------------------------
// Computed from the watches above rather than read from memory. The lookup
// returns a watch's integer value, or nothing when it was unreadable - so a
// predicate over a null pointer chain comes back unknown instead of false.
using IntLookup = std::function<std::optional<s64>(std::string_view)>;

struct DerivedPredicate
{
  std::string name;
  std::string description;
  std::optional<bool> (*evaluate)(const IntLookup&) = nullptr;
};

const std::vector<DerivedPredicate>& DerivedPredicates();
}  // namespace MemInspect
