// FeaturesFragment.kt -- This file is part of tiny_container.
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

import android.Manifest
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.activity.result.contract.ActivityResultContracts
import androidx.fragment.app.Fragment
import androidx.fragment.app.viewModels
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.fct.tc4.TinyMicrophone
import com.fct.tc4.R
import com.fct.tc4.databinding.Tc4FragmentFeaturesBinding
import com.fct.tc4.databinding.Tc4FeatureAudioBinding
import com.fct.tc4.databinding.Tc4FeatureAvncBinding
import com.fct.tc4.databinding.Tc4FeatureMicrophoneBinding
import com.fct.tc4.databinding.Tc4FeaturePrintBinding
import com.fct.tc4.databinding.Tc4FeatureWebviewBinding
import com.fct.tc4.databinding.Tc4FeatureUnknownBinding
import com.fct.tc4.databinding.Tc4FeatureX11Binding
import com.fct.tc4.databinding.Tc4FeatureLstatCacheBinding
import com.fct.tc4.ui.misc.FeatureEditDialogFragment
import com.fct.tc4.ui.misc.Global
import com.google.android.material.listitem.ListItemViewHolder
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.launch
import java.io.File
import kotlin.math.pow
import kotlin.math.roundToInt
import java.net.Inet4Address
import java.net.NetworkInterface

class FeaturesFragment : Fragment() {

    private var _binding: Tc4FragmentFeaturesBinding? = null
    private val binding get() = _binding!!

    private val parentVM: ContainerMainViewModel by lazy {
        (requireParentFragment() as ContainerMainFragment).viewModel
    }

    private val viewModel: FeaturesViewModel by viewModels {
        FeaturesViewModel.Factory(requireActivity().application)
    }

    private lateinit var adapter: FeaturesAdapter

