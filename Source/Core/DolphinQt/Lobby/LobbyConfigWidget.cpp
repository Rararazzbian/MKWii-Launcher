// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Lobby/LobbyConfigWidget.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

#ifdef HAVE_CUBEB
#include "AudioCommon/CubebUtils.h"
#endif

#include "Core/Config/MainSettings.h"
#include "Core/NetPlayProto.h"

#include "DolphinQt/QtUtils/UTF8CodePointCountValidator.h"

namespace
{
// Splits "host:port". Returns false if either half is missing or the port is
// out of range, which is what gates the Play button / wizard Next.
bool ParseAddress(const QString& text, QString* host, int* port)
{
  const int colon = text.lastIndexOf(QLatin1Char(':'));
  if (colon <= 0 || colon == text.size() - 1)
    return false;

  bool ok = false;
  const int parsed = text.mid(colon + 1).toInt(&ok);
  if (!ok || parsed < 1 || parsed > 65535)
    return false;

  if (host)
    *host = text.left(colon).trimmed();
  if (port)
    *port = parsed;
  return !text.left(colon).trimmed().isEmpty();
}
}  // namespace

LobbyConfigWidget::LobbyConfigWidget(bool show_microphone, QWidget* parent) : QWidget(parent)
{
  m_nickname = new QLineEdit;
  m_nickname->setValidator(new UTF8CodePointCountValidator(NetPlay::MAX_NAME_LENGTH, m_nickname));
  m_nickname->setText(QString::fromStdString(Config::Get(Config::MAIN_LOBBY_NICKNAME)));

  auto* const identity_box = new QGroupBox(tr("Identity"));
  auto* const identity_layout = new QGridLayout(identity_box);
  identity_layout->addWidget(new QLabel(tr("Nickname:")), 0, 0);
  identity_layout->addWidget(m_nickname, 0, 1);

  if (show_microphone)
  {
    m_microphone = new QComboBox;
#ifndef HAVE_CUBEB
    m_microphone->addItem(QLatin1String("(%1)").arg(tr("Audio backend unsupported")), QString{});
#else
    m_microphone->addItem(QLatin1String("(%1)").arg(tr("Autodetect preferred microphone")),
                          QString{});
    for (const auto& [device_id, device_name] : CubebUtils::ListInputDevices())
      m_microphone->addItem(QString::fromStdString(device_name), QString::fromStdString(device_id));
#endif
    // findData returns -1 for a device that has since been unplugged, which
    // conveniently falls back to the autodetect entry at index 0.
    const int saved =
        m_microphone->findData(QString::fromStdString(Config::Get(Config::MAIN_LOBBY_MICROPHONE)));
    m_microphone->setCurrentIndex(saved < 0 ? 0 : saved);

    identity_layout->addWidget(new QLabel(tr("Microphone:")), 1, 0);
    identity_layout->addWidget(m_microphone, 1, 1);
  }

  m_host = new QRadioButton(tr("Host the lobby"));
  m_client = new QRadioButton(tr("Join someone else's lobby"));

  auto* const role_group = new QButtonGroup(this);
  role_group->addButton(m_host);
  role_group->addButton(m_client);

  const bool is_host = Config::Get(Config::MAIN_LOBBY_IS_HOST);
  m_host->setChecked(is_host);
  m_client->setChecked(!is_host);

  auto* const role_box = new QGroupBox(tr("Role"));
  auto* const role_layout = new QGridLayout(role_box);
  role_layout->addWidget(m_host, 0, 0);
  role_layout->addWidget(m_client, 1, 0);

  m_port = new QSpinBox;
  m_port->setRange(1, 65535);
  m_port->setValue(Config::Get(Config::MAIN_LOBBY_PORT));

  m_host_box = new QGroupBox(tr("Hosting"));
  auto* const host_layout = new QGridLayout(m_host_box);
  host_layout->addWidget(new QLabel(tr("Listen port:")), 0, 0);
  host_layout->addWidget(m_port, 0, 1);
  host_layout->setColumnStretch(1, 1);

  m_address = new QLineEdit;
  m_address->setPlaceholderText(QStringLiteral("192.168.1.10:7788"));
  m_address->setText(QString::fromStdString(Config::Get(Config::MAIN_LOBBY_HOST_ADDRESS)));

  m_client_box = new QGroupBox(tr("Joining"));
  auto* const client_layout = new QGridLayout(m_client_box);
  client_layout->addWidget(new QLabel(tr("Host address:")), 0, 0);
  client_layout->addWidget(m_address, 0, 1);
  client_layout->setColumnStretch(1, 1);

  auto* const layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(identity_box);
  layout->addWidget(role_box);
  layout->addWidget(m_host_box);
  layout->addWidget(m_client_box);

  UpdateRoleVisibility();

  connect(m_nickname, &QLineEdit::textChanged, this, &LobbyConfigWidget::Changed);
  connect(m_address, &QLineEdit::textChanged, this, &LobbyConfigWidget::Changed);
  connect(m_port, &QSpinBox::valueChanged, this, &LobbyConfigWidget::Changed);
  if (m_microphone)
    connect(m_microphone, &QComboBox::currentIndexChanged, this, &LobbyConfigWidget::Changed);
  connect(m_host, &QRadioButton::toggled, this, [this] {
    UpdateRoleVisibility();
    emit Changed();
  });
}

void LobbyConfigWidget::UpdateRoleVisibility()
{
  // Only one of the two is ever relevant, and showing the other greyed out
  // just invites people to fill in a field that does nothing.
  const bool is_host = m_host->isChecked();
  m_host_box->setVisible(is_host);
  m_client_box->setVisible(!is_host);
}

bool LobbyConfigWidget::IsComplete() const
{
  if (m_nickname->text().trimmed().isEmpty())
    return false;
  if (m_host->isChecked())
    return true;
  return ParseAddress(m_address->text().trimmed(), nullptr, nullptr);
}

void LobbyConfigWidget::Save() const
{
  Config::SetBase(Config::MAIN_LOBBY_NICKNAME, m_nickname->text().trimmed().toStdString());
  Config::SetBase(Config::MAIN_LOBBY_IS_HOST, m_host->isChecked());
  Config::SetBase(Config::MAIN_LOBBY_PORT, m_port->value());
  Config::SetBase(Config::MAIN_LOBBY_HOST_ADDRESS, m_address->text().trimmed().toStdString());

  if (m_microphone)
  {
    Config::SetBase(Config::MAIN_LOBBY_MICROPHONE,
                    m_microphone->currentData().toString().toStdString());
  }
}
