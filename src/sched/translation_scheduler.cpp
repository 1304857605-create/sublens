/// @file translation_scheduler.cpp
/// @brief 推测式(草稿 + 正式双路)翻译调度的实现(本项目的核心算法之二)。
///
/// 算法动机(同时传译的"稳定性优先"):
///   稳定前缀对齐解决了"哪些原文可以信"; 但实时字幕的体验还取决于
///   "什么时候把什么送到屏幕上"。这里把翻译拆成两路:
///     - 正式路(draft=false): 稳定片段 → 直接累加进 HUD 的已定稿行, 只增不改;
///     - 草稿路(draft=true): 不稳定尾部 → 只更新 HUD 的草稿行, 允许整行替换。
///   草稿是投机: 它基于的原文随时可能被 ASR 改写, 所以结果回来时必须校验
///   "这句之后有没有新的正式提交 / 这句是否已定稿", 过时就整条丢弃。
///   丢一条草稿的代价只是草稿行晚几十毫秒; 而让已上屏的译文回退一次, 用户
///   就会以为设备坏了。所以判断宁保守, 不可漏 —— 这就是"稳定性优先"的取舍。
///
/// 正式路为什么发"整句累计前缀"而不是单个片段:
///   翻译质量依赖上下文 —— 词序/搭配/指代/量词都是跨片段才成立的。把
///   "能你 tellme 怎么到" 拆成片段逐个翻, 拿到的只能是逐词硬拼的语序
///   (实测 demo 就是「打扰一下，能你tellme怎么到get到火车站？」)。所以每次
///   正式提交都发该句到目前为止的**完整原文前缀**, 让 MT 在整句上下文里
///   重新翻译。代价: 请求 token 量随句长增长(一句 n 个片段的累计发送量约
///   O(n²)) —— 端侧短语翻译可以忽略, 接云端 MT 时这是明确的时间/费用成本,
///   要靠 min_chunk_chars(少提交几次)与 draft_throttle_ms 控制。
///
/// 重译结果怎么上屏(差量追加, 保住"只增不改"的 HUD 契约):
///   主行一旦画出就不能回退。所以把新译文与**该句上一次完整译文**求最长
///   公共前缀, 只把公共前缀之后新增的尾部追加到主行:
///     - 新译文是旧译文的延长 → 追加新增部分(流式字幕的正常路径);
///     - 新译文重写了已显示的内容(公共前缀变短, 词序/搭配被 MT 调整) →
///       本次不追加。已上屏的字收不回来, 追加新译文的剩余部分只会和主行
///       已有的尾部重复; 只把参照译文更新成新版本, 下一次重译从新版本继续
///       算差量。这会牺牲一点"最终一致性"换"绝不闪变"。
///
/// 线程契约:
///   on_asr_partial / on_mt_result 可能来自不同线程(ASR 线程 / MT 回调线程)。
///   所有状态由 mu_ 保护; 但 mt_.submit 与 sink 一律在锁外调用 —— 同步回调的
///   MT 后端(以及单元测试里的假后端)会在 submit 内部直接回调 on_mt_result,
///   sink 也可能再回调调度器接口, 持锁调用就是自己等自己(死锁)。

