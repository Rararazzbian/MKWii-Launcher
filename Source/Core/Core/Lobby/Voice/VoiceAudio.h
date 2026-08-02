// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The microphone and the speakers.
//
// Voice chat does not go through Dolphin's mixer. It has its own pair of cubeb
// streams, so it keeps working while the emulator is paused, while no game is
// loaded, and while the game's own audio is muted - all of which are states a
// lobby spends real time in.
//
// The cubeb callbacks do nothing but move samples in and out of a ring. The
// work happens on the voice worker, which is allowed to allocate and take
// locks; a cubeb callback is not.

#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

#include <cubeb/cubeb.h>

struct cubeb;
struct cubeb_stream;

namespace Lobby::Voice
{
// A quarter of a second of mono audio. Deep enough to ride out a scheduling
// hiccup on either side, shallow enough that the latency it can accumulate is
// not itself the problem.
constexpr std::size_t RING_SAMPLES = 12000;

// Playback is stereo so voices can be placed left and right. Capture stays mono:
// a microphone has one position, and sending two identical channels would double
// the bitrate to say nothing.
constexpr std::size_t OUTPUT_CHANNELS = 2;

// A fixed-size circular buffer of mono float samples, written by one thread and
// read by another.
class SampleRing
{
public:
  explicit SampleRing(std::size_t capacity);

  // Overwrites the oldest samples when full. For audio that is the right
  // failure: falling behind should cost the stalest audio, not the newest.
  void Write(const float* samples, std::size_t count);
  // Zero-fills whatever it could not supply, and says how much was real.
  std::size_t Read(float* out, std::size_t count);
  std::size_t Available() const;
  void Clear();

private:
  mutable std::mutex m_mutex;
  std::vector<float> m_buffer;
  std::size_t m_write = 0;
  std::size_t m_read = 0;
  std::size_t m_filled = 0;
};

class AudioDevices
{
public:
  AudioDevices();
  ~AudioDevices();
  AudioDevices(const AudioDevices&) = delete;
  AudioDevices& operator=(const AudioDevices&) = delete;

  // Either may fail on its own; capture failing still leaves you able to hear,
  // which is more useful than refusing to start.
  bool StartCapture(const std::string& device_id);
  bool StartPlayback(const std::string& device_id);
  void Stop();

  bool IsCapturing() const { return m_input_stream != nullptr; }
  bool IsPlaying() const { return m_output_stream != nullptr; }

  SampleRing& Captured() { return m_captured; }
  SampleRing& ToPlay() { return m_to_play; }

private:
  static long InputCallback(cubeb_stream* stream, void* user, const void* input, void* output,
                            long frames);
  static long OutputCallback(cubeb_stream* stream, void* user, const void* input, void* output,
                             long frames);
  static void StateCallback(cubeb_stream* stream, void* user, cubeb_state state);

  std::shared_ptr<cubeb> m_context;
  cubeb_stream* m_input_stream = nullptr;
  cubeb_stream* m_output_stream = nullptr;

  SampleRing m_captured{RING_SAMPLES};
  // Interleaved, so it holds the same span of time as the mono capture ring.
  SampleRing m_to_play{RING_SAMPLES * OUTPUT_CHANNELS};
};
}  // namespace Lobby::Voice
