/// @file engine_jni.cpp
/// @brief Android 端唯一的一层适配: AAudio 采集 + JNI 回调 + 引擎生命周期。
///
/// 设计取舍:
///   为了"Android 上只需要极薄的壳", 这里把所有逻辑都留在 C++ 里:
///     - 音频采集: AAudio(Android 自带低延迟采集, API 26+);
///     - 引擎: 直接复用桌面同一份 Pipeline;
///     - 显示: 不自己搭 EGL/GLES, 而是把 HudSnapshot 通过 JNI 推给 Java 的
///       HudView 绘制。这样做的好处是引擎与 UI 解耦, 换厂商 SDK(比如直接用
///       厂商的 HUD 层 API)时只改 Kotlin 那 60 行;
///       如果要做零 Java 依赖的方案, 把这层换成 GLES 渲染即可(接口已就绪)。
///
/// ⚠️ 说明: 本文件需要 Android NDK 才能编译, 本仓库在 Windows 上只验证了
///         桌面链路(MinGW), Android 适配层属于"已写好、待真机验证"。

#include <jni.h>

#include <android/log.h>
#include <aaudio/AAudio.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "sublens/asr.hpp"
#include "sublens/audio_source.hpp"
#include "sublens/hud.hpp"
#include "sublens/mt.hpp"
#include "sublens/pipeline.hpp"
#include "sublens/script.hpp"

#define LOG_TAG "SubLens"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

JavaVM* g_vm = nullptr;
std::mutex g_state_mutex;
std::unique_ptr<sublens::Pipeline> g_pipeline;
std::thread g_worker;
std::atomic<bool> g_running{false};

// HudView 上的回调(全局引用, 避免被 GC)
jobject g_hud_callback = nullptr;
jmethodID g_on_hud = nullptr;

/// 把当前线程 attach 到 JVM(AAudio/引擎线程都需要)
class ScopedEnv {
 public:
  ScopedEnv() {
    if (g_vm == nullptr) return;
    attached_ = g_vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) == JNI_OK;
    if (!attached_) {
      JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("sublens-hud"), nullptr};
      if (g_vm->AttachCurrentThread(&env_, &args) == JNI_OK) attached_ = true;
    }
  }
  ~ScopedEnv() {
    if (attached_ && g_vm) g_vm->DetachCurrentThread();
  }
  JNIEnv* env() const { return env_; }

 private:
  JNIEnv* env_ = nullptr;
  bool attached_ = false;
};

// ---------------------------------------------------------------------------
// 1. 音频采集: AAudio 低延迟输入
// ---------------------------------------------------------------------------
/// AAudio 采集适配。引擎只需要 16kHz 单声道 PCM16, 所以这里让 AAudio
/// 直接以这个格式开流, 避免上层重采样(重采样会引入额外延迟与音质损失)。
class AaudioSource : public sublens::IAudioSource {
 public:
  ~AaudioSource() override { stop(); }

  bool start() override {
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return false;

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(builder, sublens::kSampleRate);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    // 20ms 一帧(与引擎的帧长一致)
    AAudioStreamBuilder_setFramesPerDataCallback(builder, sublens::kFrameSamples);

    const aaudio_result_t rc =
        AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (rc != AAUDIO_OK || stream_ == nullptr) {
      LOGE("AAudio openStream 失败: %d", static_cast<int>(rc));
      return false;
    }
    if (AAudioStream_requestStart(stream_) != AAUDIO_OK) {
      LOGE("AAudio requestStart 失败");
      return false;
    }
    LOGI("AAudio 已启动: %d Hz, %d ch", AAudioStream_getSampleRate(stream_),
         AAudioStream_getChannelCount(stream_));
    return true;
  }

  void stop() override {
    if (stream_ == nullptr) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
  }

  int read(int16_t* out, int max_samples) override {
    if (stream_ == nullptr) return -1;
    // 100ms 超时: 正常情况下每次都能读到 20ms, 超时说明麦克风被占用/静默
    const aaudio_result_t got = AAudioStream_read(stream_, out, max_samples, 100'000'000);
    if (got < 0) {
      LOGE("AAudio read 失败: %d", static_cast<int>(got));
      return -1;
    }
    return static_cast<int>(got);
  }

  const char* name() const override { return "aaudio-mic"; }

 private:
  AAudioStream* stream_ = nullptr;
};

// ---------------------------------------------------------------------------
// 2. HUD: 把快照推给 Java 的 HudView
// ---------------------------------------------------------------------------
class JniHudRenderer : public sublens::HudRenderer {
 public:
  bool start() override { return true; }
  void stop() override {}