    private val requestMicPermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) {
            TinyMicrophone.start()
            viewModel.onEnabledToggle(micPendingIndex, true)
        } else {
            viewModel.onEnabledToggle(micPendingIndex, false)
            Snackbar.make(binding.root, R.string.tc4_feature_mic_permission, Snackbar.LENGTH_SHORT).show()
        }
    }
    private var micPendingIndex = -1

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?
    ): View {
        _binding = Tc4FragmentFeaturesBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        // 注册编辑对话框结果监听
        childFragmentManager.setFragmentResultListener(
            FeatureEditDialogFragment.REQUEST_SAVE_SEND, viewLifecycleOwner
        ) { _, bundle ->
            applyEditResult(bundle)
            val command = bundle.getString(FeatureEditDialogFragment.KEY_CMD, "")
            if (command.isNotBlank()) Global.sendCommand(command)
            Snackbar.make(binding.root, R.string.tc4_feature_cmd_sent, Snackbar.LENGTH_SHORT).show()
        }

        childFragmentManager.setFragmentResultListener(
            FeatureEditDialogFragment.REQUEST_SAVE_EXIT, viewLifecycleOwner
        ) { _, bundle ->
            applyEditResult(bundle)
        }

        adapter = FeaturesAdapter(object : FeatureCallbacks {
            override fun onEnabledToggle(index: Int, enabled: Boolean) {
                viewModel.onEnabledToggle(index, enabled)
            }
            override fun onAdaptToScreenToggle(index: Int, adapt: Boolean) {
                viewModel.onAdaptToScreenToggle(index, adapt)
            }
            override fun onScaleRatioChange(index: Int, ratio: Float) {
                viewModel.onScaleRatioChange(index, ratio)
            }
            override fun onEditFeature(index: Int, type: String) {
                val config = viewModel.getFeatureConfig(index) ?: return
                val name = config["name"] as? String ?: ""
                val description = config["description"] as? String ?: ""
                val command = config["command"] as? String ?: ""
                val link = config["link"] as? String ?: ""
                val path = (config["path"] as? List<*>)?.joinToString("\n") ?: ""
                val args = (config["args"] as? List<*>)?.joinToString("\n") ?: ""
                FeatureEditDialogFragment.newInstance(type, index, name, description, command, link, path, args)
                    .show(childFragmentManager, "edit_feature_$index")
            }
            override fun onShareWebView(index: Int) {
                val config = viewModel.getFeatureConfig(index) ?: return
                val rawLink = config["link"] as? String ?: return
                val ip = getDeviceIpAddress() ?: return
                val resolvedLink = rawLink
                    .replace("127.0.0.1", ip)
                    .replace("localhost", ip)
                val clipboard = requireContext().getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                clipboard.setPrimaryClip(ClipData.newPlainText("link", resolvedLink))
                Snackbar.make(binding.root, getString(R.string.tc4_feature_link_copied, resolvedLink), Snackbar.LENGTH_SHORT).show()
            }
            override fun onMicEnabledToggle(index: Int, enabled: Boolean) {
                if (enabled) {
                    micPendingIndex = index
                    requestMicPermission.launch(Manifest.permission.RECORD_AUDIO)
                } else {
                    TinyMicrophone.stop()
                    viewModel.onEnabledToggle(index, false)
                }
            }
        })
        binding.recyclerView.adapter = adapter
        binding.recyclerView.layoutManager = LinearLayoutManager(requireContext())

        // 注入 parent VM 的 config（同一引用，不重新加载）
        viewLifecycleOwner.lifecycleScope.launch {
            parentVM.configState.collect { config ->
                if (config != null) {
                    viewModel.attach(config, save = { parentVM.saveConfig() })
                }
            }
        }

        // 驱动适配器
        viewLifecycleOwner.lifecycleScope.launch {
            viewModel.featureItems.collect { items ->
                adapter.submitList(items)
                val isEmpty = items.isEmpty()
                binding.recyclerView.visibility = if (isEmpty) View.GONE else View.VISIBLE
                binding.emptyHint.visibility = if (isEmpty) View.VISIBLE else View.GONE
            }
        }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    /** 应用编辑对话框结果 */
    @Suppress("UNCHECKED_CAST")
    private fun applyEditResult(bundle: android.os.Bundle) {
        val index = bundle.getInt(FeatureEditDialogFragment.KEY_INDEX)
        val name = bundle.getString(FeatureEditDialogFragment.KEY_NAME, "")
        val desc = bundle.getString(FeatureEditDialogFragment.KEY_DESC, "")
        val command = bundle.getString(FeatureEditDialogFragment.KEY_CMD, "")
        val link = bundle.getString(FeatureEditDialogFragment.KEY_LINK, "")
        val path = bundle.getStringArrayList(FeatureEditDialogFragment.KEY_PATH)?.toList()
        val args = bundle.getStringArrayList(FeatureEditDialogFragment.KEY_ARGS)?.toList()
        viewModel.updateFeatureConfig(index, name, desc, command, link, path, args)
    }

    /** 获取设备当前活跃的非回环 IPv4 地址 */
    private fun getDeviceIpAddress(): String? {
        val interfaces = NetworkInterface.getNetworkInterfaces() ?: return null
        while (interfaces.hasMoreElements()) {
            val networkInterface = interfaces.nextElement()
            if (networkInterface.isLoopback || !networkInterface.isUp) continue
            val addresses = networkInterface.inetAddresses
            while (addresses.hasMoreElements()) {
                val address = addresses.nextElement()
                if (address is Inet4Address && !address.isLoopbackAddress) {
                    return address.hostAddress
                }
            }
        }
        return null
    }
}

// ===================== 回调接口 =====================

interface FeatureCallbacks {
    fun onEnabledToggle(index: Int, enabled: Boolean)
    fun onAdaptToScreenToggle(index: Int, adapt: Boolean)
    fun onScaleRatioChange(index: Int, ratio: Float)
    fun onEditFeature(index: Int, type: String)
    fun onShareWebView(index: Int)
    fun onMicEnabledToggle(index: Int, enabled: Boolean) {}
}

// ===================== 适配器与 ViewHolder =====================

