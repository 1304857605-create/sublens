/// @file phrase_table_mt.cpp
/// @brief 离线短语级中英双向翻译器: 整句模板 + 高频短语/词表 + 数字规则。
///
/// 定位: 不是要跟神经网络翻译比质量, 而是让整条链路在断网、无 GPU 的机器上
/// 也能真实跑起来, 并且延迟是**真实的 CPU 计算延迟**(不做 sleep 造假)。它同时
/// 是回归测试的"参考翻译器": demo 脚本里的 12 句必须逐字命中模板, 这样流水线
/// 测试才有确定的期望值。
///
/// 三层策略(从快到慢, 从准到糙):
///   1) 整句模板: 用 demo 两个场景的 source_text ↔ target_text 建表, 12 句
///      双语各 12 条。**不 include src/**, 而是在这里调
///      demo_scenario_en2zh()/zh2en() 遍历建表, 脚本改了模板自动跟着变;
///   2) 高频短语/词表: 中英双向各 80+ 条, 按"最长匹配"贪心替换, 用于没命中
///      模板的句子 —— 至少能翻出主干, 不会退化成原文照抄;
///   3) 兜底: 未命中的内容原样保留, 并在词之间补空格保证可读; 另外做一层
///      标点与数字的规则转换(英文数字↔中文数字)。
///
/// 异步模型与 ASR 后端一致: submit() 只入队, 单工作线程顺序翻译并回调,
/// 这样上层调度器看到的 MtResult 顺序与提交顺序一致(推测翻译依赖这一点)。

#include "sublens/mt.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sublens/script.hpp"

