#!/usr/bin/env python3
"""把引擎输出的 HUD 纯文本帧渲染成"眼镜视角"示意图(docs/hud-preview.png)。

用途: README / 作品集需要一张图, 说明"戴上去看到的是什么"。
画面内容取自 `sublens_demo` 的真实收官帧(`--no-color` 输出里的纯文本块),
不是手绘的假数据 —— 布局/字号/配色才是我调的效果部分。

用法:
    python tools/render_preview.py
需要: Pillow + 系统里的中文字体(微软雅黑)
"""

from __future__ import annotations

import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "hud-preview.png"

# 取自 sublens_demo en2zh 的真实收官帧(节选两行有代表性的内容)
HEADER = "SubLens HUD   EN → ZH   ·   listening"
MAIN_LINE = "今天天气很适合散步。"
DRAFT_LINE = "… 感谢"
STATUS = "延迟 758ms     FPS 20.0     已定稿 5 句"

WIDTH, HEIGHT = 1600, 900
AMBER = (255, 196, 92)
WHITE = (246, 248, 252)
DIM = (150, 160, 178)
BG_TOP = (9, 12, 18)
BG_BOTTOM = (18, 24, 36)


def find_font() -> str:
    candidates = [
        r"C:\Windows\Fonts\msyh.ttc",
        r"C:\Windows\Fonts\msyhl.ttc",
        r"C:\Windows\Fonts\simhei.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/System/Library/Fonts/PingFang.ttc",
    ]
    for path in candidates:
        if pathlib.Path(path).exists():
            return path
    sys.exit("找不到中文字体, 请改 find_font() 里的路径")


def main() -> None:
    font_path = find_font()
    f_header = ImageFont.truetype(font_path, 34)
    f_main = ImageFont.truetype(font_path, 96)
    f_draft = ImageFont.truetype(font_path, 62)

    img = Image.new("RGB", (WIDTH, HEIGHT), BG_TOP)
    # 竖向渐变背景, 模拟波导在暗环境下的观感
    for y in range(HEIGHT):
        t = y / HEIGHT
        color = tuple(
            int(BG_TOP[i] * (1 - t) + BG_BOTTOM[i] * t) for i in range(3)
        )
        ImageDraw.Draw(img).line([(0, y), (WIDTH, y)], fill=color)

    d = ImageDraw.Draw(img)

    # HUD 安全区(单目波导大约在视野中央, 四边留黑)
    left, top, right, bottom = 120, 250, WIDTH - 120, HEIGHT - 190
    d.rounded_rectangle([left, top, right, bottom], radius=28, outline=(70, 84, 110), width=3)
    d.rounded_rectangle(
        [left + 6, top + 6, right - 6, bottom - 6], radius=24, fill=(6, 8, 12)
    )

    d.text((left + 40, top + 34), HEADER, font=f_header, fill=DIM)
    # 主行: 已定稿译文(高亮)
    d.text((left + 40, top + 110), MAIN_LINE, font=f_main, fill=WHITE)
    # 草稿行: 未确认尾部(暗色, ... 前缀表示还在推演)
    d.text((left + 44, top + 240), DRAFT_LINE, font=f_draft, fill=AMBER)
    # 底栏状态
    d.text((left + 40, bottom - 62), STATUS, font=f_header, fill=DIM)

    # 右上角"录制中"小点, 说明这是真实运行画面
    d.ellipse([right - 48, top + 40, right - 34, top + 54], fill=(230, 80, 80))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    img.save(OUT)
    print(f"已生成: {OUT}  ({img.size[0]}x{img.size[1]})")


if __name__ == "__main__":
    main()
