// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model

enum class FloatSetting(
    private val file: String,
    private val section: String,
    private val key: String,
    private val defaultValue: Float
) : AbstractFloatSetting {
    // These entries have the same names and order as in C++, just for consistency.
    MAIN_EMULATION_SPEED(Settings.FILE_DOLPHIN, Settings.SECTION_INI_CORE, "EmulationSpeed", 1.0f),
    MAIN_OVERCLOCK(Settings.FILE_DOLPHIN, Settings.SECTION_INI_CORE, "Overclock", 1.0f),
    MAIN_VI_OVERCLOCK(Settings.FILE_DOLPHIN, Settings.SECTION_INI_CORE, "VIOverclock", 1.0f),
    GFX_CC_GAME_GAMMA(Settings.FILE_GFX, Settings.SECTION_GFX_COLOR_CORRECTION, "GameGamma", 2.35f),
    GFX_STEREO_DEPTH(Settings.FILE_GFX, Settings.SECTION_STEREOSCOPY, "StereoDepth", 20.0f),
    GFX_STEREO_CONVERGENCE(Settings.FILE_GFX, Settings.SECTION_STEREOSCOPY, "StereoConvergence", 20.0f),

    // Fork: the MKWii launcher's voice chat.
    MAIN_VOICE_GATE_DB(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_MKW_VOICE,
        "NoiseGateDb",
        -60.0f
    ),
    // How far away another racer can be and still be heard.
    MAIN_VOICE_PROXIMITY_RANGE(
        Settings.FILE_DOLPHIN,
        Settings.SECTION_INI_MKW_VOICE,
        "ProximityRange",
        9000.0f
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
        if (!NativeConfig.isSettingSaveable(file, section, key)) {
            throw UnsupportedOperationException("Unsupported setting: $file, $section, $key")
        }
        return NativeConfig.deleteKey(settings.writeLayer, file, section, key)
    }

    override val float: Float
        get() = NativeConfig.getFloat(NativeConfig.LAYER_ACTIVE, file, section, key, defaultValue)

    override fun setFloat(settings: Settings, newValue: Float) {
        if (!NativeConfig.isSettingSaveable(file, section, key)) {
            throw UnsupportedOperationException("Unsupported setting: $file, $section, $key")
        }
        NativeConfig.setFloat(settings.writeLayer, file, section, key, newValue)
    }

    fun setFloat(layer: Int, newValue: Float) {
        NativeConfig.setFloat(layer, file, section, key, newValue)
    }

    companion object {
        private val NOT_RUNTIME_EDITABLE_ARRAY = arrayOf(
            // Voice keeps a live copy of these, refreshed only when it starts, so
            // editing one while a console is running would appear to do nothing.
            MAIN_VOICE_GATE_DB,
            MAIN_VOICE_PROXIMITY_RANGE
        )

        private val NOT_RUNTIME_EDITABLE: Set<FloatSetting> =
            HashSet(listOf(*NOT_RUNTIME_EDITABLE_ARRAY))
    }
}
