// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Lobby/Voice/VoiceChat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Core/Lobby/LobbyNet.h"
#include "Core/Lobby/NetTrace.h"
#include "Core/Lobby/Voice/VoiceAudio.h"
#include "Core/Lobby/Voice/VoiceCodec.h"
#include "Core/Lobby/Voice/VoiceDsp.h"
#include "Core/Lobby/Voice/VoiceSettings.h"
#include "Core/MemInspect/Inspector.h"

namespace Lobby::Voice
{
namespace
{
constexpr float PI_F = 3.14159265358979323846f;
using Trace::Cat;

constexpr u8 WIRE_VERSION = 1;

// Control message kinds, in the payload of a TYPE_VOICE_CONTROL frame.
enum : u8
{
  CTRL_IDENTITY = 1,
  CTRL_BITRATE = 2,
};

constexpr u8 IDENTITY_RACING = 1 << 0;
constexpr u8 IDENTITY_FINISHED = 1 << 1;

// How often identity and race state go out. Twice a second is far more often
// than a licence changes and quite often enough for a finish to take effect
// without anyone noticing the delay.
constexpr int IDENTITY_INTERVAL_MS = 500;

// How much decoded audio to keep queued for the output device. Two frames of
// slack absorbs a late wake-up without adding latency anyone would notice.
constexpr std::size_t PLAYBACK_TARGET_FRAMES = 3;

// Jitter buffer depth. Wait for this many frames before starting to play a
// peer, so a burst of reordering at the start does not immediately underrun.
constexpr std::size_t JITTER_PREBUFFER = 2;
// Past this, the link is delivering faster than it is being played and the
// buffer is pure latency. Drop back to the prebuffer depth.
constexpr std::size_t JITTER_MAX = 10;

// Never run more than this many frames in one pass, so a stall cannot turn into
// an unbounded catch-up that blocks the worker for seconds.
constexpr int MAX_FRAMES_PER_PASS = 8;

// --- Effects ---------------------------------------------------------------

// Pitch ratios for the states, as playback rates: above 1 is higher, below 1 is
// lower. Tuned by ear - unmistakable, but words still have to be words, and the
// deep one in particular stops well short of the octave down that turns speech
// into a growl.
constexpr float RATIO_DEEP = 0.75f;       // mega, bullet
constexpr float RATIO_CHIPMUNK = 1.28f;   // shocked, crushed
constexpr float STAR_ECHO_WET = 0.5f;

// The "speed of sound" for Doppler, in world units per second. Not physical -
// the game's units are not metres - and deliberately high, because the honest
// figure makes a pass-by sound like a siren rather than like a kart.
//
// A kart at 100 km/h covers roughly 3800 units a second, so a head-on pass
// closes at up to twice that; against this the shift stays subtle enough to
// read as movement rather than as an effect.
constexpr float DOPPLER_SOUND_SPEED = 48000.0f;
constexpr float DOPPLER_MIN_RATIO = 0.7f;
constexpr float DOPPLER_MAX_RATIO = 1.4f;

// --- Wire helpers ----------------------------------------------------------

void PutU16(std::vector<u8>& out, u16 value)
{
  out.push_back(static_cast<u8>(value >> 8));
  out.push_back(static_cast<u8>(value & 0xFF));
}

void PutString(std::vector<u8>& out, const std::string& text)
{
  const std::size_t length = std::min<std::size_t>(text.size(), 255);
  out.push_back(static_cast<u8>(length));
  out.insert(out.end(), text.begin(), text.begin() + length);
}

bool TakeString(const u8*& cursor, const u8* end, std::string* out)
{
  if (cursor >= end)
    return false;
  const std::size_t length = *cursor++;
  if (static_cast<std::size_t>(end - cursor) < length)
    return false;
  out->assign(reinterpret_cast<const char*>(cursor), length);
  cursor += length;
  return true;
}

// Names come from two places that do not agree on case or padding - one from a
// save file, one from a roster - so they are compared loosely.
std::string NameKey(std::string_view name)
{
  std::string out(StripWhitespace(name));
  std::ranges::transform(out, out.begin(),
                         [](char c) { return static_cast<char>(std::tolower(
                                          static_cast<unsigned char>(c))); });
  return out;
}

// --- Per-peer state --------------------------------------------------------

struct QueuedFrame
{
  u16 sequence = 0;
  std::vector<u8> payload;
};

struct Peer
{
  u32 ip = 0;
  std::string nickname;
  std::string license;
  bool racing = false;
  bool finished = false;

