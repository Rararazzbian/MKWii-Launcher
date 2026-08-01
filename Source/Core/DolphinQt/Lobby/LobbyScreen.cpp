// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Lobby/LobbyScreen.h"

#include <QVBoxLayout>

#include "DolphinQt/Lobby/LobbyConfigWidget.h"

LobbyScreen::LobbyScreen(QWidget* parent) : QWidget(parent)
{
  m_config = new LobbyConfigWidget(false);

  auto* const layout = new QVBoxLayout(this);
  layout->addWidget(m_config);
  layout->addStretch();

  // No OK button here, so there is no later point at which to save. Writing on
  // every change also means Play always uses what is currently on screen.
  connect(m_config, &LobbyConfigWidget::Changed, this, [this] { m_config->Save(); });
}

bool LobbyScreen::IsReadyToPlay() const
{
  return m_config->IsComplete();
}

void LobbyScreen::Save() const
{
  m_config->Save();
}
