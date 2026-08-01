// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Setup/FinishPage.h"

#include <QLabel>
#include <QVBoxLayout>

FinishPage::FinishPage(QWidget* parent) : QWizardPage(parent)
{
  setTitle(tr("Almost done"));

  auto* const message =
      new QLabel(tr("Please create a Mii before playing Mario Kart Wii.\n\nPressing Finish will "
                    "start the Mii Channel."));
  message->setWordWrap(true);

  auto* const layout = new QVBoxLayout(this);
  layout->addWidget(message);
  layout->addStretch();
}
