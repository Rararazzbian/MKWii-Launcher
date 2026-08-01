// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Voice chat for a lobby.
//
// Everyone hears everyone. While the lobby is merely up - in menus, on the
// launcher screen, between races - that is all it is: a flat voice channel with
// a per-person volume.
//
// Once an online race is loaded it becomes positional. Every Dolphin instance
// can already read every kart's position out of its own copy of the game, so
// distance does not have to be sent anywhere; the only thing that does is who
// each instance is playing as. Each one reports its active licence name, which
// is matched against the racer names the memory inspector resolves, and from
// then on the two are the same person.
//
// The same applies to the voice filters. A racer's star, mega, bullet, shock
// and squash are all readable locally, so the effect on someone's voice is
// decided by the listener from what they can see happening on their own screen.
//
// Proximity ends for a racer when they cross the finish line, at which point
// they are back on the flat channel - which is the useful behaviour, because
// the alternative is being stuck talking to nobody while you wait for everyone
// else to come in.

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Lobby::Voice
{
struct PeerInfo
{
  u32 ip = 0;
  std::string nickname;
  // The Mii name of their active licence, once they have reported it.
  std::string license;
  // "nickname (licence)", or just the nickname until the licence is known.
  std::string display;
  bool is_local = false;

  // 0-100. Meaningless for the local entry.
  int volume = 100;
  // 0-1, for the meter beside their name.
  float level = 0.0f;
  bool talking = false;

  // Whether proximity is being applied to this peer right now, and how far
  // away they are in world units. -1 when they are not placed on the track.
  bool proximity = false;
  float distance = -1.0f;
  // The gain proximity is currently applying, 0-1.
  float proximity_gain = 1.0f;
};

// Brought up with the lobby and taken down with it. Safe to call twice.
void Start();
void Stop();
bool IsRunning();

// Everyone in the lobby, this machine included, ordered by lobby address.
std::vector<PeerInfo> GetPeers();

// 0-1, post-gain and pre-gate, so the meter shows what the microphone slider is
// actually feeding the gate.
float GetMicLevel();
// 0-1 of everything being played, after per-person volume, proximity and the
// master slider - which is what the master slider's own meter should show.
float GetOutputLevel();
// True while the gate is letting audio through.
bool IsTransmitting();

bool IsCaptureWorking();
bool IsPlaybackWorking();
// One line for the settings window, e.g. why the microphone did not open.
std::string GetStatusText();

// Host only. Applies locally and is pushed to every client, which is why it is
// here rather than only in VoiceSettings.
void SetBitrateAndPush(int bits_per_second);

// Re-opens the audio devices. Called when the device settings change.
void RestartDevices();
}  // namespace Lobby::Voice
