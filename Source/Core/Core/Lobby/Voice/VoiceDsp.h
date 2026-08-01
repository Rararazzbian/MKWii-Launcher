// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The signal processing: a noise gate on the way out, and on the way in, the
// pitch and echo that turn a racer's state and speed into something audible.
//
// All of it works on mono float at the codec's sample rate, in place, one 20 ms
// frame at a time.

#pragma once

#include <cstddef>
#include <vector>

#include "Common/CommonTypes.h"

namespace Lobby::Voice
{
constexpr int SAMPLE_RATE = 48000;
// Opus's natural frame for speech. Short enough that a lost one is a blip
// rather than a syllable, long enough that the per-packet overhead of sending
// it over ENet is not most of the bandwidth.
constexpr int FRAME_MS = 20;
constexpr std::size_t FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000;  // 960

// Peak-following level for a meter, 0 to 1. Rises instantly so a consonant
// registers, falls slowly so the bar is readable rather than flickering.
class LevelMeter
{
public:
  void Push(const float* samples, std::size_t count);
  // Decays even with no audio, so a meter does not stick where the talking
  // stopped.
  void Idle(std::size_t samples);
  float Level() const { return m_level; }

private:
  float m_level = 0.0f;
};

// Opens above the threshold and closes below it, with a hold so that the gaps
// between words do not chop the end off every one.
class NoiseGate
{
public:
  // `threshold_db` is dBFS. Returns true if the frame was let through; the
  // frame is faded rather than cut, because a hard edge is more audible than
  // the noise the gate is there to remove.
  bool Process(float* samples, std::size_t count, float threshold_db);
  bool IsOpen() const { return m_gain > 0.01f; }

private:
  float m_gain = 0.0f;
  int m_hold = 0;
};

// Delay-line pitch shifter: two taps half a window apart, crossfaded so the
// discontinuity where a tap wraps lands exactly where its gain is zero. Cheap,
// and its slight graininess suits a voice that is meant to sound altered.
//
// `ratio` is the playback rate: above 1 raises the pitch, below 1 lowers it. It
// can change every frame, which is what Doppler needs.
class PitchShifter
{
public:
  PitchShifter();
  void Process(float* samples, std::size_t count, float ratio);
  void Reset();

private:
  float Read(float delay) const;

  std::vector<float> m_buffer;
  std::size_t m_write = 0;
  float m_phase = 0.0f;
};

// Feedback delay, for the star.
class Echo
{
public:
  Echo();
  // `wet` 0 disables it and costs nothing but the branch.
  void Process(float* samples, std::size_t count, float wet);
  void Reset();

private:
  std::vector<float> m_buffer;
  std::size_t m_write = 0;
};

// A one-pole smoother, so a gain that jumps between frames does not click.
class SmoothedGain
{
public:
  void Process(float* samples, std::size_t count, float target);
  void SnapTo(float value) { m_current = value; }

private:
  float m_current = 0.0f;
};

float DbToLinear(float db);
float LinearToDb(float linear);
}  // namespace Lobby::Voice
