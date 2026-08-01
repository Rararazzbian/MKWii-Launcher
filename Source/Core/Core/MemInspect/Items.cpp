// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/MemInspect/Items.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>

#include "Core/HW/Memmap.h"
#include "Core/MemInspect/Registry.h"

namespace MemInspect
{
namespace
{
// Canonical names, then the aliases people actually type. Both are matched
// case-insensitively after collapsing runs of whitespace.
const std::map<std::string, int, std::less<>>& ItemsByName()
{
  static const std::map<std::string, int, std::less<>> table = {
      {"green", 0},           {"red", 1},          {"banana", 2},
      {"fib", 3},             {"mushroom", 4},     {"triple mushroom", 5},
      {"bomb", 6},            {"blue", 7},         {"lightning", 8},
      {"star", 9},            {"golden", 10},      {"mega", 11},
      {"blooper", 12},        {"pow", 13},         {"mega2", 14},
      {"bullet", 15},         {"triple green", 16},{"triple red", 17},
      {"triple banana", 18},  {"bugged banana", 19},
      {"none", ITEM_NONE},
  };
  return table;
}

const std::map<std::string, std::string, std::less<>>& Aliases()
{
  static const std::map<std::string, std::string, std::less<>> table = {
      {"green shell", "green"},      {"greenshell", "green"},
      {"gs", "green"},               {"red shell", "red"},
      {"redshell", "red"},           {"rs", "red"},
      {"fake", "fib"},               {"fake item box", "fib"},
      {"fake box", "fib"},           {"shroom", "mushroom"},
      {"mush", "mushroom"},          {"triple shroom", "triple mushroom"},
      {"3 mushroom", "triple mushroom"},
      {"bob-omb", "bomb"},           {"bobomb", "bomb"},
      {"bomb-omb", "bomb"},          {"blue shell", "blue"},
      {"spiny", "blue"},             {"bs", "blue"},
      {"thunder", "lightning"},      {"thunderbolt", "lightning"},
      {"golden mushroom", "golden"}, {"gold", "golden"},
      {"gm", "golden"},              {"mega mushroom", "mega"},
      {"squid", "blooper"},          {"ink", "blooper"},
      {"pow block", "pow"},          {"mega mushroom 2", "mega2"},
      {"mega 2", "mega2"},           {"bullet bill", "bullet"},
      {"bill", "bullet"},            {"3 green", "triple green"},
      {"tgs", "triple green"},       {"3 red", "triple red"},
      {"trs", "triple red"},         {"3 banana", "triple banana"},
      {"triple nana", "triple banana"},
      {"empty", "none"},             {"nothing", "none"},
      {"clear", "none"},
  };
  return table;
}

// Items that come as a set of three.
int DefaultCount(int id)
{
  switch (id)
  {
  case 5:   // Triple Mushrooms
  case 16:  // Triple Green Shells
  case 17:  // Triple Red Shells
  case 18:  // Triple Bananas
    return 3;
  case ITEM_NONE:
    return 0;
  default:
    return 1;
  }
}

std::string Normalise(std::string_view text)
{
  std::string out;
  out.reserve(text.size());
  bool pending_space = false;
  for (const char c : text)
  {
    if (std::isspace(static_cast<unsigned char>(c)) != 0)
    {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space)
    {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}
}  // namespace

int KartCount(const Reader& reader)
{
  const auto manager = reader.Ptr(KART_MGR_PTR);
  if (!manager)
    return 0;
  const auto count = reader.U32(*manager + KART_COUNT_OFF);
  if (!count || *count < 1 || *count > static_cast<u32>(MAX_RACERS))
    return 0;
  return static_cast<int>(*count);
}

u32 ItemSlotAddress(const Reader& reader, int k)
{
  if (k < 0 || k >= MAX_RACERS)
    return 0;
  const auto director = reader.Ptr(ITEM_DIRECTOR_PTR);
  if (!director)
    return 0;
  const auto array = reader.Ptr(*director + ITEM_ARRAY_OFF);
  if (!array)
    return 0;
  return *array + ITEM_STRIDE * static_cast<u32>(k);
}

std::optional<ItemSlot> ReadItem(const Reader& reader, int k)
{
  const u32 base = ItemSlotAddress(reader, k);
  if (base == 0)
    return std::nullopt;
  const auto id = reader.U32(base + ITEM_ID_OFF);
  const auto count = reader.U32(base + ITEM_COUNT_OFF);
  if (!id || !count)
    return std::nullopt;

  ItemSlot slot;
  slot.index = k;
  slot.id = *id;
  slot.count = *count;
  slot.name = ItemName(static_cast<int>(*id));
  slot.empty = *id == static_cast<u32>(ITEM_NONE);
  return slot;
}

std::vector<ItemSlot> ReadAllItems(const Reader& reader)
{
  std::vector<ItemSlot> out;
  const int count = KartCount(reader);
  out.reserve(static_cast<std::size_t>(count));
  for (int k = 0; k < count; ++k)
  {
    if (auto slot = ReadItem(reader, k))
      out.push_back(std::move(*slot));
  }
  return out;
}

std::optional<int> LookupItem(std::string_view text)
{
  const std::string key = Normalise(text);
  if (key.empty())
    return std::nullopt;

  if (std::ranges::all_of(key, [](char c) { return c >= '0' && c <= '9'; }))
  {
    // Bounded so a pasted address cannot become an item id.
    if (key.size() > 4)
      return std::nullopt;
    return std::stoi(key);
  }

  const auto& aliases = Aliases();
  const auto alias = aliases.find(key);
  const std::string& canonical = alias != aliases.end() ? alias->second : key;

  const auto& items = ItemsByName();
  const auto found = items.find(canonical);
  if (found == items.end())
    return std::nullopt;
  return found->second;
}

std::string ItemName(int id)
{
  const auto& labels = ItemLabels();
  const auto found = labels.find(id);
  if (found != labels.end())
    return found->second;
  return "id " + std::to_string(id);
}

std::string_view ItemRisk(int id)
{
  if (id == 19)
    return "bugged unused Triple Bananas";
  if (id == ITEM_CRASH_ID)
    return "past the end of the item table - crashes the game";
  if (id > ITEM_CRASH_ID)
    return "reads past the end of the item table";
  return {};
}

bool GiveItem(Memory::MemoryManager& memory, int k, int id, std::optional<int> count, bool force)
{
  if (!force && (id < 0 || id > ITEM_MAX_ID))
    return false;

  const Reader reader(memory);
  const u32 base = ItemSlotAddress(reader, k);
  if (base == 0)
    return false;

  const int wanted = count.value_or(DefaultCount(id));
  if (!WriteU32(memory, base + ITEM_ID_OFF, static_cast<u32>(id)))
    return false;
  return WriteU32(memory, base + ITEM_COUNT_OFF, static_cast<u32>(std::max(0, wanted)));
}
}  // namespace MemInspect