namespace sublens {
namespace {

// ---------------------------------------------------------------------------
// 基础判断与字符串小工具
// ---------------------------------------------------------------------------

bool is_ascii_word_char(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         c == '\'';
}

bool is_cjk(unsigned char c) { return c >= 0x80; }

std::size_t utf8_seq_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  return 4;
}

std::string lower_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string strip_hspace(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') out.push_back(c);
  }
  return out;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// 输出拼装: 中文之间不留空格, 英文单词之间留一个空格, 中英相邻时也留空格
/// (否则 "to蓝色" / "门票ticket" 这种粘连很难读)。英文半角标点后接英文单词
/// 也要补空格, 否则 "right,about" 会粘在一起。
void append_output(std::string& out, const std::string& token) {
  if (token.empty()) return;
  if (!out.empty()) {
    const unsigned char prev = static_cast<unsigned char>(out.back());
    const unsigned char cur = static_cast<unsigned char>(token.front());
    const bool prev_word = is_ascii_word_char(prev);
    const bool cur_word = is_ascii_word_char(cur);
    const bool prev_ascii_punct = prev < 0x80 && !prev_word && prev != ' ' && prev != '(' &&
                                  prev != '/' && prev != '\'';
    if ((prev_word && (cur_word || is_cjk(cur))) || (is_cjk(prev) && cur_word) ||
        (prev_ascii_punct && cur_word)) {
      out.push_back(' ');
    }
  }
  out += token;
}

std::string map_punct_zh_to_en(const std::string& t) {
  if (t == "，") return ",";
  if (t == "。") return ".";
  if (t == "？") return "?";
  if (t == "！") return "!";
  if (t == "：") return ":";
  if (t == "；") return ";";
  if (t == "、") return ",";
  if (t == "“" || t == "”") return "\"";
  if (t == "（") return "(";
  if (t == "）") return ")";
  return t;
}

std::string map_punct_en_to_zh(const std::string& t) {
  if (t == ",") return "，";
  if (t == ".") return "。";
  if (t == "?") return "？";
  if (t == "!") return "！";
  if (t == ":") return "：";
  if (t == ";") return "；";
  return t;
}

// ---------------------------------------------------------------------------
// 数字规则: 阿拉伯数字 ↔ 中文数字
// ---------------------------------------------------------------------------

/// 0~9999 的整数字转中文(十五、一百、一千九百零八)。
std::string zh_number(int n) {
  static const char* kDigits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  static const char* kUnits[] = {"", "十", "百", "千"};
  if (n <= 0) return "零";
  std::string out;
  bool started = false;
  bool zero_pending = false;
  for (int place = 3; place >= 0; --place) {
    int div = 1;
    for (int k = 0; k < place; ++k) div *= 10;
    const int digit = (n / div) % 10;
    if (digit == 0) {
      if (started) zero_pending = true;
      continue;
    }
    if (zero_pending) {
      out += "零";
      zero_pending = false;
    }
    // "十几" 读作"十五"而不是"一十五"
    if (digit == 1 && place == 1 && !started) {
      out += "十";
    } else {
      out += kDigits[digit];
      out += kUnits[place];
    }
    started = true;
  }
  return out;
}

/// 数字串转中文: 短数按数值读法, 长数字/带前导零按位读(像年份 1908)。
std::string digits_to_zh(const std::string& digits) {
  if (digits.empty()) return digits;
  static const char* kDigits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  const bool positional = digits.size() > 4 || (digits.size() > 1 && digits[0] == '0');
  if (!positional) {
    long value = 0;
    for (char c : digits) value = value * 10 + (c - '0');
    if (value >= 0 && value <= 9999) return zh_number(static_cast<int>(value));
  }
  std::string out;
  for (char c : digits) {
    if (c >= '0' && c <= '9') out += kDigits[c - '0'];
  }
  return out;
}

bool all_digits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 短语表数据
// ---------------------------------------------------------------------------

struct PhrasePair {
  const char* src;
  const char* dst;
};

/// 中文 → 英文: 覆盖 demo 句子里出现的词 + 高频旅游/日常词汇(80+ 条)。
constexpr PhrasePair kZhToEn[] = {
  // ---- demo 场景 2(中→英)整词覆盖 ----
  // 每个 ScriptWord 都有一个"整词"词条: 最长匹配会盖过单字条目, 因此
  // 逐词前缀翻译不会残留汉字; 长度也比单字拼更长, 满足"越说越长"。
  {"前面", "ahead"},
  {"路口", "intersection"},
  {"右转", "turn right"},
  {"大约", "about"},
  {"三百米", "three hundred meters"},
  {"就到了", "arrive there"},
  {"这座桥", "this bridge"},
  {"建于", "was built in"},
  {"一九零八年", "1908"},
  {"已经有", "has already"},
  {"一百多年", "more than a hundred years"},
  {"历史", "history"},
  {"门票", "the ticket"},
  {"是", "is"},
  {"四十元", "forty yuan"},
  {"学生", "students"},
  {"可以", "can"},
  {"打半价", "get half price"},
  {"如果您", "if you"},
  {"时间", "time"},
  {"充裕", "enough"},
  {"建议", "I suggest"},
  {"先去", "go first"},
  {"后山", "the back hill"},
  {"看日落", "see the sunset"},
  {"最后一班车", "the last bus"},
  {"晚上", "in the evening"},
  {"九点半", "half past nine"},
  {"千万别", "do not"},
  {"错过", "miss it"},
  {"祝您", "I hope you"},
  {"在这里", "here"},
  {"玩得", "enjoy"},
  {"开心", "happy"},
  // ---- 通用高频词(用于非 demo 句子的兜底) ----
  {"我", "I"},
  {"我们", "we"},
  {"你", "you"},
  {"您", "you"},
  {"你们", "you"},
  {"他", "he"},
  {"她", "she"},
  {"他们", "they"},
  {"它", "it"},
  {"我的", "my"},
  {"你的", "your"},
  {"他的", "his"},
  {"她的", "her"},
  {"我们的", "our"},
  {"这", "this"},
  {"这个", "this"},
  {"那", "that"},
  {"那个", "that"},
  {"哪里", "where"},
  {"哪儿", "where"},
  {"什么", "what"},
  {"谁", "who"},
  {"怎么", "how"},
  {"为什么", "why"},
  {"什么时候", "when"},
  {"请问", "excuse me"},
  {"请", "please"},
  {"谢谢", "thank you"},
  {"谢谢你", "thank you"},
  {"不客气", "you are welcome"},
  {"对不起", "sorry"},
  {"没关系", "it is fine"},
  {"你好", "hello"},
  {"再见", "goodbye"},
  {"不是", "is not"},
  {"有", "have"},
  {"没有", "do not have"},
  {"想", "want to"},
  {"想要", "want"},
  {"要", "want"},
  {"能", "can"},
  {"会", "will"},
  {"需要", "need"},
  {"喜欢", "like"},
  {"知道", "know"},
  {"觉得", "think"},
  {"帮我", "help me"},
  {"帮助", "help"},
  {"去", "go to"},
  {"去洗手间", "go to the bathroom"},
  {"来", "come"},
  {"走", "walk"},
  {"打车", "take a taxi"},
  {"坐", "take"},
  {"看", "see"},
  {"看见", "see"},
  {"听", "listen"},
  {"说", "say"},
  {"买", "buy"},
  {"吃", "eat"},
  {"喝", "drink"},
  {"开", "open"},
  {"关", "close"},
  {"找", "look for"},
  {"等", "wait"},
  {"到", "arrive"},
  {"从", "from"},
  {"在", "at"},
  {"和", "and"},
  {"的", "of"},
  {"吗", ""},
  {"呢", ""},
  {"吧", ""},
  {"很", "very"},
  {"太", "too"},
  {"非常", "very"},
  {"真的", "really"},
  {"好", "good"},
  {"大", "big"},
  {"小", "small"},
  {"多", "many"},
  {"少", "few"},
  {"一些", "some"},
  {"一点", "a little"},
  {"大概", "about"},
  {"已经", "already"},
  {"现在", "now"},
  {"今天", "today"},
  {"明天", "tomorrow"},
  {"昨天", "yesterday"},
  {"早上", "morning"},
  {"下午", "afternoon"},
  {"分钟", "minute"},
  {"小时", "hour"},
  {"天", "day"},
  {"星期", "week"},
  {"月", "month"},
  {"年", "year"},
  {"钱", "money"},
  {"多少钱", "how much"},
  {"多少钱一晚", "how much per night"},
  {"一晚", "per night"},
  {"价格", "price"},
  {"元", "yuan"},
  {"块", "yuan"},
  {"百", "hundred"},
  {"千", "thousand"},
  {"米", "meter"},
  {"公里", "kilometer"},
  {"洗手间", "bathroom"},
  {"厕所", "restroom"},
  {"酒店", "hotel"},
  {"宾馆", "hotel"},
  {"车站", "station"},
  {"火车站", "train station"},
  {"地铁站", "subway station"},
  {"地铁", "subway"},
  {"机场", "airport"},
  {"医院", "hospital"},
  {"银行", "bank"},
  {"商店", "shop"},
  {"超市", "supermarket"},
  {"餐厅", "restaurant"},
  {"咖啡", "coffee"},
  {"咖啡厅", "cafe"},
  {"拿铁", "latte"},
  {"茶", "tea"},
  {"水", "water"},
  {"啤酒", "beer"},
  {"饭", "rice"},
  {"面条", "noodles"},
  {"票", "ticket"},
  {"路", "road"},
  {"街", "street"},
  {"左转", "turn left"},
  {"左", "left"},
  {"右", "right"},
  {"后面", "behind"},
  {"附近", "nearby"},
  {"那里", "there"},
  {"地图", "map"},
  {"手机", "phone"},
  {"电话", "phone"},
  {"名字", "name"},
  {"问题", "question"},
  {"朋友", "friend"},
  {"老师", "teacher"},
  {"博物馆", "museum"},
  {"桥", "bridge"},
  {"山", "hill"},
  {"日落", "sunset"},
  {"瓶", "bottle"},
  {"杯", "cup"},
  {"两瓶啤酒", "two bottles of beer"},
  {"一杯咖啡", "a cup of coffee"},
  {"在哪儿", "where is"},
  {"在哪里", "where is"},
  {"洗手间在哪里", "where is the bathroom"},
  {"怎么走", "how to get"},
  {"怎么去", "how to get to"},
  {"告诉我", "tell me"},
  {"一下", "a moment"},
  {"公交车", "bus"},
  {"出租车", "taxi"},
  {"一", "one"},
  {"两", "two"},
  {"二", "two"},
  {"三", "three"},
  {"四", "four"},
  {"五", "five"},
  {"六", "six"},
  {"七", "seven"},
  {"八", "eight"},
  {"九", "nine"},
  {"十", "ten"},
  {"十五", "fifteen"},
  {"四十", "forty"},
  {"一百", "one hundred"},
  {"三百", "three hundred"},
};


/// 英文 → 中文: 与上表镜像, 补足动词/介词/数字(80+ 条)。
constexpr PhrasePair kEnToZh[] = {
  // ---- demo 场景 1(英→中)短语覆盖 ----
  // 最长匹配最多吃到 5 个词, 用于修正语序(could you tell me → 你能告诉我);
  // 单字条目保证任何前缀都能被完整覆盖, 不会把英文原样漏进中文字幕。
  {"excuse me", "请问"},
  {"excuse", "请问"},
  {"me", "我"},
  {"could you tell me", "你能告诉我"},
  {"could you", "你能"},
  {"could", "能"},
  {"you", "你"},
  {"tell me", "告诉我"},
  {"tell", "告诉"},
  {"how to get to", "怎么去"},
  {"how", "怎么"},
  {"to", ""},
  {"get", "到"},
  {"the train station", "火车站"},
  {"train station", "火车站"},
  {"the", ""},
  {"train", "火车"},
  {"station", "车站"},
  {"it is", "是"},
  {"it", "它"},
  {"is", ""},
  {"about", "大约"},
  {"fifteen minutes", "十五分钟"},
  {"fifteen", "十五"},
  {"minutes", "分钟"},
  {"on foot", "步行"},
  {"on", "在"},
  {"foot", "脚"},
  {"from here", "从这里"},
  {"from", "从"},
  {"here", "这里"},
  {"do you know", "你知道吗"},
  {"do", "做"},
  {"know", "知道"},
  {"if", "如果"},
  {"the museum", "博物馆"},
  {"museum", "博物馆"},
  {"is open", "开门"},
  {"open", "开门"},
  {"today", "今天"},
  {"i would like", "我想要"},
  {"i would", "我想"},
  {"would like", "想要"},
  {"would", "想"},
  {"i", "我"},
  {"like", "喜欢"},
  {"a large latte", "一大杯拿铁"},
  {"large latte", "大杯拿铁"},
  {"a", "一个"},
  {"large", "大"},
  {"latte", "拿铁"},
  {"with", "和"},
  {"no sugar", "不加糖"},
  {"no", "不"},
  {"sugar", "糖"},
  {"please", "请"},
  {"the weather", "天气"},
  {"weather", "天气"},
  {"really nice", "真的很好"},
  {"really", "很"},
  {"nice", "好"},
  {"for", ""},
  {"a walk", "散步"},
  {"walk", "散步"},
  {"thank you so much", "非常感谢"},
  {"thank you", "谢谢"},
  {"thank", "感谢"},
  {"so much", "非常"},
  {"so", "这么"},
  {"much", "多"},
  {"your help", "你的帮助"},
  {"your", "你的"},
  {"help", "帮助"},
  // ---- 通用高频词(非 demo 句子兜底) ----
  {"we", "我们"},
  {"they", "他们"},
  {"he", "他"},
  {"she", "她"},
  {"him", "他"},
  {"them", "他们"},
  {"us", "我们"},
  {"my", "我的"},
  {"his", "他的"},
  {"her", "她的"},
  {"our", "我们的"},
  {"this", "这"},
  {"these", "这些"},
  {"that", "那"},
  {"those", "那些"},
  {"what", "什么"},
  {"where", "哪里"},
  {"when", "什么时候"},
  {"who", "谁"},
  {"why", "为什么"},
  {"which", "哪个"},
  {"how much", "多少钱"},
  {"how much is", "多少钱"},
  {"are", "是"},
  {"am", "是"},
  {"was", "是"},
  {"were", "是"},
  {"be", "是"},
  {"is not", "不是"},
  {"are not", "不是"},
  {"do not", "不"},
  {"don't", "不"},
  {"not", "不"},
  {"have", "有"},
  {"has", "有"},
  {"had", "有"},
  {"do not have", "没有"},
  {"can", "可以"},
  {"will", "会"},
  {"should", "应该"},
  {"must", "必须"},
  {"may", "可以"},
  {"want", "想要"},
  {"need", "需要"},
  {"think", "觉得"},
  {"see", "看"},
  {"look", "看"},
  {"look for", "找"},
  {"go", "去"},
  {"go to", "去"},
  {"come", "来"},
  {"walk to", "走到"},
  {"take", "乘坐"},
  {"eat", "吃"},
  {"drink", "喝"},
  {"buy", "买"},
  {"close", "关门"},
  {"wait", "等"},
  {"arrive", "到达"},
  {"find", "找到"},
  {"turn left", "左转"},
  {"turn right", "右转"},
  {"on the left", "在左边"},
  {"on the right", "在右边"},
  {"left", "左"},
  {"right", "右"},
  {"around", "大约"},
  {"already", "已经"},
  {"very", "非常"},
  {"too", "太"},
  {"a little", "一点"},
  {"some", "一些"},
  {"many", "很多"},
  {"few", "少"},
  {"good", "好"},
  {"bad", "坏"},
  {"big", "大"},
  {"small", "小"},
  {"long", "长"},
  {"short", "短"},
  {"new", "新"},
  {"old", "旧"},
  {"now", "现在"},
  {"tomorrow", "明天"},
  {"yesterday", "昨天"},
  {"morning", "早上"},
  {"afternoon", "下午"},
  {"evening", "晚上"},
  {"night", "夜晚"},
  {"time", "时间"},
  {"weather", "天气"},
  {"minute", "分钟"},
  {"hour", "小时"},
  {"hours", "小时"},
  {"day", "天"},
  {"days", "天"},
  {"week", "星期"},
  {"month", "月"},
  {"year", "年"},
  {"years", "年"},
  {"money", "钱"},
  {"price", "价格"},
  {"yuan", "元"},
  {"hundred", "百"},
  {"thousand", "千"},
  {"half", "半"},
  {"half price", "半价"},
  {"meter", "米"},
  {"meters", "米"},
  {"kilometer", "公里"},
  {"kilometers", "公里"},
  {"bathroom", "洗手间"},
  {"restroom", "洗手间"},
  {"toilet", "厕所"},
  {"hotel", "酒店"},
  {"airport", "机场"},
  {"hospital", "医院"},
  {"bank", "银行"},
  {"shop", "商店"},
  {"store", "商店"},
  {"supermarket", "超市"},
  {"restaurant", "餐厅"},
  {"cafe", "咖啡厅"},
  {"coffee", "咖啡"},
  {"tea", "茶"},
  {"water", "水"},
  {"beer", "啤酒"},
  {"rice", "饭"},
  {"noodles", "面条"},
  {"ticket", "门票"},
  {"tickets", "门票"},
  {"road", "路"},
  {"street", "街"},
  {"intersection", "路口"},
  {"next", "下一个"},
  {"corner", "拐角"},
  {"nearby", "附近"},
  {"near", "附近"},
  {"there", "那里"},
  {"ahead", "前面"},
  {"behind", "后面"},
  {"map", "地图"},
  {"phone", "手机"},
  {"name", "名字"},
  {"question", "问题"},
  {"friend", "朋友"},
  {"student", "学生"},
  {"students", "学生"},
  {"teacher", "老师"},
  {"bridge", "桥"},
  {"hill", "山"},
  {"sunset", "日落"},
  {"history", "历史"},
  {"bottle", "瓶"},
  {"bottles", "瓶"},
  {"cup", "杯"},
  {"cups", "杯"},
  {"one", "一"},
  {"two", "二"},
  {"three", "三"},
  {"four", "四"},
  {"five", "五"},
  {"six", "六"},
  {"seven", "七"},
  {"eight", "八"},
  {"nine", "九"},
  {"ten", "十"},
  {"eleven", "十一"},
  {"twelve", "十二"},
  {"twenty", "二十"},
  {"thirty", "三十"},
  {"forty", "四十"},
  {"fifty", "五十"},
  {"sixty", "六十"},
  {"of", "的"},
  {"in", "在"},
  {"at", "在"},
  {"and", "和"},
  {"or", "或"},
  {"but", "但是"},
  {"thanks", "谢谢"},
  {"sorry", "对不起"},
  {"hello", "你好"},
  {"hi", "你好"},
  {"goodbye", "再见"},
  {"bye", "再见"},
  {"yes", "是"},
  {"ok", "好的"},
  {"okay", "好的"},
  {"by taxi", "打车"},
  {"take a taxi", "打车"},
  {"could you tell me how to get to", "你能告诉我怎么去"},
  {"is there", "有没有"},
  {"there is", "有"},
  {"there are", "有"},
  {"where is the bathroom", "洗手间在哪里"},
  {"bus", "公交车"},
  {"taxi", "出租车"},
  {"two bottles of beer", "两瓶啤酒"},
  {"a cup of coffee", "一杯咖啡"},
  {"the train station", "火车站"},
};


/// 英文短语需要按"词序列"匹配, 这里把 key 预先切成小写词元。
struct EnPhrase {
  std::vector<std::string> src_tokens;
  std::string dst;
};

/// 英文分词的产物: 保留原文区间, 未命中的内容才能原样回填。
struct EnToken {
  std::size_t begin = 0;
  std::size_t end = 0;
  bool word = false;
  std::string lower;
};

std::vector<EnToken> tokenize_en(const std::string& s) {
  std::vector<EnToken> out;
  std::size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++i;
      continue;
    }
    EnToken t;
    t.begin = i;
    if (is_ascii_word_char(c)) {
      while (i < s.size() && is_ascii_word_char(static_cast<unsigned char>(s[i]))) ++i;
      t.end = i;
      t.word = true;
      t.lower = lower_ascii(s.substr(t.begin, t.end - t.begin));
    } else {
      // 标点(包括输入里可能混入的中文标点)单独成一个 token
      i = std::min(s.size(), i + utf8_seq_len(c));
      t.end = i;
      t.word = false;
      t.lower = s.substr(t.begin, t.end - t.begin);
    }
    out.push_back(std::move(t));
  }
  return out;
}

}  // namespace

