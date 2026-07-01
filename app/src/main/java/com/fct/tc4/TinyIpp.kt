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

    /* ---------- called from JNI (any thread) ---------- */

    /**
     * Called by native code after receiving a complete print job.
     * Schedules a [PrintManager.print] call on the main thread which
     * shows the system print dialog for user confirmation.
     */
    @JvmStatic
    fun onPrintJob(jobName: String, tempFilePath: String, documentFormat: String) {
        android.util.Log.i(TAG, "JNI upcall received: name=$jobName fmt=$documentFormat file=$tempFilePath")
        mainHandler.post { submitToPrintManager(jobName, tempFilePath, documentFormat) }
    }

    private fun submitToPrintManager(jobName: String, tempFilePath: String, documentFormat: String) {
        android.util.Log.i(TAG, "submitToPrintManager: activity=${currentActivity?.javaClass?.simpleName}")
        val ctx = currentActivity ?: run {
            android.util.Log.e(TAG, "No foreground activity, cannot show print dialog")
            return
        }
        val file = File(tempFilePath)

        if (!file.isFile || file.length() == 0L) {
            android.util.Log.e(TAG, "Print job file missing or empty: $tempFilePath")
            return
        }

        val printManager = ctx.getSystemService(Context.PRINT_SERVICE) as? PrintManager
        if (printManager == null) {
            android.util.Log.e(TAG, "PrintManager not available")
            return
        }

        val displayName = jobName.ifBlank { "Print Job" }
        val adapter = IppPrintAdapter(displayName, file)

        try {
            printManager.print(displayName, adapter, PrintAttributes.Builder().build())
            android.util.Log.i(TAG, "Print submitted: $displayName (${file.length()} B, $documentFormat)")
        } catch (e: Exception) {
            android.util.Log.e(TAG, "Print submit failed: ${e.message}")
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

        val dir = File(Global.appContext.cacheDir, "run/cups")
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
    private val jobName: String,
    private val file: File
) : PrintDocumentAdapter() {

    override fun onLayout(
        oldAttributes: PrintAttributes?,
        newAttributes: PrintAttributes?,
        cancellationSignal: CancellationSignal?,
        callback: LayoutResultCallback,
        extras: android.os.Bundle?
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
        // Clean up the temp file after printing is done
        file.delete()
    }
}