private class FeaturesAdapter(
    private val callbacks: FeatureCallbacks
) : RecyclerView.Adapter<RecyclerView.ViewHolder>() {

    companion object {
        private const val TYPE_AUDIO = 0
        private const val TYPE_MICROPHONE = 1
        private const val TYPE_WEBVIEW = 2
        private const val TYPE_AVNC = 3
        private const val TYPE_X11 = 4
        private const val TYPE_PRINT = 7
        private const val TYPE_UNKNOWN = 5
        private const val TYPE_LSTAT_CACHE = 6
    }

    private var items: List<FeatureItem> = emptyList()

    fun submitList(newItems: List<FeatureItem>) {
        val oldItems = items
        val diffResult = DiffUtil.calculateDiff(object : DiffUtil.Callback() {
            override fun getOldListSize() = oldItems.size
            override fun getNewListSize() = newItems.size
            override fun areItemsTheSame(old: Int, new: Int) =
                oldItems[old].index == newItems[new].index
            override fun areContentsTheSame(old: Int, new: Int) =
                oldItems[old] == newItems[new]
            override fun getChangePayload(oldItemPosition: Int, newItemPosition: Int): Any? {
                val old = oldItems[oldItemPosition]
                val new = newItems[newItemPosition]
                if (old::class != new::class) return null
                val changes = mutableListOf<String>()
                when {
                    old is AudioFeature && new is AudioFeature -> {
                        if (old.enabled != new.enabled) changes.add("enabled")
                    }
                    old is PrintFeature && new is PrintFeature -> {
                        if (old.enabled != new.enabled) changes.add("enabled")
                    }
                    old is WebViewFeature && new is WebViewFeature -> {
                        if (old.enabled != new.enabled) changes.add("enabled")
                    }
                    old is AvncFeature && new is AvncFeature -> {
                        if (old.enabled != new.enabled) changes.add("enabled")
                        if (old.adaptToScreenSize != new.adaptToScreenSize) changes.add("adapt")
                        if (old.scaleRatio != new.scaleRatio) changes.add("ratio")
                    }
                    old is X11Feature && new is X11Feature -> {
                        if (old.enabled != new.enabled) changes.add("enabled")
                    }
                    old is LstatCacheFeature && new is LstatCacheFeature -> {
                        if (old.enabled != new.enabled) changes.add("enabled")
                    }
                }
                return if (changes.isEmpty()) null else changes
            }
        })
        items = newItems
        diffResult.dispatchUpdatesTo(this)
    }

    override fun getItemViewType(position: Int): Int = when (items[position]) {
        is AudioFeature -> TYPE_AUDIO
        is MicrophoneFeature -> TYPE_MICROPHONE
        is PrintFeature -> TYPE_PRINT
        is WebViewFeature -> TYPE_WEBVIEW
        is AvncFeature -> TYPE_AVNC
        is X11Feature -> TYPE_X11
        is LstatCacheFeature -> TYPE_LSTAT_CACHE
        is UnknownFeature -> TYPE_UNKNOWN
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder {
        val inflater = LayoutInflater.from(parent.context)
        return when (viewType) {
            TYPE_AUDIO -> {
                val b = Tc4FeatureAudioBinding.inflate(inflater, parent, false)
                AudioVH(b, callbacks)
            }
            TYPE_MICROPHONE -> {
                val b = Tc4FeatureMicrophoneBinding.inflate(inflater, parent, false)
                MicrophoneVH(b, callbacks)
            }
            TYPE_WEBVIEW -> {
                val b = Tc4FeatureWebviewBinding.inflate(inflater, parent, false)
                WebViewVH(b, callbacks)
            }
            TYPE_AVNC -> {
                val b = Tc4FeatureAvncBinding.inflate(inflater, parent, false)
                AvncVH(b, callbacks)
            }
            TYPE_X11 -> {
                val b = Tc4FeatureX11Binding.inflate(inflater, parent, false)
                X11VH(b, callbacks)
            }
            TYPE_PRINT -> {
                val b = Tc4FeaturePrintBinding.inflate(inflater, parent, false)
                PrintVH(b, callbacks)
            }
            TYPE_UNKNOWN -> {
                val b = Tc4FeatureUnknownBinding.inflate(inflater, parent, false)
                UnknownVH(b)
            }
            TYPE_LSTAT_CACHE -> {
                val b = Tc4FeatureLstatCacheBinding.inflate(inflater, parent, false)
                LstatCacheVH(b, callbacks)
            }
            else -> error("unknown view type: $viewType")
        }
    }

    override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
        val itemCount = itemCount
        when (holder) {
            is AudioVH -> holder.bind(items[position] as AudioFeature, position, itemCount)
            is MicrophoneVH -> holder.bind(items[position] as MicrophoneFeature, position, itemCount)
            is WebViewVH -> holder.bind(items[position] as WebViewFeature, position, itemCount)
            is AvncVH -> holder.bind(items[position] as AvncFeature, position, itemCount)
            is X11VH -> holder.bind(items[position] as X11Feature, position, itemCount)
            is LstatCacheVH -> holder.bind(items[position] as LstatCacheFeature, position, itemCount)
            is PrintVH -> holder.bind(items[position] as PrintFeature, position, itemCount)
            is UnknownVH -> holder.bind(items[position] as UnknownFeature, position, itemCount)
        }
    }

    override fun onBindViewHolder(
        holder: RecyclerView.ViewHolder, position: Int, payloads: List<Any>
    ) {
        if (payloads.isEmpty()) {
            onBindViewHolder(holder, position)
            return
        }
        @Suppress("UNCHECKED_CAST")
        val changes = payloads[0] as? List<String> ?: run {
            onBindViewHolder(holder, position)
            return
        }
        when (holder) {
            is AudioVH -> holder.bindPartial(items[position] as AudioFeature, changes)
            is WebViewVH -> holder.bindPartial(items[position] as WebViewFeature, changes)
            is AvncVH -> holder.bindPartial(items[position] as AvncFeature, changes)
            is PrintVH -> holder.bindPartial(items[position] as PrintFeature, changes)
            is X11VH -> holder.bindPartial(items[position] as X11Feature, changes)
            is LstatCacheVH -> holder.bindPartial(items[position] as LstatCacheFeature, changes)
        }
    }

    override fun getItemCount() = items.size
}

