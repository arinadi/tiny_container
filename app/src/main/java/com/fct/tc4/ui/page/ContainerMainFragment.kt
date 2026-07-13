// ContainerMainFragment.kt -- This file is part of tiny_container.
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

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import androidx.activity.addCallback
import androidx.fragment.app.activityViewModels
import androidx.fragment.app.viewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.viewpager2.adapter.FragmentStateAdapter
import androidx.viewpager2.widget.ViewPager2
import com.fct.tc4.R
import com.fct.tc4.databinding.Tc4FragmentContainerMainBinding
import com.fct.tc4.ui.main.MainViewModel
import com.fct.tc4.ui.misc.ConfirmDialogFragment
import com.fct.tc4.ui.misc.setCurrentItemWithFixedDuration
import com.google.android.material.snackbar.Snackbar
import com.gaurav.avnc.ui.vnc.createVncIntent
import com.gaurav.avnc.vnc.VncUri
import kotlinx.coroutines.launch
import kotlin.math.pow
import androidx.core.net.toUri

class ContainerMainFragment : Fragment() {
    private var _binding: Tc4FragmentContainerMainBinding? = null
    private val binding get() = _binding!!

    private var pendingNavSync: Runnable? = null
    private var pageChangeCallback: ViewPager2.OnPageChangeCallback? = null

    val viewModel: ContainerMainViewModel by viewModels {
        ContainerMainViewModel.Factory(
            requireActivity().application,
            arguments?.getString("code") ?: ""
        )
    }

    private val mainViewModel: MainViewModel by activityViewModels()

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = Tc4FragmentContainerMainBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        // 触发 ViewModel 创建 → init → 启动容器
        viewModel

        // ViewPager2 适配器
        binding.viewPager.adapter = PagerAdapter(this)

        // 底部导航 → ViewPager2
        val navListener = { item: android.view.MenuItem ->
            itemIdToPosition(item.itemId)?.let { pos ->
                binding.viewPager.setCurrentItemWithFixedDuration(pos)
                true
            } ?: false
        }
        binding.bottomNavigation.setOnItemSelectedListener(navListener)

        // ViewPager2 → 底部导航（临时移除监听器，避免回环中断动画）
        pageChangeCallback = object : ViewPager2.OnPageChangeCallback() {
            override fun onPageSelected(position: Int) {
                val b = _binding ?: return
                b.bottomNavigation.setOnItemSelectedListener(null)
                b.bottomNavigation.selectedItemId = positionToItemId(position)
                b.bottomNavigation.setOnItemSelectedListener(navListener)
            }
        }
        binding.viewPager.registerOnPageChangeCallback(pageChangeCallback!!)

        // 首次创建或旋转重建后，同步导航栏与当前页
        pendingNavSync = Runnable {
            val b = _binding ?: return@Runnable
            b.bottomNavigation.setOnItemSelectedListener(null)
            b.bottomNavigation.selectedItemId = positionToItemId(b.viewPager.currentItem)
            b.bottomNavigation.setOnItemSelectedListener(navListener)
        }
        binding.viewPager.post(pendingNavSync!!)

