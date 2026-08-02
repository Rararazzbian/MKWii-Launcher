// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Lobby/Voice/VoiceAudio.h"

#include <algorithm>
#include <cstring>

#include <cubeb/cubeb.h>

#include "AudioCommon/CubebUtils.h"
#include "Common/Logging/Log.h"
#include "Core/Lobby/Voice/VoiceDsp.h"

namespace Lobby::Voice
{
SampleRing::SampleRing(std::size_t capacity) : m_buffer(capacity, 0.0f)
{
}

void SampleRing::Write(const float* samples, std::size_t count)
{
  std::lock_guard lock(m_mutex);
  const std::size_t capacity = m_buffer.size();
  if (count >= capacity)
  {
    // More than the ring holds: keep the tail, which is the newest audio.
    samples += count - capacity;
    count = capacity;
    m_read = 0;
    m_write = 0;
    m_filled = capacity;
    std::memcpy(m_buffer.data(), samples, capacity * sizeof(float));
    return;
  }

  for (std::size_t i = 0; i < count; ++i)
  {
    m_buffer[m_write] = samples[i];
    m_write = (m_write + 1) % capacity;
  }

  m_filled += count;
  if (m_filled > capacity)
  {
    // Overrun. Drop the oldest rather than the newest: stale audio is worth
    // less than current audio, and this is the failure mode of a consumer that
    // has fallen behind.
    m_read = m_write;
    m_filled = capacity;
  }
}

std::size_t SampleRing::Read(float* out, std::size_t count)
{
  std::lock_guard lock(m_mutex);
  const std::size_t capacity = m_buffer.size();
  const std::size_t available = std::min(count, m_filled);

  for (std::size_t i = 0; i < available; ++i)
  {
    out[i] = m_buffer[m_read];
    m_read = (m_read + 1) % capacity;
  }
  for (std::size_t i = available; i < count; ++i)
    out[i] = 0.0f;

  m_filled -= available;
  return available;
}

std::size_t SampleRing::Available() const
{
  std::lock_guard lock(m_mutex);
  return m_filled;
}

void SampleRing::Clear()
{
  std::lock_guard lock(m_mutex);
  m_read = 0;
  m_write = 0;
  m_filled = 0;
}

AudioDevices::AudioDevices() = default;

AudioDevices::~AudioDevices()
{
  Stop();
}

long AudioDevices::InputCallback(cubeb_stream*, void* user, const void* input, void* /*output*/,
                                 long frames)
{
  auto* self = static_cast<AudioDevices*>(user);
  if (input != nullptr && frames > 0)
    self->m_captured.Write(static_cast<const float*>(input), static_cast<std::size_t>(frames));
  return frames;
}

long AudioDevices::OutputCallback(cubeb_stream*, void* user, const void* /*input*/, void* output,
                                  long frames)
{
  auto* self = static_cast<AudioDevices*>(user);
  if (output == nullptr || frames <= 0)
    return frames;
  // Read zero-fills what it does not have, so an empty ring plays silence
  // rather than whatever the driver left in the buffer. Frames are stereo pairs;
  // the ring holds individual samples.
  self->m_to_play.Read(static_cast<float*>(output),
                       static_cast<std::size_t>(frames) * OUTPUT_CHANNELS);
  return frames;
}

void AudioDevices::StateCallback(cubeb_stream*, void*, cubeb_state state)
{
  if (state == CUBEB_STATE_ERROR)
    ERROR_LOG_FMT(AUDIO, "Voice chat: a cubeb stream reported an error");
}

bool AudioDevices::StartCapture(const std::string& device_id)
{
  if (m_input_stream)
    return true;
  if (!m_context)
    m_context = CubebUtils::GetContext();
  if (!m_context)
    return false;

  cubeb_stream_params params{};
  params.format = CUBEB_SAMPLE_FLOAT32NE;
  params.rate = SAMPLE_RATE;
  params.channels = 1;
  params.layout = CUBEB_LAYOUT_MONO;
  // Tell the backend this is speech rather than a recording. It costs nothing
  // where it means nothing, and on Android it decides which capture path the
  // system hands over: without it the microphone is opened as a camcorder,
  // which is deliberately unprocessed and picks up the game coming back out of
  // the speaker a few centimetres away.
  params.prefs = CUBEB_STREAM_PREF_VOICE;

  u32 latency = 0;
  if (cubeb_get_min_latency(m_context.get(), &params, &latency) != CUBEB_OK)
    latency = 512;
  // A 20 ms working frame with a much smaller device buffer just means more
  // callbacks; the floor is there so a backend reporting an absurdly small
  // minimum does not have us woken thousands of times a second.
  latency = std::max<u32>(latency, 256);

  cubeb_devid device = CubebUtils::GetInputDeviceById(device_id);
  if (cubeb_stream_init(m_context.get(), &m_input_stream, "Dolphin Lobby Voice (in)", device,
                        &params, nullptr, nullptr, latency, InputCallback, StateCallback,
                        this) != CUBEB_OK)
  {
    ERROR_LOG_FMT(AUDIO, "Voice chat: could not open the microphone");
    m_input_stream = nullptr;
    return false;
  }
  if (cubeb_stream_start(m_input_stream) != CUBEB_OK)
  {
    ERROR_LOG_FMT(AUDIO, "Voice chat: could not start the microphone");
    cubeb_stream_destroy(m_input_stream);
    m_input_stream = nullptr;
    return false;
  }
  m_captured.Clear();
  return true;
}

bool AudioDevices::StartPlayback(const std::string& device_id)
{
  if (m_output_stream)
    return true;
  if (!m_context)
    m_context = CubebUtils::GetContext();
  if (!m_context)
    return false;

  cubeb_stream_params params{};
  params.format = CUBEB_SAMPLE_FLOAT32NE;
  params.rate = SAMPLE_RATE;
  // Stereo even when spatial audio is off, in which case both channels get the
  // same thing. The alternative is tearing the stream down and rebuilding it
  // whenever the setting changes, mid-race, which is worse than one wasted
  // channel of bandwidth to the sound card.
  params.channels = OUTPUT_CHANNELS;
  params.layout = CUBEB_LAYOUT_STEREO;

  u32 latency = 0;
  if (cubeb_get_min_latency(m_context.get(), &params, &latency) != CUBEB_OK)
    latency = 512;
  latency = std::max<u32>(latency, 256);

  cubeb_devid device = CubebUtils::GetOutputDeviceById(device_id);
  if (cubeb_stream_init(m_context.get(), &m_output_stream, "Dolphin Lobby Voice (out)", nullptr,
                        nullptr, device, &params, latency, OutputCallback, StateCallback,
                        this) != CUBEB_OK)
  {
    ERROR_LOG_FMT(AUDIO, "Voice chat: could not open the output device");
    m_output_stream = nullptr;
    return false;
  }
  if (cubeb_stream_start(m_output_stream) != CUBEB_OK)
  {
    ERROR_LOG_FMT(AUDIO, "Voice chat: could not start the output device");
    cubeb_stream_destroy(m_output_stream);
    m_output_stream = nullptr;
    return false;
  }
  m_to_play.Clear();
  return true;
}

void AudioDevices::Stop()
{
  if (m_input_stream)
  {
    cubeb_stream_stop(m_input_stream);
    cubeb_stream_destroy(m_input_stream);
    m_input_stream = nullptr;
  }
  if (m_output_stream)
  {
    cubeb_stream_stop(m_output_stream);
    cubeb_stream_destroy(m_output_stream);
    m_output_stream = nullptr;
  }
  m_captured.Clear();
  m_to_play.Clear();
  m_context.reset();
}
}  // namespace Lobby::Voice