#include "sublens/sched.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace sublens {
namespace {

// 与 partial_stabilizer.cpp 用同一套码点规则。这里只需要"字数"一个用途,
// 为了不为一个 10 行的小工具新开私有头文件, 就地在匿名命名空间里实现。
inline size_t char_len_at(const std::string& s, size_t pos) {
  const unsigned char c = static_cast<unsigned char>(s[pos]);
  size_t n = 1;
  if ((c & 0x80) == 0x00) {
    n = 1;
  } else if ((c & 0xE0) == 0xC0) {
    n = 2;
  } else if ((c & 0xF0) == 0xE0) {
    n = 3;
  } else if ((c & 0xF8) == 0xF0) {
    n = 4;
  }
  if (pos + n > s.size()) return 1;
  for (size_t i = 1; i < n; ++i) {
    if ((static_cast<unsigned char>(s[pos + i]) & 0xC0) != 0x80) return 1;
  }
  return n;
}

inline size_t utf8_char_count(const std::string& s) {
  size_t pos = 0;
  size_t count = 0;
  while (pos < s.size()) {
    pos += char_len_at(s, pos);
    ++count;
  }
  return count;
}

/// 该字节是否是一个码点的起始字节(0xxxxxxx / 110xxxxx / 1110xxxx / 11110xxx)
inline bool is_lead_byte(unsigned char c) { return (c & 0xC0) != 0x80; }

/// 两个合法 UTF-8 串的最长公共前缀(按码点对齐)。
///
/// 重译差量上屏要用它: 先按字节求公共长度, 再回退到码点边界, 保证"追加"
/// 与"已上屏"的拼接不会把某个汉字切成两半(切片点落在字符内部就是乱码)。
/// 因为公共部分的字节完全相同, 判断边界只看 a 就够。
inline std::string longest_common_prefix_utf8(const std::string& a,
                                              const std::string& b) {
  const size_t n = std::min(a.size(), b.size());
  size_t i = 0;
  while (i < n && a[i] == b[i]) ++i;
  while (i > 0 && i < a.size() && !is_lead_byte(static_cast<unsigned char>(a[i]))) {
    --i;
  }
  return a.substr(0, i);
}

/// 最多保留多少句的调度状态。
///
/// 长跑程序不能无界增长; 但云端 MT 慢于 ASR 时, 旧句的译文结果可能晚到。
/// 留 16 句(十几秒)的余量足够覆盖正常的往返抖动; 更老的结果会被丢弃并打
/// 警告日志(见 on_mt_result)—— 那条路径说明 MT 已经慢到不可用, 补发旧句
/// 反而会让 HUD 主行回退。
constexpr size_t kMaxUttStates = 16;

}  // namespace

TranslationScheduler::TranslationScheduler(MtBackend& mt, Config cfg)
    : mt_(mt), cfg_(cfg), stabilizer_(cfg.stabilizer) {
  // 参数兜底: 负的节流间隔会让每个 partial 都发一次草稿(翻译费用/带宽直接
  // 翻几倍), 负的长度上限同理。统一夹到 0 —— 0 表示"不节流", 语义仍然正确。
  if (cfg_.draft_throttle_ms < 0) cfg_.draft_throttle_ms = 0;
  if (cfg_.max_draft_chars < 0) cfg_.max_draft_chars = 0;
}

void TranslationScheduler::set_sink(Sink sink) {
  std::lock_guard<std::mutex> lk(mu_);
  sink_ = std::move(sink);
}

