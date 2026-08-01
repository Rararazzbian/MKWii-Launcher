// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWizardPage>

class LobbyConfigWidget;

// Step 1: who you are in the lobby, whether you host it, which microphone you
// speak into, and how the connection is made.
class LobbyPage final : public QWizardPage
{
  Q_OBJECT

public:
  explicit LobbyPage(QWidget* parent = nullptr);

  bool isComplete() const override;
  // Saves this page's settings. Called by QWizard when Next/Finish is pressed.
  bool validatePage() override;

private:
  LobbyConfigWidget* m_config;
};
