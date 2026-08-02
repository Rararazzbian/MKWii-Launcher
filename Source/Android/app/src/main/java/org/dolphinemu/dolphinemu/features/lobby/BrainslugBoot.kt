// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.lobby

import androidx.fragment.app.FragmentActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.activities.EmulationActivity
import org.dolphinemu.dolphinemu.features.settings.model.NativeConfig
import org.dolphinemu.dolphinemu.features.settings.model.Settings
import org.dolphinemu.dolphinemu.features.settings.model.StringSetting
import org.dolphinemu.dolphinemu.model.GameFile

/**
 * Boots the game the way this build is meant to boot it.
 *
 * Not by starting the disc. Booting the disc directly starts plain Mario Kart
 * Wii, whose multiplayer looks for Nintendo's servers - which are gone. Instead
 * Brainslug is booted off the emulated SD card; it applies the LAN Play Module
 * and then starts the game itself.
 *
 * Brainslug finds the game through Dolphin's default ISO setting, which is the
 * only reason it has anything to start, so that is written here immediately
 * before the boot rather than left to whatever set it last.
 */
object BrainslugBoot {
    /** The one disc the LAN Play Module patches. */
    private const val PATCHED_GAME_ID = "RMCE01"

    fun play(activity: FragmentActivity, game: GameFile?) {
        if (game == null) return

        // Homebrew, and anything else that is not the patched game, boots
        // directly. There is nothing for Brainslug to apply to it. It still gets
        // the lobby and the virtual network, because those are brought up around
        // every boot rather than by this path.
        if (game.getGameId() != PATCHED_GAME_ID) {
            EmulationActivity.launch(activity, arrayOf(game.getPath()), false)
            return
        }

        if (!LanModule.isInstalled) {
            // Booting anyway is allowed. The game runs, its multiplayer just
            // cannot reach anybody - and saying so is more use than a refusal.
            MaterialAlertDialogBuilder(activity)
                .setTitle(R.string.lan_module_missing_title)
                .setMessage(R.string.lan_module_missing)
                .setPositiveButton(R.string.lan_module_boot_anyway) { _, _ ->
                    EmulationActivity.launch(activity, arrayOf(game.getPath()), false)
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
            return
        }

        // Written to the base layer so it outlives this launch, the same as the
        // desktop wizard does. Brainslug reads it through Dolphin's own boot
        // path, not through anything of ours, so it has to be a real setting
        // rather than something passed along with the launch.
        Settings().use { settings ->
            settings.loadSettings()
            StringSetting.MAIN_DEFAULT_ISO.setString(settings, game.getPath())
            settings.saveSettings()
        }

        EmulationActivity.launch(activity, arrayOf(LanModule.brainslugPath), false)
    }
}
