/// @file scripted_asr_backend.cpp
/// @brief 脚本化流式 ASR 模拟器 —— 延迟预算实验与离线演示的"识别器"。
///
/// 为什么需要它: 端到端延迟由 ASR 的**首字延迟**和**尾部收敛速度**主导, 而这两个
/// 量只有在真实网络里才能测。把它们参数化之后, 同一台没有麦克风、没有云服务的
/// 机器上就能做可复现的延迟实验, 稳定前缀/推测翻译也能拿到"真实节奏"的 partial
/// 做回归, 而不是靠 sleep 硬编码。
///
/// 线程模型(与真实云端流式 ASR 对齐):
///
///   submit() --push--> [FIFO 队列] --单工作线程--> 回调(AsrPartial)
///
///   - submit() 只入队, 绝不 sleep: 上层流水线(可能是 VAD 线程)不会被 ASR 拖住;
///   - 单个工作线程顺序消费: 同一句的 partial 严格有序, 不同句不会交错串号;
///   - 停止用条件变量唤醒, stop() 返回之后保证不再有任何回调 —— 上层销毁
///     调度器/统计对象时不会读到悬空指针。

#include "sublens/asr.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sublens {
namespace {

/// UTF-8 码点长度。脚本与上游都是合法 UTF-8, 这里只是防止越界读取。
std::size_t utf8_seq_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  return 4;
}

/// 拼接"当前已听到的词"。
///
/// 规则: 只有"前一个词尾和后一个词头都是中文"时不补空格, 其余情况补一个空格。
///   - 英文词之间 → 空格, 拼出来就是完整英文句子;
///   - 中文词之间 → 不留空格(中文本身不需要分词空格);
///   - 中文词尾是半角逗号(脚本里就是 ',')再接中文 → 补空格, 与脚本
///     source_text 的写法完全一致; 于是模拟 ASR 的定稿文本能逐字命中
///     PhraseTableMt 的整句模板, 演示链路才拿得到参考译文。
void append_word(std::string& out, const std::string& word) {
  if (word.empty()) return;
  if (!out.empty()) {
    const unsigned char prev = static_cast<unsigned char>(out.back());
    const unsigned char cur = static_cast<unsigned char>(word.front());
    if (!(prev >= 0x80 && cur >= 0x80)) out.push_back(' ');
  }
  out += word;
}

/// 生成"等字节长度的错字": 中文码点换成 '错'(3 字节), ASCII 换成 'x'(1 字节)。
///
/// 为什么要求等字节长度: 测试还要断言 partial 是前缀递增, 等长替换让长度曲线
/// 不被破坏; 内容则明显错误, 正好用来验证稳定前缀机制不会被识别错误带歪。
std::string corrupt_word(const std::string& word) {
  std::string out;
  out.reserve(word.size());
  for (std::size_t i = 0; i < word.size();) {
    const unsigned char c = static_cast<unsigned char>(word[i]);
    const std::size_t n = utf8_seq_len(c);
    if (n == 1) {
      out.push_back('x');
    } else {
      out += "错";
    }
    i += n;
  }
  return out;
}

/// 按 utt_id 在脚本里找对应语句。
///
/// 契约缺口: AsrBackend::submit(Utterance) 只带 id + PCM, 不带文本, 所以模拟器
/// 必须靠该 id 回查脚本。上游 VAD 的 next_id_ 从 1 递增, 与演示脚本 id 对齐,
/// 因此离线演示能工作; 接真实 ASR 时文本来自网络, 不依赖这一约定。
const ScriptUtterance* find_script_utterance(const ConversationScript* script, uint64_t id) {
  if (script == nullptr) return nullptr;
  for (const auto& u : script->utterances) {
    if (u.id == id) return &u;
  }
  return nullptr;
}

}  // namespace

struct ScriptedAsrBackend::Impl {
  explicit Impl(ScriptedAsrConfig c) : cfg(std::move(c)), rng(cfg.seed) {}

  ScriptedAsrConfig cfg;
  std::mt19937 rng;  ///< 只在工作线程里用, 无需加锁

  mutable std::mutex mu;
  std::condition_variable cv;
  std::deque<Utterance> queue;
  Callback cb;
  std::thread worker;
  bool started = false;
  bool stopping = false;
  bool busy = false;            ///< 正在处理一条语句(flush 要等它)
  bool warned_missing = false;  ///< "脚本里没有这个 id" 只告警一次, 免得刷屏

  ~Impl() { stop(); }

  void start_locked() {
    if (started || stopping) return;
    started = true;
    worker = std::thread([this] { run(); });
  }

  bool start() {
    std::lock_guard<std::mutex> lk(mu);
    if (stopping) return false;
    start_locked();
    return true;
  }

  void set_callback(Callback c) {
    std::lock_guard<std::mutex> lk(mu);
    cb = std::move(c);
  }

  void submit(Utterance utt) {
    {
      std::lock_guard<std::mutex> lk(mu);
      if (stopping) return;  // 停止后提交直接丢弃, 不再产生回调
      start_locked();        // 允许"先 submit 后 start"的顺序, 降低上层时序耦合
      queue.push_back(std::move(utt));
    }
    cv.notify_one();
  }

