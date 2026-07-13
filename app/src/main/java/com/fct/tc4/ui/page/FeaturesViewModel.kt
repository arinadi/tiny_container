// FeaturesViewModel.kt -- This file is part of tiny_container.
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

package com.fct.tc4.ui.page

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import com.fct.tc4.ui.misc.ConfigManager
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

// ===================== Data Models =====================

sealed class FeatureItem {
    abstract val index: Int
    abstract val type: String
    abstract val name: String
    abstract val description: String
}

data class AudioFeature(
    override val index: Int,
    override val type: String,
    override val name: String,
    override val description: String,
    val enabled: Boolean
) : FeatureItem()

data class PrintFeature(
    override val index: Int,
    override val type: String,
    override val name: String,
    override val description: String,
    val enabled: Boolean
) : FeatureItem()

data class MicrophoneFeature(
    override val index: Int,
    override val type: String,
    override val name: String,
    override val description: String,
    val enabled: Boolean = false
) : FeatureItem()

data class WebViewFeature(
    override val index: Int,
    override val type: String,
    override val name: String,
    override val description: String,
    val enabled: Boolean
) : FeatureItem()

data class AvncFeature(
    override val index: Int,
    override val type: String,
    override val name: String,
    override val description: String,
    val enabled: Boolean,
    val adaptToScreenSize: Boolean,
    val scaleRatio: Double,
    val useUnixSocket: Boolean
) : FeatureItem()

data class X11Feature(
    override val index: Int,
    override val type: String,
    override val name: String,
    override val description: String,
    val enabled: Boolean
) : FeatureItem()

data class LstatCacheFeature(
    override val index: Int,
    override val type: String,
    override val name: String,
    override val description: String,
    val enabled: Boolean,
    val path: List<String>
) : FeatureItem()

data class StorageFeature(
    override val index: Int,
    override val type: String,
    override val name: String,
    override val description: String,
    val enabled: Boolean
) : FeatureItem()

/** 未实现的功能类型兜底，避免未知 type 导致崩溃 */
data class UnknownFeature(
    override val index: Int,
    override val type: String,
    override val name: String,
    override val description: String
) : FeatureItem()

// ===================== ViewModel =====================

class FeaturesViewModel(application: Application) : AndroidViewModel(application) {

    companion object {
        private val GUI_TYPES = setOf("webview", "avnc", "x11")
    }

    /** 指向 ContainerMainViewModel.config 的同一引用（不重新加载） */
    private var configRef: MutableMap<String, Any>? = null

    /** 由 Fragment 注入的保存回调 */
    private var saveAction: (() -> Unit)? = null

    /** 麦克风启用状态（仅内存，不持久化） */
    private var micEnabled = false

    private val _featureItems = MutableStateFlow<List<FeatureItem>>(emptyList())
    val featureItems: StateFlow<List<FeatureItem>> = _featureItems.asStateFlow()

    /**
     * 绑定父容器的 config 引用。设防重复绑定（旋转重建时 ViewModel 存活，忽略后续注入）。
     * @param config 父 VM 已加载的完整 config（同一个 Map 引用）
     * @param save 持久化回调，由 Fragment 桥接至 ContainerMainViewModel.saveConfig()
     */
    fun attach(config: Map<String, Any>, save: () -> Unit) {
        if (configRef != null) return
        @Suppress("UNCHECKED_CAST")
        configRef = config as MutableMap<String, Any>
        saveAction = save
        enforceGuiMutualExclusion()
        refresh()
    }

    // ===================== 用户交互 =====================

    @Suppress("UNCHECKED_CAST")
    fun onEnabledToggle(index: Int, enabled: Boolean) {
        val maps = featureMaps ?: return
        if (index !in maps.indices) return
        val map = maps[index]
        val type = map["type"] as? String ?: return

        // 麦克风不持久化，仅维护内存状态
        if (type == "microphone") {
            micEnabled = enabled
            refresh()
            return
        }

        map["enabled"] = enabled

        // GUI 互斥：所有 GUI 类型子项中最多一个 enabled
        if (enabled && type in GUI_TYPES) {
            for ((i, m) in maps.withIndex()) {
                if (i == index) continue
                if ((m["type"] as? String) in GUI_TYPES) {
                    m["enabled"] = false
                }
            }
        }

        saveAction?.invoke()
        refresh()
    }

    @Suppress("UNCHECKED_CAST")
    fun onAdaptToScreenToggle(index: Int, adapt: Boolean) {
        featureMaps?.getOrNull(index)?.let { map ->
            map["adapt_to_screen_size"] = adapt
            saveAction?.invoke()
            refresh()
        }
    }