  void render(const sublens::HudSnapshot& s, int64_t /*now_ms*/) override {
    if (g_hud_callback == nullptr || g_on_hud == nullptr) return;
    ScopedEnv env;
    if (env.env() == nullptr) return;

    jstring main_text = env.env()->NewStringUTF(s.cur_dst.c_str());
    jstring draft_text = env.env()->NewStringUTF(s.draft_dst.c_str());
    env.env()->CallVoidMethod(g_hud_callback, g_on_hud, main_text, draft_text,
                              static_cast<jint>(s.last_latency_ms),
                              static_cast<jint>(s.finalized),
                              static_cast<jboolean>(s.draft_active));
    env.env()->DeleteLocalRef(main_text);
    env.env()->DeleteLocalRef(draft_text);
  }

  const char* name() const override { return "jni-hud"; }
};

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  g_vm = vm;
  return JNI_VERSION_1_6;
}

/// 启动引擎。scenario: "en2zh" / "zh2en"
JNIEXPORT jboolean JNICALL
Java_com_sublens_glasses_SubLensBridge_nativeStart(JNIEnv* env, jobject /*thiz*/,
                                                  jstring scenario,
                                                  jobject hud_callback) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_running.load()) return JNI_FALSE;

  // 缓存 HUD 回调
  if (g_hud_callback != nullptr) {
    env->DeleteGlobalRef(g_hud_callback);
    g_hud_callback = nullptr;
  }
  if (hud_callback != nullptr) {
    g_hud_callback = env->NewGlobalRef(hud_callback);
    jclass cls = env->GetObjectClass(hud_callback);
    g_on_hud = env->GetMethodID(cls, "onHud", "(Ljava/lang/String;Ljava/lang/String;IIZ)V");
    if (g_on_hud == nullptr) {
      LOGE("HudCallback.onHud 签名不匹配, 请检查 Kotlin 侧");
      return JNI_FALSE;
    }
  }

  const char* scenario_utf = env->GetStringUTFChars(scenario, nullptr);
  const std::string name = scenario_utf ? scenario_utf : "en2zh";
  env->ReleaseStringUTFChars(scenario, scenario_utf);

  const sublens::ConversationScript& script =
      (name == "zh2en") ? sublens::demo_scenario_zh2en() : sublens::demo_scenario_en2zh();

  sublens::PipelineConfig cfg;
  cfg.name = "android-" + name;
  cfg.realtime_pacing = true;
  cfg.hud_fps = 20;

  sublens::ScriptedAudioConfig acfg;  // 没接真麦克风时先用合成音源跑通链路
  acfg.script = &script;
  acfg.realtime_pacing = true;

  sublens::ScriptedAsrConfig ascfg;
  ascfg.script = &script;

  sublens::PhraseMtConfig mcfg;
  mcfg.source_lang = script.source_lang;
  mcfg.target_lang = script.target_lang;

  sublens::Pipeline::Deps deps;
  // 换成真麦克风只需改这一行: std::make_unique<AaudioSource>()
  deps.audio = std::make_unique<sublens::ScriptedAudioSource>(acfg);
  deps.asr = std::make_unique<sublens::ScriptedAsrBackend>(ascfg);
  deps.mt = std::make_unique<sublens::PhraseTableMt>(mcfg);
  deps.hud = std::make_unique<JniHudRenderer>();

  g_pipeline = std::make_unique<sublens::Pipeline>(cfg, std::move(deps));
  g_running.store(true);
  g_worker = std::thread([] {
    if (g_pipeline) g_pipeline->run();
    g_running.store(false);
    LOGI("引擎退出");
  });
  LOGI("引擎启动: %s", cfg.name.c_str());
  return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_sublens_glasses_SubLensBridge_nativeStop(JNIEnv* /*env*/, jobject /*thiz*/) {
  std::unique_ptr<sublens::Pipeline> pipeline;
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_running.load()) return;
    if (g_pipeline) g_pipeline->request_stop();
    worker = std::move(g_worker);
  }
  if (worker.joinable()) worker.join();
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_pipeline.reset();
  }
  LOGI("引擎已停止");
}

/// 是否在运行(Kotlin 侧用来更新按钮状态)
JNIEXPORT jboolean JNICALL
Java_com_sublens_glasses_SubLensBridge_nativeIsRunning(JNIEnv*, jobject) {
  return g_running.load() ? JNI_TRUE : JNI_FALSE;
}

}  // extern "C"
