// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Setup/DownloadsPage.h"

#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

// mz_zip_rw.h uses the stream callback typedefs without including the header
// that declares them, so mz_strm.h has to come first.
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include "Common/CommonTypes.h"
#include "Common/FatFsUtil.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/IOFile.h"
#include "Common/MinizipUtil.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"

#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/WiiUtils.h"

#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/ParallelProgressDialog.h"
#include "DolphinQt/WiiUpdate.h"

namespace
{
constexpr char PATCH_URL[] = "https://www.chadsoft.co.uk/downloads/LAN_MKW_v0.9.zip";

// The two directories the payload ships, relative to the SD card root.
constexpr std::string_view PATCH_DIRS[] = {"apps", "bslug"};

// Maps a path inside the zip to a path relative to the SD card root, or
// nullopt if it is not part of the payload.
//
// The archive nests these under a "release/" folder, but that is not worth
// hardcoding: matching on the "apps" and "bslug" components means a repackaged
// or differently nested archive still installs to the right place.
std::optional<std::string> MapZipEntry(const std::string& name)
{
  // Reject traversal outright rather than trying to normalise it. Nothing in a
  // legitimate payload needs "..", so anything that does is corrupt or hostile.
  if (name.find("..") != std::string::npos)
    return std::nullopt;

  for (const std::string_view dir : PATCH_DIRS)
  {
    const std::string prefix = std::string(dir) + "/";
    if (name.starts_with(prefix))
      return name;

    // Only match at a component boundary, so "myapps/" does not match "apps/".
    const std::size_t pos = name.find("/" + prefix);
    if (pos != std::string::npos)
      return name.substr(pos + 1);
  }
  return std::nullopt;
}
}  // namespace

DownloadsPage::DownloadsPage(QWidget* parent) : QWizardPage(parent)
{
  setTitle(tr("Downloads"));

  m_update_button = new NonDefaultQPushButton(tr("Download && Install"));
  m_update_status = new QLabel;
  m_patch_button = new NonDefaultQPushButton(tr("Download && Install"));
  m_patch_status = new QLabel;

  auto* const update_box = new QGroupBox(tr("Wii System Software (USA)"));
  auto* const update_layout = new QGridLayout(update_box);
  update_layout->addWidget(new QLabel(tr("Download and install the Wii system files.")), 0, 0, 1, 2);
  update_layout->addWidget(m_update_button, 1, 0);
  update_layout->addWidget(m_update_status, 1, 1);
  update_layout->setColumnStretch(1, 1);

  auto* const patch_box = new QGroupBox(tr("Proximity Chat Patch"));
  auto* const patch_layout = new QGridLayout(patch_box);
  patch_layout->addWidget(
      new QLabel(tr("Download and install Mario Kart Wii's LAN Play Module.")), 0, 0, 1, 2);
  patch_layout->addWidget(m_patch_button, 1, 0);
  patch_layout->addWidget(m_patch_status, 1, 1);
  patch_layout->setColumnStretch(1, 1);

  auto* const layout = new QVBoxLayout(this);
  layout->addWidget(update_box);
  layout->addWidget(patch_box);
  layout->addStretch();

  connect(m_update_button, &QPushButton::clicked, this, &DownloadsPage::InstallSystemUpdate);
  connect(m_patch_button, &QPushButton::clicked, this, &DownloadsPage::InstallProximityPatch);
}

void DownloadsPage::initializePage()
{
  RefreshStatus();
}

bool DownloadsPage::isComplete() const
{
  return IsSystemUpdateInstalled() && IsProximityPatchInstalled();
}

bool DownloadsPage::IsSystemUpdateInstalled()
{
  // The System Menu is what the wizard boots at the end and what an online
  // update installs, so its presence is the thing worth checking.
  return WiiUtils::IsTitleInstalled(Titles::SYSTEM_MENU);
}

bool DownloadsPage::IsProximityPatchInstalled()
{
  const std::string root = File::GetUserPath(D_WIISDCARDSYNCFOLDER_IDX);
  for (const std::string_view dir : PATCH_DIRS)
  {
    if (!File::IsDirectory(root + std::string(dir)))
      return false;
  }
  return true;
}

void DownloadsPage::RefreshStatus()
{
  // Re-checked rather than cached, so a user who installed either one outside
  // the wizard is not made to do it again.
  const bool update_done = IsSystemUpdateInstalled();
  m_update_status->setText(update_done ? tr("Installed") : tr("Not installed"));
  m_update_button->setText(update_done ? tr("Reinstall") : tr("Download && Install"));

  const bool patch_done = IsProximityPatchInstalled();
  m_patch_status->setText(patch_done ? tr("Installed") : tr("Not installed"));
  m_patch_button->setText(patch_done ? tr("Reinstall") : tr("Download && Install"));

  emit completeChanged();
}

void DownloadsPage::InstallSystemUpdate()
{
  // Reuses the same code path as Tools > Perform Online System Update, so every
  // failure case and the certificate extraction on success behave exactly as
  // they do elsewhere in Dolphin.
  WiiUpdate::PerformOnlineUpdate("USA", this);
  RefreshStatus();
}

