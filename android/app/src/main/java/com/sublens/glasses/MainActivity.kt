package com.sublens.glasses

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.Gravity
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.Toast

/**
 * 极薄的 Activity: 申请录音权限 → 全屏 HUD → 调 C++ 引擎。
 *
 * 这里刻意不放任何业务逻辑: 音频采集、端点检测、翻译、HUD 状态机全在
 * C++ 引擎里(见 src/), Java 只负责窗口和权限。
 */
class MainActivity : Activity(), HudCallback {

    private lateinit var hud: HudView
    private lateinit var toggle: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        hud = HudView(this)

        toggle = Button(this).apply {
            text = "开始实时翻译"
            setOnClickListener { toggleEngine() }
        }

        val root = FrameLayout(this)
        root.addView(hud, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT))
        root.addView(toggle, FrameLayout.LayoutParams(360, 96, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply {
            bottomMargin = 32
        })
        setContentView(root)

        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQ_AUDIO)
        }
    }

    private fun toggleEngine() {
        if (SubLensBridge.nativeIsRunning()) {
            SubLensBridge.nativeStop()
            toggle.text = "开始实时翻译"
            return
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            Toast.makeText(this, "需要麦克风权限", Toast.LENGTH_SHORT).show()
            return
        }
        val ok = SubLensBridge.nativeStart("en2zh", this)
        toggle.text = if (ok) "停止" else "启动失败"
    }

    /** C++ 引擎每帧回调(HUD 线程), HudView 内部会 post 到主线程 */
    override fun onHud(main: String, draft: String, latencyMs: Int, finalized: Int, draftActive: Boolean) {
        hud.update(main, draft, latencyMs, finalized, draftActive)
    }

    override fun onDestroy() {
        SubLensBridge.nativeStop()
        super.onDestroy()
    }

    private companion object {
        const val REQ_AUDIO = 1
    }
}