  void flush() {
    std::unique_lock<std::mutex> lk(mu);
    // 队列空且没有正在处理的语句 = 所有已提交语句的末回调都已返回
    cv.wait(lk, [this] { return stopping || (queue.empty() && !busy); });
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lk(mu);
      stopping = true;
    }
    cv.notify_all();  // 唤醒可能正卡在首字延迟/词间隔里的工作线程
    if (worker.joinable()) worker.join();
    std::deque<Utterance> dropped;
    {
      std::lock_guard<std::mutex> lk(mu);
      dropped.swap(queue);  // 未处理的语句随 stop 丢弃, 不再回调
      busy = false;
    }
  }

  // ---- 工作线程 ---------------------------------------------------------

  void run() {
    for (;;) {
      Utterance utt;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return stopping || !queue.empty(); });
        if (stopping) return;
        utt = std::move(queue.front());
        queue.pop_front();
        busy = true;
      }
      process(utt);
      {
        std::lock_guard<std::mutex> lk(mu);
        busy = false;
      }
      cv.notify_all();
    }
  }

  /// 可被 stop() 打断的等待。返回 false 表示已经进入停止流程, 调用方应立刻返回。
  bool sleep_ms(int ms) {
    if (ms <= 0) {
      std::lock_guard<std::mutex> lk(mu);
      return !stopping;
    }
    std::unique_lock<std::mutex> lk(mu);
    cv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return stopping; });
    return !stopping;
  }

  int jitter(int max) {
    if (max <= 0) return 0;
    return static_cast<int>(rng() % static_cast<unsigned>(max + 1));
  }

  /// 回调前在锁内检查 stopping: stop() 返回后绝不再有回调。
  /// 回调本身在锁外执行 —— 上层回调只做无锁队列 push, 但不能假设它不会反过来
  /// 调用本对象(那样会死锁), 所以临界区要尽量小。
  bool emit_event(const AsrPartial& p) {
    Callback c;
    {
      std::lock_guard<std::mutex> lk(mu);
      if (stopping) return false;
      c = cb;
    }
    if (c) c(p);
    return true;
  }

  void warn_missing_once(uint64_t id) {
    std::lock_guard<std::mutex> lk(mu);
    if (warned_missing) return;
    warned_missing = true;
    SL_LOG(Warn) << "scripted-asr: 脚本里找不到 utt_id=" << id
                 << " 的语句(ScriptedAsrConfig::script 未覆盖), 该句被跳过";
  }

  AsrPartial make_partial(const Utterance& utt, const std::vector<ScriptWord>& words,
                          std::size_t count, std::size_t err_index, bool is_final) const {
    AsrPartial p;
    p.utt_id = utt.id;
    // 语句边界原样透传: 上层用它算 speech_end → emit 的延迟, 这是本项目唯一
    // 有意义的端到端指标
    p.speech_start_ms = utt.speech_start_ms;
    p.speech_end_ms = utt.speech_end_ms;
    p.emit_ms = now_ms();
    p.is_final = is_final;
    p.confidence = is_final ? 0.98 : 0.9;

    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
      if (i == err_index) {
        append_word(text, corrupt_word(words[i].text));
      } else {
        append_word(text, words[i].text);
      }
    }
    p.text = std::move(text);
    return p;
  }

  void process(const Utterance& utt) {
    const ScriptUtterance* script_utt = find_script_utterance(cfg.script, utt.id);
    if (script_utt == nullptr || script_utt->words.empty()) {
      warn_missing_once(utt.id);
      return;
    }
    const std::vector<ScriptWord>& words = script_utt->words;
    const std::size_t n = words.size();

    // ---- 首字延迟: 从 submit 到第一个 partial ----
    if (!sleep_ms(cfg.first_partial_delay_ms + jitter(cfg.jitter_ms))) return;

    // 第一个 partial 露出几个词: 用脚本自己的逐词时间轴(相对开口)近似
    // "首字延迟结束时已经解码到哪"。至少露一个词, 否则首个 partial 是空串,
    // 上层会误以为"听到了空白"。
    const std::vector<int64_t> word_ends = word_end_offsets(*script_utt);
    std::size_t revealed = 1;
    for (std::size_t i = 0; i < word_ends.size(); ++i) {
      if (word_ends[i] <= cfg.first_partial_delay_ms) revealed = i + 1;
    }
    revealed = std::min(revealed, n);

    // 错误注入: 只挑首词之后的词, 保证"稳定前缀"的首词永远正确; 同一句内
    // 错误固定不变, 模拟真实流式 ASR "错就错到底、直到 final 才可能修正"。
    std::size_t err_index = n;  // == n 表示不注入
    if (cfg.inject_errors && n >= 2) {
      const std::size_t span = (n >= 3) ? (n - 2) : 1;  // 下标范围 [1, n-2]
      err_index = 1 + static_cast<std::size_t>(rng() % span);
    }

    if (!emit_event(make_partial(utt, words, revealed, err_index, false))) return;

    // ---- 逐词推进: 每个词一个新 partial(文本 = 已听到的词拼接) ----
    while (revealed < n) {
      if (!sleep_ms(cfg.word_step_ms + jitter(cfg.jitter_ms))) return;
      ++revealed;
      if (!emit_event(make_partial(utt, words, revealed, err_index, false))) return;
    }

    // ---- 定稿: 尾部收敛延迟 ----
    if (!sleep_ms(cfg.final_extra_ms + jitter(cfg.jitter_ms))) return;
    emit_event(make_partial(utt, words, n, err_index, true));
  }
};

ScriptedAsrBackend::ScriptedAsrBackend(Config cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

ScriptedAsrBackend::~ScriptedAsrBackend() = default;

void ScriptedAsrBackend::set_callback(Callback cb) { impl_->set_callback(std::move(cb)); }

bool ScriptedAsrBackend::start() { return impl_->start(); }

void ScriptedAsrBackend::submit(Utterance utt) { impl_->submit(std::move(utt)); }

void ScriptedAsrBackend::flush() { impl_->flush(); }

void ScriptedAsrBackend::stop() { impl_->stop(); }

}  // namespace sublens
