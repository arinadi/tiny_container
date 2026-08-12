// TinyIpp.kt -- This file is part of tiny_container.
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

import android.app.Activity
import android.app.Application
import android.content.Context
import android.os.Bundle
import android.os.CancellationSignal
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.print.PrintAttributes
import android.print.PrintDocumentAdapter
import android.print.PrintDocumentInfo
import android.print.PrintManager
import com.fct.tc4.ui.misc.Global
import java.io.File
import java.io.FileInputStream

/**
 * TinyIpp – IPP (Internet Printing Protocol) server.
 *
 * Listens on a Unix domain socket at [cacheDir]/run/cups/cups.sock,
 * receives print jobs from inside the Linux container, and submits
 * them to the Android Print framework.
 *
 * Job template attributes selected in the container (paper size,
 * orientation, duplex, color mode, resolution) are forwarded and
 * pre-selected in the Android print dialog via [PrintAttributes].
 *
 * Usage:
 *   TinyIpp.start()   // create socket & listen (retries for 60 s)
 *   TinyIpp.stop()    // stop listening
 */
object TinyIpp {

    private const val TAG = "TinyIpp"

    @Volatile
    private var running = false

    private var workerThread: Thread? = null

    private val mainHandler = Handler(Looper.getMainLooper())

    /* ------ activity tracking (PrintManager requires Activity context) ------ */
    @Volatile
    private var currentActivity: Activity? = null

    private val lifecycleCallbacks = object : Application.ActivityLifecycleCallbacks {
        override fun onActivityResumed(a: Activity)  { currentActivity = a }
        override fun onActivityPaused(a: Activity)   { if (currentActivity === a) currentActivity = null }
        override fun onActivityCreated(a: Activity, b: Bundle?) {}
        override fun onActivityStarted(a: Activity) {}
        override fun onActivityStopped(a: Activity) {}
        override fun onActivitySaveInstanceState(a: Activity, b: Bundle) {}
        override fun onActivityDestroyed(a: Activity) {}
    }

    /* ---------- native methods ---------- */
    @JvmStatic
    private external fun nativeStart(socketPath: String): Boolean

    @JvmStatic
    private external fun nativeStop()

    @JvmStatic
    external fun nativeJobFinished(jobId: Int)

    /* ---------- called from JNI (any thread) ---------- */

    /**
     * Called by native code after receiving a complete print job.
     * Schedules a [PrintManager.print] call on the main thread which
     * shows the system print dialog for user confirmation.
     *
     * @param mediaWidthHm/mediaHeightHm  media size in hundredths of a mm (0 = unknown)
     * @param orientation  IPP orientation-requested (3=portrait 4=landscape
     *                     5=reverse-landscape 6=reverse-portrait, 0 = unknown)
     * @param resXDpi/resYDpi  printer resolution in dpi (0 = unknown)
     */
    @JvmStatic
    fun onPrintJob(
        jobId: Int,
        jobName: String,
        tempFilePath: String,
        documentFormat: String,
        copies: Int,
        media: String,
        mediaWidthHm: Int,
        mediaHeightHm: Int,
        orientation: Int,
        sides: String,
        colorMode: String,
        resXDpi: Int,
        resYDpi: Int
    ) {
        android.util.Log.i(
            TAG,
            "JNI upcall: job=$jobId name=$jobName fmt=$documentFormat copies=$copies " +
                "media=$media(${mediaWidthHm}x${mediaHeightHm}) orient=$orientation " +
                "sides=$sides color=$colorMode res=${resXDpi}x${resYDpi} file=$tempFilePath"
        )
        mainHandler.post {
            submitToPrintManager(
                jobId, jobName, tempFilePath, documentFormat,
                media, mediaWidthHm, mediaHeightHm, orientation, sides, colorMode, resXDpi, resYDpi
            )
        }
    }

