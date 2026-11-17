package com.sublens.glasses

/**
 * JNI 桥: Kotlin 侧只有这三个函数, 所有逻辑都在 C++ 里。
 *
 * 方法名必须和 android/app/src/main/cpp/engine_jni.cpp 里的
 * Java_com_sublens_glasses_SubLensBridge_xxx 一一对应。
 */
object SubLensBridge {
    init {
        System.loadLibrary("sublens")
    }

    /**
     * 启动引擎。
     * @param scenario "en2zh" 英→中 / "zh2en" 中→英
     * @param callback HUD 更新回调(每帧最多 20 次)
     */
    external fun nativeStart(scenario: String, callback: HudCallback): Boolean

    external fun nativeStop()

    external fun nativeIsRunning(): Boolean
}

/** HUD 回调: C++ 引擎每帧把快照推过来, 由 HudView 负责画。 */
interface HudCallback {
    fun onHud(main: String, draft: String, latencyMs: Int, finalized: Int, draftActive: Boolean)
}
