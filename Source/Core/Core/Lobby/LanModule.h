// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The LAN Play Module, and the Brainslug loader that applies it.
//
// Mario Kart Wii's multiplayer talks to Nintendo's servers, which are gone. The
// module patches it to play over a LAN instead, which is what makes the lobby's
// virtual network worth having. It is not part of this build: it is chadsoft's,
// downloaded on demand and unpacked onto the emulated SD card.
//
// Nothing here knows about a frontend. The desktop wizard and the Android setup
// both need the same download, the same unpack and the same SD sync, so it lives
// in Core with a progress callback rather than in either of them.

#pragma once

#include <functional>
#include <string>

#include "Common/CommonTypes.h"

namespace Lobby::LanModule
{
// Called as the install proceeds. `total` is zero while the size is unknown, or
// for stages that cannot report one. Return false to give up; the install then
// stops at the next check and reports itself cancelled.
using ProgressCallback = std::function<bool(const std::string& stage, s64 current, s64 total)>;

// Downloads the module and unpacks it onto the SD card, then writes the card
// image so a booting console can see it.
//
// Returns an empty string on success, or a message worth showing someone. A
// cancelled install returns a message too - it did not finish, and silence would
// be indistinguishable from success.
std::string DownloadAndInstall(const ProgressCallback& progress);

// Whether the Brainslug loader is on the SD card. Not a checksum: it answers
// "is there something to boot", not "is it the version we shipped against".
bool IsInstalled();

// The loader to boot instead of the disc, so the module is applied first.
// Brainslug then starts the game itself, which it finds through the default ISO
// setting - so that has to be pointing at the game before this is booted.
std::string BrainslugPath();
}  // namespace Lobby::LanModule
