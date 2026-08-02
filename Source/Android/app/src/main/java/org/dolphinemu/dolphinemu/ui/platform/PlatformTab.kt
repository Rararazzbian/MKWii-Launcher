// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.ui.platform

import org.dolphinemu.dolphinemu.R

/**
 * Enum to represent platform tabs in the UI.
 *
 * Each platform tab corresponds to one or more platforms.
 */
enum class PlatformTab(private val value: Int, val headerName: Int, val idString: String) {
    GAMECUBE(0, R.string.platform_gamecube, "GameCube Games"),
    WII(1, R.string.platform_wii, "Wii Games"),
    WIIWARE(2, R.string.platform_wiiware, "WiiWare Games");

    fun toInt(): Int {
        return value
    }

    companion object {
        /**
         * Fork: the tabs this build actually shows.
         *
         * The Wii tab is narrowed to the one game the LAN Play Module patches, so
         * a GameCube tab could only ever be empty. Homebrew stays: it gets the
         * lobby and the virtual network like anything else, it just boots
         * directly rather than through Brainslug.
         *
         * The enum keeps all three because a game's platform is still one of
         * them and the TV launcher still walks the whole set - it is only the
         * pager that is narrowed.
         */
        val VISIBLE = listOf(WII, WIIWARE)

        fun fromInt(i: Int): PlatformTab {
            return values()[i]
        }

        /** The tab at a position in the pager, which only shows [VISIBLE]. */
        fun fromPosition(position: Int): PlatformTab {
            return VISIBLE[position]
        }

        /** Where a tab sits in the pager, or -1 if it is not shown. */
        fun positionOf(platformTab: PlatformTab): Int = VISIBLE.indexOf(platformTab)
    }
}