void TranslationScheduler::on_asr_partial(const AsrPartial& p) {
  // 在锁内只做状态决策, 把"外部调用"收集到 to_submit, 解锁后统一发出。
  std::vector<MtRequest> to_submit;
  {
    std::lock_guard<std::mutex> lk(mu_);

    const int64_t now = now_ms();
    ++stats_.partials;

    // 1) 稳定前缀对齐: 这一帧能提交什么、还剩什么没确认。
    const PartialStabilizer::Update u = stabilizer_.on_partial(p);

    // 2) 找/建该句状态。utts_ 是"最近几句"的小数组, 线性扫描足够快;
    //    超过上限时淘汰最旧的一句, 两张表(状态 / 簿记)必须一起删, 否则
    //    同一 utt_id 复用时会把旧簿记当成自己的。
    auto find_state = [this](uint64_t id) -> UttState* {
      for (auto& kv : utts_) {
        if (kv.first == id) return &kv.second;
      }
      return nullptr;
    };
    UttState* st = find_state(p.utt_id);
    UttAux* ua = nullptr;
    {
      auto it = utt_aux_.find(p.utt_id);
      if (it != utt_aux_.end()) ua = &it->second;
    }
    if (st == nullptr || ua == nullptr) {
      if (utts_.size() >= kMaxUttStates) {
        utt_aux_.erase(utts_.front().first);
        utts_.erase(utts_.begin());
      }
      utts_.push_back({p.utt_id, UttState{}});
      st = &utts_.back().second;
      ua = &utt_aux_[p.utt_id];
      *ua = UttAux{};  // 重建状态: 簿记从头记, 不继承任何历史
    }
    if (p.speech_end_ms > 0) st->speech_end_ms = p.speech_end_ms;

    // 3) 正式路: 稳定片段必须翻出来上屏。
    //
    //    发出去的请求文本是**该句累计的原文前缀**(committed_src), 不是这一个
    //    片段: MT 需要整句上下文才能给出正确词序/搭配/指代(片段级翻译会把
    //    句子拆成逐词硬拼)。这会让请求变长(一句 n 次提交的累计发送量约
    //    O(n²)), 是拿带宽/费用换翻译质量的明确取舍。
    //
    //    队列里仍然只存"本次新增的片段": 它只用于回填 HudUpdate.src(契约要求
    //    src 是本次提交的原文片段, HUD 主行按片段长出来), 结果配对靠 FIFO。
    if (!u.committed.empty()) {
      st->committed_src += u.committed;
      st->last_commit_ms = now;
      ua->pending_commit_src.push_back(u.committed);
      ++ua->commit_seq;
      ++stats_.commits;
      to_submit.push_back(MtRequest{p.utt_id, st->committed_src, /*draft=*/false});
    }

    // 定稿帧: 句末校正的触发器。
    //
    // 为什么定稿时还要再发一次翻译: 短语级/流式 MT 对"逐段累加的前缀"和
    // "整句"给出的译文不是前缀关系(句末会重排词序), 而最后一个稳定片段
    // 的 MT 结果往往在 is_final 到达之前就回来了 —— 那时还不知道"这句说完了",
    // 没法做整行校正。所以在定稿帧补发一次整句请求: 它的结果回来时该句已标记
    // finalized, 才能合法地触发一次 kReplace 把主行校正成完整译文。
    //
    // 代价: 每句多一次翻译请求(句数级, 不是片段级), 对端侧短语 MT 可忽略,
    // 接云端 MT 时也只是每句多一次调用 —— 换来"主行最终等于整句译文"。
    // 只在"定稿帧没有新内容可提交"时补发: 如果定稿帧本身带来了新片段, 上面
    // 那条正式请求发的就已经是完整前缀, 且 finalized 会在本次调用里置位。
    if (u.final && u.committed.empty() && !p.text.empty()) {
      st->committed_src = p.text;  // 以定稿假设为准(可能修正了中间的错误字)
      ua->pending_commit_src.push_back(std::string());
      ++ua->commit_seq;
      ++stats_.commits;
      ++stats_.final_requests;
      to_submit.push_back(MtRequest{p.utt_id, st->committed_src, /*draft=*/false});
    }

    // 定稿标记: 该句之后不再接受任何草稿。已定稿行只增不改, 草稿行属于
    // "未定稿"的展示位, 定稿后再让草稿上屏就是可见的回退。
    if (u.final) ua->finalized = true;

    // 4) 草稿路: 不稳定尾部 → 只更新 HUD 的第二行。三个门限缺一不可:
    //    - 节流: 同一句两次草稿至少间隔 draft_throttle_ms。否则每个 partial
    //      (约 100ms)就是一次翻译请求, 费用/带宽翻几倍, 草稿行也来不及整行替换;
    //    - 长度上限: 尾部太长说明 ASR 迟迟不收敛, 这种草稿没有阅读价值;
    //    - 必须真的有未提交内容且尚未定稿。
    if (cfg_.draft_enabled && !u.final && !u.tail.empty()) {
      // last_draft_ms == 0 表示这句还没发过草稿: 第一次不受节流限制, 否则
      // 进程刚启动(单调时钟很小)时会被 0 - 0 < throttle 误拦掉首个草稿。
      const bool first_draft = st->last_draft_ms == 0;
      const bool throttled = (now - st->last_draft_ms) >= cfg_.draft_throttle_ms;
      const bool short_enough =
          utf8_char_count(u.tail) <= static_cast<size_t>(cfg_.max_draft_chars);
      if ((first_draft || throttled) && short_enough) {
        st->last_draft_ms = now;
        // 入队"草稿原文 + 发请求时的正式提交代数": 结果回来时逐条 FIFO 配对。
        // 节流 150ms 而 MT 往返可能 300ms, 同一句可以同时有多条草稿在途;
        // 只用一个槽位会让旧结果配到新草稿的原文(src/dst 张冠李戴)。
        ua->pending_draft_src.push_back(PendingDraft{u.tail, ua->commit_seq});
        ++stats_.drafts;
        to_submit.push_back(MtRequest{p.utt_id, u.tail, /*draft=*/true});
      }
    }
  }  // 解锁

  // 5) 锁外提交: 正式请求排在草稿前面, 保证同一 partial 内"先正式后草稿"的
  //    上屏顺序(草稿行永远反映正式行之后的未确认内容)。
  for (auto& req : to_submit) mt_.submit(req);
}

