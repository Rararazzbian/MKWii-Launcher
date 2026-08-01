// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Lobby/VoiceChatWindow.h"

#include <algorithm>
#include <cmath>

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QVBoxLayout>

#include "AudioCommon/CubebUtils.h"
#include "Core/Lobby/LobbyNet.h"
#include "Core/Lobby/Voice/VoiceChat.h"
#include "Core/Lobby/Voice/VoiceSettings.h"

namespace
{
// Meters at 30 Hz. Fast enough to look live, slow enough that the panel is not
// what the machine is spending its time on.
constexpr int REFRESH_MS = 33;

const QColor METER_COLOUR(0x3F, 0xB9, 0x50, 190);
const QColor METER_HOT(0xD2, 0x99, 0x22, 190);

QString FormatDistance(float units)
{
  if (units < 0.0f)
    return QObject::tr("not on the track");
  // World units are not metres and there is no honest conversion, so they are
  // shown as they are rather than dressed up as a distance in something.
  return QObject::tr("%1 units away").arg(static_cast<int>(units));
}
}  // namespace

MeterSlider::MeterSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent)
{
}

void MeterSlider::SetLevel(float level)
{
  level = std::clamp(level, 0.0f, 1.0f);
  // Repaint only on a visible change; this runs thirty times a second for every
  // person in the lobby.
  if (std::abs(level - m_level) < 0.005f)
    return;
  m_level = level;
  update();
}

void MeterSlider::paintEvent(QPaintEvent* event)
{
  QSlider::paintEvent(event);
  if (m_level <= 0.0f)
    return;

  QStyleOptionSlider option;
  initStyleOption(&option);
  const QRect groove =
      style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
  const QRect handle =
      style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);

  QRect bar = groove.adjusted(1, 1, -1, -1);
  if (bar.width() <= 0 || bar.height() <= 0)
    return;
  bar.setWidth(static_cast<int>(bar.width() * m_level));

  QPainter painter(this);
  // Around the handle rather than under it, so the setting stays readable while
  // the level moves behind it.
  QRegion clip(rect());
  clip -= handle;
  painter.setClipRegion(clip);
  painter.fillRect(bar, m_level > 0.9f ? METER_HOT : METER_COLOUR);
}

VoiceChatWindow::VoiceChatWindow(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("Voice Chat"));
  setMinimumWidth(520);

  CreateWidgets();
  ConnectWidgets();
  LoadSettings();

  auto* timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, &VoiceChatWindow::Refresh);
  timer->start(REFRESH_MS);
  Refresh();
}

