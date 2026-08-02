// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.lobby

import android.app.Activity
import android.app.ProgressDialog
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.dolphinemu.dolphinemu.R
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * Runs the LAN Play Module install with something on screen while it happens.
 *
 * The install is a download, an unpack and then writing the SD card image, and
 * the last of those can take a while on a phone, so it is not something to do
 * silently behind a frozen screen.
 */
object LanModuleInstaller {
    fun install(activity: Activity, onFinished: () -> Unit = {}) {
        @Suppress("DEPRECATION")
        val dialog = ProgressDialog(activity).apply {
            setTitle(R.string.lan_module)
            setMessage(activity.getString(R.string.lan_module_connecting))
            setCancelable(false)
            // Cancelling is honoured between chunks rather than immediately, so
            // the button is a request, not a stop.
            setButton(
                android.app.AlertDialog.BUTTON_NEGATIVE,
                activity.getString(android.R.string.cancel)
            ) { _, _ -> }
            isIndeterminate = true
            show()
        }

        val handler = Handler(Looper.getMainLooper())
        // Set on the UI thread, read on the install thread, so it needs to be
        // something with defined cross-thread visibility rather than a local.
        val cancelled = AtomicBoolean(false)
        dialog.getButton(android.app.AlertDialog.BUTTON_NEGATIVE)
            ?.setOnClickListener { cancelled.set(true) }

        // Off the UI thread: this blocks for as long as the download takes.
        thread(name = "LanModuleInstall") {
            val error = LanModule.downloadAndInstall { stage, current, total ->
                handler.post {
                    when (stage) {
                        "download" -> {
                            if (total > 0) {
                                dialog.isIndeterminate = false
                                dialog.max = 100
                                dialog.progress = (current * 100 / total).toInt()
                                dialog.setMessage(
                                    activity.getString(
                                        R.string.lan_module_downloading,
                                        current / 1024 / 1024,
                                        total / 1024 / 1024
                                    )
                                )
                            }
                        }

                        "extract" -> {
                            dialog.isIndeterminate = true
                            dialog.setMessage(
                                activity.getString(R.string.lan_module_extracting, current)
                            )
                        }

                        "sync" -> {
                            dialog.isIndeterminate = true
                            dialog.setMessage(activity.getString(R.string.lan_module_writing))
                        }
                    }
                }
                !cancelled.get()
            }

            handler.post {
                dialog.dismiss()
                if (error.isEmpty()) {
                    Toast.makeText(activity, R.string.lan_module_installed, Toast.LENGTH_SHORT)
                        .show()
                    onFinished()
                } else {
                    MaterialAlertDialogBuilder(activity)
                        .setTitle(R.string.lan_module)
                        .setMessage(error)
                        .setPositiveButton(R.string.ok, null)
                        .show()
                }
            }
        }
    }
}
