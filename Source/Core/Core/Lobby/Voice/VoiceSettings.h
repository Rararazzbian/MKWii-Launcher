// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Everything the voice chat can be adjusted by, and where it is kept.
//
// The scalars live in Dolphin.ini under [MKWiiVoice] through the usual Config
// system. Per-peer volumes cannot: their keys are player nicknames, which are
// not known at build time, so they go in User\Config\LobbyVoice.ini instead -
// keyed by nickname rather than by lobby address, because an address is handed
// out fresh each session and would forget everyone's volume the moment the
// lobby was rebuilt.
//
// Reads happen on the audio worker sixty times a second and writes happen from
// the UI, so the live values are held in an atomic snapshot rather than being
// fetched from the config system on every frame.

#pragma once

#include <map>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Lobby::Voice
{
// Opus is happy from 6 kbit/s up, but a lobby is a LAN and there is no reason
// to go below telephone quality. The ceiling is what the user asked for and is
// well past the point where Opus stops improving on speech.
constexpr int MIN_BITRATE = 8000;
constexpr int DEFAULT_BITRATE = 64000;
constexpr int MAX_BITRATE = 256000;

// A gate this low is effectively open; it exists so a noisy room can be shut
// out without also cutting off quiet speech.
constexpr float MIN_GATE_DB = -90.0f;
constexpr float DEFAULT_GATE_DB = -60.0f;
constexpr float MAX_GATE_DB = 0.0f;

// World units. A kart at 100 km/h covers roughly 3800 of them a second, so the
// default range is a little over two seconds of driving - far enough to hear
// someone coming, close enough that the far side of the track is silent.
constexpr float DEFAULT_PROXIMITY_RANGE = 9000.0f;
// Inside this, no attenuation at all. Without it, two karts side by side would
// still be quieter than two karts touching, which is not what anyone expects.
constexpr float DEFAULT_PROXIMITY_NEAR = 1200.0f;

struct Settings
{
  bool enabled = true;
  // Stop sending. Deafen also stops sending, as it does everywhere else - being
  // heard while unable to hear the reply is not a state anyone wants.
  bool muted = false;
  bool deafened = false;

  // Set by the host and pushed to everyone; a client's own value is only what
  // it last heard.
  int bitrate = DEFAULT_BITRATE;

  float gate_db = DEFAULT_GATE_DB;
  // Percent. 100 is unity for all three.
  int mic_gain = 100;      // 0-200
  int master_volume = 100; // 0-200
  int doppler = 100;       // 0-200

  float proximity_range = DEFAULT_PROXIMITY_RANGE;

  // cubeb device ids. Empty means the system default.
  std::string input_device;
  std::string output_device;
};

// The live values. Cheap enough to call per audio frame.
Settings Get();

// Each of these writes through to Dolphin.ini as well as to the live copy.
void SetEnabled(bool enabled);
void SetMuted(bool muted);
void SetDeafened(bool deafened);
void SetBitrate(int bitrate);
void SetGateDb(float db);
void SetMicGain(int percent);
void SetMasterVolume(int percent);
void SetDoppler(int percent);
void SetProximityRange(float range);
void SetInputDevice(std::string device);
void SetOutputDevice(std::string device);

// Applies a bitrate that arrived from the host. Does not write to Dolphin.ini:
// a client's stored bitrate should stay whatever it would use if it hosted,
// rather than being overwritten by whoever it last joined.
void ApplyHostBitrate(int bitrate);

// Per-peer volume, 0-100, keyed by nickname. Unknown peers are 100.
int GetPeerVolume(std::string_view nickname);
void SetPeerVolume(std::string_view nickname, int percent);
// Everything on file, for the settings window to show peers who are not in the
// lobby right now.
std::map<std::string, int> GetAllPeerVolumes();

// Loads Dolphin.ini values and LobbyVoice.ini into the live copy. Called when
// voice chat starts; safe to call again.
void Load();
}  // namespace Lobby::Voice