    private fun submitToPrintManager(
        jobId: Int,
        jobName: String,
        tempFilePath: String,
        documentFormat: String,
        media: String,
        mediaWidthHm: Int,
        mediaHeightHm: Int,
        orientation: Int,
        sides: String,
        colorMode: String,
        resXDpi: Int,
        resYDpi: Int
    ) {
        val ctx = currentActivity ?: run {
            android.util.Log.e(TAG, "No foreground activity, cannot show print dialog")
            return
        }
        val file = File(tempFilePath)

        if (!file.isFile || file.length() == 0L) {
            android.util.Log.e(TAG, "Print job file missing or empty: $tempFilePath")
            nativeJobFinished(jobId)
            return
        }

        val printManager = ctx.getSystemService(Context.PRINT_SERVICE) as? PrintManager
        if (printManager == null) {
            android.util.Log.e(TAG, "PrintManager not available")
            nativeJobFinished(jobId)
            return
        }

        val displayName = jobName.ifBlank { "Print Job" }
        val adapter = IppPrintAdapter(jobId, displayName, file)
        val attrs = buildPrintAttributes(
            media, mediaWidthHm, mediaHeightHm, orientation, sides, colorMode, resXDpi, resYDpi
        )

        try {
            printManager.print(displayName, adapter, attrs)
            android.util.Log.i(TAG, "Print submitted: $displayName (${file.length()} B, $documentFormat)")
        } catch (e: Exception) {
            android.util.Log.e(TAG, "Print submit failed: ${e.message}")
            nativeJobFinished(jobId)
        }
    }

    /* ---------- IPP → Android attribute mapping ---------- */

    private fun buildPrintAttributes(
        media: String,
        mediaWidthHm: Int,
        mediaHeightHm: Int,
        orientation: Int,
        sides: String,
        colorMode: String,
        resXDpi: Int,
        resYDpi: Int
    ): PrintAttributes {
        val builder = PrintAttributes.Builder()
            .setMediaSize(resolveMediaSize(media, mediaWidthHm, mediaHeightHm, orientation))
            .setColorMode(
                if (colorMode.equals("monochrome", true) || colorMode.equals("bi-level", true))
                    PrintAttributes.COLOR_MODE_MONOCHROME
                else
                    PrintAttributes.COLOR_MODE_COLOR
            )
            .setDuplexMode(
                when (sides) {
                    "two-sided-long-edge" -> PrintAttributes.DUPLEX_MODE_LONG_EDGE
                    "two-sided-short-edge" -> PrintAttributes.DUPLEX_MODE_SHORT_EDGE
                    else -> PrintAttributes.DUPLEX_MODE_NONE
                }
            )
        if (resXDpi > 0 && resYDpi > 0) {
            builder.setResolution(PrintAttributes.Resolution("ipp", "IPP", resXDpi, resYDpi))
        }
        return builder.build()
    }

    private fun resolveMediaSize(
        media: String,
        mediaWidthHm: Int,
        mediaHeightHm: Int,
        orientation: Int
    ): PrintAttributes.MediaSize {
        var size: PrintAttributes.MediaSize? = when (media) {
            "iso_a4_210x297mm" -> PrintAttributes.MediaSize.ISO_A4
            "iso_a5_148x210mm" -> PrintAttributes.MediaSize.ISO_A5
            "iso_a3_297x420mm" -> PrintAttributes.MediaSize.ISO_A3
            "iso_b5_176x250mm" -> PrintAttributes.MediaSize.ISO_B5
            "na_letter_8.5x11in" -> PrintAttributes.MediaSize.NA_LETTER
            "na_legal_8.5x14in" -> PrintAttributes.MediaSize.NA_LEGAL
            "na_index-4x6_4x6in" -> PrintAttributes.MediaSize.NA_INDEX_4X6
            else -> null
        }
        if (size == null && mediaWidthHm > 0 && mediaHeightHm > 0) {
            // hundredths of a mm → thousandths of an inch (1 mil = 2.54 hm)
            val wMils = (mediaWidthHm * 100 + 127) / 254
            val hMils = (mediaHeightHm * 100 + 127) / 254
            size = PrintAttributes.MediaSize(
                "ipp_${mediaWidthHm}x$mediaHeightHm",
                "${mediaWidthHm / 100.0} x ${mediaHeightHm / 100.0} mm",
                wMils, hMils
            )
        }
        if (size == null) size = PrintAttributes.MediaSize.ISO_A4

        return when (orientation) {
            4, 5 -> size.asLandscape()
            3, 6 -> size.asPortrait()
            else -> size
        }
    }

