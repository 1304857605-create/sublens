# SubLens · Android 端适配层

> ⚠️ **验证状态**: 桌面端(Windows + MinGW)已完整编译、跑通测试与基准;
> Android 这一层是**按 NDK/AAudio 官方 API 写好、尚未在真机上验证**的适配代码。
> 目的是把"核心引擎纯 C++、端侧只需薄壳"这件事落到工程结构上, 而不是停留在 PPT。
> 第一次真机集成预计要改的地方很少(见文末清单)。

---

## 1. 结构

```text
android/
├── app/
│   ├── build.gradle.kts              # minSdk 26(AAudio), targetSdk 34
│   └── src/main/
│       ├── AndroidManifest.xml       # RECORD_AUDIO / 全屏横屏 / 保活
│       ├── cpp/
│       │   ├── CMakeLists.txt        # 直接复用 ../../../../../src 的核心源码
│       │   └── engine_jni.cpp        # AAudio 采集 + JNI 桥 + 引擎生命周期
│       └── java/com/sublens/glasses/
│           ├── SubLensBridge.kt      # 3 个 external fun, 全部逻辑在 C++
│           ├── HudView.kt            # 只负责画两行字(Canvas)
│           └── MainActivity.kt       # 只负责权限 + 按钮 + 全屏
├── build.gradle.kts
├── settings.gradle.kts
└── gradle.properties
```

Kotlin 侧总共不到 200 行, 没有任何业务逻辑: 音频、端点、翻译、HUD 状态机全在 C++ 引擎里。
这么切的好处是**同一份 C++ 代码在桌面和眼镜上跑**, 桌面能用基准测试把性能钉死,
眼镜端只需要替换两个适配器(音频源、显示层)。

---

## 2. 构建步骤

```bash
# 前置: JDK 17, Android SDK, NDK r25+
# 1. 用 Android Studio 打开 android/ 目录, 或命令行:
cd android
./gradlew :app:assembleDebug          # Windows: gradlew.bat :app:assembleDebug

# 2. 安装到眼镜/设备
adb install -r app/build/outputs/apk/debug/app-debug.apk

# 3. 看引擎日志
adb logcat -s SubLens
```

NDK 构建会调用 `app/src/main/cpp/CMakeLists.txt`, 它直接把仓库根目录的
`src/**/*.cpp`(排除桌面 main)编成 `libsublens.so`, 所以**改引擎代码不需要同步两份**。

---

## 3. 从"合成音源"切到"真麦克风"

`engine_jni.cpp` 里现在是:

```cpp
deps.audio = std::make_unique<sublens::ScriptedAudioSource>(acfg);
```

文件里已经写好了 `AaudioSource`(AAudio 低延迟输入, 16kHz 单声道 PCM16, 20ms 一帧),
改成:

```cpp
deps.audio = std::make_unique<AaudioSource>();
```

即可。AAudio 的采样率/声道数由 `IAudioSource` 契约固定为 16kHz/单声道,
如果设备不支持该采样率, AAudio 会拒绝开流(此时需要加一层重采样, 见下)。

---

## 4. 厂商适配要点

| 厂商/设备 | 注意点 |
|---|---|
| **Rokid / RayNeo** | 有自己的 HUD 层 SDK 与语音 SDK; 若用系统 AAudio 采集, 注意厂商可能独占麦克风 |
| **Xreal (Nreal)** | 眼镜常作为手机的外接显示, 应用通常跑在手机上, HUD 坐标系与手机屏幕映射不同 |
| **通用 Android 眼镜** | 单目 640×480 左右的显示区; 字幕安全区要留边(波导边缘有畸变与暗角) |
| 无触摸屏设备 | 交互靠实体键/手势: 把 `Button` 换成 KeyEvent 监听 |
| 功耗/发热 | 20fps 渲染 + 常开麦克风是耗电大头; 可降到 15fps, 并在静默 >10s 时降采样 |

## 5. 真机第一次跑起来预计要改的清单

1. **采样率**: 12kHz/48kHz 设备需要加重采样(建议用 AAudio 的 `setSampleRate` 先试 16k,
   失败再按设备支持的采样率 + 一个简单的线性/多相重采样器);
2. **HUD 安全区**: 按厂商文档调整 `HudView` 的边距与字号(波导边缘会裁切);
3. **保活**: 长时间翻译需要前台服务 + 通知, 否则息屏后进程被回收;
4. **录音合规**: 必须加"正在录音"的可见指示(指示灯/角标)与一键关闭;
5. **权限**: 部分定制 ROM 需要额外的"后台录音"或"悬浮窗"权限。

---

## 6. 为什么显示不放在 C++ 里

可以用 EGL + GLES 在 C++ 里画字, 但没有中文字形就得自带字体文件与光栅化器,
包体与复杂度都不划算。当前方案让 C++ 只产出"视图模型"(两行文本 + 状态),
Kotlin 侧用系统 Canvas 画 —— 这样换厂商 HUD SDK、改字号、加图标都不用碰引擎。
如果将来要做零 Java 依赖(比如跑在 Linux 的定制眼镜上), 只需要实现一个
`HudRenderer`(接口已在 `include/sublens/hud.hpp` 里), 引擎一行不改。
