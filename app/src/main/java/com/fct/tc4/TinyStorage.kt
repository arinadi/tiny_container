// TinyStorage.kt -- This file is part of tiny_container.
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

package com.fct.tc4

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Environment
import android.os.storage.StorageManager
import android.os.storage.StorageVolume
import com.fct.tc4.ui.misc.Global
import java.io.File

/**
 * TinyStorage – monitors external storage (USB drives, SD cards) and forwards
 * mount/umount events to the proot socket server so it can dynamically add/remove
 * bind mounts under /mnt/ inside the container.
 *
 * Usage:
 *   TinyStorage.start()   // connect to socket & register listeners
 *   TinyStorage.stop()    // disconnect & unregister listeners
 */

object TinyStorage {

    @Volatile
    private var running = false

    private var workerThread: Thread? = null
    private var mediaReceiver: BroadcastReceiver? = null
    private var volumeCallback: Pair<StorageManager, Pair<java.util.concurrent.ExecutorService, StorageManager.StorageVolumeCallback>>? = null

    // Cache mount-time (path, label) keyed by volume name so unmount
    // can sendRemove without relying on volume.state or volume.directory.
    private val mountedVolumes = mutableMapOf<String, Pair<String, String>>()

    /* ---------- native methods ---------- */
    @JvmStatic
    private external fun nativeStart(socketPath: String): Boolean

    @JvmStatic
    private external fun nativeStop()

    @JvmStatic
    private external fun nativeSend(action: Byte, path: String, name: String): Boolean

    /* ---------- public API ---------- */

    @Synchronized
    fun start() {
        if (running) return
        running = true

        val socketPath = File(Global.appContext.cacheDir, "proot_tmp/.tiny.storage").absolutePath

        workerThread = Thread({
            var success = false
            val deadline = System.currentTimeMillis() + 60_000L

            while (running && !success && System.currentTimeMillis() < deadline) {
                try {
                    success = nativeStart(socketPath)
                } catch (_: Exception) { }
                if (!success && running) {
                    try { Thread.sleep(2000) } catch (_: InterruptedException) { break }
                }
            }

            if (success) {
                for ((path, label) in detectExternalStorage()) {
                    sendAdd(path, label)
                }
            } else {
                android.util.Log.e("TinyStorage", "Failed to connect within 60s")
            }
        }, "TinyStorage-start").apply {
            isDaemon = true
            start()
        }

        registerListeners()
    }

    @Synchronized
    fun stop() {
        if (!running) return
        running = false
        workerThread?.interrupt()
        workerThread = null
        unregisterListeners()
        nativeStop()
    }

    /* ---------- listeners ---------- */