void VoiceChatWindow::CreateWidgets()
{
  auto* layout = new QVBoxLayout(this);

  m_status = new QLabel;
  m_status->setWordWrap(true);
  layout->addWidget(m_status);

  auto* toggles = new QHBoxLayout;
  m_mute = new QPushButton(tr("Mute"));
  m_deafen = new QPushButton(tr("Deafen"));
  for (QPushButton* button : {m_mute, m_deafen})
  {
    button->setCheckable(true);
    toggles->addWidget(button);
  }
  toggles->addStretch();
  layout->addLayout(toggles);

  // --- Devices -------------------------------------------------------------
  auto* devices = new QGroupBox(tr("Devices"));
  auto* device_layout = new QGridLayout(devices);
  m_input_device = new QComboBox;
  m_output_device = new QComboBox;

  m_input_device->addItem(tr("Default"), QString{});
  for (const auto& [id, name] : CubebUtils::ListInputDevices())
    m_input_device->addItem(QString::fromStdString(name), QString::fromStdString(id));
  m_output_device->addItem(tr("Default"), QString{});
  for (const auto& [id, name] : CubebUtils::ListOutputDevices())
    m_output_device->addItem(QString::fromStdString(name), QString::fromStdString(id));

  device_layout->addWidget(new QLabel(tr("Microphone:")), 0, 0);
  device_layout->addWidget(m_input_device, 0, 1);
  device_layout->addWidget(new QLabel(tr("Output:")), 1, 0);
  device_layout->addWidget(m_output_device, 1, 1);
  layout->addWidget(devices);

  // --- Levels --------------------------------------------------------------
  auto* levels = new QGroupBox(tr("Levels"));
  auto* level_layout = new QGridLayout(levels);
  int row = 0;

  m_mic_gain = new MeterSlider;
  m_mic_gain->setRange(0, 200);
  m_mic_gain_label = new QLabel;
  level_layout->addWidget(new QLabel(tr("Microphone:")), row, 0);
  level_layout->addWidget(m_mic_gain, row, 1);
  level_layout->addWidget(m_mic_gain_label, row, 2);

  m_master = new MeterSlider;
  m_master->setRange(0, 200);
  m_master_label = new QLabel;
  level_layout->addWidget(new QLabel(tr("Everyone's volume:")), ++row, 0);
  level_layout->addWidget(m_master, row, 1);
  level_layout->addWidget(m_master_label, row, 2);

  m_gate = new MeterSlider;
  // Whole decibels: finer than that is beyond what anyone can set by ear.
  m_gate->setRange(static_cast<int>(Lobby::Voice::MIN_GATE_DB),
                   static_cast<int>(Lobby::Voice::MAX_GATE_DB));
  m_gate_label = new QLabel;
  level_layout->addWidget(new QLabel(tr("Noise gate:")), ++row, 0);
  level_layout->addWidget(m_gate, row, 1);
  level_layout->addWidget(m_gate_label, row, 2);
  layout->addWidget(levels);

  // --- Proximity -----------------------------------------------------------
  auto* proximity = new QGroupBox(tr("Proximity"));
  auto* proximity_layout = new QGridLayout(proximity);

  m_doppler = new QSlider(Qt::Horizontal);
  m_doppler->setRange(0, 200);
  m_doppler_label = new QLabel;

  m_proximity_range = new QSpinBox;
  m_proximity_range->setRange(1000, 60000);
  m_proximity_range->setSingleStep(500);
  m_proximity_range->setSuffix(tr(" units"));

  proximity_layout->addWidget(new QLabel(tr("Doppler:")), 0, 0);
  proximity_layout->addWidget(m_doppler, 0, 1);
  proximity_layout->addWidget(m_doppler_label, 0, 2);
  proximity_layout->addWidget(new QLabel(tr("Hearing range:")), 1, 0);
  proximity_layout->addWidget(m_proximity_range, 1, 1);
  layout->addWidget(proximity);

  // --- Quality -------------------------------------------------------------
  auto* quality = new QGroupBox(tr("Quality"));
  auto* quality_layout = new QGridLayout(quality);
  m_bitrate = new QSlider(Qt::Horizontal);
  m_bitrate->setRange(Lobby::Voice::MIN_BITRATE / 1000, Lobby::Voice::MAX_BITRATE / 1000);
  m_bitrate->setSingleStep(8);
  m_bitrate->setPageStep(32);
  m_bitrate_label = new QLabel;
  quality_layout->addWidget(new QLabel(tr("Bitrate:")), 0, 0);
  quality_layout->addWidget(m_bitrate, 0, 1);
  quality_layout->addWidget(m_bitrate_label, 0, 2);
  layout->addWidget(quality);

  // --- People --------------------------------------------------------------
  auto* people = new QGroupBox(tr("In the lobby"));
  auto* people_outer = new QVBoxLayout(people);
  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setMinimumHeight(160);

  auto* holder = new QWidget;
  m_peer_layout = new QVBoxLayout(holder);
  m_peer_empty = new QLabel(tr("Nobody else is here yet."));
  m_peer_layout->addWidget(m_peer_empty);
  m_peer_layout->addStretch();
  scroll->setWidget(holder);
  people_outer->addWidget(scroll);
  layout->addWidget(people, 1);
}

void VoiceChatWindow::SetMicGain(int value, QWidget* from)
{
  Lobby::Voice::SetMicGain(value);
  m_mic_gain_label->setText(tr("%1%").arg(value));

  if (from != m_mic_gain)
  {
    const QSignalBlocker blocker(m_mic_gain);
    m_mic_gain->setValue(value);
  }
  // Your own row carries the same setting, so it follows along.
  for (auto& [ip, row] : m_peer_rows)
  {
    if (!row.is_local || row.volume == from)
      continue;
    const QSignalBlocker blocker(row.volume);
    row.volume->setValue(value);
    row.volume_label->setText(tr("%1%").arg(value));
  }
}

