package com.sublens.glasses

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.view.View

/**
 * 眼镜 HUD 的显示层: 只做"把两行字画出来"这一件事。
 *
 * 为什么显示要放在 Java 而不是 C++ 里:
 *   - 波导显示器的对比度/亮度/安全区各家不一样, 用系统 Canvas 更容易按厂商
 *     文档调参;
 *   - 引擎与显示解耦后, 换到 Unity / 厂商 HUD SDK 只需要替换这个类。
 * 如果要做零 Java 依赖, 把这里换成 GLES 纹理绘制即可(引擎那侧不用动)。
 */
class HudView(context: Context) : View(context) {

    private var mainText = ""
    private var draftText = ""
    private var latencyMs = 0
    private var finalized = 0
    private var draftActive = false

    private val mainPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE            // 主行: 已定稿译文, 高对比
        textSize = 64f
        typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)
        setShadowLayer(8f, 0f, 0f, Color.BLACK)  // 波导叠加时靠阴影保证可读
    }
    private val draftPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(160, 255, 255, 255)   // 草稿行: 半透明, 表示"还没定"
        textSize = 44f
        typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.NORMAL)
        setShadowLayer(6f, 0f, 0f, Color.BLACK)
    }
    private val hintPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(120, 255, 255, 255)
        textSize = 22f
    }

    /** C++ 引擎每帧回调到这里(在主线程外调用, 需要 post 回来) */
    fun update(main: String, draft: String, latency: Int, doneCount: Int, active: Boolean) {
        post {
            mainText = main
            draftText = draft
            latencyMs = latency
            finalized = doneCount
            draftActive = active
            invalidate()
        }
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        // 主行: 垂直居中偏上; 草稿行紧贴其下
        val cx = width / 2f
        val cy = height / 2f

        if (mainText.isNotEmpty()) {
            drawCentered(canvas, mainText, mainPaint, cx, cy)
        }
        if (draftActive && draftText.isNotEmpty()) {
            drawCentered(canvas, "… $draftText", draftPaint, cx, cy + 64f)
        }
        // 角落状态: 延迟与句数, 真实产品里可以用极小的字或干脆去掉
        canvas.drawText("${latencyMs}ms · $finalized 句", 16f, height - 16f, hintPaint)
    }

    private fun drawCentered(canvas: Canvas, text: String, paint: Paint, cx: Float, cy: Float) {
        val w = paint.measureText(text)
        canvas.drawText(text, cx - w / 2f, cy, paint)
    }
}
