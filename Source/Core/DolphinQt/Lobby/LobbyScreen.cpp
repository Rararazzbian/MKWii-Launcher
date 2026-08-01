// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Lobby/LobbyScreen.h"

#include <QButtonGroup>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QVBoxLayout>

#include "Core/Config/MainSettings.h"
#include "Core/NetPlayProto.h"

#include "DolphinQt/QtUtils/UTF8CodePointCountValidator.h"

LobbyScreen::LobbyScreen(QWidget* parent) : QWidget(parent)
{
  m_nickname = new QLineEdit;
  m_nickname->setValidator(new UTF8CodePointCountValidator(NetPlay::MAX_NAME_LENGTH, m_nickname));
  m_nickname->setText(QString::fromStdString(Config::Get(Config::MAIN_LOBBY_NICKNAME)));

  m_host = new QRadioButton(tr("Host the lobby"));
  m_client = new QRadioButton(tr("Join someone else's lobby"));

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

  auto* const role_box = new QGroupBox(tr("Role"));
  auto* const role_layout = new QGridLayout(role_box);
  role_layout->addWidget(m_host, 0, 0);
  role_layout->addWidget(m_client, 1, 0);

  // No buttons here: Play, Config, Graphics and Controllers live on the
  // toolbar, which is the only place they appear.
  auto* const layout = new QVBoxLayout(this);
  layout->addWidget(identity_box);
  layout->addWidget(role_box);
  layout->addStretch();

  connect(m_nickname, &QLineEdit::textChanged, this, &LobbyScreen::SaveIdentity);
  connect(m_host, &QRadioButton::toggled, this, &LobbyScreen::SaveIdentity);
}

void LobbyScreen::SaveIdentity()
{
  // Not trimmed on the way in, or the field would fight the user mid-word;
  // trimming happens here instead so what gets stored is still clean.
  Config::SetBase(Config::MAIN_LOBBY_NICKNAME, m_nickname->text().trimmed().toStdString());
  Config::SetBase(Config::MAIN_LOBBY_IS_HOST, m_host->isChecked());
}
