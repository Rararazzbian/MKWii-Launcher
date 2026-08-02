// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The Voice Chat panel, opened from the toolbar beside Config, Graphics and
// Controllers.
//
// Every control writes straight through to VoiceSettings, which persists it, so
// there is no apply button and nothing to lose by closing the window. The
// meters are driven by a timer rather than by signals, because the levels they
// show live on the audio worker and are read rather than pushed.

#pragma once

#include <map>
#include <string>

#include <QCheckBox>
#include <QDialog>
#include <QSlider>

#include "Common/CommonTypes.h"

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

// A slider with an audio level painted inside its groove.
//
// The level and the setting belong together - how loud someone is and how loud
// you have made them are the same question - so they share one control rather
// than sitting in two widgets that have to be read side by side. On the noise
// gate this is the whole point: the handle is the threshold, and you can see
// the signal it is about to cut.
class MeterSlider final : public QSlider
{
  Q_OBJECT

public:
  explicit MeterSlider(QWidget* parent = nullptr);

  void SetLevel(float level);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  float m_level = 0.0f;
};

class VoiceChatWindow final : public QDialog
{
  Q_OBJECT

public:
  explicit VoiceChatWindow(QWidget* parent = nullptr);

private:
  void CreateWidgets();
  void ConnectWidgets();
  void LoadSettings();
  // Rebuilds the per-person rows when the lobby roster changes.
  void RebuildPeers();
  // Meters, distances and the host-only enablement, thirty times a second.
  void Refresh();
  // Keeps the microphone slider in the Levels group and the one on your own row
  // showing the same number, since they are the same setting.
  void SetMicGain(int value, QWidget* from);

  QLabel* m_status;

  QPushButton* m_mute;
  QPushButton* m_deafen;

  QComboBox* m_input_device;
  QComboBox* m_output_device;

  MeterSlider* m_mic_gain;
  QLabel* m_mic_gain_label;

  MeterSlider* m_master;
  QLabel* m_master_label;

  MeterSlider* m_gate;
  QLabel* m_gate_label;

  QSlider* m_doppler;
  QLabel* m_doppler_label;

  QSlider* m_bitrate;
  QLabel* m_bitrate_label;

  QSpinBox* m_proximity_range;
  QCheckBox* m_spatial;

  QVBoxLayout* m_peer_layout;
  QLabel* m_peer_empty;

  struct PeerRow
  {
    QWidget* container = nullptr;
    QLabel* name = nullptr;
    QLabel* detail = nullptr;
    MeterSlider* volume = nullptr;
    QLabel* volume_label = nullptr;
    // What the row was built for, so a rename rebuilds rather than silently
    // saving one person's volume under another's name.
    std::string nickname;
    bool is_local = false;
  };
  std::map<u32, PeerRow> m_peer_rows;
};
