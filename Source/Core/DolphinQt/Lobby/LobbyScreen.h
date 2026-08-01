// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

class LobbyConfigWidget;

// The launcher's home screen, shown where Dolphin would normally put the game
// list. There is only ever one game here, so a browsable list is not useful;
// what matters before starting is who you are, whether you host, and how the
// connection is made.
class LobbyScreen final : public QWidget
{
  Q_OBJECT

public:
  explicit LobbyScreen(QWidget* parent = nullptr);

  // False when the nickname is blank, or when joining without a usable
  // host address. Play refuses in that case.
  bool IsReadyToPlay() const;
  void Save() const;

private:
  LobbyConfigWidget* m_config;
};
