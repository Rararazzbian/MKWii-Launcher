# Porting the launcher to Android

Notes on what it would take to run this fork on Android, and what has already
been proven to work. This is an assessment, not a plan of record — nothing in
the tree has been changed for Android beyond what upstream already does.

## Summary

The native side is already portable. Everything this fork adds to `Source/Core/Core`
cross-compiles for `arm64-v8a` unmodified, and the full Android JNI library links.
The work that remains is almost entirely frontend: Android sets `ENABLE_QT 0`, so
the ~1,500 lines of Qt in `DolphinQt/Lobby/` and `DolphinQt/Setup/` are never built,
and nothing drives the lobby.

## What was verified

Cross-compiled against NDK `30.0.15729638` (r30-beta2), the version pinned in
`Source/Android/app/build.gradle.kts`:

```sh
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DANDROID_STL=c++_static \
  -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build-android libmain.so tests
```

Results:

* **Configure succeeds with no changes.** Gradle points `externalNativeBuild` at the
  root `CMakeLists.txt`, so the fork's `add_subdirectory(Externals/opus)` is picked
  up on Android automatically.
* **Opus builds for aarch64 in seconds**, with the NEON intrinsics
  (`celt/arm/*_neon_intr.c`, `silk/arm/*_neon.c`) compiled in. The fork already
  forces `OPUS_BUILD_SHARED_LIBRARY OFF`, which is what Android wants.
* **All 14 of the fork's new Core translation units compile clean** — no errors and
  no warnings — under `Lobby/`, `Lobby/Voice/` and `MemInspect/`.
* **`libmain.so` links** for `arm64-v8a`.
* **The fork's unit tests build.** `VirtualNetTest.cpp` and `VirtualNetPeerTest.cpp`
  compile into `Binaries/Tests/tests`. They were not *run*: the output is an Android
  binary needing `/system/bin/linker64`, so it needs a device or emulator.
* cubeb selects the **OpenSL ES** backend (`Externals/cubeb/CMakeLists.txt` force-disables
  AAudio), and `libOpenSLES.so` resolves from the NDK sysroot.

Supporting facts, from reading the tree:

* There is no platform-specific code in `Core/Lobby` or `Core/MemInspect` — no
  `#ifdef _WIN32`, no Windows headers, no x86 intrinsics.
* Neither has any frontend coupling: no `Host_*`, no Qt. The API is free functions
  (`Lobby::Start`, `VirtualNet::Initialize`, `Voice::Start`), which binds to JNI easily.
* Every third-party dependency the fork's Core code uses — enet, fmt, SFML-network,
  picojson, cubeb — is already built for Android upstream. Opus was the only new one.
* The unconditional-portable-mode change in `UICommon::SetUserDirectory` is already
  `#ifndef ANDROID` guarded, so Android keeps upstream's user-directory logic.

## What is missing

### 1. The frontend (the bulk of the work)

`CMakeLists.txt` sets `ENABLE_QT 0` for Android, so none of the fork's UI is built.
Every call that brings a lobby up lives in `DolphinQt/MainWindow.cpp` (around
`StartGame`), and would need a Kotlin + JNI equivalent:

* **Setup wizard** — `DolphinQt/Setup/`, 666 lines across four pages. The backend work
  is already portable: `DownloadsPage` uses `Common::HttpRequest` and minizip, and the
  JNI layer already exposes `installWAD`, `doOnlineUpdate`, `isSystemMenuInstalled` and
  `syncSdFolderToSdImage` in `Source/Android/jni/WiiUtils.cpp`. Only the UI is Qt.
* **Lobby home screen** — `LobbyScreen`, `LobbyConfigWidget`, replacing the game list.
* **Boot hook** — `Lobby::Start` → `VirtualNet::Initialize` → `Voice::Start` around
  emulation start, with teardown on stop, plus the Brainslug DOL / default-ISO boot path.
* **Settings** — 21 new `MAIN_LOBBY_*` / `MAIN_VOICE_*` entries to surface. These fit
  Android's existing `BooleanSetting` / `StringSetting` enum pattern directly.

### 2. The voice panel is a redesign, not a port

On desktop, `VoiceChatWindow` (571 lines) is a separate always-available window: mute,
deafen, device pickers, mic gain, master volume, noise gate, Doppler, hearing range,
bitrate, and a per-person row with a level meter painted inside each volume slider.
Android is fullscreen during emulation, so this has to become an overlay inside
`EmulationActivity`. There is no upstream equivalent to copy.

### 3. Android-specific problems the desktop build does not have

* **Disc image selection via SAF.** `Setup/GamePathPage` takes a filesystem path and
  checks the ID is `RMCE01`; Android hands you a `content://` URI and needs the
  content-URI-aware DiscIO openers.
* **The voice device pickers cannot work.** cubeb's OpenSL backend has
  `enumerate_devices = nullptr` (`cubeb_opensl.cpp`), so `MAIN_LOBBY_MICROPHONE` and
  `MAIN_VOICE_OUTPUT_DEVICE` have nothing to enumerate. This degrades safely rather than
  breaking — `CubebUtils::GetInputDeviceById` returns `nullptr`, logs a warning and falls
  back to the system default — but the pickers should be dropped from the Android UI.
  Capture itself *is* supported, and float32 is handled with an automatic int16 conversion,
  so the audio path works. `get_min_latency` is also null, and `AudioDevices::StartCapture`
  already falls back to 512 frames when it fails.
* **Doze and background execution** for the lobby and voice threads while the screen is off.
* **AP isolation** on phone hotspots will silently break the ENet link between instances.

Permissions are already in place: the manifest declares `RECORD_AUDIO` and `INTERNET`,
and `PermissionsHandler.kt` already performs the runtime microphone request for Wii Speak.

## Unknowns

* **Performance.** CTGP Revolution plus Brainslug, per-frame memory sampling and Opus
  encode/decode on top. Mario Kart Wii is one of the lighter Wii titles and the sampler is
  cheap, but this is only measurable on a device.
* **Behaviour beyond two peers.** Per the README, the desktop build itself has only been
  verified with two consoles, so a port inherits that.

The memory inspector needs no work — its addresses are guest memory offsets, independent
of the host architecture.
