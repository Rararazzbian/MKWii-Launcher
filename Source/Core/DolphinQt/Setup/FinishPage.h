// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWizardPage>

// Step 4: what happens after Finish. Pressing it boots the Mii Channel
// directly - the console's own first-time setup is not needed for LAN play.
class FinishPage final : public QWizardPage
{
  Q_OBJECT

public:
  explicit FinishPage(QWidget* parent = nullptr);
};
