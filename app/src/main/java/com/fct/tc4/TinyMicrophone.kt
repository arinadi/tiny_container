// TinyMicrophone.kt -- This file is part of tiny_container.
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

import com.fct.tc4.ui.misc.Global
import java.io.File

/**
 * TinyMicrophone – captures audio from the device microphone via AAudio
 * and sends PCM over a Unix domain socket to the Linux-side PipeWire
 * virtual microphone module.
 *
 * Uses NDK for all Unix-socket operations and AAudio capture.
 *
 * Usage:
 *   TinyMicrophone.start()   // connect & stream  (retries for 60 s)
 *   TinyMicrophone.stop()    // stop capture & disconnect
 */
object TinyMicrophone {

    @Volatile
    private var running = false

    private var workerThread: Thread? = null

    /* ---------- native methods ---------- */
    @JvmStatic
    private external fun nativeStart(socketPath: String): Boolean

    @JvmStatic
    private external fun nativeStop()

    /* ---------- public API ---------- */

    /**
     * Start microphone forwarding.
     *
     * Connects to [cacheDir]/tmp/.tiny.mic and begins sending
     * PCM at 48000 Hz mono via AAudio capture.
     *
     * Retries every 2 seconds for up to 60 seconds if the initial
     * connection fails.
     */
    @Synchronized
    fun start() {
        if (running) return
        running = true

        val socketPath = File(Global.appContext.filesDir, "tmp/.tiny.mic").absolutePath

        workerThread = Thread({
            var success = false
            val deadline = System.currentTimeMillis() + 60_000L

            while (running && !success && System.currentTimeMillis() < deadline) {
                try {
                    success = nativeStart(socketPath)
                } catch (_: Exception) {
                    // nativeStart may throw if JNI not loaded
                }
                if (!success && running) {
                    try {
                        Thread.sleep(2000)
                    } catch (_: InterruptedException) {
                        break
                    }
                }
            }

            if (!success) {
                android.util.Log.e("TinyMicrophone", "Failed to connect within 60 s")
            }
        }, "TinyMic-start").apply {
            isDaemon = true
            start()
        }
    }

    /**
     * Stop microphone forwarding. Closes AAudio capture and socket.
     */
    @Synchronized
    fun stop() {
        if (!running) return
        running = false
        workerThread?.interrupt()
        workerThread = null
        nativeStop()
    }

    /* ---------- JNI load ---------- */
    init {
        System.loadLibrary("tiny_microphone_jni")
    }
}