struct PhraseTableMt::Impl {
  explicit Impl(PhraseMtConfig c) : cfg(std::move(c)) {
    // 语言方向: 源语言以 "en" 开头 → 英译中, 其余(默认 zh) → 中译英
    zh_to_en = (cfg.source_lang.rfind("en", 0) != 0);
    build_templates();
    build_phrases();
  }

  PhraseMtConfig cfg;
  bool zh_to_en = true;

  // 整句模板: 原文 → 译文。exact 区分原文, norm 去掉空白后再查一次,
  // 用来吃掉"脚本词表拼接后逗号后丢空格"这类格式差异(见 build_templates)。
  std::unordered_map<std::string, std::string> zh2en_exact;
  std::unordered_map<std::string, std::string> zh2en_norm;
  std::unordered_map<std::string, std::string> en2zh_exact;
  std::unordered_map<std::string, std::string> en2zh_norm;

  std::vector<std::pair<std::string, std::string>> zh_phrases;
  std::vector<EnPhrase> en_phrases;

  // 异步执行(与 ASR 后端相同的单工作线程模型)
  mutable std::mutex mu;
  std::condition_variable cv;
  std::deque<MtRequest> queue;
  Callback cb;
  std::thread worker;
  bool started = false;
  bool stopping = false;
  bool busy = false;

