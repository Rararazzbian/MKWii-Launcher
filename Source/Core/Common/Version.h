// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace Common
{
const std::string& GetEmulatorName();
// The name shown to the user in the main window title and while a game is
// running. This fork brands itself, so it is deliberately NOT GetScmRevStr():
// that string is written into savestate headers and shader cache files, where
// changing it would invalidate every file produced by a stock build.
const std::string& GetLauncherTitleStr();
const std::string& GetScmDescStr();
const std::string& GetScmBranchStr();
const std::string& GetScmRevStr();
const std::string& GetScmRevGitStr();
const std::string& GetUserAgentStr();
const std::string& GetScmDistributorStr();
const std::string& GetScmUpdateTrackStr();
const std::string& GetNetplayDolphinVer();
int GetScmCommitsAheadMaster();
}  // namespace Common
