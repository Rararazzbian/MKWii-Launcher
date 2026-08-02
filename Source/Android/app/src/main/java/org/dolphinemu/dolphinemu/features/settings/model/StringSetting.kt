// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model

import org.dolphinemu.dolphinemu.NativeLibrary
import java.util.*

enum class StringSetting(
    private val file: String,
    private val section: String,
    private val key: String,
    private val defaultValue: String
) : AbstractStringSetting {
    // These entries have the same names and order as in C++, just for consistency.
    MAIN_DEFAULT_ISO(Settings.FILE_DOLPHIN, Settings.SECTION_INI_CORE, "DefaultISO", ""),
    MAIN_BBA_MAC(Settings.FILE_DOLPHIN, Settings.SECTION_INI_CORE, "BBA_MAC", ""),
    MAIN_BBA_XLINK_IP(Settings.FILE_DOLPHIN, Settings.SECTION_INI_CORE, "BBA_XLINK_IP", ""),

    // Schthack PSO Server - https://schtserv.com/
    MAIN_BBA_BUILTIN_DNS(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_CORE,
        "BBA_BUILTIN_DNS",
        "3.18.217.27"
    ),
    MAIN_BBA_TAPSERVER_DESTINATION(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_CORE,
        "BBA_TAPSERVER_DESTINATION",
        "/tmp/dolphin-tap"
    ),
    MAIN_MODEM_TAPSERVER_DESTINATION(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_CORE,
        "MODEM_TAPSERVER_DESTINATION",
        "/tmp/dolphin-modem-tap"
    ),
    MAIN_CUSTOM_RTC_VALUE(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_CORE,
        "CustomRTCValue",
        "0x386d4380"
    ),
    MAIN_GFX_BACKEND(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_CORE,
        "GFXBackend",
        NativeLibrary.GetDefaultGraphicsBackendConfigName()
    ),
    MAIN_DUMP_PATH(Settings.FILE_DOLPHIN, Settings.SECTION_INI_GENERAL, "DumpPath", ""),
    MAIN_LOAD_PATH(Settings.FILE_DOLPHIN, Settings.SECTION_INI_GENERAL, "LoadPath", ""),
    MAIN_RESOURCEPACK_PATH(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_GENERAL,
        "ResourcePackPath",
        ""
    ),
    MAIN_FS_PATH(Settings.FILE_DOLPHIN, Settings.SECTION_INI_GENERAL, "NANDRootPath", ""),
    MAIN_WII_SD_CARD_IMAGE_PATH(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_GENERAL,
        "WiiSDCardPath",
        ""
    ),
    MAIN_WII_SD_CARD_SYNC_FOLDER_PATH(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_GENERAL,
        "WiiSDCardSyncFolder",
        ""
    ),
    MAIN_WFS_PATH(Settings.FILE_DOLPHIN, Settings.SECTION_INI_GENERAL, "WFSPath", ""),
    MAIN_GBA_BIOS_PATH(Settings.FILE_DOLPHIN, Settings.SECTION_INI_GBA, "BIOS", ""),
    MAIN_GB_PLAYER_ROM(Settings.FILE_DOLPHIN, Settings.SECTION_INI_GBA, "GBPlayerRom", ""),
    MAIN_GBA_SAVES_PATH(Settings.FILE_DOLPHIN, Settings.SECTION_INI_GBA, "SavesPath", ""),
    MAIN_TRIFORCE_IP_REDIRECTIONS(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_CORE,
        "TriforceIPRedirections",
        "0.0.0.0/0=127.0.0.1"
    ),
    GFX_ENHANCE_POST_SHADER(
        Settings.FILE_GFX,
        Settings.SECTION_GFX_ENHANCEMENTS,
        "PostProcessingShader",
        ""
    ),
    GFX_DRIVER_LIB_NAME(
        Settings.FILE_GFX,
        Settings.SECTION_GFX_SETTINGS,
        "DriverLibName",
        ""
    ),
    ACHIEVEMENTS_USERNAME(
        Settings.FILE_ACHIEVEMENTS,
        Settings.SECTION_ACHIEVEMENTS,
        "Username",
        ""
    ),
    ACHIEVEMENTS_API_TOKEN(
        Settings.FILE_ACHIEVEMENTS,
        Settings.SECTION_ACHIEVEMENTS,
        "ApiToken",
        ""
    ),
    NETPLAY_TRAVERSAL_CHOICE(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_NETPLAY,
        "TraversalChoice",
        "direct"
    ),
    NETPLAY_HOST_CODE(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_NETPLAY,
        "HostCode",
        ""
    ),
    NETPLAY_ADDRESS(Settings.FILE_DOLPHIN, Settings.SECTION_INI_NETPLAY, "Address", "127.0.0.1"),
    NETPLAY_NICKNAME(Settings.FILE_DOLPHIN, Settings.SECTION_INI_NETPLAY, "Nickname", "Player"),
    NETPLAY_GAME(Settings.FILE_DOLPHIN, Settings.SECTION_INI_NETPLAY, "Game", ""),
    NETPLAY_NETWORK_MODE(Settings.FILE_DOLPHIN, Settings.SECTION_INI_NETPLAY, "NetworkMode", "fixeddelay"),

    // Fork: the MKWii launcher's lobby.
    //
    // The microphone and voice output device settings are deliberately absent.
    // cubeb's OpenSL backend - the one Android uses - does not implement
    // enumerate_devices, so there is nothing to populate a picker with and a
    // stored device id would never match anything. Both fall back to the system
    // default, which is what a phone offers anyway.
    MAIN_LOBBY_NICKNAME(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_MKW_LAUNCHER,
        "Nickname",
        ""
    ),
    MAIN_MKW_GAME_PATH(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_MKW_LAUNCHER,
        "GamePath",
        ""
    ),
    // "host" or "host:port". Only consulted when joining rather than hosting.
    MAIN_LOBBY_HOST_ADDRESS(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_MKW_LAUNCHER,
        "LobbyHost",
        ""
    );

    override val isOverridden: Boolean
        get() = NativeConfig.isOverridden(file, section, key)

    override val isRuntimeEditable: Boolean
        get() {
            for (setting in NOT_RUNTIME_EDITABLE) {
                if (setting == this) return false
            }
            return NativeConfig.isSettingSaveable(file, section, key)
        }

    override fun delete(settings: Settings): Boolean {
        return NativeConfig.deleteKey(settings.writeLayer, file, section, key)
    }

    override val string: String
        get() = if (!NativeConfig.isSettingSaveable(file, section, key)) {
            throw UnsupportedOperationException("Unsupported setting: $file, $section, $key")
        } else NativeConfig.getString(NativeConfig.LAYER_ACTIVE, file, section, key, defaultValue)

    override fun setString(settings: Settings, newValue: String) {
        NativeConfig.setString(settings.writeLayer, file, section, key, newValue)
    }

    fun setString(layer: Int, newValue: String) {
        NativeConfig.setString(layer, file, section, key, newValue)
    }

    companion object {
        private val NOT_RUNTIME_EDITABLE_ARRAY = arrayOf(
            MAIN_CUSTOM_RTC_VALUE,
            MAIN_GFX_BACKEND,
            // The lobby reads these once, when it starts. Changing one while a
            // console is running would appear to do nothing until the next boot.
            MAIN_LOBBY_NICKNAME,
            MAIN_MKW_GAME_PATH,
            MAIN_LOBBY_HOST_ADDRESS
        )

        private val NOT_RUNTIME_EDITABLE: Set<StringSetting> =
            HashSet(listOf(*NOT_RUNTIME_EDITABLE_ARRAY))
    }
}