  ~Impl() { stop(); }

  // ---- 建表 -------------------------------------------------------------

  static void add_template(std::unordered_map<std::string, std::string>& exact,
                           std::unordered_map<std::string, std::string>& norm,
                           const std::string& src, const std::string& dst) {
    if (src.empty() || dst.empty()) return;
    exact.emplace(src, dst);
    norm.emplace(strip_hspace(src), dst);
  }

  /// 从 demo 脚本建整句模板: 两个场景的 source↔target 双向入表,
  /// 于是中→英和英→中各有 12 条模板。脚本改了这里自动跟着变, 不存在两份数据。
  void build_templates() {
    const ConversationScript& en2zh = demo_scenario_en2zh();
    const ConversationScript& zh2en = demo_scenario_zh2en();
    for (const auto& u : en2zh.utterances) {
      add_template(en2zh_exact, en2zh_norm, u.source_text, u.target_text);
      add_template(zh2en_exact, zh2en_norm, u.target_text, u.source_text);
    }
    for (const auto& u : zh2en.utterances) {
      add_template(zh2en_exact, zh2en_norm, u.source_text, u.target_text);
      add_template(en2zh_exact, en2zh_norm, u.target_text, u.source_text);
    }
  }

  void build_phrases() {
    for (const auto& p : kZhToEn) zh_phrases.emplace_back(p.src, p.dst);
    for (const auto& p : kEnToZh) {
      EnPhrase ep;
      ep.dst = p.dst;
      // key 用空格分隔的词序列; 表里都是 ASCII, 按空格切即可
      const std::string key = p.src;
      std::size_t begin = 0;
      while (begin < key.size()) {
        const std::size_t end = key.find(' ', begin);
        const std::size_t stop = (end == std::string::npos) ? key.size() : end;
        if (stop > begin) ep.src_tokens.push_back(key.substr(begin, stop - begin));
        begin = (end == std::string::npos) ? key.size() : end + 1;
      }
      if (!ep.src_tokens.empty()) en_phrases.push_back(std::move(ep));
    }
  }

