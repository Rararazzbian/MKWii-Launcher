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

    /**
     * The room code to give people, when hosting through a traversal server.
     *
     * Empty when hosting directly, when joining, and until the traversal server
     * has answered - which is also why hosting that way does not report itself
     * connected until there is a code to hand out.
     */
    val hostCode: String get() = nativeGetHostCode()

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

    /**
     * One person in the voice channel.
     *
     * @param display    "nickname (licence)", or just the nickname until their
     *                   licence is known. For showing.
     * @param nickname   What their volume is stored against. For [setPeerVolume].
     * @param volume     0-100. Meaningless for the local entry.
     * @param level      0-1, for a meter beside their name.
     * @param isLocal    True for this device's own entry.
     * @param isTalking  Whether they are speaking right now.
     * @param proximity  Whether distance is being applied to them right now, which
     *                   only happens while both of you are in an online race.
     */
    data class VoicePeer(
        val display: String,
        val nickname: String,
        val volume: Int,
        val level: Float,
        val isLocal: Boolean,
        val isTalking: Boolean,
        val proximity: Boolean
    )

    /**
     * Everyone in the voice channel, this device included.
     *
     * Taken as one snapshot rather than a call per column: peers come and go on
     * the lobby thread, so reading each column separately could produce a table
     * whose rows do not line up.
     */
    val voicePeers: List<VoicePeer>
        get() {
            val count = nativeVoicePeerSnapshot()
            if (count == 0) return emptyList()
            val displays = nativeVoicePeerDisplays()
            val nicknames = nativeVoicePeerNicknames()
            val volumes = nativeVoicePeerVolumes()
            val levels = nativeVoicePeerLevels()
            val flags = nativeVoicePeerFlags()
            return (0 until count).map {
                VoicePeer(
                    displays[it],
                    nicknames[it],
                    volumes[it],
                    levels[it],
                    flags[it] and 1 != 0,
                    flags[it] and 2 != 0,
                    flags[it] and 4 != 0
                )
            }
        }

    /** How loudly one person is heard, 0-100. Keyed by [VoicePeer.nickname]. */
    fun setPeerVolume(nickname: String, percent: Int) = nativeSetPeerVolume(nickname, percent)

    /** Percent, 0-200. 100 is unity. Safe to change mid-race. */
    var masterVolume: Int
        get() = nativeGetMasterVolume()
        set(value) = nativeSetMasterVolume(value)

    /** Percent, 0-200. 100 is unity. Safe to change mid-race. */
    var micGain: Int
        get() = nativeGetMicGain()
        set(value) = nativeSetMicGain(value)

    /**
     * The noise gate, in dBFS: how loud the microphone has to be before anything
     * is sent at all.
     *
     * This is what stops a phone broadcasting the game coming back out of its own
     * speaker. Higher means more has to be spoken before it opens. Safe to change
     * mid-race, which is the only practical way to tune it - the effect is on what
     * other people hear, so it has to be adjusted while they are listening.
     */
    var voiceGateDb: Float
        get() = nativeGetGateDb()
        set(value) = nativeSetGateDb(value)

    /**
     * Place each voice left or right by where that kart is, relative to which way
     * you are facing.
     *
     * Off by default on Android: the usual case there is a phone at arm's length
     * with its speakers a few centimetres apart, where panning is an unbalanced
     * mix rather than a direction. Worth turning on for headphones.
     */
    var isVoiceSpatial: Boolean
        get() = nativeIsSpatial()
        set(value) = nativeSetSpatial(value)

    val minVoiceGateDb: Float get() = nativeGetMinGateDb()
    val maxVoiceGateDb: Float get() = nativeGetMaxGateDb()

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
    private external fun nativeGetHostCode(): String

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

    @JvmStatic
    private external fun nativeVoicePeerSnapshot(): Int

    @JvmStatic
    private external fun nativeVoicePeerDisplays(): Array<String>

    @JvmStatic
    private external fun nativeVoicePeerNicknames(): Array<String>

    @JvmStatic
    private external fun nativeVoicePeerVolumes(): IntArray

    @JvmStatic
    private external fun nativeVoicePeerLevels(): FloatArray

    @JvmStatic
    private external fun nativeVoicePeerFlags(): IntArray

    @JvmStatic
    private external fun nativeSetPeerVolume(nickname: String, percent: Int)

    @JvmStatic
    private external fun nativeSetMasterVolume(percent: Int)

    @JvmStatic
    private external fun nativeGetMasterVolume(): Int

    @JvmStatic
    private external fun nativeSetMicGain(percent: Int)

    @JvmStatic
    private external fun nativeGetMicGain(): Int

    @JvmStatic
    private external fun nativeSetGateDb(db: Float)

    @JvmStatic
    private external fun nativeGetGateDb(): Float

    @JvmStatic
    private external fun nativeGetMinGateDb(): Float

    @JvmStatic
    private external fun nativeGetMaxGateDb(): Float

    @JvmStatic
    private external fun nativeSetSpatial(spatial: Boolean)

    @JvmStatic
    private external fun nativeIsSpatial(): Boolean
}
