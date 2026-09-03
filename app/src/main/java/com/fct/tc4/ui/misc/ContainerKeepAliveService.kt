// ContainerKeepAliveService.kt -- This file is part of tiny_container.
//
// Copyright (C) 2026 Caten Hu
//
// Tiny Container is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published
// by the Free Software Foundation, either version 3 of the License,
// or any later version.
//
// Tiny Container is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.

package com.fct.tc4.ui.misc

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import com.fct.tc4.R
import com.fct.tc4.ui.main.MainActivity

/**
 * Keeps the container's underlying process alive with the screen off.
 *
 * Without a foreground service + wake lock, [Global.terminalSession] (and
 * everything it spawns, including the running proot container) is just a
 * field on an app-process-lifetime singleton — Android reclaims that whole
 * process a few minutes after the screen turns off, same as any ordinary
 * background app, regardless of the battery-optimization exemption. This is
 * the same mechanism Termux itself uses (`termux-wake-lock`) for its own
 * sessions.
 *
 * Started from [com.fct.tc4.ui.page.ContainerMainViewModel.launchContainer]
 * and stopped from its `exitContainer()` — see the start/stop symmetry
 * already established there for the X server's own service.
 */
class ContainerKeepAliveService : Service() {

    private var wakeLock: PowerManager.WakeLock? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // The explicit-type overload matches the manifest's specialUse
        // declaration exactly, per Android's own guidance — the type must be
        // requested this way from API 34 (where specialUse itself exists);
        // older OS versions reject a type value they don't recognize, so
        // they get the plain call and just use whatever the manifest says.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(
                NOTIFICATION_ID,
                buildNotification(),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
            )
        } else {
            startForeground(NOTIFICATION_ID, buildNotification())
        }
        acquireWakeLock()
        return START_STICKY
    }

    override fun onDestroy() {
        releaseWakeLock()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun acquireWakeLock() {
        if (wakeLock?.isHeld == true) return
        val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = powerManager.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK,
            "$packageName:ContainerKeepAliveService"
        ).apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    private fun releaseWakeLock() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.tc4_keepalive_notification_channel),
            NotificationManager.IMPORTANCE_MIN
        )
        manager.createNotificationChannel(channel)
    }

    private fun buildNotification(): android.app.Notification {
        val openAppIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.tc4_keepalive_notification_text))
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentIntent(openAppIntent)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_MIN)
            .build()
    }

    companion object {
        private const val CHANNEL_ID = "container_keepalive"
        private const val NOTIFICATION_ID = 1001
    }
}