  Decoder decoder;
  std::deque<QueuedFrame> queue;
  bool playing = false;
  u16 next_sequence = 0;

  PitchShifter pitch;
  Echo echo;
  SmoothedGain gain;
  LevelMeter meter;

  // Where they are on the track, as this console sees it.
  int racer_index = -1;
  float distance = -1.0f;
  // Smoothed rate of change of `distance`, world units per second. Positive is
  // moving apart. This is what Doppler is computed from, which makes it
  // relative by construction: two karts holding station produce zero however
  // fast they are both going.
  float closing_rate = 0.0f;
  bool have_distance = false;
  float proximity_gain = 1.0f;
  bool proximity_active = false;
  // -1 hard left, 0 centre, +1 hard right. Smoothed towards the target rather
  // than applied raw: positions are sampled at 20 Hz, and a voice jumping
  // between ears on every sample is worse than one that lags slightly.
  float pan = 0.0f;

  std::chrono::steady_clock::time_point last_heard{};
};

struct State
{
  std::atomic<bool> running{false};
  std::atomic<bool> stop{false};
  std::thread thread;

  AudioDevices devices;
  // Built in Start() rather than here: creating an Opus encoder allocates tens
  // of kilobytes, and a build that never joins a lobby should not pay for it
  // before main() has run.
  std::unique_ptr<Encoder> encoder;

  std::atomic<float> mic_level{0.0f};
  std::atomic<float> output_level{0.0f};
  std::atomic<bool> transmitting{false};
  std::atomic<bool> capture_ok{false};
  std::atomic<bool> playback_ok{false};
  std::atomic<bool> restart_devices{false};

  NoiseGate gate;
  LevelMeter mic_meter;
  LevelMeter output_meter;
  u16 out_sequence = 0;

  std::mutex mutex;  // guards everything below
  std::map<u32, std::unique_ptr<Peer>> peers;
  std::string status_text = "Not running";
  std::string local_license;
  bool local_racing = false;
  bool local_finished = false;
};

State s_state;

Peer& EnsurePeer(u32 ip)
{
  auto& slot = s_state.peers[ip];
  if (!slot)
  {
    slot = std::make_unique<Peer>();
    slot->ip = ip;
  }
  return *slot;
}

void SetStatus(std::string text)
{
  std::lock_guard lock(s_state.mutex);
  s_state.status_text = std::move(text);
}

// --- Reading the local game ------------------------------------------------

std::optional<s64> FindInt(const MemInspect::Snapshot& snapshot, std::string_view key)
{
  for (const MemInspect::WatchValue& value : snapshot.values)
  {
    if (value.key == key)
    {
      if (!value.reading.ok)
        return std::nullopt;
      return value.reading.integer;
    }
  }
  return std::nullopt;
}

std::optional<float> FindFloat(const MemInspect::Snapshot& snapshot, std::string_view key)
{
  for (const MemInspect::WatchValue& value : snapshot.values)
  {
    if (value.key == key)
    {
      if (!value.reading.ok)
        return std::nullopt;
      return static_cast<float>(value.reading.real);
    }
  }
  return std::nullopt;
}

std::string FindText(const MemInspect::Snapshot& snapshot, std::string_view key)
{
  for (const MemInspect::WatchValue& value : snapshot.values)
  {
    if (value.key == key)
      return value.reading.ok ? value.reading.text : std::string();
  }
  return {};
}

std::optional<bool> FindDerived(const MemInspect::Snapshot& snapshot, std::string_view name)
{
  for (const auto& [key, value] : snapshot.derived)
  {
    if (key == name)
      return value;
  }
  return std::nullopt;
}

struct KartPosition
{
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  // Column 2 of the kart's rotation: the direction it is pointing, in world
  // space, on the horizontal plane. Only read for the listener, and only to work
  // out what "left" means to them. Zero when it could not be read, which is the
  // signal to leave the voice centred rather than to guess a facing.
  float fwd_x = 0.0f;
  float fwd_z = 0.0f;
};

std::optional<KartPosition> ReadKart(const MemInspect::Snapshot& snapshot, int index)
{
  if (index < 0)
    return std::nullopt;
  const auto x = FindFloat(snapshot, fmt::format("r{}_pos_x", index));
  const auto y = FindFloat(snapshot, fmt::format("r{}_pos_y", index));
  const auto z = FindFloat(snapshot, fmt::format("r{}_pos_z", index));
  if (!x || !y || !z)
    return std::nullopt;
  if (!std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*z))
    return std::nullopt;

