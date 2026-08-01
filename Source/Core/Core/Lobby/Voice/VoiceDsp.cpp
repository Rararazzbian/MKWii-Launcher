// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Lobby/Voice/VoiceDsp.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Lobby::Voice
{
namespace
{
// Pitch shifter window. 25 ms is long enough that the crossfade does not sound
// like tremolo and short enough that the added latency is not noticeable in
// conversation.
constexpr std::size_t SHIFT_WINDOW = SAMPLE_RATE * 25 / 1000;
constexpr std::size_t SHIFT_BUFFER = SHIFT_WINDOW * 4;

constexpr std::size_t ECHO_DELAY = SAMPLE_RATE * 110 / 1000;
constexpr float ECHO_FEEDBACK = 0.42f;

// How fast the gate opens and closes, per sample. Opening quickly matters more
// than closing quickly: a slow attack eats the start of a word.
constexpr float GATE_ATTACK = 1.0f / (SAMPLE_RATE * 0.005f);
constexpr float GATE_RELEASE = 1.0f / (SAMPLE_RATE * 0.120f);
// Frames to stay open after the level drops, so the pause between words does
// not close it.
constexpr int GATE_HOLD_FRAMES = 12;

constexpr float METER_RELEASE = 0.9995f;
}  // namespace

float DbToLinear(float db)
{
  return std::pow(10.0f, db / 20.0f);
}

float LinearToDb(float linear)
{
  // Floored rather than -inf so a silent frame compares sanely against a
  // threshold instead of propagating an infinity through the gate.
  return linear <= 1e-7f ? -140.0f : 20.0f * std::log10(linear);
}

void LevelMeter::Push(const float* samples, std::size_t count)
{
  float peak = 0.0f;
  for (std::size_t i = 0; i < count; ++i)
    peak = std::max(peak, std::abs(samples[i]));

  if (peak > m_level)
    m_level = peak;
  else
    m_level *= std::pow(METER_RELEASE, static_cast<float>(count));
  m_level = std::clamp(m_level, 0.0f, 1.0f);
}

void LevelMeter::Idle(std::size_t samples)
{
  m_level *= std::pow(METER_RELEASE, static_cast<float>(samples));
}

bool NoiseGate::Process(float* samples, std::size_t count, float threshold_db)
{
  if (count == 0)
    return m_gain > 0.0f;

  // RMS rather than peak: a single click should not hold the gate open, and
  // speech is what the threshold is meant to be set against.
  float sum = 0.0f;
  for (std::size_t i = 0; i < count; ++i)
    sum += samples[i] * samples[i];
  const float rms = std::sqrt(sum / static_cast<float>(count));

  const bool above = LinearToDb(rms) >= threshold_db;
  if (above)
    m_hold = GATE_HOLD_FRAMES;
  else if (m_hold > 0)
    --m_hold;

  const float target = (above || m_hold > 0) ? 1.0f : 0.0f;
  const float step = target > m_gain ? GATE_ATTACK : -GATE_RELEASE;

  for (std::size_t i = 0; i < count; ++i)
  {
    m_gain = std::clamp(m_gain + step, 0.0f, 1.0f);
    samples[i] *= m_gain;
  }
  return m_gain > 0.0f;
}

PitchShifter::PitchShifter() : m_buffer(SHIFT_BUFFER, 0.0f)
{
}

void PitchShifter::Reset()
{
  std::ranges::fill(m_buffer, 0.0f);
  m_write = 0;
  m_phase = 0.0f;
}

float PitchShifter::Read(float delay) const
{
  float position = static_cast<float>(m_write) - delay;
  const float size = static_cast<float>(m_buffer.size());
  while (position < 0.0f)
    position += size;

  const std::size_t index = static_cast<std::size_t>(position) % m_buffer.size();
  const std::size_t next = (index + 1) % m_buffer.size();
  const float fraction = position - std::floor(position);
  return m_buffer[index] * (1.0f - fraction) + m_buffer[next] * fraction;
}

void PitchShifter::Process(float* samples, std::size_t count, float ratio)
{
  // Bypass rather than run the taps for nothing. Unity ratio is the common case
  // - most of the time nobody is starred, shocked or moving relative to you.
  if (std::abs(ratio - 1.0f) < 0.001f)
  {
    for (std::size_t i = 0; i < count; ++i)
    {
      m_buffer[m_write] = samples[i];
      m_write = (m_write + 1) % m_buffer.size();
    }
    return;
  }

  const float window = static_cast<float>(SHIFT_WINDOW);
  for (std::size_t i = 0; i < count; ++i)
  {
    m_buffer[m_write] = samples[i];

    const float first = m_phase;
    const float second = std::fmod(m_phase + window * 0.5f, window);
    // sin over half a period gives a constant-power pair: the two gains are a
    // quarter cycle apart, so their squares always sum to one.
    const float gain_a = std::sin(std::numbers::pi_v<float> * first / window);
    const float gain_b = std::sin(std::numbers::pi_v<float> * second / window);

    samples[i] = Read(first + 1.0f) * gain_a + Read(second + 1.0f) * gain_b;

    // The phase IS the read delay, so the read pointer moves at
    // 1 - (rate of change of delay) samples per output sample. Growing the
    // delay slows playback and lowers the pitch, which is why this subtracts:
    // it makes `ratio` the playback rate, so above 1 is higher and below 1 is
    // lower, the way every caller assumes.
    m_phase += 1.0f - ratio;
    if (m_phase < 0.0f)
      m_phase += window;
    else if (m_phase >= window)
      m_phase -= window;

    m_write = (m_write + 1) % m_buffer.size();
  }
}

Echo::Echo() : m_buffer(ECHO_DELAY, 0.0f)
{
}

void Echo::Reset()
{
  std::ranges::fill(m_buffer, 0.0f);
  m_write = 0;
}

void Echo::Process(float* samples, std::size_t count, float wet)
{
  if (wet <= 0.0f)
  {
    // Still feed the line, so switching the effect on does not start from a
    // buffer full of whatever was said several seconds ago.
    for (std::size_t i = 0; i < count; ++i)
    {
      m_buffer[m_write] = m_buffer[m_write] * ECHO_FEEDBACK;
      m_write = (m_write + 1) % m_buffer.size();
    }
    return;
  }

  for (std::size_t i = 0; i < count; ++i)
  {
    const float delayed = m_buffer[m_write];
    m_buffer[m_write] = samples[i] + delayed * ECHO_FEEDBACK;
    m_write = (m_write + 1) % m_buffer.size();
    samples[i] = samples[i] * (1.0f - wet * 0.5f) + delayed * wet;
  }
}

void SmoothedGain::Process(float* samples, std::size_t count, float target)
{
  if (count == 0)
    return;

  // Reach the target by the end of the frame. Per-frame linear interpolation is
  // enough here - the gains this smooths change at most fifty times a second.
  const float step = (target - m_current) / static_cast<float>(count);
  for (std::size_t i = 0; i < count; ++i)
  {
    m_current += step;
    samples[i] *= m_current;
  }
  m_current = target;
}
}  // namespace Lobby::Voice