  // ---- 翻译 -------------------------------------------------------------

  std::string translate(const std::string& text) const {
    if (text.empty()) return std::string();
    if (zh_to_en) {
      auto it = zh2en_exact.find(text);
      if (it != zh2en_exact.end()) return it->second;
      it = zh2en_norm.find(strip_hspace(text));
      if (it != zh2en_norm.end()) return it->second;
      return translate_zh_to_en(text);
    }
    auto it = en2zh_exact.find(text);
    if (it != en2zh_exact.end()) return it->second;
    it = en2zh_norm.find(strip_hspace(text));
    if (it != en2zh_norm.end()) return it->second;
    return translate_en_to_zh(text);
  }

  /// 中→英: 按 UTF-8 码点推进, 每个位置尝试最长短语匹配; 未命中的内容原样保留。
  std::string translate_zh_to_en(const std::string& text) const {
    // 中文不需要空格, 但脚本里的 source_text 标点后可能有空格; 去掉后再匹配,
    // 让"词表拼接出来的 ASR 文本"也能命中同一套短语/模板。
    const std::string in = strip_hspace(text);
    std::string out;
    std::size_t i = 0;
    while (i < in.size()) {
      // 最长匹配: 词表不大(180 条), 线性扫描足够快, 且延迟是真实 CPU 时间
      const std::pair<std::string, std::string>* hit = nullptr;
      for (const auto& p : zh_phrases) {
        if (p.first.size() > in.size() - i) continue;
        if (in.compare(i, p.first.size(), p.first) != 0) continue;
        if (hit == nullptr || p.first.size() > hit->first.size()) hit = &p;
      }
      if (hit != nullptr) {
        append_output(out, hit->second);
        i += hit->first.size();
        continue;
      }
      const unsigned char c = static_cast<unsigned char>(in[i]);
      if (is_ascii_word_char(c)) {
        // 未知的连续 ASCII 内容整段保留, 避免 "wifi" 被拆成 "w i f i"
        std::size_t j = i;
        while (j < in.size() && is_ascii_word_char(static_cast<unsigned char>(in[j]))) ++j;
        append_output(out, in.substr(i, j - i));
        i = j;
      } else if (c < 0x80) {
        append_output(out, map_punct_zh_to_en(std::string(1, static_cast<char>(c))));
        ++i;
      } else {
        const std::size_t n = std::min(in.size() - i, utf8_seq_len(c));
        append_output(out, map_punct_zh_to_en(in.substr(i, n)));
        i += n;
      }
    }
    // 中文疑问语气词(吗/呢)在词表里映射为空, 这里补回问号, 否则看不出在提问
    if (!out.empty() && out.back() != '?' && out.back() != '.' && out.back() != '!') {
      if (ends_with(text, "吗") || ends_with(text, "呢")) out.push_back('?');
    }
    return out;
  }

