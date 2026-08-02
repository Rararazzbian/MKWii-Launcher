// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.lobby

import androidx.annotation.Keep

/**
 * Chadsoft's LAN Play Module, and the Brainslug loader that applies it.
 *
 * Mario Kart Wii's multiplayer talks to Nintendo's servers, which are gone. The
 * module patches it to play over a LAN instead, which is what makes the lobby's
 * virtual network worth having. It is not shipped with this app - it is
 * downloaded on demand and unpacked onto the emulated SD card.
 */
object LanModule {
    /**
     * Reports how the install is going, and decides whether it continues.
     *
     * Called from whatever thread [downloadAndInstall] was called on, many times
     * a second during the download. Return false to give up.
     *
     * @param stage    "download", "extract" or "sync".
     * @param current  Bytes for "download", files for "extract".
     * @param total    Zero when it is not known, which it often is.
     */
    @Keep
    fun interface Progress {
        fun onProgress(stage: String, current: Long, total: Long): Boolean
    }

    /**
     * Whether there is something to boot. Not a checksum - it does not tell you
     * the module is the version this was built against.
     */
    val isInstalled: Boolean get() = nativeIsInstalled()

    /**
     * The loader to boot instead of the disc.
     *
     * Brainslug applies the module and then starts the game itself, which it
     * finds through Dolphin's default ISO setting - so that has to be pointing at
     * the game before this is booted.
     */
    val brainslugPath: String get() = nativeGetBrainslugPath()

    /**
     * Downloads and installs. Blocks; call it off the UI thread.
     *
     * Returns an empty string when it worked, or a message worth showing. A
     * cancelled install returns a message too, because it did not finish and
     * silence would look like success.
     */
    fun downloadAndInstall(progress: Progress): String = nativeDownloadAndInstall(progress)

    @JvmStatic
    private external fun nativeIsInstalled(): Boolean

    @JvmStatic
    private external fun nativeGetBrainslugPath(): String

    @JvmStatic
    private external fun nativeDownloadAndInstall(progress: Progress): String
}
