/// @file cloud_asr_backend.cpp
/// @brief 云端流式 ASR 的**骨架**实现: 明确失败, 而不是静默降级。
///
/// 演示工程最怕"看起来能跑、其实没接上": 没有 endpoint 时如果 start() 也返回
/// true, 上层会以为云端识别在工作, 实际一句字幕都不会出。所以这里的策略是
///   - endpoint 为空 → start() 返回 false + WARN, 明确告诉用户去配什么;
///   - endpoint 已配置但网络层未实现 → 同样返回 false + WARN 指向接入文档;
/// 离线演示/回归一律用 ScriptedAsrBackend, 两者在 AsrBackend 接口后面可互换。
///
/// 真实接入需要做什么(按依赖顺序):
///   1) 连接: 用 endpoint 建立 TLS WebSocket(wss://), 握手时在 header/query 里
///      带 api_key 与 model, 超时用 connect_timeout_ms; 失败要有指数退避重连。
///   2) 上传: Utterance.pcm 是 16kHz/16bit/单声道。真实流式 ASR 一般要求
///      100ms 一个二进制帧(3200 字节), 所以要按帧切分并保留"句首/句中/句尾"
///      标记(paraformer 的 sentence 参数), 边发边收 partial。
///   3) 解析: 服务端返回 JSON 事件(final/partial/sentence_end), 需要把增量文本
///      拼成 AsrPartial.text(接口约定是"到目前为止的完整假设", 不是增量),
///      并把 emit_ms/speech_start_ms/speech_end_ms 填成单调时钟。
///   4) 线程: 网络线程(或 asio io_context)只做收发, 解码/回调走内部队列;
///      submit() 必须非阻塞, 背压用有界队列 + 丢旧帧策略。
///   5) 停止: stop() 先发 close 帧, 等对端 ack 或超时, 再 join; 之后不再回调。
///
/// 细节与分片协议、错误码见 docs/CLOUD-INTEGRATION.md(由文档任务维护)。

#include "sublens/asr.hpp"

#include <memory>
#include <string>
#include <utility>

namespace sublens {

CloudAsrBackend::CloudAsrBackend(Config cfg) : cfg_(std::move(cfg)) {}

void CloudAsrBackend::set_callback(Callback cb) { cb_ = std::move(cb); }

bool CloudAsrBackend::start() {
  if (cfg_.endpoint.empty()) {
    SL_LOG(Warn) << "cloud-asr: 未配置云端 ASR endpoint, 拒绝启动。"
                 << " 请设置 CloudAsrConfig::endpoint(wss://...) 并填好 api_key; "
                 << "离线演示请改用 ScriptedAsrBackend。";
    return false;
  }
  // 骨架不做真实网络: 即使 endpoint 配了也明确失败, 避免"假成功"。
  SL_LOG(Warn) << "cloud-asr: 已配置接入点 " << cfg_.endpoint
               << " (model=" << cfg_.model
               << "), 但本仓库只保留骨架实现, 真实 WebSocket 接入步骤见 "
               << "docs/CLOUD-INTEGRATION.md";
  return false;
}

void CloudAsrBackend::submit(Utterance utt) {
  // 真实实现: 校验状态 → 按 100ms 切片 utt.pcm → 写 WebSocket → 收到 partial
  // 后再回调。这里只记录到 DEBUG, 默认日志级别下不打扰演示输出。
  SL_LOG(Debug) << "cloud-asr: 忽略 submit(utt_id=" << utt.id << ", samples="
                << utt.pcm.size() << "), 骨架实现未接入网络";
}

void CloudAsrBackend::flush() {
  // 真实实现: 等待已发送分片的最后一个 final 回调完成。
}

void CloudAsrBackend::stop() {
  // 真实实现: 发送 close 帧, 等待对端关闭(或超时), join 网络线程, 之后不再回调。
}

}  // namespace sublens
