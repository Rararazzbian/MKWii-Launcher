// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Setup/SetupWizard.h"

#include "Common/Version.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"

#include "DolphinQt/Resources.h"
#include "DolphinQt/Setup/DownloadsPage.h"
#include "DolphinQt/Setup/FinishPage.h"
#include "DolphinQt/Setup/GamePathPage.h"
#include "DolphinQt/Setup/LobbyPage.h"

SetupWizard::SetupWizard(QWidget* parent) : QWizard(parent)
{
  setWindowTitle(QString::fromStdString(Common::GetLauncherTitleStr()));
  setWindowIcon(Resources::GetAppIcon());

  // ClassicStyle draws the page title with the normal palette and skips the
  // header band and separator rule that ModernStyle and AeroStyle paint. Those
  // are hardcoded light-on-white in several Qt styles and look nothing like the
  // rest of Dolphin under a dark theme.
  setWizardStyle(QWizard::ClassicStyle);
  setOption(QWizard::NoBackButtonOnStartPage, true);
  setOption(QWizard::NoCancelButtonOnLastPage, true);
  // Dolphin's own dialogs do not put a help button on anything.
  setOption(QWizard::HaveHelpButton, false);

  setMinimumWidth(560);

  addPage(new LobbyPage(this));
  addPage(new DownloadsPage(this));
  addPage(new GamePathPage(this));
  addPage(new FinishPage(this));

  connect(this, &QDialog::accepted, this, &SetupWizard::MarkComplete);
}

bool SetupWizard::IsRequired()
{
  return !Config::Get(Config::MAIN_SETUP_WIZARD_COMPLETE);
}

void SetupWizard::MarkComplete()
{
  Config::SetBase(Config::MAIN_SETUP_WIZARD_COMPLETE, true);
  Config::Save();
}
