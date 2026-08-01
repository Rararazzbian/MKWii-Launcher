// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Setup/GamePathPage.h"

#include <string>

#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "Core/Config/MainSettings.h"

#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"

#include "UICommon/GameFile.h"

namespace
{
// Mario Kart Wii, NTSC-U. R=disc, MC=Mario Kart, E=NTSC-U, 01=Nintendo.
constexpr char REQUIRED_GAME_ID[] = "RMCE01";
}  // namespace

GamePathPage::GamePathPage(QWidget* parent) : QWizardPage(parent)
{
  setTitle(tr("Game"));

  m_path = new QLineEdit;
  m_path->setPlaceholderText(tr("Path to your Mario Kart Wii disc image"));
  m_path->setText(QString::fromStdString(Config::Get(Config::MAIN_MKW_GAME_PATH)));

  auto* const browse = new NonDefaultQPushButton(tr("Browse..."));
  m_status = new QLabel;
  m_status->setWordWrap(true);

  auto* const box = new QGroupBox(tr("Mario Kart Wii"));
  auto* const box_layout = new QGridLayout(box);
  box_layout->addWidget(new QLabel(tr("Only the NTSC-U release (%1) is supported.")
                                       .arg(QString::fromLatin1(REQUIRED_GAME_ID))),
                        0, 0, 1, 2);
  box_layout->addWidget(m_path, 1, 0);
  box_layout->addWidget(browse, 1, 1);
  box_layout->addWidget(m_status, 2, 0, 1, 2);
  box_layout->setColumnStretch(0, 1);

  auto* const layout = new QVBoxLayout(this);
  layout->addWidget(box);
  layout->addStretch();

  connect(browse, &QPushButton::clicked, this, &GamePathPage::Browse);
  // Validation opens the disc, so it runs when the user finishes typing rather
  // than on every keystroke.
  connect(m_path, &QLineEdit::editingFinished, this, &GamePathPage::Validate);
  connect(m_path, &QLineEdit::textChanged, this, [this] {
    // Any edit invalidates the previous verdict until it is re-checked.
    m_valid = false;
    m_status->clear();
    emit completeChanged();
  });
}

void GamePathPage::initializePage()
{
  if (!m_path->text().isEmpty())
    Validate();
}

bool GamePathPage::isComplete() const
{
  return m_valid;
}

bool GamePathPage::validatePage()
{
  const std::string path = m_path->text().toStdString();
  Config::SetBase(Config::MAIN_MKW_GAME_PATH, path);
  // Also Dolphin's default ISO: the Brainslug loader that Play starts boots
  // whatever disc is configured there, so the two have to agree.
  Config::SetBase(Config::MAIN_DEFAULT_ISO, path);
  return true;
}

void GamePathPage::Browse()
{
  const QString path = DolphinFileDialog::getOpenFileName(
      this, tr("Select Mario Kart Wii"), m_path->text(),
      tr("Wii disc images (*.iso *.wbfs *.ciso *.gcz *.wia *.rvz);;All Files (*)"));

  if (path.isEmpty())
    return;

  m_path->setText(path);
  Validate();
}

void GamePathPage::Validate()
{
  m_valid = false;

  const QString path = m_path->text();
  if (path.isEmpty())
  {
    m_status->clear();
    emit completeChanged();
    return;
  }

  // Opening a disc image reads and decrypts headers, which is quick but not
  // instant on a spinning disk, so give the user a busy cursor rather than an
  // apparently frozen dialog.
  QGuiApplication::setOverrideCursor(Qt::WaitCursor);
  const UICommon::GameFile game{path.toStdString()};
  QGuiApplication::restoreOverrideCursor();

  if (!game.IsValid())
  {
    m_status->setText(tr("That file is not a game disc image Dolphin can read."));
  }
  else if (game.GetGameID() != REQUIRED_GAME_ID)
  {
    m_status->setText(tr("Wrong game: this disc is %1, but %2 is required.")
                          .arg(QString::fromStdString(game.GetGameID()))
                          .arg(QString::fromLatin1(REQUIRED_GAME_ID)));
  }
  else
  {
    m_valid = true;
    m_status->setText(tr("Mario Kart Wii (%1) verified.").arg(QString::fromLatin1(REQUIRED_GAME_ID)));
  }

  emit completeChanged();
}
