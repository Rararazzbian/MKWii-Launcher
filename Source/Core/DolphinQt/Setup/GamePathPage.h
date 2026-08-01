// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWizardPage>

class QLabel;
class QLineEdit;

// Step 3: locate the game. Only RMCE01 is accepted - the LAN Play Module
// patches that exact build, so any other region or revision would either fail
// to boot or misbehave in ways that are hard to diagnose later.
class GamePathPage final : public QWizardPage
{
  Q_OBJECT

public:
  explicit GamePathPage(QWidget* parent = nullptr);

  void initializePage() override;
  bool isComplete() const override;
  bool validatePage() override;

private:
  void Browse();
  // Re-reads the disc at the current path and updates m_status / m_valid.
  void Validate();

  QLineEdit* m_path;
  QLabel* m_status;
  bool m_valid = false;
};
