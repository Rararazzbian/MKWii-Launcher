// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.lobby

/**
 * The lobby link the emulated console's virtual network runs over.
 *
 * Start it before booting and stop it after the console has stopped. The order
 * matters: the console asks for its address within milliseconds of booting, and
 * starting the lobby afterwards would leave it answering with a placeholder for
 * the rest of the session.
 *
 * [start] blocks - a client waits up to eight seconds for the host to answer the
 * handshake - so call it off the UI thread.
 *
 * Configuration (nickname, host or join, host address, port) is read from
 * Dolphin.ini on each [start], so change it through the ordinary settings path
 * before calling rather than passing it here.
 */
object Lobby {
    enum class StartResult {
        /** The link is up. For a client, an address has been assigned. */
        OK,

        /** Set to join, but no host address is configured. */
        NOT_CONFIGURED,

        /** The link could not be created - usually the port is already in use. */
        START_FAILED,

        /** Started, but the host never answered. */
        NO_REPLY;

        companion object {
            // Must match the RESULT_ constants in jni/Lobby/Lobby.cpp.
            fun fromNative(value: Int): StartResult = entries.getOrElse(value) { START_FAILED }
        }
    }

    /** One member of the lobby. [isRemote] is false for this device's own entry. */
    data class Peer(val address: String, val name: String, val isRemote: Boolean)

    fun start(): StartResult = StartResult.fromNative(nativeStart())

    fun stop() = nativeStop()

    val isActive: Boolean get() = nativeIsActive()

    val isConnected: Boolean get() = nativeIsConnected()

    /** Human-readable, for a status line or an error dialog. */
    val statusText: String get() = nativeGetStatusText()

    /** This device's address on the lobby's private range, or empty until assigned. */
    val localAddress: String get() = nativeGetLocalAddress()

    /** Everyone in the lobby, this device included, ordered by address. */
    val peers: List<Peer>
        get() {
            // Three parallel arrays rather than one array of objects: it keeps the
            // native side from having to construct Kotlin objects through JNI.
            val addresses = nativeGetPeerAddresses()
            val names = nativeGetPeerNames()
            val remote = nativeGetPeerIsRemote()
            return addresses.indices.map { Peer(addresses[it], names[it], remote[it]) }
        }

    /**
     * Drops the connection and dials the host again, keeping the console running.
     * Returns false when there is nothing to reconnect to.
     */
    fun reconnect(): Boolean = nativeReconnect()

    /** Whether [reconnect] would do anything, for greying out a menu entry. */
    val canReconnect: Boolean get() = nativeCanReconnect()

    val isVoiceRunning: Boolean get() = nativeIsVoiceRunning()

    /**
     * False when the microphone could not be opened - on Android, usually a
     * missing RECORD_AUDIO grant. Voice still runs and this device can still
     * hear everyone else, so request the permission before [start] and surface
     * this rather than treating it as a failure.
     */
    val isVoiceCaptureWorking: Boolean get() = nativeIsCaptureWorking()

    val voiceStatusText: String get() = nativeGetVoiceStatusText()

    /** Stop sending. */
    var isVoiceMuted: Boolean
        get() = nativeIsVoiceMuted()
        set(value) = nativeSetVoiceMuted(value)

    /** Stop hearing, and stop sending with it. */
    var isVoiceDeafened: Boolean
        get() = nativeIsVoiceDeafened()
        set(value) = nativeSetVoiceDeafened(value)

    @JvmStatic
    private external fun nativeStart(): Int

    @JvmStatic
    private external fun nativeStop()

    @JvmStatic
    private external fun nativeIsActive(): Boolean

    @JvmStatic
    private external fun nativeIsConnected(): Boolean

    @JvmStatic
    private external fun nativeGetStatusText(): String

    @JvmStatic
    private external fun nativeGetLocalAddress(): String

    @JvmStatic
    private external fun nativeGetPeerAddresses(): Array<String>

    @JvmStatic
    private external fun nativeGetPeerNames(): Array<String>

    @JvmStatic
    private external fun nativeGetPeerIsRemote(): BooleanArray

    @JvmStatic
    private external fun nativeReconnect(): Boolean

    @JvmStatic
    private external fun nativeCanReconnect(): Boolean

    @JvmStatic
    private external fun nativeIsVoiceRunning(): Boolean

    @JvmStatic
    private external fun nativeIsCaptureWorking(): Boolean

    @JvmStatic
    private external fun nativeGetVoiceStatusText(): String

    @JvmStatic
    private external fun nativeSetVoiceMuted(muted: Boolean)

    @JvmStatic
    private external fun nativeIsVoiceMuted(): Boolean

    @JvmStatic
    private external fun nativeSetVoiceDeafened(deafened: Boolean)

    @JvmStatic
    private external fun nativeIsVoiceDeafened(): Boolean
}