// ===================== ViewHolders =====================

private class AudioVH(
    private val binding: Tc4FeatureAudioBinding,
    private val callbacks: FeatureCallbacks
) : ListItemViewHolder(binding.root) {

    fun bind(item: AudioFeature, position: Int, itemCount: Int) {
        super.bind(position, itemCount)
        binding.name.text = item.name
        binding.description.text = item.description
        binding.enabled.setOnCheckedChangeListener(null)
        binding.enabled.isChecked = item.enabled
        binding.enabled.setOnCheckedChangeListener { _, isChecked ->
            callbacks.onEnabledToggle(item.index, isChecked)
        }
    }

    fun bindPartial(item: AudioFeature, changes: List<String>) {
        if ("enabled" in changes) {
            binding.enabled.setOnCheckedChangeListener(null)
            binding.enabled.isChecked = item.enabled
            binding.enabled.setOnCheckedChangeListener { _, isChecked ->
                callbacks.onEnabledToggle(item.index, isChecked)
            }
        }
    }
}

private class MicrophoneVH(
    private val binding: Tc4FeatureMicrophoneBinding,
    private val callbacks: FeatureCallbacks
) : ListItemViewHolder(binding.root) {

    fun bind(item: MicrophoneFeature, position: Int, itemCount: Int) {
        super.bind(position, itemCount)
        binding.name.text = item.name
        binding.description.text = item.description
        binding.enabled.setOnCheckedChangeListener(null)
        binding.enabled.isChecked = item.enabled
        binding.enabled.setOnCheckedChangeListener { _, isChecked ->
            callbacks.onMicEnabledToggle(item.index, isChecked)
        }
    }
}

