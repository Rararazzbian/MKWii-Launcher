// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The dashboard page, embedded in the binary so the server has nothing to find
// on disk and nothing to get out of sync with.
//
// Only built when MKW_MEMORY_INSPECTOR_WEB is on. See WebServer.h.

#pragma once

#include <string_view>

namespace MemInspect::Web
{
std::string_view DashboardHtml();
}  // namespace MemInspect::Web