    /* ---------- public API ---------- */

    /**
     * Start the IPP server.
     *
     * Creates [cacheDir]/run/cups/jobs/ directory and binds
     * a Unix domain socket at [cacheDir]/run/cups/cups.sock.
     *
     * Retries every 2 seconds for up to 60 seconds.
     */
    @Synchronized
    fun start() {
        if (running) return
        running = true

        (Global.appContext as Application).registerActivityLifecycleCallbacks(lifecycleCallbacks)

        val dir = File(Global.appContext.filesDir, "run/cups")
        dir.mkdirs()
        File(dir, "jobs").mkdirs()

        // Remove stale socket file
        val sockFile = File(dir, "cups.sock")
        sockFile.delete()

        val socketPath = sockFile.absolutePath

        workerThread = Thread({
            var success = false
            val deadline = System.currentTimeMillis() + 60_000L

            while (running && !success && System.currentTimeMillis() < deadline) {
                try {
                    success = nativeStart(socketPath)
                } catch (_: Exception) { /* JNI may not be loaded yet */ }
                if (!success && running) {
                    try {
                        Thread.sleep(2000)
                    } catch (_: InterruptedException) {
                        break
                    }
                }
            }
            if (!success) {
                android.util.Log.e(TAG, "Failed to start IPP server within 60 s")
            }
        }, "TinyIpp-start").apply {
            isDaemon = true
            start()
        }
    }

    /**
     * Stop the IPP server. Closes the listening socket.
     */
    @Synchronized
    fun stop() {
        if (!running) return
        running = false
        workerThread?.interrupt()
        workerThread = null
        nativeStop()
        currentActivity = null
        (Global.appContext as Application).unregisterActivityLifecycleCallbacks(lifecycleCallbacks)
    }

    /* ---------- JNI load ---------- */
    init {
        System.loadLibrary("tiny_ipp_jni")
    }
}

/**
 * [PrintDocumentAdapter] that feeds a pre-existing file to the
 * Android print system.
 */
private class IppPrintAdapter(
    private val jobId: Int,
    private val jobName: String,
    private val file: File
) : PrintDocumentAdapter() {

    override fun onLayout(
        oldAttributes: PrintAttributes?,
        newAttributes: PrintAttributes?,
        cancellationSignal: CancellationSignal?,
        callback: LayoutResultCallback,
        extras: Bundle?
    ) {
        if (cancellationSignal?.isCanceled == true) {
            callback.onLayoutCancelled()
            return
        }
        val info = PrintDocumentInfo.Builder(jobName)
            .setContentType(PrintDocumentInfo.CONTENT_TYPE_DOCUMENT)
            .setPageCount(PrintDocumentInfo.PAGE_COUNT_UNKNOWN)
            .build()
        callback.onLayoutFinished(info, true)
    }

    override fun onWrite(
        pages: Array<out android.print.PageRange>,
        destination: ParcelFileDescriptor,
        cancellationSignal: CancellationSignal?,
        callback: WriteResultCallback
    ) {
        try {
            FileInputStream(file).use { input ->
                ParcelFileDescriptor.AutoCloseOutputStream(destination).use { output ->
                    val buf = ByteArray(16384)
                    var n: Int
                    while (input.read(buf).also { n = it } != -1) {
                        if (cancellationSignal?.isCanceled == true) {
                            callback.onWriteCancelled()
                            return
                        }
                        output.write(buf, 0, n)
                    }
                }
            }
            callback.onWriteFinished(arrayOf(android.print.PageRange.ALL_PAGES))
        } catch (e: Exception) {
            android.util.Log.e("TinyIpp", "onWrite failed: ${e.message}")
            callback.onWriteFailed(e.message)
        }
    }

    override fun onFinish() {
        // Notify native code (marks the job completed & removes the spool file)
        try {
            TinyIpp.nativeJobFinished(jobId)
        } catch (e: Exception) {
            android.util.Log.e("TinyIpp", "nativeJobFinished failed: ${e.message}")
        }
    }
}
