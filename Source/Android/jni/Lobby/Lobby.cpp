// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The Android frontend's view of the lobby.
//
// On desktop this lifecycle lives in MainWindow: the lobby link is brought up
// before boot so the console's first GETHOSTID already answers with a virtual
// address, and torn down after the console stops so the port is not held open
// while sitting on the launcher screen. Android has no MainWindow, so the same
// sequence is exposed here for the Kotlin side to drive at the same two moments.
//
// Everything below is a thin pass-through. The ordering rules are the part that
// matters and they are duplicated deliberately rather than shared: Start() and
// Stop() here are the whole contract, and a frontend that calls them in the
// wrong order gets a console with no network rather than an obvious failure.

#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <jni.h>

#include "Common/CommonTypes.h"

#include "Core/Config/MainSettings.h"
#include "Core/Lobby/LanModule.h"
#include "Core/Lobby/LobbyNet.h"
#include "Core/Lobby/NetTrace.h"
#include "Core/Lobby/VirtualNet.h"
#include "Core/Lobby/Voice/VoiceChat.h"
#include "Core/Lobby/Voice/VoiceSettings.h"

#include "jni/AndroidCommon/AndroidCommon.h"

// Must match StartResult in Lobby.kt.
namespace
{
constexpr jint RESULT_OK = 0;
// Set to join, but no host address was configured. Refused rather than booted:
// joining without one leaves the console broadcasting into nothing.
constexpr jint RESULT_NOT_CONFIGURED = 1;
// The link itself could not be created - a port already in use, usually.
constexpr jint RESULT_START_FAILED = 2;
// Started, but the host never answered the handshake.
constexpr jint RESULT_NO_REPLY = 3;

// The voice panel's peer list, read as one snapshot rather than a call per
// column. Peers come and go on the lobby thread, so six independent calls could
// each see a different set and hand back arrays of different lengths for what is
// meant to be one table.
std::mutex s_snapshot_mutex;
std::vector<Lobby::Voice::PeerInfo> s_snapshot;

// Pulls one field out of the snapshot into a flat array, under the lock.
template <typename T, typename F>
std::vector<T> MapSnapshot(F&& field)
{
  std::lock_guard lock(s_snapshot_mutex);
  std::vector<T> out;
  out.reserve(s_snapshot.size());
  for (const Lobby::Voice::PeerInfo& peer : s_snapshot)
    out.push_back(field(peer));
  return out;
}
}  // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeStart(JNIEnv*, jclass)
{
  // Already up, because a boot was queued behind a console that was still
  // stopping and this is the second time through. The virtual network is still
  // (re)initialised: it is torn down with each console, the link is not.
  if (Lobby::IsActive())
  {
    IOS::HLE::VirtualNet::Initialize();
    return RESULT_OK;
  }

  const bool is_host = Config::Get(Config::MAIN_LOBBY_IS_HOST);
  const std::string host_address = Config::Get(Config::MAIN_LOBBY_HOST_ADDRESS);

  if (!is_host && host_address.empty())
    return RESULT_NOT_CONFIGURED;

  if (!Lobby::Start(is_host ? Lobby::Role::Host : Lobby::Role::Client, host_address,
                    static_cast<u16>(Config::Get(Config::MAIN_LOBBY_PORT)),
                    Config::Get(Config::MAIN_LOBBY_NICKNAME)))
  {
    return RESULT_START_FAILED;
  }

  // A client has no address until the host gives it one, and the console asks
  // for its address within milliseconds of booting. Wait for the handshake
  // rather than letting it start with a placeholder. Blocking is fine here
  // because this is called off the UI thread; see Lobby.kt.
  if (!is_host && !Lobby::WaitForAddress(8000))
  {
    Lobby::Voice::Stop();
    IOS::HLE::VirtualNet::Shutdown();
    Lobby::Stop();
    return RESULT_NO_REPLY;
  }

  // Both roles have an address by this point - the host from the moment it
  // started listening, a client from the handshake above - so the virtual
  // network can describe an interface. It has to be up before the console opens
  // its first socket, which happens moments after boot.
  IOS::HLE::VirtualNet::Initialize();

  // Voice rides the same link but is between Dolphin instances rather than
  // between consoles, so it does not care whether a game has booted yet.
  Lobby::Voice::Start();
  return RESULT_OK;
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeStop(JNIEnv*, jclass)
{
  // The stack goes first: it detaches its handler, so no frame can arrive while
  // the transport underneath is being taken apart.
  Lobby::Voice::Stop();
  IOS::HLE::VirtualNet::Shutdown();
  Lobby::Stop();
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeIsActive(JNIEnv*, jclass)
{
  return static_cast<jboolean>(Lobby::IsActive());
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeIsConnected(JNIEnv*, jclass)
{
  return static_cast<jboolean>(Lobby::GetStatus() == Lobby::Status::Connected);
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeGetStatusText(JNIEnv* env, jclass)
{
  return ToJString(env, Lobby::GetStatusText());
}

// Empty until this machine has been given an address.
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeGetLocalAddress(JNIEnv* env, jclass)
{
  const u32 ip = Lobby::GetVirtualIP();
  return ToJString(env, ip == 0 ? std::string{} : Lobby::Trace::FormatIP(ip));
}

// The three peer arrays below are parallel and always the same length: entry i
// of each describes one lobby member, this console included, ordered by address.
JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeGetPeerAddresses(JNIEnv* env, jclass)
{
  const std::vector<Lobby::Peer> peers = Lobby::GetPeers();
  std::vector<std::string> addresses;
  addresses.reserve(peers.size());
  for (const Lobby::Peer& peer : peers)
    addresses.push_back(Lobby::Trace::FormatIP(peer.ip));
  return SpanToJStringArray(env, std::span<const std::string>(addresses));
}

JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeGetPeerNames(JNIEnv* env, jclass)
{
  const std::vector<Lobby::Peer> peers = Lobby::GetPeers();
  std::vector<std::string> names;
  names.reserve(peers.size());
  for (const Lobby::Peer& peer : peers)
    names.push_back(peer.name);
  return SpanToJStringArray(env, std::span<const std::string>(names));
}

// False for the entry describing this console itself.
JNIEXPORT jbooleanArray JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeGetPeerIsRemote(JNIEnv* env, jclass)
{
  const std::vector<Lobby::Peer> peers = Lobby::GetPeers();
  std::vector<jboolean> remote;
  remote.reserve(peers.size());
  for (const Lobby::Peer& peer : peers)
    remote.push_back(static_cast<jboolean>(peer.is_remote));

  const auto size = static_cast<jsize>(remote.size());
  jbooleanArray result = env->NewBooleanArray(size);
  env->SetBooleanArrayRegion(result, 0, size, remote.data());
  return result;
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeReconnect(JNIEnv*, jclass)
{
  return static_cast<jboolean>(Lobby::Reconnect());
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeCanReconnect(JNIEnv*, jclass)
{
  return static_cast<jboolean>(Lobby::CanReconnect());
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeIsVoiceRunning(JNIEnv*, jclass)
{
  return static_cast<jboolean>(Lobby::Voice::IsRunning());
}

// False when the microphone could not be opened, which on Android is what a
// missing RECORD_AUDIO grant looks like from here. Voice still runs - the
// device can hear everyone else - so this is worth surfacing rather than
// treating as a failure to start.
JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeIsCaptureWorking(JNIEnv*, jclass)
{
  return static_cast<jboolean>(Lobby::Voice::IsCaptureWorking());
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeGetVoiceStatusText(JNIEnv* env, jclass)
{
  return ToJString(env, Lobby::Voice::GetStatusText());
}

// Mute and deafen go through VoiceSettings rather than Config directly: it keeps
// a live copy the audio threads read per frame, and writing only the ini would
// leave that copy stale until the next Load().
JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeSetVoiceMuted(JNIEnv*, jclass,
                                                                       jboolean muted)
{
  Lobby::Voice::SetMuted(muted == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeIsVoiceMuted(JNIEnv*, jclass)
{
  return static_cast<jboolean>(Lobby::Voice::Get().muted);
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeSetVoiceDeafened(JNIEnv*, jclass,
                                                                          jboolean deafened)
{
  Lobby::Voice::SetDeafened(deafened == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeIsVoiceDeafened(JNIEnv*, jclass)
{
  return static_cast<jboolean>(Lobby::Voice::Get().deafened);
}

// The microphone and output pickers cannot be offered on Android: cubeb's OpenSL
// backend does not implement enumerate_devices, so there is nothing to list and
// a stored device id would never match. Both fall back to the system default,
// which is what phones give you anyway. Deliberately not bridged.

// Takes the snapshot the accessors below read from. Returns how many peers it
// holds, which is the length every one of them will return until this is called
// again.
JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeVoicePeerSnapshot(JNIEnv*, jclass)
{
  std::vector<Lobby::Voice::PeerInfo> peers = Lobby::Voice::GetPeers();
  std::lock_guard lock(s_snapshot_mutex);
  s_snapshot = std::move(peers);
  return static_cast<jint>(s_snapshot.size());
}

// "nickname (licence)", or just the nickname until the licence is known.
JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeVoicePeerDisplays(JNIEnv* env, jclass)
{
  const auto values = MapSnapshot<std::string>([](const auto& p) { return p.display; });
  return SpanToJStringArray(env, std::span<const std::string>(values));
}

// The key volumes are stored against, not for display.
JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeVoicePeerNicknames(JNIEnv* env, jclass)
{
  const auto values = MapSnapshot<std::string>([](const auto& p) { return p.nickname; });
  return SpanToJStringArray(env, std::span<const std::string>(values));
}

// 0-100. Meaningless for the local entry.
JNIEXPORT jintArray JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeVoicePeerVolumes(JNIEnv* env, jclass)
{
  const auto values = MapSnapshot<jint>([](const auto& p) { return static_cast<jint>(p.volume); });
  const auto size = static_cast<jsize>(values.size());
  jintArray result = env->NewIntArray(size);
  env->SetIntArrayRegion(result, 0, size, values.data());
  return result;
}

// 0-1, for the meter beside each name.
JNIEXPORT jfloatArray JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeVoicePeerLevels(JNIEnv* env, jclass)
{
  const auto values = MapSnapshot<jfloat>([](const auto& p) { return static_cast<jfloat>(p.level); });
  const auto size = static_cast<jsize>(values.size());
  jfloatArray result = env->NewFloatArray(size);
  env->SetFloatArrayRegion(result, 0, size, values.data());
  return result;
}

// Bit 0: this console. Bit 1: talking. Bit 2: proximity is being applied.
// Packed rather than three more arrays - they are only ever read together, and
// each extra accessor is another chance for the lengths to drift apart.
JNIEXPORT jintArray JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeVoicePeerFlags(JNIEnv* env, jclass)
{
  const auto values = MapSnapshot<jint>([](const auto& p) {
    return static_cast<jint>((p.is_local ? 1 : 0) | (p.talking ? 2 : 0) | (p.proximity ? 4 : 0));
  });
  const auto size = static_cast<jsize>(values.size());
  jintArray result = env->NewIntArray(size);
  env->SetIntArrayRegion(result, 0, size, values.data());
  return result;
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeSetPeerVolume(JNIEnv* env, jclass,
                                                                       jstring nickname,
                                                                       jint percent)
{
  Lobby::Voice::SetPeerVolume(GetJString(env, nickname), percent);
}

// Master volume and microphone gain, 0-200 percent. These go through
// VoiceSettings for the same reason mute and deafen do - it holds the copy the
// audio threads actually read - which is also what makes them safe to change
// from here while a race is running.
JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeSetMasterVolume(JNIEnv*, jclass,
                                                                         jint percent)
{
  Lobby::Voice::SetMasterVolume(percent);
}

JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeGetMasterVolume(JNIEnv*, jclass)
{
  return static_cast<jint>(Lobby::Voice::Get().master_volume);
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeSetMicGain(JNIEnv*, jclass, jint percent)
{
  Lobby::Voice::SetMicGain(percent);
}

JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeGetMicGain(JNIEnv*, jclass)
{
  return static_cast<jint>(Lobby::Voice::Get().mic_gain);
}

// The noise gate, in dBFS. How loud the microphone has to be before anything is
// sent at all - the control that stops a phone broadcasting its own speaker.
JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeSetGateDb(JNIEnv*, jclass, jfloat db)
{
  Lobby::Voice::SetGateDb(db);
}

JNIEXPORT jfloat JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeGetGateDb(JNIEnv*, jclass)
{
  return static_cast<jfloat>(Lobby::Voice::Get().gate_db);
}

JNIEXPORT jfloat JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeGetMinGateDb(JNIEnv*, jclass)
{
  return static_cast<jfloat>(Lobby::Voice::MIN_GATE_DB);
}

JNIEXPORT jfloat JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_Lobby_nativeGetMaxGateDb(JNIEnv*, jclass)
{
  return static_cast<jfloat>(Lobby::Voice::MAX_GATE_DB);
}

// --------------------------------------------------------------------------
// The LAN Play Module.
// --------------------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_LanModule_nativeIsInstalled(JNIEnv*, jclass)
{
  return static_cast<jboolean>(Lobby::LanModule::IsInstalled());
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_LanModule_nativeGetBrainslugPath(JNIEnv* env, jclass)
{
  return ToJString(env, Lobby::LanModule::BrainslugPath());
}

// Blocking: downloads, unpacks and writes the card image. Returns an empty
// string on success, or a message worth showing. `callback` is a Kotlin
// LanModule.Progress, invoked from this thread, and returning false from it
// gives up.
JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_lobby_LanModule_nativeDownloadAndInstall(JNIEnv* env,
                                                                                jclass,
                                                                                jobject callback)
{
  jclass callback_class = env->GetObjectClass(callback);
  jmethodID on_progress =
      env->GetMethodID(callback_class, "onProgress", "(Ljava/lang/String;JJ)Z");

  const std::string error = Lobby::LanModule::DownloadAndInstall(
      [&](const std::string& stage, s64 current, s64 total) {
        jstring jstage = ToJString(env, stage);
        const jboolean keep_going =
            env->CallBooleanMethod(callback, on_progress, jstage,
                                   static_cast<jlong>(current), static_cast<jlong>(total));
        env->DeleteLocalRef(jstage);
        return keep_going == JNI_TRUE;
      });

  return ToJString(env, error);
}

}  // extern "C"
