// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Setup/LobbyPage.h"

#include <QVBoxLayout>

#include "DolphinQt/Lobby/LobbyConfigWidget.h"

LobbyPage::LobbyPage(QWidget* parent) : QWizardPage(parent)
{
  setTitle(tr("Lobby"));

  m_config = new LobbyConfigWidget(true);

  auto* const layout = new QVBoxLayout(this);
  layout->addWidget(m_config);
  layout->addStretch();

  connect(m_config, &LobbyConfigWidget::Changed, this, &LobbyPage::completeChanged);
}

bool LobbyPage::isComplete() const
{
  return m_config->IsComplete();
}

bool LobbyPage::validatePage()
{
  m_config->Save();
  return true;
}