  /// 英→中: 词序列最长匹配; 未命中的英文原样保留; 数字转中文读法。
  std::string translate_en_to_zh(const std::string& text) const {
    const std::vector<EnToken> tokens = tokenize_en(text);
    std::string out;
    std::size_t i = 0;
    while (i < tokens.size()) {
      const EnPhrase* hit = nullptr;
      for (const auto& p : en_phrases) {
        if (p.src_tokens.size() > tokens.size() - i) continue;
        bool matched = true;
        for (std::size_t k = 0; k < p.src_tokens.size(); ++k) {
          const EnToken& t = tokens[i + k];
          if (!t.word || t.lower != p.src_tokens[k]) {
            matched = false;
            break;
          }
        }
        if (!matched) continue;
        if (hit == nullptr || p.src_tokens.size() > hit->src_tokens.size()) hit = &p;
      }
      if (hit != nullptr) {
        append_output(out, hit->dst);
        i += hit->src_tokens.size();
        continue;
      }
      const EnToken& t = tokens[i];
      const std::string raw = text.substr(t.begin, t.end - t.begin);
      if (t.word) {
        append_output(out, all_digits(t.lower) ? digits_to_zh(t.lower) : raw);
      } else {
        append_output(out, map_punct_en_to_zh(raw));
      }
      ++i;
    }
    return out;
  }

