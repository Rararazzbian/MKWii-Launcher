// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.utils

import android.os.Build
import androidx.annotation.Keep
import androidx.fragment.app.FragmentActivity
import org.dolphinemu.dolphinemu.DolphinApplication
import org.dolphinemu.dolphinemu.dialogs.AnalyticsDialog
import org.dolphinemu.dolphinemu.features.settings.model.BooleanSetting
import org.dolphinemu.dolphinemu.features.settings.model.Settings

object Analytics {
    private const val DEVICE_MANUFACTURER = "DEVICE_MANUFACTURER"
    private const val DEVICE_OS = "DEVICE_OS"
    private const val DEVICE_MODEL = "DEVICE_MODEL"
    private const val DEVICE_TYPE = "DEVICE_TYPE"

    /**
     * Fork: does nothing.
     *
     * The build sets ENABLE_ANALYTICS OFF unconditionally, so AnalyticsReporter
     * compiles to nothing and there is no code left that could report anything.
     * Asking permission for it would be asking to enable something the binary
     * cannot do - and a launcher for a private group should not open with a
     * dialog about phoning home at all.
     *
     * Kept as an empty function rather than deleted so the two call sites, in
     * StartupHandler and UserDataActivity, stay as they are upstream.
     */
    @JvmStatic
    fun checkAnalyticsInit(activity: FragmentActivity) {
    }

    fun firstAnalyticsAdd(enabled: Boolean) {
        Settings().use { settings ->
            settings.loadSettings()
            BooleanSetting.MAIN_ANALYTICS_ENABLED.setBoolean(settings, enabled)
            BooleanSetting.MAIN_ANALYTICS_PERMISSION_ASKED.setBoolean(settings, true)

            settings.saveSettings()
        }
    }

    @Keep
    @JvmStatic
    fun getValue(key: String?): String {
        return when (key) {
            DEVICE_MODEL -> Build.MODEL
            DEVICE_MANUFACTURER -> Build.MANUFACTURER
            DEVICE_OS -> Build.VERSION.SDK_INT.toString()
            DEVICE_TYPE -> if (TvUtil.isLeanback(DolphinApplication.getAppContext())) "android-tv" else "android-mobile"
            else -> ""
        }
    }
}
