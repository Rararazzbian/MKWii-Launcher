// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Lobby/Voice/VoiceSettings.h"

#include <algorithm>
#include <mutex>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"

namespace Lobby::Voice
{
namespace
{
constexpr char PEER_SECTION[] = "PeerVolumes";

std::mutex s_mutex;
Settings s_settings;
std::map<std::string, int> s_peer_volumes;
bool s_loaded = false;

std::string PeerFilePath()
{
  return File::GetUserPath(D_CONFIG_IDX) + "LobbyVoice.ini";
}

// A nickname is free-form and an ini key is not: '=' would split the line and a
// leading '[' would look like a section. Percent-encoding everything outside a
// conservative set keeps the file readable for ordinary names while making any
// name at all round-trip.
std::string EncodeKey(std::string_view nickname)
{
  std::string out;
  out.reserve(nickname.size());
  for (const unsigned char c : nickname)
  {
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '-' || c == '_' || c == '.';
    if (plain)
      out.push_back(static_cast<char>(c));
    else
      out += fmt::format("%{:02X}", c);
  }
  return out;
}

std::string DecodeKey(std::string_view key)
{
  std::string out;
  out.reserve(key.size());
  for (std::size_t i = 0; i < key.size(); ++i)
  {
    if (key[i] == '%' && i + 2 < key.size())
    {
      const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        return -1;
      };
      const int hi = digit(key[i + 1]);
      const int lo = digit(key[i + 2]);
      if (hi >= 0 && lo >= 0)
      {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(key[i]);
  }
  return out;
}

void SavePeerVolumes()
{
  Common::IniFile ini;
  const std::string path = PeerFilePath();
  ini.Load(path);
  auto* section = ini.GetOrCreateSection(PEER_SECTION);
  for (const auto& [nickname, volume] : s_peer_volumes)
    section->Set(EncodeKey(nickname), volume);
  ini.Save(path);
}

int ClampPercent(int value, int maximum)
{
  return std::clamp(value, 0, maximum);
}
}  // namespace

void Load()
{
  std::lock_guard lock(s_mutex);

  s_settings.enabled = Config::Get(Config::MAIN_VOICE_ENABLED);
  s_settings.muted = Config::Get(Config::MAIN_VOICE_MUTED);
  s_settings.deafened = Config::Get(Config::MAIN_VOICE_DEAFENED);
  s_settings.bitrate = std::clamp(Config::Get(Config::MAIN_VOICE_BITRATE), MIN_BITRATE,
                                  MAX_BITRATE);
  s_settings.gate_db =
      std::clamp(Config::Get(Config::MAIN_VOICE_GATE_DB), MIN_GATE_DB, MAX_GATE_DB);
  s_settings.mic_gain = ClampPercent(Config::Get(Config::MAIN_VOICE_MIC_GAIN), 200);
  s_settings.master_volume = ClampPercent(Config::Get(Config::MAIN_VOICE_MASTER_VOLUME), 200);
  s_settings.doppler = ClampPercent(Config::Get(Config::MAIN_VOICE_DOPPLER), 200);
  s_settings.proximity_range =
      std::max(500.0f, Config::Get(Config::MAIN_VOICE_PROXIMITY_RANGE));
  // Shared with the rest of the launcher: the wizard asks for a microphone once
  // and voice chat uses whatever it was told.
  s_settings.input_device = Config::Get(Config::MAIN_LOBBY_MICROPHONE);
  s_settings.output_device = Config::Get(Config::MAIN_VOICE_OUTPUT_DEVICE);

  s_peer_volumes.clear();
  Common::IniFile ini;
  if (ini.Load(PeerFilePath()))
  {
    if (const auto* section = ini.GetSection(PEER_SECTION))
    {
      for (const auto& [key, value] : section->GetValues())
      {
        int volume = 100;
        if (TryParse(value, &volume))
          s_peer_volumes[DecodeKey(key)] = ClampPercent(volume, 100);
      }
    }
  }

  s_loaded = true;
}

Settings Get()
{
  {
    std::lock_guard lock(s_mutex);
    if (s_loaded)
      return s_settings;
  }
  // First call. Two threads racing here both run Load(), which is idempotent
  // and cheap - cheaper than holding the lock across file IO on every read.
  Load();
  std::lock_guard lock(s_mutex);
  return s_settings;
}

void SetEnabled(bool enabled)
{
  {
    std::lock_guard lock(s_mutex);
    s_settings.enabled = enabled;
  }
  Config::SetBase(Config::MAIN_VOICE_ENABLED, enabled);
}

void SetMuted(bool muted)
{
  {
    std::lock_guard lock(s_mutex);
    s_settings.muted = muted;
  }
  Config::SetBase(Config::MAIN_VOICE_MUTED, muted);
}

void SetDeafened(bool deafened)
{
  {
    std::lock_guard lock(s_mutex);
    s_settings.deafened = deafened;
  }
  Config::SetBase(Config::MAIN_VOICE_DEAFENED, deafened);
}

void SetBitrate(int bitrate)
{
  bitrate = std::clamp(bitrate, MIN_BITRATE, MAX_BITRATE);
  {
    std::lock_guard lock(s_mutex);
    s_settings.bitrate = bitrate;
  }
  Config::SetBase(Config::MAIN_VOICE_BITRATE, bitrate);
}

void ApplyHostBitrate(int bitrate)
{
  std::lock_guard lock(s_mutex);
  s_settings.bitrate = std::clamp(bitrate, MIN_BITRATE, MAX_BITRATE);
}

void SetGateDb(float db)
{
  db = std::clamp(db, MIN_GATE_DB, MAX_GATE_DB);
  {
    std::lock_guard lock(s_mutex);
    s_settings.gate_db = db;
  }
  Config::SetBase(Config::MAIN_VOICE_GATE_DB, db);
}

void SetMicGain(int percent)
{
  percent = ClampPercent(percent, 200);
  {
    std::lock_guard lock(s_mutex);
    s_settings.mic_gain = percent;
  }
  Config::SetBase(Config::MAIN_VOICE_MIC_GAIN, percent);
}

void SetMasterVolume(int percent)
{
  percent = ClampPercent(percent, 200);
  {
    std::lock_guard lock(s_mutex);
    s_settings.master_volume = percent;
  }
  Config::SetBase(Config::MAIN_VOICE_MASTER_VOLUME, percent);
}

void SetDoppler(int percent)
{
  percent = ClampPercent(percent, 200);
  {
    std::lock_guard lock(s_mutex);
    s_settings.doppler = percent;
  }
  Config::SetBase(Config::MAIN_VOICE_DOPPLER, percent);
}

void SetProximityRange(float range)
{
  range = std::max(500.0f, range);
  {
    std::lock_guard lock(s_mutex);
    s_settings.proximity_range = range;
  }
  Config::SetBase(Config::MAIN_VOICE_PROXIMITY_RANGE, range);
}

void SetInputDevice(std::string device)
{
  {
    std::lock_guard lock(s_mutex);
    s_settings.input_device = device;
  }
  Config::SetBase(Config::MAIN_LOBBY_MICROPHONE, device);
}

void SetOutputDevice(std::string device)
{
  {
    std::lock_guard lock(s_mutex);
    s_settings.output_device = device;
  }
  Config::SetBase(Config::MAIN_VOICE_OUTPUT_DEVICE, device);
}

int GetPeerVolume(std::string_view nickname)
{
  std::lock_guard lock(s_mutex);
  const auto found = s_peer_volumes.find(std::string(nickname));
  if (found == s_peer_volumes.end())
    return 100;
  return found->second;
}

void SetPeerVolume(std::string_view nickname, int percent)
{
  if (nickname.empty())
    return;
  std::lock_guard lock(s_mutex);
  s_peer_volumes[std::string(nickname)] = ClampPercent(percent, 100);
  SavePeerVolumes();
}

std::map<std::string, int> GetAllPeerVolumes()
{
  std::lock_guard lock(s_mutex);
  return s_peer_volumes;
}
}  // namespace Lobby::Voice
