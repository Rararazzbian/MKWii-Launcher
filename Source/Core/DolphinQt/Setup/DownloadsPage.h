// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include <QWizardPage>

class QLabel;
class QPushButton;

// Step 2: the two things that have to be installed before Mario Kart will run
// on a LAN with proximity chat - the Wii system software, and Chadsoft's
// LAN_MKW payload on the virtual SD card.
class DownloadsPage final : public QWizardPage
{
  Q_OBJECT

public:
  explicit DownloadsPage(QWidget* parent = nullptr);

  void initializePage() override;
  bool isComplete() const override;

private:
  void InstallSystemUpdate();
  void InstallProximityPatch();

  // Both are re-checked on every page entry rather than cached, so a user who
  // installed either one outside the wizard is not made to do it again.
  static bool IsSystemUpdateInstalled();
  static bool IsProximityPatchInstalled();

  // Download, unzip into the SD sync folder, and pack the SD image. Returns an
  // empty string on success or a human-readable reason on failure.
  std::string DownloadAndInstallPatch();

  void RefreshStatus();

  QPushButton* m_update_button;
  QLabel* m_update_status;
  QPushButton* m_patch_button;
  QLabel* m_patch_status;
};
