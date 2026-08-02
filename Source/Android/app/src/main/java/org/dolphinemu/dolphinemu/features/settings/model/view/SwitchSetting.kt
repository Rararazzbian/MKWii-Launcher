// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model.view

import android.content.Context
import org.dolphinemu.dolphinemu.features.settings.model.AbstractBooleanSetting
import org.dolphinemu.dolphinemu.features.settings.model.AbstractSetting
import org.dolphinemu.dolphinemu.features.settings.model.Settings

open class SwitchSetting : SettingsItem {
    override val type: Int = TYPE_SWITCH

    protected var booleanSetting: AbstractBooleanSetting

    override val setting: AbstractSetting
        get() = booleanSetting

    constructor(
        context: Context,
        setting: AbstractBooleanSetting,
        titleId: Int,
        descriptionId: Int
    ) : super(context, titleId, descriptionId) {
        booleanSetting = setting
    }

    constructor(
        setting: AbstractBooleanSetting,
        title: CharSequence,
        description: CharSequence
    ) : super(title, description) {
        booleanSetting = setting
    }

    /**
     * Fork: run after this switch is toggled, for a switch that changes which
     * other settings make sense. Set it to rebuild the list and the ones that no
     * longer apply can be left out of it entirely, rather than shown alongside
     * their opposite with a note explaining when to ignore them.
     */
    var onChanged: (() -> Unit)? = null

    open val isChecked: Boolean
        get() = booleanSetting.boolean

    open fun setChecked(settings: Settings, checked: Boolean) {
        booleanSetting.setBoolean(settings, checked)
    }
}
