// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.lobby

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.features.settings.model.BooleanSetting
import org.dolphinemu.dolphinemu.features.settings.model.StringSetting
import org.dolphinemu.dolphinemu.utils.Log

/**
 * Brings the lobby up around a running console.
 *
 * On desktop this lives in MainWindow, either side of the boot. Here it hangs off
 * the emulation thread for the same reason: the console asks for its address
 * within milliseconds of starting, so the link has to already be up, and
 * [Lobby.start] blocks for up to eight seconds waiting for a host to answer -
 * which is fine on that thread and would not be on the UI one.
 */
object LobbyBoot {
    /**
     * Starts the lobby if one is configured. Returns whether [stop] needs calling.
     *
     * Being unconfigured is not an error and does not stop the boot. This is still
     * Dolphin underneath and it can still run anything; someone who has not filled
     * in a nickname yet should get a game, not a refusal.
     *
     * A lobby that is configured but fails to come up does not stop the boot
     * either - the game is playable, just alone - but it says so, because
     * otherwise the only symptom is an online mode where nobody ever appears.
     */
    fun startIfConfigured(context: Context): Boolean {
        if (!isConfigured()) return false

        return when (Lobby.start()) {
            Lobby.StartResult.OK -> {
                Log.info("[Lobby] up at ${Lobby.localAddress}")
                true
            }

            Lobby.StartResult.NOT_CONFIGURED -> false

            Lobby.StartResult.START_FAILED -> {
                report(context, R.string.lobby_failed_to_start)
                false
            }

            Lobby.StartResult.NO_REPLY -> {
                // start() has already torn its own half down in this case.
                report(context, R.string.lobby_no_reply)
                false
            }
        }
    }

    /** Safe to call whether or not [startIfConfigured] returned true. */
    fun stop() = Lobby.stop()

    /**
     * Hosting needs only a nickname - the address is this device's own. Joining
     * needs somewhere to join.
     */
    private fun isConfigured(): Boolean {
        if (StringSetting.MAIN_LOBBY_NICKNAME.string.isEmpty()) return false
        if (BooleanSetting.MAIN_LOBBY_IS_HOST.boolean) return true
        return StringSetting.MAIN_LOBBY_HOST_ADDRESS.string.isNotEmpty()
    }

    private fun report(context: Context, messageId: Int) {
        val detail = Lobby.statusText
        Log.error("[Lobby] $detail")
        Handler(Looper.getMainLooper()).post {
            Toast.makeText(
                context,
                context.getString(messageId, detail),
                Toast.LENGTH_LONG
            ).show()
        }
    }
}
