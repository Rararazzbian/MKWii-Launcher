// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

class QComboBox;
class QGroupBox;
class QLineEdit;
class QRadioButton;
class QSpinBox;

// Nickname, host/client, and the connection details for whichever of the two is
// selected. Shared by the first-run wizard and the launcher's home screen so
// the two cannot drift apart.
class LobbyConfigWidget final : public QWidget
{
  Q_OBJECT

public:
  // The microphone belongs on the wizard page, where the user is being walked
  // through setup. Elsewhere it lives in Settings > Audio and would just be a
  // second place to change the same thing.
  explicit LobbyConfigWidget(bool show_microphone, QWidget* parent = nullptr);

  // Nickname present, and a parseable address if joining rather than hosting.
  bool IsComplete() const;

  void Save() const;

signals:
  // Any edit at all. Distinct from completeness, because the port changing
  // never affects whether the form is valid but still has to be saved.
  void Changed();

private:
  void UpdateRoleVisibility();

  QLineEdit* m_nickname;
  QComboBox* m_microphone = nullptr;
  QRadioButton* m_host;
  QRadioButton* m_client;
  QSpinBox* m_port;
  QLineEdit* m_address;
  QGroupBox* m_host_box;
  QGroupBox* m_client_box;
};
