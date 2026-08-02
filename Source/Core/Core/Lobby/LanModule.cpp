// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Lobby/LanModule.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include "Common/CommonPaths.h"
#include "Common/FatFsUtil.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/IOFile.h"
#include "Common/MinizipUtil.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"

namespace Lobby::LanModule
{
namespace
{
constexpr char PATCH_URL[] = "https://www.chadsoft.co.uk/downloads/LAN_MKW_v0.9.zip";

// The two directories the payload ships, relative to the SD card root.
constexpr std::string_view PATCH_DIRS[] = {"apps", "bslug"};

// Maps a path inside the zip to a path relative to the SD card root, or nullopt
// if it is not part of the payload.
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

std::string BrainslugPath()
{
  // Forward slashes rather than DIR_SEP: this is a path into the emulated card's
  // host folder, and every platform this runs on accepts them.
  return File::GetUserPath(D_WIISDCARDSYNCFOLDER_IDX) + "apps/brainslug/boot.dol";
}

bool IsInstalled()
{
  return File::Exists(BrainslugPath());
}

std::string DownloadAndInstall(const ProgressCallback& progress)
{
  bool cancelled = false;

  // curl's connect and stall timeouts, not a cap on the whole transfer, so a
  // slow but progressing download is not killed part way through.
  Common::HttpRequest request{std::chrono::seconds{15},
                              [&](s64 dltotal, s64 dlnow, s64, s64) {
                                if (!progress("download", dlnow, dltotal))
                                {
                                  cancelled = true;
                                  return false;
                                }
                                return true;
                              }};
  request.FollowRedirects();

  Common::HttpRequest::Response response = request.Get(PATCH_URL);
  if (cancelled)
    return "Cancelled.";
  if (!response)
    return "Could not download the LAN Play Module. Check your Internet connection.";

  void* zip_reader = mz_zip_reader_create();
  if (!zip_reader)
    return "Could not start reading the archive.";
  Common::ScopeGuard reader_guard{[&] { mz_zip_reader_delete(&zip_reader); }};

  // Read straight out of the downloaded bytes; no temporary file to clean up.
  if (mz_zip_reader_open_buffer(zip_reader, response->data(), static_cast<s32>(response->size()),
                                0) != MZ_OK)
  {
    return "The downloaded file is not a valid zip archive.";
  }

  if (mz_zip_reader_goto_first_entry(zip_reader) != MZ_OK)
    return "The archive is empty.";

  const std::string sd_root = File::GetUserPath(D_WIISDCARDSYNCFOLDER_IDX);
  int extracted = 0;

  do
  {
    if (!progress("extract", extracted, 0))
      return "Cancelled.";

    mz_zip_file* info = nullptr;
    if (mz_zip_reader_entry_get_info(zip_reader, &info) != MZ_OK)
      return "Could not read the archive.";

    const std::string name = info->filename;
    if (name.empty() || name.back() == '/')
      continue;  // directory entry

    const std::optional<std::string> relative = MapZipEntry(name);
    if (!relative)
      continue;

    const std::string target = sd_root + *relative;
    std::string dir;
    if (!SplitPath(target, &dir, nullptr, nullptr) || !File::CreateFullPath(dir))
      return "Could not create " + dir;

    const auto size = static_cast<std::size_t>(info->uncompressed_size);
    std::vector<u8> data(size);
    if (size != 0 && !Common::ReadFileFromZip(zip_reader, data.data(), size))
      return "Could not extract " + name;

    File::IOFile out(target, "wb");
    if (!out || (size != 0 && !out.WriteBytes(data.data(), size)))
      return "Could not write " + target;

    ++extracted;
  } while (mz_zip_reader_goto_next_entry(zip_reader) == MZ_OK);

  if (extracted == 0)
    return "The archive did not contain the expected apps and bslug folders.";

  // Unpacking onto the host folder is not enough: the console reads a card
  // image, and until the folder is written into it there is nothing to load.
  if (!progress("sync", 0, 0))
    return "Cancelled.";
  if (!Common::SyncSDFolderToSDImage([&] { return !progress("sync", 0, 0); }, false))
    return "Could not write the SD card image.";

  return "";
}
}  // namespace Lobby::LanModule
