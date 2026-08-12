// UpdateChecker.kt -- This file is part of tiny_container.
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

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

sealed class UpdateResult {
    data object UpToDate : UpdateResult()
    data class NewVersion(val version: String) : UpdateResult()
    data class CheckFailed(val reason: String) : UpdateResult()
}

object UpdateChecker {
    private const val TAG = "UpdateChecker"
    private const val API_URL = "https://api.github.com/repos/Cateners/tiny_container/releases/latest"

    suspend fun check(context: Context): UpdateResult = withContext(Dispatchers.IO) {
        try {
            val currentVersion = context.packageManager
                .getPackageInfo(context.packageName, 0)
                .versionName ?: return@withContext UpdateResult.CheckFailed("versionName is null")

            val json = fetchLatestRelease()
            val tagName = json.getString("tag_name")
            val latestVersion = tagName.removePrefix("v")

            if (compareVersions(latestVersion, currentVersion) > 0) {
                Log.i(TAG, "New version available: $latestVersion (current: $currentVersion)")
                UpdateResult.NewVersion(latestVersion)
            } else {
                UpdateResult.UpToDate
            }
        } catch (e: Exception) {
            Log.w(TAG, "Update check failed", e)
            UpdateResult.CheckFailed(e.message ?: "Unknown error")
        }
    }

    private fun fetchLatestRelease(): JSONObject {
        val connection = URL(API_URL).openConnection() as HttpURLConnection
        try {
            connection.requestMethod = "GET"
            connection.setRequestProperty("Accept", "application/vnd.github+json")
            connection.connectTimeout = 10_000
            connection.readTimeout = 10_000

            val code = connection.responseCode
            if (code != HttpURLConnection.HTTP_OK) {
                throw RuntimeException("HTTP $code")
            }

            val body = connection.inputStream.bufferedReader().use { it.readText() }
            return JSONObject(body)
        } finally {
            connection.disconnect()
        }
    }

    /**
     * 按 "X.Y.Z..." 逐段数值比较版本号大小。
     * 返回 > 0 表示 latest 更新，== 0 相同，< 0 更旧。
     * 版本号非纯数字时抛 NumberFormatException，由调用方兜底为 CheckFailed。
     */
    private fun compareVersions(latest: String, current: String): Int {
        val a = latest.split('-')[0].split('.').map { it.toInt() }
        val b = current.split('-')[0].split('.').map { it.toInt() }
        val len = maxOf(a.size, b.size)
        for (i in 0 until len) {
            val x = a.getOrNull(i) ?: 0
            val y = b.getOrNull(i) ?: 0
            if (x != y) return x.compareTo(y)
        }
        return 0
    }
}