void DownloadsPage::InstallProximityPatch()
{
  // Set before the worker starts: the config system is not thread safe, and
  // these have to be on for the emulated Wii to mount what we are about to
  // write. Without folder sync the image is rebuilt from a different source on
  // boot and the payload silently would not be there.
  Config::SetBase(Config::MAIN_WII_SD_CARD, true);
  Config::SetBase(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC, true);

  const std::string error = DownloadAndInstallPatch();
  if (!error.empty())
    ModalMessageBox::critical(this, tr("Installation failed"), QString::fromStdString(error));

  RefreshStatus();
}

std::string DownloadsPage::DownloadAndInstallPatch()
{
  ParallelProgressDialog progress(tr("Connecting..."), tr("Cancel"), 0, 100, this);
  progress.GetRaw()->setWindowTitle(tr("Proximity Chat Patch"));
  progress.GetRaw()->setWindowModality(Qt::WindowModal);
  progress.GetRaw()->setMinimumSize(400, 120);

  auto worker = std::async(std::launch::async, [&]() -> std::string {
    Common::ScopeGuard close_dialog{[&] { progress.Reset(); }};

    // This timeout is curl's connect timeout and its stall timeout, not a cap
    // on the whole transfer, so a slow but progressing download is not killed.
    Common::HttpRequest request{std::chrono::seconds{15}, [&](s64 dltotal, s64 dlnow, s64, s64) {
                                  if (dltotal > 0)
                                  {
                                    progress.SetRange(0, 100);
                                    progress.SetValue(static_cast<int>(dlnow * 100 / dltotal));
                                    progress.SetLabelText(tr("Downloading... %1 / %2 MiB")
                                                              .arg(dlnow / 1024 / 1024)
                                                              .arg(dltotal / 1024 / 1024));
                                  }
                                  return !progress.WasCanceled();
                                }};
    request.FollowRedirects();

    Common::HttpRequest::Response response = request.Get(PATCH_URL);
    if (progress.WasCanceled())
      return "";
    if (!response)
      return tr("Could not download the patch. Check your Internet connection.").toStdString();

    progress.SetRange(0, 0);  // switches the bar to a busy indicator
    progress.SetLabelText(tr("Extracting..."));

    void* zip_reader = mz_zip_reader_create();
    if (!zip_reader)
      return tr("Failed to create zip reader.").toStdString();
    Common::ScopeGuard reader_guard{[&] { mz_zip_reader_delete(&zip_reader); }};

    // Read straight out of the downloaded bytes; no temporary file to clean up.
    if (mz_zip_reader_open_buffer(zip_reader, response->data(),
                                  static_cast<s32>(response->size()), 0) != MZ_OK)
    {
      return tr("The downloaded file is not a valid zip archive.").toStdString();
    }

    if (mz_zip_reader_goto_first_entry(zip_reader) != MZ_OK)
      return tr("The archive is empty.").toStdString();

    const std::string sd_root = File::GetUserPath(D_WIISDCARDSYNCFOLDER_IDX);
    int extracted = 0;

    do
    {
      if (progress.WasCanceled())
        return "";

      mz_zip_file* info = nullptr;
      if (mz_zip_reader_entry_get_info(zip_reader, &info) != MZ_OK)
        return tr("Could not read the archive.").toStdString();

      const std::string name = info->filename;
      if (name.empty() || name.back() == '/')
        continue;  // directory entry

      const std::optional<std::string> relative = MapZipEntry(name);
      if (!relative)
        continue;

      const std::string target = sd_root + *relative;
      std::string dir;
      if (!SplitPath(target, &dir, nullptr, nullptr) || !File::CreateFullPath(dir))
        return tr("Could not create %1").arg(QString::fromStdString(dir)).toStdString();

      const auto size = static_cast<std::size_t>(info->uncompressed_size);
      std::vector<u8> data(size);
      if (size != 0 && !Common::ReadFileFromZip(zip_reader, data.data(), size))
        return tr("Could not extract %1").arg(QString::fromStdString(name)).toStdString();

      File::IOFile out(target, "wb");
      if (!out || (size != 0 && !out.WriteBytes(data.data(), size)))
        return tr("Could not write %1").arg(QString::fromStdString(target)).toStdString();

      ++extracted;
      progress.SetLabelText(tr("Extracting... (%1 files)").arg(extracted));
    } while (mz_zip_reader_goto_next_entry(zip_reader) == MZ_OK);

    if (extracted == 0)
      return tr("The archive did not contain the expected apps and bslug folders.").toStdString();

    progress.SetLabelText(tr("Writing SD card image..."));
    if (!Common::SyncSDFolderToSDImage([&] { return progress.WasCanceled(); }, false))
      return tr("Could not write the SD card image.").toStdString();

    return "";
  });

  progress.GetRaw()->exec();
  return worker.get();
}
