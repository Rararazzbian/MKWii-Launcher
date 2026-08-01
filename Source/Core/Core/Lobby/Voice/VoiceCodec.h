// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Opus, wrapped to the two things this needs: turn a 20 ms frame into bytes,
// and turn bytes back into a 20 ms frame - including when the bytes never
// arrived, which is the case the decoder handles better than anything above it
// could.

#pragma once

#include <cstddef>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Lobby/Voice/VoiceDsp.h"

struct OpusEncoder;
struct OpusDecoder;

namespace Lobby::Voice
{
class Encoder
{
public:
  Encoder();
  ~Encoder();
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;

  bool IsValid() const { return m_encoder != nullptr; }
  // Takes effect on the next frame. Ignored if unchanged, because Opus resets
  // some of its state when the target moves.
  void SetBitrate(int bits_per_second);

  // FRAME_SAMPLES of mono float in, encoded bytes out. Empty on failure.
  std::vector<u8> Encode(const float* samples);

private:
  OpusEncoder* m_encoder = nullptr;
  int m_bitrate = 0;
};

class Decoder
{
public:
  Decoder();
  ~Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  bool IsValid() const { return m_decoder != nullptr; }

  // Writes FRAME_SAMPLES of mono float. Pass nullptr to conceal a lost frame:
  // Opus extrapolates from what it has already decoded, which sounds far better
  // than the silence or repeat anything here could substitute.
  bool Decode(const u8* data, std::size_t length, float* out);

private:
  OpusDecoder* m_decoder = nullptr;
};
}  // namespace Lobby::Voice