private class PrintVH(
    private val binding: Tc4FeaturePrintBinding,
    private val callbacks: FeatureCallbacks
) : ListItemViewHolder(binding.root) {

    fun bind(item: PrintFeature, position: Int, itemCount: Int) {
        super.bind(position, itemCount)
        binding.name.text = item.name
        binding.description.text = item.description
        binding.enabled.setOnCheckedChangeListener(null)
        binding.enabled.isChecked = item.enabled
        binding.enabled.setOnCheckedChangeListener { _, isChecked ->
            callbacks.onEnabledToggle(item.index, isChecked)
        }
    }

    fun bindPartial(item: PrintFeature, changes: List<String>) {
        if ("enabled" in changes) {
            binding.enabled.setOnCheckedChangeListener(null)
            binding.enabled.isChecked = item.enabled
            binding.enabled.setOnCheckedChangeListener { _, isChecked ->
                callbacks.onEnabledToggle(item.index, isChecked)
            }
        }
    }
}

private class WebViewVH(
    private val binding: Tc4FeatureWebviewBinding,
    private val callbacks: FeatureCallbacks
) : ListItemViewHolder(binding.root) {

    fun bind(item: WebViewFeature, position: Int, itemCount: Int) {
        super.bind(position, itemCount)
        binding.name.text = item.name
        binding.description.text = item.description
        binding.enabled.setOnCheckedChangeListener(null)
        binding.enabled.isChecked = item.enabled
        binding.enabled.setOnCheckedChangeListener { _, isChecked ->
            callbacks.onEnabledToggle(item.index, isChecked)
        }
        binding.edit.setOnClickListener {
            callbacks.onEditFeature(item.index, item.type)
        }
        binding.share.setOnClickListener {
            callbacks.onShareWebView(item.index)
        }
        // 按钮点击先留空
    }

    fun bindPartial(item: WebViewFeature, changes: List<String>) {
        if ("enabled" in changes) {
            binding.enabled.setOnCheckedChangeListener(null)
            binding.enabled.isChecked = item.enabled
            binding.enabled.setOnCheckedChangeListener { _, isChecked ->
                callbacks.onEnabledToggle(item.index, isChecked)
            }
        }
    }
}

private class AvncVH(
    private val binding: Tc4FeatureAvncBinding,
    private val callbacks: FeatureCallbacks
) : ListItemViewHolder(binding.root) {

    fun bind(item: AvncFeature, position: Int, itemCount: Int) {
        super.bind(position, itemCount)
        binding.name.text = item.name
        binding.description.text = item.description

        binding.enabled.setOnCheckedChangeListener(null)
        binding.enabled.isChecked = item.enabled
        binding.enabled.setOnCheckedChangeListener { _, isChecked ->
            callbacks.onEnabledToggle(item.index, isChecked)
        }

        binding.adaptToScreen.setOnCheckedChangeListener(null)
        binding.adaptToScreen.isChecked = item.adaptToScreenSize
        binding.adaptToScreen.setOnCheckedChangeListener { _, isChecked ->
            callbacks.onAdaptToScreenToggle(item.index, isChecked)
        }
        binding.ratio.isEnabled = item.adaptToScreenSize

        binding.ratio.value = ((item.scaleRatio * 20).roundToInt() * 0.05).toFloat()
        binding.ratioText.text = formatScaleRatio(binding.ratio.value)
        binding.ratio.tag = item.index
        binding.ratio.addOnChangeListener { _, value, fromUser ->
            if (fromUser) {
                val idx = binding.ratio.tag as Int
                binding.ratioText.text = formatScaleRatio(value)
                callbacks.onScaleRatioChange(idx, value)
            }
        }
        binding.ratio.setLabelFormatter { value: Float ->
            "%.2f".format(4.0.pow(value.toDouble()))
        }

        binding.avncSettings.setOnClickListener {
            val intent = android.content.Intent(itemView.context, com.gaurav.avnc.ui.prefs.PrefsActivity::class.java)
            itemView.context.startActivity(intent)
        }
        binding.avncLicence.setOnClickListener {
            val intent = android.content.Intent(itemView.context, com.gaurav.avnc.ui.about.AboutActivity::class.java)
            itemView.context.startActivity(intent)
        }
        binding.edit.setOnClickListener {
            callbacks.onEditFeature(item.index, item.type)
        }
    }

    fun bindPartial(item: AvncFeature, changes: List<String>) {
        if ("enabled" in changes) {
            binding.enabled.setOnCheckedChangeListener(null)
            binding.enabled.isChecked = item.enabled
            binding.enabled.setOnCheckedChangeListener { _, isChecked ->
                callbacks.onEnabledToggle(item.index, isChecked)
            }
        }
        if ("adapt" in changes) {
            binding.adaptToScreen.setOnCheckedChangeListener(null)
            binding.adaptToScreen.isChecked = item.adaptToScreenSize
            binding.adaptToScreen.setOnCheckedChangeListener { _, isChecked ->
                callbacks.onAdaptToScreenToggle(item.index, isChecked)
            }
            binding.ratio.isEnabled = item.adaptToScreenSize
        }
        if ("ratio" in changes) {
            binding.ratio.value = ((item.scaleRatio * 20).roundToInt() * 0.05).toFloat()
            binding.ratioText.text = formatScaleRatio(binding.ratio.value)
        }
    }

    private fun formatScaleRatio(value: Float): String {
        val factor = 4.0.pow(value.toDouble())
        return itemView.context.getString(
            R.string.tc4_feature_screen_scale,
            "%.2f".format(factor).trimEnd('0').trimEnd('.')
        )
    }
}

