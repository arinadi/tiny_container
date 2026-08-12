// TinyAudio.kt -- This file is part of tiny_container.
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
 * TinyAudio – receives PCM audio from the Linux-side PipeWire module
 * over a Unix domain socket and plays it through AAudio.
 *
 * Uses NDK for all Unix-socket operations and AAudio playback.
 *
 * Usage:
 *   TinyAudio.start()   // connect & play (retries for 60s)
 *   TinyAudio.stop()    // stop & disconnect
 */
object TinyAudio {

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
     * Start audio reception.
     *
     * Connects to [cacheDir]/tmp/.tiny.audio and begins receiving
     * PCM at 48000 Hz stereo via AAudio.
     *
     * Retries every 2 seconds for up to 60 seconds if the initial
     * connection fails.
     */
    @Synchronized
    fun start() {
        if (running) return
        running = true

        val socketPath = File(Global.appContext.filesDir, "tmp/.tiny.audio").absolutePath

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
                android.util.Log.e("TinyAudio", "Failed to connect within 60s")
            }
        }, "TinyAudio-start").apply {
            isDaemon = true
            start()
        }
    }

    /**
     * Stop audio reception. Closes the socket and AAudio stream.
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
        System.loadLibrary("tiny_audio_jni")
    }
}
