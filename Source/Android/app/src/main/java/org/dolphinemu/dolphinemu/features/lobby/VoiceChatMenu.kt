// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.lobby

import android.app.Activity
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView
import androidx.appcompat.widget.SwitchCompat
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.dolphinemu.dolphinemu.R

/**
 * The voice chat panel, opened from the button on the input overlay.
 *
 * A dialog rather than a screen of its own on purpose: the game keeps running
 * underneath. Emulation is only paused by the activity actually stopping, which a
 * dialog does not cause, so this can be opened mid-race to turn someone down
 * without leaving the track.
 *
 * The panel is rebuilt from scratch whenever the set of people changes and
 * refreshed in place otherwise, so dragging a volume slider is not interrupted by
 * a peer's level meter moving.
 */
object VoiceChatMenu {
    private const val REFRESH_MS = 250L

    fun show(activity: Activity) {
        val density = activity.resources.displayMetrics.density
        fun dp(value: Int) = (value * density).toInt()

        val content = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(8), dp(24), dp(8))
        }

        val status = TextView(activity)
        content.addView(status)

        val localAddress = TextView(activity).apply {
            alpha = 0.7f
            setPadding(0, dp(2), 0, dp(8))
        }
        content.addView(localAddress)

        val mute = SwitchCompat(activity).apply {
            setText(R.string.voice_mute)
            isChecked = Lobby.isVoiceMuted
            setOnClickListener { Lobby.isVoiceMuted = isChecked }
        }
        content.addView(mute)

        val deafen = SwitchCompat(activity).apply {
            setText(R.string.voice_deafen)
            isChecked = Lobby.isVoiceDeafened
            setOnClickListener {
                Lobby.isVoiceDeafened = isChecked
                // Deafening stops sending too, so the mute switch would otherwise
                // sit there claiming the microphone is live.
                mute.isChecked = Lobby.isVoiceMuted || Lobby.isVoiceDeafened
            }
        }
        content.addView(deafen)

        // Percent sliders run to 200 because unity is 100 - the same range the
        // desktop panel offers.
        content.addView(sliderLabel(activity, R.string.voice_master_volume, dp(12)))
        content.addView(percentSlider(activity, Lobby.masterVolume) { Lobby.masterVolume = it })

        content.addView(sliderLabel(activity, R.string.voice_mic_gain, dp(8)))
        content.addView(percentSlider(activity, Lobby.micGain) { Lobby.micGain = it })

        content.addView(sliderLabel(activity, R.string.voice_people, dp(16)))

        val peerList = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        content.addView(peerList)

        val empty = TextView(activity).apply {
            setText(R.string.voice_nobody_here)
            alpha = 0.7f
        }
        content.addView(empty)

        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.voice_chat)
            .setView(ScrollView(activity).apply { addView(content) })
            .setPositiveButton(R.string.ok, null)
            .create()

        // One row per person, rebuilt only when the set of names changes. Rebuilding
        // every tick would snatch a volume slider out from under a finger.
        val rows = HashMap<String, PeerRow>()
        var shownNicknames = emptyList<String>()

        val handler = Handler(Looper.getMainLooper())
        val refresh = object : Runnable {
            override fun run() {
                status.text = Lobby.voiceStatusText
                val address = Lobby.localAddress
                localAddress.visibility = if (address.isEmpty()) View.GONE else View.VISIBLE
                localAddress.text = activity.getString(R.string.voice_your_address, address)

                val peers = Lobby.voicePeers
                val nicknames = peers.map { it.nickname }
                if (nicknames != shownNicknames) {
                    shownNicknames = nicknames
                    rows.clear()
                    peerList.removeAllViews()
                    for (peer in peers) {
                        val row = PeerRow(activity, peer, ::dp)
                        rows[peer.nickname] = row
                        peerList.addView(row.view)
                    }
                }
                empty.visibility = if (peers.isEmpty()) View.VISIBLE else View.GONE

                for (peer in peers) rows[peer.nickname]?.update(peer)

                handler.postDelayed(this, REFRESH_MS)
            }
        }

        dialog.setOnShowListener { handler.post(refresh) }
        dialog.setOnDismissListener { handler.removeCallbacks(refresh) }
        dialog.show()
    }

    private fun sliderLabel(activity: Activity, textId: Int, topPadding: Int): TextView =
        TextView(activity).apply {
            setText(textId)
            setPadding(0, topPadding, 0, 0)
        }

    private fun percentSlider(activity: Activity, initial: Int, onChange: (Int) -> Unit): SeekBar =
        SeekBar(activity).apply {
            max = 200
            progress = initial.coerceIn(0, 200)
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                    // Only when dragged. Otherwise the periodic refresh writing a
                    // value back would count as a change and fight the finger.
                    if (fromUser) onChange(value)
                }

                override fun onStartTrackingTouch(bar: SeekBar) = Unit
                override fun onStopTrackingTouch(bar: SeekBar) = Unit
            })
        }

    /**
     * One person: their name, a level meter, and how loudly this device hears them.
     *
     * The local entry gets no volume slider - turning yourself down here would do
     * nothing, since what everyone else hears is set on their own device.
     */
    private class PeerRow(activity: Activity, peer: Lobby.VoicePeer, dp: (Int) -> Int) {
        val view = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(8), 0, 0)
        }

        private val name = TextView(activity)
        private val level = ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(3))
        }
        private val volume: SeekBar? =
            if (peer.isLocal) null
            else SeekBar(activity).apply {
                max = 100
                progress = peer.volume.coerceIn(0, 100)
                setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                    override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                        if (fromUser) Lobby.setPeerVolume(peer.nickname, value)
                    }

                    override fun onStartTrackingTouch(bar: SeekBar) = Unit
                    override fun onStopTrackingTouch(bar: SeekBar) = Unit
                })
            }

        init {
            val header = LinearLayout(activity).apply { gravity = Gravity.CENTER_VERTICAL }
            header.addView(name, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            view.addView(header)
            view.addView(level)
            volume?.let { view.addView(it) }
        }

        fun update(peer: Lobby.VoicePeer) {
            val suffix = when {
                peer.isLocal -> " (you)"
                peer.proximity -> " (nearby)"
                else -> ""
            }
            name.text = peer.display + suffix
            // Talking is what the noise gate decided, so the name following it is a
            // more honest "are they audible" than the meter alone.
            name.alpha = if (peer.isTalking || peer.isLocal) 1.0f else 0.6f
            level.progress = (peer.level * 100).toInt().coerceIn(0, 100)
        }
    }
}