private class X11VH(
    private val binding: Tc4FeatureX11Binding,
    private val callbacks: FeatureCallbacks
) : ListItemViewHolder(binding.root) {

    fun bind(item: X11Feature, position: Int, itemCount: Int) {
        super.bind(position, itemCount)
        binding.name.text = item.name
        binding.description.text = item.description
        binding.enabled.setOnCheckedChangeListener(null)
        binding.enabled.isChecked = item.enabled
        binding.enabled.setOnCheckedChangeListener { _, isChecked ->
            callbacks.onEnabledToggle(item.index, isChecked)
        }
        binding.x11Settings.setOnClickListener {
            val intent = android.content.Intent(itemView.context, com.termux.x11.LoriePreferences::class.java)
            itemView.context.startActivity(intent)
        }
        binding.edit.setOnClickListener {
            callbacks.onEditFeature(item.index, item.type)
        }
    }

    fun bindPartial(item: X11Feature, changes: List<String>) {
        if ("enabled" in changes) {
            binding.enabled.setOnCheckedChangeListener(null)
            binding.enabled.isChecked = item.enabled
            binding.enabled.setOnCheckedChangeListener { _, isChecked ->
                callbacks.onEnabledToggle(item.index, isChecked)
            }
        }
    }
}

private class UnknownVH(
    private val binding: Tc4FeatureUnknownBinding
) : ListItemViewHolder(binding.root) {

    fun bind(item: UnknownFeature, position: Int, itemCount: Int) {
        super.bind(position, itemCount)
        binding.name.text = item.name
        binding.description.text = item.description
    }
}

private class LstatCacheVH(
    private val binding: Tc4FeatureLstatCacheBinding,
    private val callbacks: FeatureCallbacks
) : ListItemViewHolder(binding.root) {

    fun bind(item: LstatCacheFeature, position: Int, itemCount: Int) {
        super.bind(position, itemCount)
        binding.name.text = item.name
        binding.description.text = item.description
        binding.enabled.setOnCheckedChangeListener(null)
        binding.enabled.isChecked = item.enabled
        binding.enabled.setOnCheckedChangeListener { _, isChecked ->
            callbacks.onEnabledToggle(item.index, isChecked)
        }
        binding.edit.setOnClickListener {
            callbacks.onEditFeature(item.index, item.type)
        }
    }

    fun bindPartial(item: LstatCacheFeature, changes: List<String>) {
        if ("enabled" in changes) {
            binding.enabled.setOnCheckedChangeListener(null)
            binding.enabled.isChecked = item.enabled
            binding.enabled.setOnCheckedChangeListener { _, isChecked ->
                callbacks.onEnabledToggle(item.index, isChecked)
            }
        }
    }
}
