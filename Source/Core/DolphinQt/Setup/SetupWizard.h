// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWizard>

// First-run setup for the MKWii launcher. Shown instead of the main window
// until it has been completed once; see Config::MAIN_SETUP_WIZARD_COMPLETE.
//
// Each page saves its own settings from validatePage(), i.e. when Next or
// Finish is pressed. This wizard only records that setup finished, so
// cancelling half way keeps whatever was already entered but still runs the
// wizard again on the next launch.
class SetupWizard final : public QWizard
{
  Q_OBJECT

public:
  explicit SetupWizard(QWidget* parent = nullptr);

  // True if the wizard still needs to run.
  static bool IsRequired();

private:
  void MarkComplete();
};