void VoiceChatWindow::ConnectWidgets()
{
  connect(m_mute, &QPushButton::toggled, this, [this](bool on) {
    Lobby::Voice::SetMuted(on);
    Refresh();
  });
  connect(m_deafen, &QPushButton::toggled, this, [this](bool on) {
    Lobby::Voice::SetDeafened(on);
    Refresh();
  });

  connect(m_input_device, &QComboBox::currentIndexChanged, this, [this](int index) {
    Lobby::Voice::SetInputDevice(m_input_device->itemData(index).toString().toStdString());
    Lobby::Voice::RestartDevices();
  });
  connect(m_output_device, &QComboBox::currentIndexChanged, this, [this](int index) {
    Lobby::Voice::SetOutputDevice(m_output_device->itemData(index).toString().toStdString());
    Lobby::Voice::RestartDevices();
  });

  connect(m_mic_gain, &QSlider::valueChanged, this,
          [this](int value) { SetMicGain(value, m_mic_gain); });
  connect(m_master, &QSlider::valueChanged, this, [this](int value) {
    Lobby::Voice::SetMasterVolume(value);
    m_master_label->setText(tr("%1%").arg(value));
  });
  connect(m_gate, &QSlider::valueChanged, this, [this](int value) {
    Lobby::Voice::SetGateDb(static_cast<float>(value));
    m_gate_label->setText(tr("%1 dB").arg(value));
  });
  connect(m_doppler, &QSlider::valueChanged, this, [this](int value) {
    Lobby::Voice::SetDoppler(value);
    m_doppler_label->setText(tr("%1%").arg(value));
  });
  connect(m_proximity_range, &QSpinBox::valueChanged, this,
          [](int value) { Lobby::Voice::SetProximityRange(static_cast<float>(value)); });

  connect(m_bitrate, &QSlider::valueChanged, this, [this](int kbps) {
    m_bitrate_label->setText(tr("%1 kbps").arg(kbps));
    // Only the host's slider does anything; a client's is disabled, so this
    // cannot fire for one.
    Lobby::Voice::SetBitrateAndPush(kbps * 1000);
  });
}

void VoiceChatWindow::LoadSettings()
{
  const Lobby::Voice::Settings settings = Lobby::Voice::Get();

  const QSignalBlocker block_mute(m_mute);
  const QSignalBlocker block_deafen(m_deafen);
  m_mute->setChecked(settings.muted);
  m_deafen->setChecked(settings.deafened);

  const auto select = [](QComboBox* box, const std::string& id) {
    const QSignalBlocker blocker(box);
    const int index = box->findData(QString::fromStdString(id));
    box->setCurrentIndex(index >= 0 ? index : 0);
  };
  select(m_input_device, settings.input_device);
  select(m_output_device, settings.output_device);

  m_mic_gain->setValue(settings.mic_gain);
  m_mic_gain_label->setText(tr("%1%").arg(settings.mic_gain));
  m_master->setValue(settings.master_volume);
  m_master_label->setText(tr("%1%").arg(settings.master_volume));
  m_gate->setValue(static_cast<int>(settings.gate_db));
  m_gate_label->setText(tr("%1 dB").arg(static_cast<int>(settings.gate_db)));
  m_doppler->setValue(settings.doppler);
  m_doppler_label->setText(tr("%1%").arg(settings.doppler));
  m_proximity_range->setValue(static_cast<int>(settings.proximity_range));

  const QSignalBlocker block_bitrate(m_bitrate);
  m_bitrate->setValue(settings.bitrate / 1000);
  m_bitrate_label->setText(tr("%1 kbps").arg(settings.bitrate / 1000));
}