        // 收集更新事件，新版本时弹出 Snackbar
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.updateEvent.collect { version ->
                    val activity = requireActivity()
                    Snackbar.make(
                        binding.root,
                        getString(R.string.tc4_update_available, version),
                        Snackbar.LENGTH_INDEFINITE
                    ).setAction(R.string.tc4_btn_update) {
                        activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("https://github.com/Cateners/tiny_container/releases")))
                    }.show()
                }
            }
        }

        // 收集导航事件，执行实际跳转
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.navigationEvents.collect { event ->
                    when (event) {
                        is GuiNavigationEvent.OpenWebView -> {
                            val intent = Intent(requireContext(), WebViewActivity::class.java).apply {
                                putExtra("url", event.link)
                            }
                            requireContext().startActivity(intent)
                        }
                        is GuiNavigationEvent.OpenAvnc -> {
                            val uriBuilder = event.link.toUri().buildUpon()
                            if (event.useUnixSocket) {
                                uriBuilder.appendQueryParameter("UnixSocket", "${requireContext().cacheDir.absolutePath}/tmp/.tiny.vnc")
                            }
                            val uri = uriBuilder.build()
                            val profile = VncUri(uri.toString()).toServerProfile().apply {
                                resizeRemoteDesktop = event.adaptToScreenSize
                                resizeRemoteDesktopScaleFactor = 4F.pow(event.scaleRatio.toFloat())
                            }
                            requireContext().startActivity(createVncIntent(requireContext(), profile))
                        }
                        is GuiNavigationEvent.OpenX11 -> {
                            val intent = Intent(requireContext(), com.termux.x11.MainActivity::class.java)
                            requireContext().startActivity(intent)
                        }
                    }
                }
            }
        }

        // 观察初始化状态，loading 完成后才显示内容
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.isInitialized.collect { initialized ->
                    binding.loadingLayout.visibility = if (initialized) View.GONE else View.VISIBLE
                    binding.viewPager.visibility = if (initialized) View.VISIBLE else View.INVISIBLE
                    binding.bottomNavigation.visibility = if (initialized) View.VISIBLE else View.INVISIBLE
                }
            }
        }

        // 设置标题为容器名称
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.configState.collect { config ->
                    val name = (config?.get("name") as? String)?.ifBlank { null }
                        ?: arguments?.getString("code") ?: ""
                    requireActivity().title = name
                }
            }
        }

        // 收集错误状态，配置丢失或启动失败时显示 Snackbar 并允许返回管理页
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                viewModel.errorMessage.collect { error ->
                    if (error != null) {
                        Snackbar.make(binding.root, error, Snackbar.LENGTH_INDEFINITE)
                            .setAction(R.string.tc4_btn_back) {
                                mainViewModel.navigateTo(MainViewModel.Screen.ContainerManage)
                            }
                            .show()
                    }
                }
            }
        }

        // 拦截返回键：弹出确认对话框
        val exitRequestKey = "confirm_exit_container"
        childFragmentManager.setFragmentResultListener(exitRequestKey, viewLifecycleOwner) { _, bundle ->
            if (bundle.getBoolean("confirmed")) {
                viewModel.exitContainer()
                mainViewModel.navigateTo(MainViewModel.Screen.ContainerManage)
            }
        }
        requireActivity().onBackPressedDispatcher.addCallback(viewLifecycleOwner) {
            if (mainViewModel.isTerminalOpen.value) {
                mainViewModel.closeTerminal()
            } else {
                ConfirmDialogFragment.show(
                    childFragmentManager,
                    exitRequestKey,
                    title = getString(R.string.tc4_dialog_exit_title),
                    message = getString(R.string.tc4_dialog_exit_message),
                    positiveText = getString(R.string.tc4_btn_ok),
                    negativeText = getString(R.string.tc4_btn_cancel)
                )
            }
        }
    }

    /** 由 Activity 菜单栏调用，进入容器的图形界面 */
    fun onEnterGui() {
        viewModel.enterGui()
    }

    override fun onDestroyView() {
        pendingNavSync?.let { _binding?.viewPager?.removeCallbacks(it) }
        pendingNavSync = null
        pageChangeCallback?.let { _binding?.viewPager?.unregisterOnPageChangeCallback(it) }
        pageChangeCallback = null
        super.onDestroyView()
        _binding = null
    }

    // ========== ViewPager2 Adapter ==========

    private inner class PagerAdapter(fragment: Fragment) : FragmentStateAdapter(fragment) {
        override fun getItemCount() = 4

        override fun createFragment(position: Int): Fragment = when (position) {
            0 -> QuicksFragment()
            1 -> FeaturesFragment()
            2 -> SettingsFragment().apply {
                arguments = Bundle().apply {
                    putString("code", this@ContainerMainFragment.arguments?.getString("code") ?: "")
                }
            }
            3 -> HelpFragment()
            else -> error("unknown position: $position")
        }
    }

    // ========== 位置 ↔ Menu ID 映射 ==========

    private fun positionToItemId(position: Int): Int = when (position) {
        0 -> R.id.shortcuts
        1 -> R.id.features
        2 -> R.id.settings
        3 -> R.id.help
        else -> R.id.shortcuts
    }

    private fun itemIdToPosition(itemId: Int): Int? = when (itemId) {
        R.id.shortcuts -> 0
        R.id.features -> 1
        R.id.settings -> 2
        R.id.help -> 3
        else -> null
    }
}