    private fun registerListeners() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            registerVolumeCallback()
        } else {
            registerMediaReceiver()
        }
    }

    private fun unregisterListeners() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            unregisterVolumeCallback()
        } else {
            unregisterMediaReceiver()
        }
    }

    /* ---------- API 30+: StorageVolumeCallback ---------- */

    private fun registerVolumeCallback() {
        val sm = Global.appContext.getSystemService(Context.STORAGE_SERVICE) as? StorageManager ?: return

        val executor = java.util.concurrent.Executors.newSingleThreadExecutor { r ->
            Thread(r, "TinyStorage-cb").apply { isDaemon = true }
        }

        val cb = object : StorageManager.StorageVolumeCallback() {
            override fun onStateChanged(volume: StorageVolume) {
                val volName = volume.mediaStoreVolumeName ?: return
                @Suppress("DEPRECATION")
                when (volume.state) {
                    Environment.MEDIA_MOUNTED -> {
                        val dir = volume.directory ?: return
                        val path = dir.absolutePath
                        val desc = volume.getDescription(Global.appContext)
                        val label = sanitizeName(
                            if (desc != null && desc.isNotBlank()) desc else dir.name, path)
                        mountedVolumes[volName] = Pair(path, label)
                        sendAdd(path, label)
                    }
                    else -> {
                        val cached = mountedVolumes.remove(volName) ?: return
                        sendRemove(cached.first, cached.second)
                    }
                }
            }
        }

        volumeCallback = Pair(sm, Pair(executor, cb))
        @Suppress("DEPRECATION")
        sm.registerStorageVolumeCallback(executor, cb)
    }

    private fun unregisterVolumeCallback() {
        val (sm, excb) = volumeCallback ?: return
        val (executor, cb) = excb
        executor.shutdownNow()
        @Suppress("DEPRECATION")
        sm.unregisterStorageVolumeCallback(cb)
        volumeCallback = null
    }

    /* ---------- BroadcastReceiver (all API levels) ---------- */

    private fun registerMediaReceiver() {
        val filter = IntentFilter().apply {
            addAction(Intent.ACTION_MEDIA_MOUNTED)
            addAction(Intent.ACTION_MEDIA_UNMOUNTED)
            addDataScheme("file")
        }

        mediaReceiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                if (intent == null) return
                val path = intent.data?.path ?: return
                when (intent.action) {
                    Intent.ACTION_MEDIA_MOUNTED -> {
                        if (!isExternalVolume(path)) return
                        val label = getVolumeLabel(path)
                        mountedVolumes[path] = Pair(path, label)
                        sendAdd(path, label)
                    }
                    Intent.ACTION_MEDIA_UNMOUNTED -> {
                        val cached = mountedVolumes.remove(path) ?: return
                        sendRemove(cached.first, cached.second)
                    }
                }
            }
        }

        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                Global.appContext.registerReceiver(
                    mediaReceiver, filter, Context.RECEIVER_EXPORTED)
            } else {
                @Suppress("DEPRECATION")
                Global.appContext.registerReceiver(mediaReceiver, filter)
            }
        } catch (_: Exception) {
            android.util.Log.e("TinyStorage", "Failed to register media receiver")
        }
    }

    private fun unregisterMediaReceiver() {
        mediaReceiver?.let {
            try { Global.appContext.unregisterReceiver(it) } catch (_: Exception) { }
        }
        mediaReceiver = null
    }

    /* ---------- storage helpers ---------- */

    private fun sendAdd(path: String, label: String) {
        if (!running) return
        val name = sanitizeName(label, path)
        try {
            nativeSend('A'.code.toByte(), path, name)
        } catch (e: Exception) {
            android.util.Log.e("TinyStorage", "sendAdd failed", e)
        }
    }

    private fun sendRemove(path: String, label: String) {
        if (!running) return
        val name = sanitizeName(label, path)
        try {
            nativeSend('R'.code.toByte(), path, name)
        } catch (e: Exception) {
            android.util.Log.e("TinyStorage", "sendRemove failed", e)
        }
    }

    private fun sanitizeName(label: String, path: String): String {
        val raw = if (label.isNotBlank()) label else File(path).name
        return raw.replace(Regex("""[\s:;'"\\!$`|&<>(){}\[\]]"""), "_")
    }

    private fun isExternalVolume(path: String): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val sm = Global.appContext.getSystemService(Context.STORAGE_SERVICE) as? StorageManager ?: return false
            for (vol in sm.storageVolumes) {
                @Suppress("DEPRECATION")
                if (vol.isRemovable && vol.state == Environment.MEDIA_MOUNTED) {
                    val dir = vol.directory
                    if (dir != null && path.startsWith(dir.absolutePath))
                        return true
                }
            }
            return false
        }
        return path.startsWith("/mnt/media_rw/") ||
                path.startsWith("/storage/") && !path.contains("/emulated/")
    }

    private fun getVolumeLabel(path: String): String {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val sm = Global.appContext.getSystemService(Context.STORAGE_SERVICE) as? StorageManager ?: return ""
            for (vol in sm.storageVolumes) {
                val dir = vol.directory ?: continue
                if (path.startsWith(dir.absolutePath)) {
                    val desc = vol.getDescription(Global.appContext)
                    if (desc != null && desc.isNotBlank()) return desc
                }
            }
        }
        return ""
    }

    /* ---------- public: launch-time device detection ---------- */

    fun detectExternalStorage(): List<Pair<String, String>> {
        val result = mutableListOf<Pair<String, String>>()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val sm = Global.appContext.getSystemService(Context.STORAGE_SERVICE) as? StorageManager ?: return result
            for (vol in sm.storageVolumes) {
                if (!vol.isRemovable && vol.isPrimary) continue
                @Suppress("DEPRECATION")
                if (vol.state != Environment.MEDIA_MOUNTED) continue

                val dir = vol.directory ?: continue
                val path = dir.absolutePath
                if (path.isBlank()) continue

                val volName = vol.mediaStoreVolumeName ?: continue
                val desc = vol.getDescription(Global.appContext)
                val label = sanitizeName(
                    if (desc != null && desc.isNotBlank()) desc else dir.name, path)

                mountedVolumes[volName] = Pair(path, label)
                result.add(Pair(path, label))
            }
        } else {
            val candidates = arrayOf("/mnt/media_rw", "/storage")
            for (base in candidates) {
                val baseDir = File(base)
                if (!baseDir.isDirectory) continue
                baseDir.listFiles()?.forEach { dir ->
                    if (dir.isDirectory && dir.name != "emulated" && dir.name != "self") {
                        val path = dir.absolutePath
                        if (path.contains("/emulated/")) return@forEach
                        mountedVolumes[path] = Pair(path, dir.name)
                        result.add(Pair(path, dir.name))
                    }
                }
            }
        }
        return result
    }

    init {
        System.loadLibrary("tiny_storage_jni")
    }
}
