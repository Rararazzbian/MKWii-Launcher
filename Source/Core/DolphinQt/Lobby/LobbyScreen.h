// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

class QLineEdit;
class QRadioButton;

// The launcher's home screen, shown where Dolphin would normally put the game
// list. There is only ever one game here, so a browsable list is not useful;
// what matters before starting is who you are and whether you host.
//
// Deliberately mirrors the first page of the setup wizard, so the two do not
// look like different applications.
class LobbyScreen final : public QWidget
{
  Q_OBJECT

public:
  explicit LobbyScreen(QWidget* parent = nullptr);

private:
  // Settings are written as they change: this screen has no OK button, so
  // there is no later point at which to save them.
  void SaveIdentity();

  QLineEdit* m_nickname;
  QRadioButton* m_host;
  QRadioButton* m_client;
};