  KartPosition kart{*x, *y, *z};
  // Missing or unreadable heading is not a failure: it only costs the panning,
  // and everything else about this kart is still usable.
  const auto fwd_x = FindFloat(snapshot, fmt::format("r{}_fwd_x", index));
  const auto fwd_z = FindFloat(snapshot, fmt::format("r{}_fwd_z", index));
  if (fwd_x && fwd_z && std::isfinite(*fwd_x) && std::isfinite(*fwd_z))
  {
    kart.fwd_x = *fwd_x;
    kart.fwd_z = *fwd_z;
  }
  return kart;
}

// Whether this console is in a race that proximity should apply to, and whether
// it has already crossed the line.
void ReadLocalRaceState(const MemInspect::Snapshot& snapshot, bool* racing, bool* finished)
{
  const auto online_race = FindDerived(snapshot, "online_race");
  const auto progress = FindInt(snapshot, "race_progress");

  // The race scene, online, counts from the moment it loads - the intro and the
  // countdown included. Waiting for GO would leave everyone on the flat channel
  // for the part of a race where they most want to talk to the kart beside them.
  *racing = online_race.value_or(false);
  // PROGRESS_FINISHED is lap-indexed and correct for a three lap race, which is
  // what online runs. On a longer race a racer would stay in proximity to the
  // end, which is the safe direction to be wrong in.
  *finished = progress.has_value() && *progress >= MemInspect::PROGRESS_FINISHED;
}

// --- Sending ---------------------------------------------------------------

void SendIdentity()
{
  std::string license;
  bool racing = false;
  bool finished = false;
  {
    std::lock_guard lock(s_state.mutex);
    license = s_state.local_license;
    racing = s_state.local_racing;
    finished = s_state.local_finished;
  }

  std::vector<u8> body;
  body.push_back(WIRE_VERSION);
  body.push_back(CTRL_IDENTITY);
  body.push_back(static_cast<u8>((racing ? IDENTITY_RACING : 0) |
                                 (finished ? IDENTITY_FINISHED : 0)));
  PutString(body, license);
  Lobby::SendVoice(Lobby::BROADCAST_IP, false, body.data(), body.size());
}

void SendBitrate(int bitrate)
{
  std::vector<u8> body;
  body.push_back(WIRE_VERSION);
  body.push_back(CTRL_BITRATE);
  const u32 value = static_cast<u32>(bitrate);
  body.push_back(static_cast<u8>(value >> 24));
  body.push_back(static_cast<u8>(value >> 16));
  body.push_back(static_cast<u8>(value >> 8));
  body.push_back(static_cast<u8>(value));
  Lobby::SendVoice(Lobby::BROADCAST_IP, false, body.data(), body.size());
}

// --- Receiving -------------------------------------------------------------

void OnControl(u32 src_ip, const u8* data, std::size_t length)
{
  if (length < 2 || data[0] != WIRE_VERSION)
    return;

  const u8 kind = data[1];
  const u8* cursor = data + 2;
  const u8* const end = data + length;

  if (kind == CTRL_IDENTITY)
  {
    if (cursor >= end)
      return;
    const u8 flags = *cursor++;
    std::string license;
    if (!TakeString(cursor, end, &license))
      return;

    std::lock_guard lock(s_state.mutex);
    Peer& peer = EnsurePeer(src_ip);
    if (peer.license != license)
    {
      VNET_TRACE(Lobby, "voice: {} is playing as \"{}\"", Trace::FormatIP(src_ip), license);
      // A different licence means a different racer; anything derived from the
      // old one is now wrong.
      peer.racer_index = -1;
      peer.have_distance = false;
      peer.closing_rate = 0.0f;
    }
    peer.license = std::move(license);
    peer.racing = (flags & IDENTITY_RACING) != 0;
    peer.finished = (flags & IDENTITY_FINISHED) != 0;
    return;
  }

  if (kind == CTRL_BITRATE)
  {
    if (end - cursor < 4)
      return;
    // Only the host gets to set this. A client saying so is either a bug or
    // someone playing games, and either way it is not obeyed.
    if (src_ip != Lobby::HOST_IP)
      return;
    const int bitrate = static_cast<int>((u32{cursor[0]} << 24) | (u32{cursor[1]} << 16) |
                                         (u32{cursor[2]} << 8) | u32{cursor[3]});
    VNET_TRACE(Lobby, "voice: host set the bitrate to {} bit/s", bitrate);
    ApplyHostBitrate(bitrate);
  }
}

void OnAudio(u32 src_ip, const u8* data, std::size_t length)
{
  if (length < 3 || data[0] != WIRE_VERSION)
    return;

  const u16 sequence = static_cast<u16>((u16{data[1]} << 8) | data[2]);

  std::lock_guard lock(s_state.mutex);
  Peer& peer = EnsurePeer(src_ip);
  peer.last_heard = std::chrono::steady_clock::now();

  // Reordered or duplicated frames are placed by sequence rather than appended,
  // because the audio channel is unsequenced by design and the order things
  // arrive in is not the order they were spoken in.
  QueuedFrame frame;
  frame.sequence = sequence;
  frame.payload.assign(data + 3, data + length);

  const auto newer = [](u16 a, u16 b) { return static_cast<s16>(a - b) > 0; };

  if (peer.playing && !newer(sequence, static_cast<u16>(peer.next_sequence - 1)))
    return;  // already played past this one

  auto at = peer.queue.begin();
  while (at != peer.queue.end() && !newer(at->sequence, sequence))
  {
    if (at->sequence == sequence)
      return;  // duplicate
    ++at;
  }
  peer.queue.insert(at, std::move(frame));

  if (peer.queue.size() > JITTER_MAX)
  {
    // Running long. Throw away the oldest so the listener catches up to what is
    // being said now rather than hearing a growing delay.
    while (peer.queue.size() > JITTER_PREBUFFER)
      peer.queue.pop_front();
    peer.next_sequence = peer.queue.front().sequence;
  }
}

void OnVoiceFrame(u32 src_ip, bool audio, const u8* data, std::size_t length)
{
  if (!s_state.running.load())
    return;
  if (audio)
    OnAudio(src_ip, data, length);
  else
    OnControl(src_ip, data, length);
}

// --- Proximity -------------------------------------------------------------

// Matches peers to racer slots and works out how far away and how fast each one
// is moving relative to this console's own kart.
void UpdateProximity(float dt)
{
  const MemInspect::Snapshot snapshot = MemInspect::Inspector::GetInstance().GetSnapshot();
  const Settings settings = Get();

  bool local_racing = false;
  bool local_finished = false;
  ReadLocalRaceState(snapshot, &local_racing, &local_finished);

  int local_index = -1;
  for (const MemInspect::RacerIdentity& racer : snapshot.racers)
  {
    if (racer.local)
    {
      local_index = racer.index;
      break;
    }
  }

  const std::optional<KartPosition> mine = ReadKart(snapshot, local_index);

  std::lock_guard lock(s_state.mutex);
  s_state.local_license = FindText(snapshot, "license_name");
  s_state.local_racing = local_racing;
  s_state.local_finished = local_finished;

  for (auto& [ip, slot] : s_state.peers)
  {
    Peer& peer = *slot;

    // Resolve who they are on the track. Their licence Mii name is the same
    // name the roster carries for them, which is what ResolveRacers used.
    peer.racer_index = -1;
    if (!peer.license.empty())
    {
      const std::string wanted = NameKey(peer.license);
      for (const MemInspect::RacerIdentity& racer : snapshot.racers)
      {
        if (!racer.local && NameKey(racer.name) == wanted)
        {
          peer.racer_index = racer.index;
          break;
        }
      }
    }

    const bool both_racing = local_racing && !local_finished && peer.racing && !peer.finished;
    const std::optional<KartPosition> theirs = ReadKart(snapshot, peer.racer_index);

    if (!both_racing || !mine || !theirs)
    {
      // Flat channel. Reset the distance history so the first frame after a
      // race starts does not produce a Doppler shift from a stale reading.
      peer.proximity_active = false;
      peer.proximity_gain = 1.0f;
      peer.distance = -1.0f;
      peer.have_distance = false;
      peer.closing_rate = 0.0f;
      peer.pan = 0.0f;
      continue;
    }

    const float dx = theirs->x - mine->x;
    const float dy = theirs->y - mine->y;
    const float dz = theirs->z - mine->z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (peer.have_distance && dt > 0.0f)
    {
      const float instant = (distance - peer.distance) / dt;
      // Heavily smoothed: the underlying positions are sampled at 20 Hz, and
      // unsmoothed differences of them make the pitch jitter audibly.
      peer.closing_rate = peer.closing_rate * 0.8f + instant * 0.2f;
    }
    peer.distance = distance;
    peer.have_distance = true;

    // Which ear. The listener's forward vector is the local +Z axis in world
    // space, so its right is (fwd_z, -fwd_x) on the horizontal plane - the same
    // vector turned a quarter turn. Projecting the direction to the other kart
    // onto that gives -1 for hard left through to +1 for hard right, and the
    // sign is what decides the side, so it is the one thing here worth being
    // careful about.
    float pan_target = 0.0f;
    const float facing = std::sqrt(mine->fwd_x * mine->fwd_x + mine->fwd_z * mine->fwd_z);
    const float flat = std::sqrt(dx * dx + dz * dz);
    if (facing > 0.0001f && flat > 0.0001f)
    {
      const float right_x = mine->fwd_z / facing;
      const float right_z = -mine->fwd_x / facing;
      pan_target = std::clamp((dx * right_x + dz * right_z) / flat, -1.0f, 1.0f);

      // Someone almost on top of you has no direction worth speaking of, and
      // the projection gets noisy as the separation goes to nothing. Fade the
      // panning out rather than let them flick between ears.
      const float centre_fade = std::clamp(flat / DEFAULT_PROXIMITY_NEAR, 0.0f, 1.0f);
      pan_target *= centre_fade;
    }
    // Roughly a tenth of a second to travel most of the way at 20 Hz.
    peer.pan += (pan_target - peer.pan) * 0.35f;

    // Full volume up close, then a squared falloff to nothing at the range.
    // Squared rather than linear because linear stays too loud too far out.
    const float near_radius = DEFAULT_PROXIMITY_NEAR;
    const float range = std::max(settings.proximity_range, near_radius + 100.0f);
    float gain = 1.0f;
    if (distance > near_radius)
    {
      const float t = std::clamp((distance - near_radius) / (range - near_radius), 0.0f, 1.0f);
      gain = (1.0f - t) * (1.0f - t);
    }

    peer.proximity_active = true;
    peer.proximity_gain = gain;
  }
}

float DopplerRatio(const Peer& peer, const Settings& settings)
{
  if (!peer.proximity_active || settings.doppler == 0)
    return 1.0f;

  const float intensity = static_cast<float>(settings.doppler) / 100.0f;
  // Receding stretches the wave and lowers the pitch; approaching raises it.
  const float shifted = DOPPLER_SOUND_SPEED / (DOPPLER_SOUND_SPEED + intensity * peer.closing_rate);
  return std::clamp(shifted, DOPPLER_MIN_RATIO, DOPPLER_MAX_RATIO);
}

// The speaker's own state decides what their voice sounds like, and every
// listener can read it directly - so nothing about it goes over the wire.
void StateEffects(const MemInspect::Snapshot& snapshot, int racer_index, float* ratio, float* echo)
{
  *ratio = 1.0f;
  *echo = 0.0f;
  if (racer_index < 0)
    return;

  for (const MemInspect::RacerState& state : snapshot.states)
  {
    if (state.index != racer_index)
      continue;

    if (state.mega || state.bullet)
      *ratio = RATIO_DEEP;
    else if (state.shocked || state.crushed)
      *ratio = RATIO_CHIPMUNK;

    if (state.star)
      *echo = STAR_ECHO_WET;
    return;
  }
}

// --- The worker ------------------------------------------------------------

void ProcessCapture(const Settings& settings)
{
  std::array<float, FRAME_SAMPLES> frame{};
  s_state.devices.Captured().Read(frame.data(), frame.size());

  const float gain = static_cast<float>(settings.mic_gain) / 100.0f;
  for (float& sample : frame)
    sample = std::clamp(sample * gain, -1.0f, 1.0f);

  // Metered after the gain and before the gate: the bar is there to show
  // whether the slider has the level in a sensible place for the threshold.
  s_state.mic_meter.Push(frame.data(), frame.size());
  s_state.mic_level.store(s_state.mic_meter.Level());

  const bool open = s_state.gate.Process(frame.data(), frame.size(), settings.gate_db);
  s_state.transmitting.store(open && !settings.muted && !settings.deafened);

  if (settings.muted || settings.deafened || !open || !s_state.encoder)
    return;

  s_state.encoder->SetBitrate(settings.bitrate);
  const std::vector<u8> encoded = s_state.encoder->Encode(frame.data());
  if (encoded.empty())
    return;

  std::vector<u8> packet;
  packet.reserve(encoded.size() + 3);
  packet.push_back(WIRE_VERSION);
  PutU16(packet, s_state.out_sequence++);
  packet.insert(packet.end(), encoded.begin(), encoded.end());
  Lobby::SendVoice(Lobby::BROADCAST_IP, true, packet.data(), packet.size());
}

void ProcessPlayback(const Settings& settings, const MemInspect::Snapshot& snapshot)
{
  // Interleaved stereo: [L0, R0, L1, R1, ...].
  std::array<float, FRAME_SAMPLES * OUTPUT_CHANNELS> mix{};
  std::array<float, FRAME_SAMPLES> voice{};

  const float master = static_cast<float>(settings.master_volume) / 100.0f;

  std::lock_guard lock(s_state.mutex);
  for (auto& [ip, slot] : s_state.peers)
  {
    Peer& peer = *slot;

    // Wait for a little to accumulate before starting, or the first words are
    // chopped up by the reordering that has not settled yet.
    if (!peer.playing)
    {
      if (peer.queue.size() < JITTER_PREBUFFER)
      {
        peer.meter.Idle(FRAME_SAMPLES);
        continue;
      }
      peer.playing = true;
      peer.next_sequence = peer.queue.front().sequence;
    }

    bool decoded = false;
    if (!peer.queue.empty() && peer.queue.front().sequence == peer.next_sequence)
    {
      const QueuedFrame& frame = peer.queue.front();
      decoded = peer.decoder.Decode(frame.payload.data(), frame.payload.size(), voice.data());
      peer.queue.pop_front();
    }
    else if (!peer.queue.empty())
    {
      // The expected frame is missing but a later one is here. Conceal exactly
      // one frame and move on rather than waiting for something that is not
      // coming.
      decoded = peer.decoder.Decode(nullptr, 0, voice.data());
    }
    else
    {
      // Nothing queued at all. Stop rather than concealing forever - a peer who
      // has stopped talking should be silent, not smeared.
      peer.playing = false;
      peer.meter.Idle(FRAME_SAMPLES);
      continue;
    }
    ++peer.next_sequence;

    if (!decoded)
      continue;

    peer.meter.Push(voice.data(), voice.size());

    float state_ratio = 1.0f;
    float echo_wet = 0.0f;
    StateEffects(snapshot, peer.racer_index, &state_ratio, &echo_wet);

    // One shifter does both jobs. Doppler and the state effect are both a
    // change of playback rate, so multiplying them is exactly right.
    peer.pitch.Process(voice.data(), voice.size(), state_ratio * DopplerRatio(peer, settings));
    peer.echo.Process(voice.data(), voice.size(), echo_wet);

    const float personal = static_cast<float>(GetPeerVolume(peer.nickname)) / 100.0f;
    const float target = settings.deafened ? 0.0f : personal * peer.proximity_gain * master;
    peer.gain.Process(voice.data(), voice.size(), target);

    // Constant power, so someone crossing in front of you does not dip in
    // loudness as they pass through the centre. At pan 0 both channels get
    // 1/sqrt(2), which is the same total power as one channel at full.
    const float pan = settings.spatial ? peer.pan : 0.0f;
    const float angle = (pan + 1.0f) * (PI_F / 4.0f);
    const float left = std::cos(angle);
    const float right = std::sin(angle);

    for (std::size_t i = 0; i < voice.size(); ++i)
    {
      mix[i * OUTPUT_CHANNELS] += voice[i] * left;
      mix[i * OUTPUT_CHANNELS + 1] += voice[i] * right;
    }
  }

  // Clip rather than let a loud moment with several people talking wrap around.
  for (float& sample : mix)
    sample = std::clamp(sample, -1.0f, 1.0f);

  s_state.output_meter.Push(mix.data(), mix.size());
  s_state.output_level.store(s_state.output_meter.Level());

  s_state.devices.ToPlay().Write(mix.data(), mix.size());
}

// Keeps the peer table in step with the lobby's roster: adds people who joined,
// removes people who left, and refreshes nicknames and volumes.
void SyncRoster()
{
  const std::vector<Lobby::Peer> roster = Lobby::GetPeers();
  const u32 local = Lobby::GetVirtualIP();

  std::lock_guard lock(s_state.mutex);
  std::map<u32, bool> present;
  for (const Lobby::Peer& entry : roster)
  {
    present[entry.ip] = true;
    Peer& peer = EnsurePeer(entry.ip);
    peer.nickname = entry.name;
    if (entry.ip == local)
      peer.license = s_state.local_license;
  }

  std::erase_if(s_state.peers, [&](const auto& item) { return !present[item.first]; });
}

void Worker()
{
  Common::SetCurrentThreadName("Lobby Voice");

  auto last_identity = std::chrono::steady_clock::now();
  auto last_proximity = last_identity;
  auto last_roster = last_identity;

  while (!s_state.stop.load())
  {
    const Settings settings = Get();
    const auto now = std::chrono::steady_clock::now();

    if (s_state.restart_devices.exchange(false))
    {
      s_state.devices.Stop();
      s_state.capture_ok.store(s_state.devices.StartCapture(settings.input_device));
      s_state.playback_ok.store(s_state.devices.StartPlayback(settings.output_device));
    }

    if (now - last_roster > std::chrono::milliseconds(500))
    {
      last_roster = now;
      SyncRoster();
    }

    if (now - last_identity > std::chrono::milliseconds(IDENTITY_INTERVAL_MS))
    {
      last_identity = now;
      SendIdentity();
    }

    // Proximity is recomputed at the rate the inspector samples at. Doing it
    // per audio frame would read the same snapshot several times over.
    const float dt = std::chrono::duration<float>(now - last_proximity).count();
    if (dt > 0.05f)
    {
      last_proximity = now;
      UpdateProximity(dt);
    }

    if (settings.enabled)
    {
      int passes = 0;
      while (s_state.devices.Captured().Available() >= FRAME_SAMPLES &&
             passes++ < MAX_FRAMES_PER_PASS)
      {
        ProcessCapture(settings);
      }

      // Self-clocking: the output device drains the ring at a steady 48 kHz, so
      // topping it up to a fixed depth paces the mixer without a timer that
      // could drift against the sound card.
      const MemInspect::Snapshot snapshot = MemInspect::Inspector::GetInstance().GetSnapshot();
      passes = 0;
      while (s_state.devices.ToPlay().Available() < PLAYBACK_TARGET_FRAMES * FRAME_SAMPLES &&
             passes++ < MAX_FRAMES_PER_PASS)
      {
        ProcessPlayback(settings, snapshot);
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}
}  // namespace

void Start()
{
  if (s_state.running.exchange(true))
    return;

  Load();
  const Settings settings = Get();

  s_state.encoder = std::make_unique<Encoder>();
  s_state.stop.store(false);
  s_state.capture_ok.store(s_state.devices.StartCapture(settings.input_device));
  s_state.playback_ok.store(s_state.devices.StartPlayback(settings.output_device));

  std::string status;
  if (!s_state.encoder->IsValid())
    status = "The Opus encoder failed to start; nobody will hear you";
  else if (!s_state.capture_ok.load() && !s_state.playback_ok.load())
    status = "No microphone and no output device";
  else if (!s_state.capture_ok.load())
    status = "No microphone: you can hear, but not be heard";
  else if (!s_state.playback_ok.load())
    status = "No output device: you can be heard, but not hear";
  else
    status = "Running";
  SetStatus(std::move(status));

  Lobby::SetVoiceHandler(&OnVoiceFrame);
  s_state.thread = std::thread(Worker);

  // A host announces its bitrate immediately so a client that joined earlier is
  // not left encoding at whatever it last used.
  if (Lobby::GetRole() == Lobby::Role::Host)
    SendBitrate(settings.bitrate);

  VNET_TRACE(Lobby, "voice chat started");
}

void Stop()
{
  if (!s_state.running.exchange(false))
    return;

  s_state.stop.store(true);
  if (s_state.thread.joinable())
    s_state.thread.join();

  Lobby::SetVoiceHandler(nullptr);
  s_state.devices.Stop();
  s_state.encoder.reset();

  {
    std::lock_guard lock(s_state.mutex);
    s_state.peers.clear();
    s_state.status_text = "Not running";
  }
  s_state.mic_level.store(0.0f);
  s_state.output_level.store(0.0f);
  s_state.transmitting.store(false);
  VNET_TRACE(Lobby, "voice chat stopped");
}

bool IsRunning()
{
  return s_state.running.load();
}

std::vector<PeerInfo> GetPeers()
{
  const u32 local = Lobby::GetVirtualIP();

  std::lock_guard lock(s_state.mutex);
  std::vector<PeerInfo> out;
  out.reserve(s_state.peers.size());
  for (const auto& [ip, slot] : s_state.peers)
  {
    const Peer& peer = *slot;
    PeerInfo info;
    info.ip = ip;
    info.nickname = peer.nickname;
    info.license = peer.license;
    info.display = peer.license.empty() ? peer.nickname :
                                          fmt::format("{} ({})", peer.nickname, peer.license);
    info.is_local = ip == local;
    info.volume = GetPeerVolume(peer.nickname);
    info.level = info.is_local ? s_state.mic_level.load() : peer.meter.Level();
    info.talking = info.level > 0.02f;
    info.proximity = peer.proximity_active;
    info.distance = peer.distance;
    info.proximity_gain = peer.proximity_gain;
    out.push_back(std::move(info));
  }
  return out;
}

float GetMicLevel()
{
  return s_state.mic_level.load();
}

float GetOutputLevel()
{
  return s_state.output_level.load();
}

bool IsTransmitting()
{
  return s_state.transmitting.load();
}

bool IsCaptureWorking()
{
  return s_state.capture_ok.load();
}

bool IsPlaybackWorking()
{
  return s_state.playback_ok.load();
}

std::string GetStatusText()
{
  std::lock_guard lock(s_state.mutex);
  return s_state.status_text;
}

void SetBitrateAndPush(int bits_per_second)
{
  SetBitrate(bits_per_second);
  if (s_state.running.load() && Lobby::GetRole() == Lobby::Role::Host)
    SendBitrate(Get().bitrate);
}

void RestartDevices()
{
  s_state.restart_devices.store(true);
}
}  // namespace Lobby::Voice
