// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Setup/LobbyPage.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QVBoxLayout>

#ifdef HAVE_CUBEB
#include "AudioCommon/CubebUtils.h"
#endif

#include "Core/Config/MainSettings.h"
#include "Core/NetPlayProto.h"

#include "DolphinQt/QtUtils/UTF8CodePointCountValidator.h"

LobbyPage::LobbyPage(QWidget* parent) : QWizardPage(parent)
{
  setTitle(tr("Lobby"));

  m_nickname = new QLineEdit;
  // Matches NetPlaySetupDialog: a code point count, not a QString length, so
  // the limit means the same thing for names that are not plain ASCII.
  m_nickname->setValidator(new UTF8CodePointCountValidator(NetPlay::MAX_NAME_LENGTH, m_nickname));
  m_nickname->setText(QString::fromStdString(Config::Get(Config::MAIN_LOBBY_NICKNAME)));

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
  const int saved = m_microphone->findData(
      QString::fromStdString(Config::Get(Config::MAIN_LOBBY_MICROPHONE)));
  m_microphone->setCurrentIndex(saved < 0 ? 0 : saved);

  m_host = new QRadioButton(tr("Host the lobby"));
  m_client = new QRadioButton(tr("Join someone else's lobby"));

  // Exclusivity is implicit for radio buttons sharing a parent, but being
  // explicit keeps it correct if these are ever re-parented.
  auto* const role_group = new QButtonGroup(this);
  role_group->addButton(m_host);
  role_group->addButton(m_client);

  if (Config::Get(Config::MAIN_LOBBY_IS_HOST))
    m_host->setChecked(true);
  else
    m_client->setChecked(true);

  auto* const identity_box = new QGroupBox(tr("Identity"));
  auto* const identity_layout = new QGridLayout(identity_box);
  identity_layout->addWidget(new QLabel(tr("Nickname:")), 0, 0);
  identity_layout->addWidget(m_nickname, 0, 1);
  identity_layout->addWidget(new QLabel(tr("Microphone:")), 1, 0);
  identity_layout->addWidget(m_microphone, 1, 1);

  auto* const role_box = new QGroupBox(tr("Role"));
  auto* const role_layout = new QGridLayout(role_box);
  role_layout->addWidget(m_host, 0, 0);
  role_layout->addWidget(m_client, 1, 0);

  auto* const layout = new QVBoxLayout(this);
  layout->addWidget(identity_box);
  layout->addWidget(role_box);
  layout->addStretch();

  connect(m_nickname, &QLineEdit::textChanged, this, &LobbyPage::completeChanged);
}

bool LobbyPage::isComplete() const
{
  return !m_nickname->text().trimmed().isEmpty();
}

bool LobbyPage::validatePage()
{
  // Written here rather than through QWizard fields because the microphone is
  // identified by a cubeb device id carried in the combo box's item data, and
  // QWizard would only hand back the selected index.
  Config::SetBase(Config::MAIN_LOBBY_NICKNAME, m_nickname->text().trimmed().toStdString());
  Config::SetBase(Config::MAIN_LOBBY_IS_HOST, m_host->isChecked());
  Config::SetBase(Config::MAIN_LOBBY_MICROPHONE,
                  m_microphone->currentData().toString().toStdString());
  return true;
}
