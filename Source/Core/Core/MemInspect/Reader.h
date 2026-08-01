// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Typed reads of emulated guest memory, and the pointer chases that reach the
// game's heap objects.
//
// This is deliberately not built on Memory::MemoryManager's Read_U32 family.
// Those call CopyFromEmu, which raises a modal PanicAlert the moment an address
// is out of range - and an inspector spends most of its time reading pointers
// that are legitimately null, because the objects they point at only exist
// during a race. Every access here is range-checked first and reports failure
// as an empty optional.
//
// Everything is big-endian: the console is PowerPC, so a u32 in guest memory is
// stored most significant byte first.

#pragma once

#include <optional>
#include <span>
#include <string>

#include "Common/CommonTypes.h"

namespace Memory
{
class MemoryManager;
}

namespace MemInspect
{
// The two regions a Wii title's data actually lives in. A retail console has
// 24 MiB of MEM1 and 64 MiB of MEM2; the bounds below are the retail sizes,
// which is what the addresses in Registry.h were derived against.
//
// Anything outside these is treated as unreadable rather than clamped, because
// a pointer that lands outside them is a broken chain, not a valid address in
// some other region.
constexpr u32 MEM1_START = 0x80000000;
constexpr u32 MEM1_END = 0x81800000;
constexpr u32 MEM2_START = 0x90000000;
constexpr u32 MEM2_END = 0x94000000;

// True when `address` looks like a pointer the game would store. Used both to
// validate reads and to decide whether a chased pointer is worth following.
bool IsGuestAddress(u32 address);

// The kinds a watch can be read as. Str16 carries its character count, so it is
// spelled out separately in Watch rather than living here.
enum class Kind
{
  U8,
  S8,
  U16,
  S16,
  U32,
  S32,
  F32,
  F64,
  Ptr,
  Bool,
  Str16,
};

// Parses "u8", "s16", "f32", "ptr", "bool", "str16:10" and so on. Returns
// false for anything unrecognised. `chars` is only written for Str16.
bool ParseKind(std::string_view text, Kind* kind, u32* chars);
// The spelling ParseKind would accept, for round-tripping into JSON.
std::string KindName(Kind kind, u32 chars);
// Bytes on the guest side. Zero for Str16, whose width depends on `chars`.
u32 KindSize(Kind kind);

// One decoded value. Exactly one of the payload fields is meaningful, chosen by
// `kind`; `ok` false means the address could not be read at all, which the
// dashboard shows as "unreadable" rather than as a zero.
struct Reading
{
  bool ok = false;
  Kind kind = Kind::U32;
  // Integral kinds and Bool and Ptr. Signed kinds arrive sign-extended.
  s64 integer = 0;
  // F32 and F64.
  double real = 0.0;
  // Str16, already converted from UTF-16BE to UTF-8.
  std::string text;
};

class Reader
{
public:
  explicit Reader(const Memory::MemoryManager& memory) : m_memory(memory) {}

  // True when [address, address + size) lies entirely inside one region.
  bool CanRead(u32 address, u32 size) const;
  // Raw bytes, exactly as stored - no byte swapping.
  bool Bytes(u32 address, u8* out, u32 size) const;

  std::optional<u8> U8(u32 address) const;
  std::optional<u16> U16(u32 address) const;
  std::optional<u32> U32(u32 address) const;
  std::optional<u64> U64(u32 address) const;
  std::optional<s8> S8(u32 address) const;
  std::optional<s16> S16(u32 address) const;
  std::optional<s32> S32(u32 address) const;
  std::optional<float> F32(u32 address) const;
  std::optional<double> F64(u32 address) const;

  // A pointer field. Empty when the field is unreadable, zero, or holds a value
  // outside MEM1/MEM2 - all three of which mean "this object does not exist
  // right now", which is the normal state outside a race.
  std::optional<u32> Ptr(u32 address) const;

  // An n-character null-padded UTF-16BE string, as the game stores Mii names.
  // Trailing padding is dropped; the result is UTF-8.
  std::optional<std::string> Str16(u32 address, u32 chars) const;

  // Follow a pointer chain. `address` is dereferenced, then each offset except
  // the last is added and dereferenced in turn; the last offset is added to the
  // final pointer and returned WITHOUT being dereferenced, because that is
  // where the value lives.
  //
  // With no offsets the address is static and comes back unchanged.
  std::optional<u32> Chain(u32 address, std::span<const u32> offsets) const;

  // Read one value of any kind. `chars` is only consulted for Kind::Str16.
  Reading Read(u32 address, Kind kind, u32 chars = 0) const;

private:
  const Memory::MemoryManager& m_memory;
};

// Writes, which are range-checked the same way. CopyToEmu panics on a bad
// range just as CopyFromEmu does.
bool WriteU32(Memory::MemoryManager& memory, u32 address, u32 value);
bool WriteS16(Memory::MemoryManager& memory, u32 address, s16 value);
}  // namespace MemInspect
