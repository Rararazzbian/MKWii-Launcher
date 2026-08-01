// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Lobby/Voice/VoiceCodec.h"

#include <opus.h>

#include "Common/Logging/Log.h"

namespace Lobby::Voice
{
namespace
{
// Comfortably above what Opus will produce for one 20 ms mono frame even at the
// top of the allowed bitrate range.
constexpr std::size_t MAX_ENCODED = 1500;
}  // namespace

Encoder::Encoder()
{
  int error = OPUS_OK;
  m_encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &error);
  if (error != OPUS_OK || m_encoder == nullptr)
  {
    ERROR_LOG_FMT(AUDIO, "Voice chat: could not create the Opus encoder: {}",
                  opus_strerror(error));
    m_encoder = nullptr;
    return;
  }

  // Speech, not music: the VOIP application above already biases towards
  // intelligibility, and this tells the encoder the same thing about content.
  opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  // In-band FEC costs a little bitrate and lets the decoder rebuild a lost
  // frame from the next one, which on a link with any loss at all is a better
  // use of those bits than more detail in the frames that did arrive.
  opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(1));
  opus_encoder_ctl(m_encoder, OPUS_SET_PACKET_LOSS_PERC(5));
  // Discontinuous transmission: silence costs almost nothing on the wire.
  opus_encoder_ctl(m_encoder, OPUS_SET_DTX(1));
}

Encoder::~Encoder()
{
  if (m_encoder)
    opus_encoder_destroy(m_encoder);
}

void Encoder::SetBitrate(int bits_per_second)
{
  if (!m_encoder || bits_per_second == m_bitrate)
    return;
  m_bitrate = bits_per_second;
  opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(bits_per_second));
}

std::vector<u8> Encoder::Encode(const float* samples)
{
  if (!m_encoder)
    return {};

  std::vector<u8> out(MAX_ENCODED);
  const opus_int32 written = opus_encode_float(m_encoder, samples, FRAME_SAMPLES, out.data(),
                                               static_cast<opus_int32>(out.size()));
  if (written < 0)
  {
    ERROR_LOG_FMT(AUDIO, "Voice chat: Opus encode failed: {}", opus_strerror(written));
    return {};
  }
  // DTX makes the encoder emit a one or two byte frame meaning "still silent".
  // Sending it keeps the decoder's timeline intact for a fraction of the
  // bandwidth, so it is not treated as nothing to send.
  out.resize(static_cast<std::size_t>(written));
  return out;
}

Decoder::Decoder()
{
  int error = OPUS_OK;
  m_decoder = opus_decoder_create(SAMPLE_RATE, 1, &error);
  if (error != OPUS_OK || m_decoder == nullptr)
  {
    ERROR_LOG_FMT(AUDIO, "Voice chat: could not create the Opus decoder: {}",
                  opus_strerror(error));
    m_decoder = nullptr;
  }
}

Decoder::~Decoder()
{
  if (m_decoder)
    opus_decoder_destroy(m_decoder);
}

bool Decoder::Decode(const u8* data, std::size_t length, float* out)
{
  if (!m_decoder)
    return false;

  const int decoded =
      data == nullptr ?
          // Concealment: no data, and Opus is told to invent a frame's worth.
          opus_decode_float(m_decoder, nullptr, 0, out, FRAME_SAMPLES, 0) :
          opus_decode_float(m_decoder, data, static_cast<opus_int32>(length), out, FRAME_SAMPLES,
                            0);

  if (decoded < 0)
    return false;

  // A short decode should not leave the tail of the frame holding whatever was
  // in the buffer last time.
  for (std::size_t i = static_cast<std::size_t>(decoded); i < FRAME_SAMPLES; ++i)
    out[i] = 0.0f;
  return true;
}
}  // namespace Lobby::Voice
