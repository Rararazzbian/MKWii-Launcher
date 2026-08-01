// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWizardPage>

class QComboBox;
class QLineEdit;
class QRadioButton;

// Step 1: who you are in the lobby, whether you host it, and which microphone
// you speak into.
class LobbyPage final : public QWizardPage
{
  Q_OBJECT

public:
  explicit LobbyPage(QWidget* parent = nullptr);

  bool isComplete() const override;
  // Saves this page's settings. Called by QWizard when Next/Finish is pressed.
  bool validatePage() override;

private:
  QLineEdit* m_nickname;
  QRadioButton* m_host;
  QRadioButton* m_client;
  QComboBox* m_microphone;
};