void TranslationScheduler::on_mt_result(const MtResult& r) {
  HudUpdate update;
  bool has_update = false;
  {
    std::lock_guard<std::mutex> lk(mu_);

    UttState* st = nullptr;
    for (auto& kv : utts_) {
      if (kv.first == r.utt_id) st = &kv.second;
    }
    auto it = utt_aux_.find(r.utt_id);
    UttAux* ua = (it == utt_aux_.end()) ? nullptr : &it->second;

    if (r.draft) {
      // 先取出这条结果对应的草稿请求(FIFO: MT 对同一句按提交顺序返回)。
      // 队列空说明状态被淘汰过, 只能按过时处理。
      const bool have_pending =
          (ua != nullptr) && !ua->pending_draft_src.empty();
      PendingDraft pending;
      if (have_pending) {
        pending = std::move(ua->pending_draft_src.front());
        ua->pending_draft_src.pop_front();
      }
      // 过时草稿判定 —— 稳定性优先的核心。以下任一成立就丢弃:
      //   1) 该句已定稿: 定稿行只增不改, 草稿位该清空而不是再塞旧内容;
      //   2) 发出草稿之后该句又有了正式提交: 草稿基于的原文已被更可信的
      //      内容覆盖, 再上屏就是一次用户可见的回退;
      //   3) 该句状态已被淘汰(结果太老)。
      // 判断宁可保守: 多丢一条草稿只是草稿行晚一点, 回退一次是体验事故。
      const bool stale = (st == nullptr) || !have_pending || ua->finalized ||
                         (pending.commit_seq != ua->commit_seq);
      if (stale) {
        ++stats_.stale_drafts;
        ++stats_.drafts_dropped;  // 目前草稿被丢弃的唯一原因就是过时
      } else {
        update.kind = HudUpdate::Kind::kDraft;
        update.utt_id = r.utt_id;
        update.src = std::move(pending.src);  // 这条草稿请求当时的原文 tail
        update.dst = r.text;
        update.speech_end_ms = st->speech_end_ms;
        update.at_ms = now_ms();
        has_update = true;
      }
    } else {
      // 正式译文: 这是一次"整句前缀"的重译结果, 要做差量追加。
      //
      // 队列配回"本次提交的原文片段"(只用于 HudUpdate.src; 差量计算用的是
      // 该句上一次完整译文 committed_dst, 不需要请求时的前缀)。
      // MT 后端对同一句的返回顺序与提交顺序一致(异步实现内部 FIFO), 先进先出。
      if (st == nullptr || ua == nullptr) {
        // 病态路径: 结果回来时该句状态已被淘汰(意味着 MT 比 16 句还慢)。
        // 这种情况下不能补发 —— HudState 的换句分支是"整体替换主行", 一条
        // 远落后于当前进度的 kCommitted 会把用户正在读的那一行拉回旧句,
        // 正是本项目最不能接受的闪变。宁可丢这一句(并留日志暴露问题)。
        SL_LOG(Warn) << "丢弃过晚到达的正式译文(utt=" << r.utt_id
                     << "): 调度状态已被淘汰, MT 返回速度异常";
      } else {
        std::string src;
        if (!ua->pending_commit_src.empty()) {
          src = std::move(ua->pending_commit_src.front());
          ua->pending_commit_src.pop_front();
        }

        // 差量追加: 与**已上屏文本**求公共前缀, 前缀之后才是"新增可读内容"。
        // 用码点对齐的 LCP, 保证差量拼接不会切半个汉字。
        const std::string& shown = st->committed_dst;
        const std::string lcp = longest_common_prefix_utf8(shown, r.text);
        // shown 不是新译文的前缀 ⇔ MT 在更长的前缀里重排了词序(短语级 MT 的
        // 典型行为: "告诉" 在 "告诉我" 里变成 "tell me")。此时 appended 会带上
        // 主行已经显示过的词, 直接追加就是重复/错位。
        const bool rewritten = lcp.size() < shown.size();
        const std::string appended = r.text.substr(lcp.size());
        ++stats_.mt_results;

        if (!rewritten) {
          // 纯增长: 追加, 主行只增不改。
          if (!appended.empty()) {
            st->committed_dst = r.text;
            update.kind = HudUpdate::Kind::kCommitted;
            update.utt_id = r.utt_id;
            update.src = std::move(src);
            update.dst = appended;
            update.speech_end_ms = st->speech_end_ms;
            update.at_ms = now_ms();
            has_update = true;
          }
        } else {
          // 已上屏内容被改写。两种情况下做整行校正:
          //   1) 该句已定稿(finalized): 这是这句最后一次机会, 用完整译文替换,
          //      否则主行会永远停在半句残文上;
          //   2) 主行还很短(≤2 字): 此时替换的视觉干扰可以忽略。
          // 其余情况(句内、已有较长主行)保持不动 —— 句内不做可见重排,
          // 稳定优先; 等 final 一次性校正。
          const bool can_replace = ua->finalized || utf8_char_count(shown) <= 2;
          if (can_replace && !r.text.empty()) {
            st->committed_dst = r.text;
            update.kind = HudUpdate::Kind::kReplace;
            update.utt_id = r.utt_id;
            update.src = st->committed_src;  // 整行校正给整句原文, 便于 HUD 显示
            update.dst = r.text;
            update.speech_end_ms = st->speech_end_ms;
            update.at_ms = now_ms();
            has_update = true;
          }
        }
      }
    }
  }  // 解锁

  // sink 在锁外调用。sink 按契约只该更新 HudState(很轻), 但它是外部回调,
  // 行为不可控: 万一它再调 set_sink 或查询统计, 持锁调用就会死锁。
  if (has_update) emit(update);
}

void TranslationScheduler::emit(const HudUpdate& u) {
  // 先拷贝 sink 再调用: 拷贝动作在锁内(防止 set_sink 并发改写的竞态),
  // 真正的回调在锁外, 这样 sink 里回调调度器接口也不会死锁。
  Sink sink;
  {
    std::lock_guard<std::mutex> lk(mu_);
    sink = sink_;
  }
  if (sink) sink(u);
}

TranslationScheduler::Stats TranslationScheduler::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  return stats_;
}

void TranslationScheduler::reset() {
  std::lock_guard<std::mutex> lk(mu_);
  stabilizer_.reset();
  utts_.clear();
  utt_aux_.clear();
  stats_ = Stats{};
}

}  // namespace sublens