void VoiceChatWindow::RebuildPeers()
{
  const std::vector<Lobby::Voice::PeerInfo> peers = Lobby::Voice::GetPeers();

  // Drop rows for people who have left, and for anyone whose nickname changed -
  // the volume is stored against the nickname, so a stale row would write one
  // person's setting under another's name.
  std::erase_if(m_peer_rows, [&](auto& item) {
    const auto found = std::ranges::find_if(
        peers, [&](const Lobby::Voice::PeerInfo& peer) { return peer.ip == item.first; });
    if (found != peers.end() && found->nickname == item.second.nickname)
      return false;
    item.second.container->deleteLater();
    return true;
  });

  for (const Lobby::Voice::PeerInfo& peer : peers)
  {
    if (m_peer_rows.contains(peer.ip))
      continue;

    PeerRow row;
    row.nickname = peer.nickname;
    row.is_local = peer.is_local;
    row.container = new QWidget;
    auto* row_layout = new QGridLayout(row.container);
    row_layout->setContentsMargins(0, 4, 0, 4);

    row.name = new QLabel;
    QFont bold = row.name->font();
    bold.setBold(true);
    row.name->setFont(bold);
    row.volume = new MeterSlider;
    row.volume_label = new QLabel;
    row.detail = new QLabel;

    row_layout->addWidget(row.name, 0, 0, 1, 2);
    row_layout->addWidget(row.volume, 1, 0);
    row_layout->addWidget(row.volume_label, 1, 1);
    row_layout->addWidget(row.detail, 2, 0, 1, 2);

    if (peer.is_local)
    {
      // Your own voice is never played back to you, so a playback volume here
      // would do nothing. The slider carries your microphone gain instead,
      // which is the setting that belongs next to your own level.
      row.volume->setRange(0, 200);
      row.volume->setValue(Lobby::Voice::Get().mic_gain);
      row.volume_label->setText(tr("%1%").arg(row.volume->value()));
      MeterSlider* const slider = row.volume;
      connect(slider, &QSlider::valueChanged, this,
              [this, slider](int value) { SetMicGain(value, slider); });
    }
    else
    {
      row.volume->setRange(0, 100);
      const std::string nickname = peer.nickname;
      row.volume->setValue(Lobby::Voice::GetPeerVolume(nickname));
      row.volume_label->setText(tr("%1%").arg(row.volume->value()));
      connect(row.volume, &QSlider::valueChanged, this, [this, nickname, ip = peer.ip](int value) {
        Lobby::Voice::SetPeerVolume(nickname, value);
        const auto found = m_peer_rows.find(ip);
        if (found != m_peer_rows.end())
          found->second.volume_label->setText(tr("%1%").arg(value));
      });
    }

    // Before the stretch that keeps the rows at the top.
    m_peer_layout->insertWidget(m_peer_layout->count() - 1, row.container);
    m_peer_rows.emplace(peer.ip, row);
  }

  m_peer_empty->setVisible(peers.size() < 2);
}

void VoiceChatWindow::Refresh()
{
  const bool running = Lobby::Voice::IsRunning();
  const Lobby::Voice::Settings settings = Lobby::Voice::Get();

  m_status->setText(running ?
                        QString::fromStdString(Lobby::Voice::GetStatusText()) :
                        tr("Voice chat starts with the lobby. Press Play to join one."));

  const bool is_host = running && Lobby::GetRole() == Lobby::Role::Host;
  m_bitrate->setEnabled(is_host || !running);

  // A client's value is pushed to it, so keep the slider showing what is
  // actually in force rather than what this machine would choose.
  if (!is_host && running && m_bitrate->value() != settings.bitrate / 1000)
  {
    const QSignalBlocker blocker(m_bitrate);
    m_bitrate->setValue(settings.bitrate / 1000);
    m_bitrate_label->setText(tr("%1 kbps").arg(settings.bitrate / 1000));
  }

  // Deafening implies muting everywhere else, and so it does here: show the
  // microphone as off rather than letting the button claim otherwise.
  m_mute->setText(settings.deafened ? tr("Muted (deafened)") :
                                      settings.muted ? tr("Muted") : tr("Mute"));
  m_mute->setEnabled(!settings.deafened);
  m_deafen->setText(settings.deafened ? tr("Deafened") : tr("Deafen"));

  const float mic = Lobby::Voice::GetMicLevel();
  m_mic_gain->SetLevel(mic);
  m_gate->SetLevel(mic);
  m_master->SetLevel(Lobby::Voice::GetOutputLevel());

  RebuildPeers();

  for (const Lobby::Voice::PeerInfo& peer : Lobby::Voice::GetPeers())
  {
    const auto found = m_peer_rows.find(peer.ip);
    if (found == m_peer_rows.end())
      continue;
    PeerRow& row = found->second;

    row.name->setText(peer.is_local ? tr("%1 (you)").arg(QString::fromStdString(peer.display)) :
                                      QString::fromStdString(peer.display));
    row.volume->SetLevel(peer.level);

    QString detail;
    if (peer.is_local)
    {
      detail = Lobby::Voice::IsTransmitting() ? tr("transmitting") : tr("gate closed");
    }
    else if (peer.proximity)
    {
      detail = tr("positional — %1, %2% volume")
                   .arg(FormatDistance(peer.distance))
                   .arg(static_cast<int>(peer.proximity_gain * 100.0f));
    }
    else
    {
      detail = tr("normal voice chat");
    }
    row.detail->setText(detail);
  }
}