    @Suppress("UNCHECKED_CAST")
    fun onScaleRatioChange(index: Int, ratio: Float) {
        featureMaps?.getOrNull(index)?.let { map ->
            map["scale_ratio"] = ratio.toDouble()
            saveAction?.invoke()
            refresh()
        }
    }

    @Suppress("UNCHECKED_CAST")
    fun onUseUnixSocketToggle(index: Int, useUnixSocket: Boolean) {
        featureMaps?.getOrNull(index)?.let { map ->
            map["use_unix_socket"] = useUnixSocket
            saveAction?.invoke()
            refresh()
        }
    }

    // ===================== 供 Fragment 调用的编辑方法 =====================

    @Suppress("UNCHECKED_CAST")
    private val featureMaps: List<MutableMap<String, Any>>?
        get() = (configRef?.get("feature") as? List<MutableMap<String, Any>>)

    /** 获取指定 feature 的原始配置 Map */
    fun getFeatureConfig(index: Int): Map<String, Any>? = featureMaps?.getOrNull(index)

    /** 更新指定 feature 的 name/description/command/link 并持久化 */
    @Suppress("UNCHECKED_CAST")
    fun updateFeatureConfig(
        index: Int, name: String, description: String,
        command: String, link: String,
        path: List<String>? = null, args: List<String>? = null
    ) {
        val map = featureMaps?.getOrNull(index) ?: return
        map["name"] = name
        map["description"] = description
        val type = map["type"] as? String
        when (type) {
            "lstat-cache" -> {
                if (path != null) map["path"] = path
            }
            "x11" -> {
                map["command"] = command
                map.remove("link")
                if (args != null) map["args"] = args
            }
            else -> {
                map["command"] = command
                map["link"] = link
            }
        }
        saveAction?.invoke()
        refresh()
    }

    /** 加载时校验 GUI 互斥：多个 GUI 子项 enabled 时只保留第一个 */
    @Suppress("UNCHECKED_CAST")
    private fun enforceGuiMutualExclusion() {
        val maps = featureMaps ?: return
        val firstGui = maps.indexOfFirst {
            (it["type"] as? String) in GUI_TYPES && it["enabled"] == true
        }
        if (firstGui == -1) return
        for ((i, m) in maps.withIndex()) {
            if (i == firstGui) continue
            if ((m["type"] as? String) in GUI_TYPES) {
                m["enabled"] = false
            }
        }
    }

    @Suppress("UNCHECKED_CAST")
    private fun refresh() {
        _featureItems.value = (configRef?.get("feature") as? List<Map<String, Any>>)
            ?.mapIndexed { i, m -> parseFeatureItem(i, m) }
            ?: emptyList()
    }

    private fun parseFeatureItem(index: Int, map: Map<String, Any>): FeatureItem {
        val type = map["type"] as? String ?: "unknown"
        val name = map["name"] as? String ?: ""
        val description = map["description"] as? String ?: ""
        return when (type) {
            "audio" -> AudioFeature(index, type, name, description,
                enabled = map["enabled"] as? Boolean ?: false)
            "print" -> PrintFeature(index, type, name, description,
                enabled = map["enabled"] as? Boolean ?: false)
            "microphone" -> MicrophoneFeature(index, type, name, description,
                enabled = micEnabled)
            "webview" -> WebViewFeature(index, type, name, description,
                enabled = map["enabled"] as? Boolean ?: false)
            "avnc" -> AvncFeature(index, type, name, description,
                enabled = map["enabled"] as? Boolean ?: false,
                adaptToScreenSize = map["adapt_to_screen_size"] as? Boolean ?: false,
                scaleRatio = (map["scale_ratio"] as? Number)?.toDouble() ?: 0.0,
                useUnixSocket = map["use_unix_socket"] as? Boolean ?: true)
            "x11" -> X11Feature(index, type, name, description,
                enabled = map["enabled"] as? Boolean ?: false)
            "lstat-cache" -> LstatCacheFeature(index, type, name, description,
                enabled = map["enabled"] as? Boolean ?: false,
                path = (map["path"] as? List<*>)?.filterIsInstance<String>() ?: emptyList())
            "storage" -> StorageFeature(index, type, name, description,
                enabled = map["enabled"] as? Boolean ?: false)
            else -> UnknownFeature(index, type, name, description)
        }
    }

    class Factory(private val application: Application) : ViewModelProvider.Factory {
        @Suppress("UNCHECKED_CAST")
        override fun <T : ViewModel> create(modelClass: Class<T>): T {
            return FeaturesViewModel(application) as T
        }
    }
}