  // ---- 异步执行 ---------------------------------------------------------

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

  void submit(MtRequest req) {
    {
      std::lock_guard<std::mutex> lk(mu);
      if (stopping) return;
      start_locked();
      queue.push_back(std::move(req));
    }
    cv.notify_one();
  }

  void flush() {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [this] { return stopping || (queue.empty() && !busy); });
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lk(mu);
      stopping = true;
    }
    cv.notify_all();
    if (worker.joinable()) worker.join();
    std::deque<MtRequest> dropped;
    {
      std::lock_guard<std::mutex> lk(mu);
      dropped.swap(queue);
      busy = false;
    }
  }

  void run() {
    for (;;) {
      MtRequest req;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return stopping || !queue.empty(); });
        if (stopping) return;
        req = std::move(queue.front());
        queue.pop_front();
        busy = true;
      }
      // 注意: 这里没有 sleep —— 翻译耗时就是真实 CPU 时间, 延迟统计不会被造假
      MtResult result;
      result.utt_id = req.utt_id;
      result.draft = req.draft;
      result.text = translate(req.text);
      result.emit_ms = now_ms();

      // 回调在锁外执行; flush() 要等回调返回才算"排空", 所以 busy 在回调之后清
      Callback c;
      bool cancelled = false;
      {
        std::lock_guard<std::mutex> lk(mu);
        cancelled = stopping;
        if (!cancelled) c = cb;
      }
      if (!cancelled && c) c(result);
      {
        std::lock_guard<std::mutex> lk(mu);
        busy = false;
      }
      cv.notify_all();
      if (cancelled) return;
    }
  }
};

PhraseTableMt::PhraseTableMt(Config cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}

PhraseTableMt::~PhraseTableMt() = default;

void PhraseTableMt::set_callback(Callback cb) { impl_->set_callback(std::move(cb)); }

bool PhraseTableMt::start() { return impl_->start(); }

void PhraseTableMt::submit(MtRequest req) { impl_->submit(std::move(req)); }

void PhraseTableMt::flush() { impl_->flush(); }

void PhraseTableMt::stop() { impl_->stop(); }

std::string PhraseTableMt::translate_sync(const std::string& text) const {
  return impl_->translate(text);
}

}  // namespace sublens
