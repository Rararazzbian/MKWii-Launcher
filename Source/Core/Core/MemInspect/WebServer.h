// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Serves the memory inspector's dashboard over HTTP, from inside Dolphin.
//
// THIS WHOLE FEATURE IS COMPILE-TIME OPTIONAL. Configure with
// -DMKW_MEMORY_INSPECTOR_WEB=OFF and neither this file nor Dashboard.cpp is
// added to the build at all: no server thread, no listening socket, no embedded
// page, no menu entry, and nothing to find in the binary. Everything that calls
// into here is guarded by #ifdef MKW_MEMORY_INSPECTOR_WEB, which the option
// defines on the core target and propagates to everything linking it.
//
// The server listens on the LOOPBACK interface only. It hands out live reads of
// a running game and accepts writes back into it, which is not something to
// expose to a network, so the bind address is not configurable.
//
// It reads snapshots rather than memory: the sampler on the CPU thread
// publishes, this thread copies, and writes asked for over HTTP are queued back
// for the CPU thread to apply. No guest memory is touched here.

#pragma once

#include <string>

#include "Common/CommonTypes.h"

namespace MemInspect::Web
{
// Start listening. Returns false if the port is taken or already running.
bool Start(u16 port);
void Stop();
bool IsRunning();
// The port actually bound, or 0 when stopped.
u16 GetPort();
// "http://127.0.0.1:<port>", or empty when stopped.
std::string GetURL();
}  // namespace MemInspect::Web
