// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/MemInspect/Reader.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"

namespace MemInspect
{
bool IsGuestAddress(u32 address)
{
  return (address >= MEM1_START && address < MEM1_END) ||
         (address >= MEM2_START && address < MEM2_END);
}

bool ParseKind(std::string_view text, Kind* kind, u32* chars)
{
  *chars = 0;

  // "str16:<n>" is the only parameterised kind. Anything with a colon that is
  // not that spelling is rejected rather than guessed at.
  constexpr std::string_view STR16 = "str16:";
  if (text.starts_with(STR16))
  {
    const std::string_view count = text.substr(STR16.size());
    if (count.empty())
      return false;
    u32 n = 0;
    for (const char c : count)
    {
      if (c < '0' || c > '9')
        return false;
      n = n * 10 + static_cast<u32>(c - '0');
      // A Mii name is 10 characters; anything near a kilobyte of string is a
      // typo, and reading it would stall the poller.
      if (n > 256)
        return false;
    }
    if (n == 0)
      return false;
    *kind = Kind::Str16;
    *chars = n;
    return true;
  }

  static constexpr std::array<std::pair<std::string_view, Kind>, 10> NAMES = {{
      {"u8", Kind::U8},
      {"s8", Kind::S8},
      {"u16", Kind::U16},
      {"s16", Kind::S16},
      {"u32", Kind::U32},
      {"s32", Kind::S32},
      {"f32", Kind::F32},
      {"f64", Kind::F64},
      {"ptr", Kind::Ptr},
      {"bool", Kind::Bool},
  }};

  for (const auto& [name, value] : NAMES)
  {
    if (text == name)
    {
      *kind = value;
      return true;
    }
  }
  return false;
}

std::string KindName(Kind kind, u32 chars)
{
  switch (kind)
  {
  case Kind::U8:
    return "u8";
  case Kind::S8:
    return "s8";
  case Kind::U16:
    return "u16";
  case Kind::S16:
    return "s16";
  case Kind::U32:
    return "u32";
  case Kind::S32:
    return "s32";
  case Kind::F32:
    return "f32";
  case Kind::F64:
    return "f64";
  case Kind::Ptr:
    return "ptr";
  case Kind::Bool:
    return "bool";
  case Kind::Str16:
    return "str16:" + std::to_string(chars);
  }
  return "u32";
}

u32 KindSize(Kind kind)
{
  switch (kind)
  {
  case Kind::U8:
  case Kind::S8:
  case Kind::Bool:
    return 1;
  case Kind::U16:
  case Kind::S16:
    return 2;
  case Kind::U32:
  case Kind::S32:
  case Kind::F32:
  case Kind::Ptr:
    return 4;
  case Kind::F64:
    return 8;
  case Kind::Str16:
    return 0;
  }
  return 0;
}

bool Reader::CanRead(u32 address, u32 size) const
{
  if (size == 0)
    return false;
  // Guard the wrap before the range test, or a huge size would let a high
  // address pass by overflowing back into the region.
  if (address > 0xFFFFFFFF - size)
    return false;
  const u32 last = address + size - 1;

  if (address >= MEM1_START && last < MEM1_END)
    return (address & 0x3FFFFFFF) + size <= m_memory.GetRamSizeReal();
  if (address >= MEM2_START && last < MEM2_END)
    return (address & 0x0FFFFFFF) + size <= m_memory.GetExRamSizeReal();
  return false;
}

bool Reader::Bytes(u32 address, u8* out, u32 size) const
{
  if (!CanRead(address, size))
    return false;
  // Safe now: the range has been checked against the real region sizes, so
  // GetPointerForRange cannot fail and cannot raise a panic alert.
  const u8* source = m_memory.GetPointerForRange(address, size);
  if (source == nullptr)
    return false;
  std::memcpy(out, source, size);
  return true;
}

std::optional<u8> Reader::U8(u32 address) const
{
  u8 value = 0;
  if (!Bytes(address, &value, sizeof(value)))
    return std::nullopt;
  return value;
}

std::optional<u16> Reader::U16(u32 address) const
{
  u16 value = 0;
  if (!Bytes(address, reinterpret_cast<u8*>(&value), sizeof(value)))
    return std::nullopt;
  return Common::swap16(value);
}

std::optional<u32> Reader::U32(u32 address) const
{
  u32 value = 0;
  if (!Bytes(address, reinterpret_cast<u8*>(&value), sizeof(value)))
    return std::nullopt;
  return Common::swap32(value);
}

std::optional<u64> Reader::U64(u32 address) const
{
  u64 value = 0;
  if (!Bytes(address, reinterpret_cast<u8*>(&value), sizeof(value)))
    return std::nullopt;
  return Common::swap64(value);
}

std::optional<s8> Reader::S8(u32 address) const
{
  const auto value = U8(address);
  if (!value)
    return std::nullopt;
  return static_cast<s8>(*value);
}

std::optional<s16> Reader::S16(u32 address) const
{
  const auto value = U16(address);
  if (!value)
    return std::nullopt;
  return static_cast<s16>(*value);
}

std::optional<s32> Reader::S32(u32 address) const
{
  const auto value = U32(address);
  if (!value)
    return std::nullopt;
  return static_cast<s32>(*value);
}

std::optional<float> Reader::F32(u32 address) const
{
  const auto value = U32(address);
  if (!value)
    return std::nullopt;
  float out = 0.0f;
  std::memcpy(&out, &*value, sizeof(out));
  return out;
}

std::optional<double> Reader::F64(u32 address) const
{
  const auto value = U64(address);
  if (!value)
    return std::nullopt;
  double out = 0.0;
  std::memcpy(&out, &*value, sizeof(out));
  return out;
}

std::optional<u32> Reader::Ptr(u32 address) const
{
  const auto value = U32(address);
  if (!value || !IsGuestAddress(*value))
    return std::nullopt;
  return *value;
}

std::optional<std::string> Reader::Str16(u32 address, u32 chars) const
{
  if (chars == 0)
    return std::nullopt;
  std::u16string raw(chars, u'\0');
  if (!Bytes(address, reinterpret_cast<u8*>(raw.data()), chars * 2))
    return std::nullopt;
  for (char16_t& c : raw)
    c = static_cast<char16_t>(Common::swap16(static_cast<u16>(c)));
  // The field is null-PADDED rather than null-terminated, so stop at the first
  // terminator; carrying the padding through would append U+0000 characters to
  // every short name. find() returning npos leaves the whole string.
  raw.resize(std::min(raw.find(u'\0'), raw.size()));
  return UTF16ToUTF8(raw);
}

std::optional<u32> Reader::Chain(u32 address, std::span<const u32> offsets) const
{
  if (offsets.empty())
    return address;

  u32 cursor = address;
  for (std::size_t i = 0; i + 1 < offsets.size(); ++i)
  {
    const auto next = Ptr(cursor);
    if (!next)
      return std::nullopt;
    cursor = *next + offsets[i];
  }
  const auto last = Ptr(cursor);
  if (!last)
    return std::nullopt;
  return *last + offsets.back();
}

Reading Reader::Read(u32 address, Kind kind, u32 chars) const
{
  Reading out;
  out.kind = kind;

  switch (kind)
  {
  case Kind::U8:
    if (const auto v = U8(address))
    {
      out.ok = true;
      out.integer = *v;
    }
    break;
  case Kind::S8:
    if (const auto v = S8(address))
    {
      out.ok = true;
      out.integer = *v;
    }
    break;
  case Kind::U16:
    if (const auto v = U16(address))
    {
      out.ok = true;
      out.integer = *v;
    }
    break;
  case Kind::S16:
    if (const auto v = S16(address))
    {
      out.ok = true;
      out.integer = *v;
    }
    break;
  case Kind::U32:
  case Kind::Ptr:
    // Ptr is read as a plain u32 here rather than through Ptr(), because a
    // watch on a pointer field wants to SEE the null - that is the whole
    // signal that the object is gone.
    if (const auto v = U32(address))
    {
      out.ok = true;
      out.integer = *v;
    }
    break;
  case Kind::S32:
    if (const auto v = S32(address))
    {
      out.ok = true;
      out.integer = *v;
    }
    break;
  case Kind::Bool:
    if (const auto v = U8(address))
    {
      out.ok = true;
      out.integer = *v != 0 ? 1 : 0;
    }
    break;
  case Kind::F32:
    if (const auto v = F32(address))
    {
      out.ok = true;
      out.real = *v;
    }
    break;
  case Kind::F64:
    if (const auto v = F64(address))
    {
      out.ok = true;
      out.real = *v;
    }
    break;
  case Kind::Str16:
    if (auto v = Str16(address, chars))
    {
      out.ok = true;
      out.text = std::move(*v);
    }
    break;
  }
  return out;
}

bool WriteU32(Memory::MemoryManager& memory, u32 address, u32 value)
{
  const Reader reader(memory);
  if (!reader.CanRead(address, sizeof(value)))
    return false;
  memory.Write_U32(value, address);
  return true;
}

bool WriteS16(Memory::MemoryManager& memory, u32 address, s16 value)
{
  const Reader reader(memory);
  if (!reader.CanRead(address, sizeof(value)))
    return false;
  memory.Write_U16(static_cast<u16>(value), address);
  return true;
}
}  // namespace MemInspect
