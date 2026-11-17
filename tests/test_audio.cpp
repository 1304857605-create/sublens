/// @file test_audio.cpp  音频输入层测试(含 SPSC 环形缓冲的并发/回绕测试)
///
/// 约定: 合成音源的时间线是后面所有回归测试的基准, 所以这里对"第几帧是语音"
/// 做逐帧断言, 而不是只检查"有没有声音"。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "sublens/audio_source.hpp"
#include "sublens/ring_buffer.hpp"
#include "sublens/script.hpp"
#include "tiny_test.hpp"

namespace {

using sublens::kFrameMs;
using sublens::kFrameSamples;
using sublens::kSampleRate;

constexpr int kSamplesPerMs = kSampleRate / 1000;  ///< 16

/// 判"这一帧有语音"的阈值: 底噪约 -58dBFS, 而句子末尾常出现只含 10ms 语音
/// 且正在淡出的半帧(约 -30~-32dBFS), 所以门限取 -35, 仍比底噪高 23dB。
constexpr double kSpeechFrameDb = -35.0;

/// 帧 RMS → dBFS, 与 EnergyVad 内部同口径, 用来断言"这一帧有没有语音能量"。
double frame_db(const int16_t* pcm, int n) {
  if (!pcm || n <= 0) return -96.0;
  double sum_sq = 0.0;
  for (int i = 0; i < n; ++i) {
    const double s = static_cast<double>(pcm[i]);
    sum_sq += s * s;
  }
  const double rms = std::sqrt(sum_sq / n) / 32768.0;
  return rms > 1e-9 ? 20.0 * std::log10(rms) : -96.0;
}

/// 20ms 一帧把音源读完, 返回每帧的 dBFS。
std::vector<double> read_all_frames_db(sublens::IAudioSource& src, int64_t* total_samples) {
  std::vector<double> dbs;
  std::vector<int16_t> buf(kFrameSamples);
  int64_t samples = 0;
  for (;;) {
    const int n = src.read(buf.data(), kFrameSamples);
    if (n < 0) break;
    SL_EXPECT_EQ(n, kFrameSamples);  // 内置脚本的总时长都是 20ms 的整数倍
    samples += n;
    dbs.push_back(frame_db(buf.data(), n));
  }
  if (total_samples) *total_samples = samples;
  return dbs;
}

// ---------------------------------------------------------------------------
// 临时 WAV 素材构造
// ---------------------------------------------------------------------------
void write_le16(std::ofstream& f, uint16_t v) {
  const char bytes[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
  f.write(bytes, 2);
}

void write_le32(std::ofstream& f, uint32_t v) {
  const char bytes[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                         static_cast<char>((v >> 16) & 0xFF),
                         static_cast<char>((v >> 24) & 0xFF)};
  f.write(bytes, 4);
}

/// 写一个最小 RIFF/WAVE: 只写 fmt + data, 内容按 samples 交错存放。
/// bits/rate/channels 故意可变, 用来构造"不支持格式"的负例。
void write_wav(const std::string& path, int channels, int rate, int bits,
               const std::vector<int16_t>& samples) {
  std::ofstream f(path, std::ios::binary);
  const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);
  f.write("RIFF", 4);
  write_le32(f, 36 + data_bytes);
  f.write("WAVE", 4);
  f.write("fmt ", 4);
  write_le32(f, 16);
  write_le16(f, 1);  // PCM
  write_le16(f, static_cast<uint16_t>(channels));
  write_le32(f, static_cast<uint32_t>(rate));
  write_le32(f, static_cast<uint32_t>(rate * channels * 2));
  write_le16(f, static_cast<uint16_t>(channels * 2));
  write_le16(f, static_cast<uint16_t>(bits));
  f.write("data", 4);
  write_le32(f, data_bytes);
  for (int16_t s : samples) write_le16(f, static_cast<uint16_t>(s));
}

std::string temp_wav_path(const char* tag) {
  static int counter = 0;
  const std::string name =
      std::string("sublens_test_") + tag + "_" + std::to_string(++counter) + ".wav";
  return (std::filesystem::temp_directory_path() / name).string();
}

void remove_file(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

}  // namespace

// ---------------------------------------------------------------------------
// SpscRing: 已实现好的传输通道, 只测不改
// ---------------------------------------------------------------------------
SL_TEST(SpscRing_push_pop_basic) {
  sublens::SpscRing<int, 8> ring;
  SL_EXPECT(ring.empty());

  const int in[5] = {1, 2, 3, 4, 5};
  SL_EXPECT_EQ(ring.push(in, 5), size_t{5});
  SL_EXPECT_EQ(ring.size(), size_t{5});

  int out[8] = {};
  SL_EXPECT_EQ(ring.pop(out, 3), size_t{3});
  SL_EXPECT_EQ(out[0], 1);
  SL_EXPECT_EQ(out[2], 3);
  SL_EXPECT_EQ(ring.pop(out, 8), size_t{2});
  SL_EXPECT_EQ(out[0], 4);
  SL_EXPECT_EQ(out[1], 5);
  SL_EXPECT(ring.empty());
  SL_EXPECT_EQ(ring.pop(out, 1), size_t{0});
}

SL_TEST(SpscRing_wrap_around) {
  sublens::SpscRing<int, 4> ring;
  const int a[3] = {10, 11, 12};
  SL_EXPECT_EQ(ring.push(a, 3), size_t{3});
  int out[4] = {};
  SL_EXPECT_EQ(ring.pop(out, 3), size_t{3});
  // 此时读写指针都在 3, 再写 2 个元素会跨过容量边界
  const int b[2] = {20, 21};
  SL_EXPECT_EQ(ring.push(b, 2), size_t{2});
  SL_EXPECT_EQ(ring.size(), size_t{2});
  SL_EXPECT_EQ(ring.pop(out, 4), size_t{2});
  SL_EXPECT_EQ(out[0], 20);
  SL_EXPECT_EQ(out[1], 21);
  SL_EXPECT(ring.empty());
}

SL_TEST(SpscRing_dropped_when_full) {
  sublens::SpscRing<int, 4> ring;
  const int in[6] = {0, 1, 2, 3, 4, 5};
  // 契约: 每个"想写但没写进去"的元素都计入 dropped(含部分写入时剩下的部分)
  SL_EXPECT_EQ(ring.push(in, 6), size_t{4});  // 只塞得下 4 个
  SL_EXPECT_EQ(ring.dropped(), uint64_t{2});
  SL_EXPECT_EQ(ring.push(in, 3), size_t{0});  // 满了, 整批被拒
  SL_EXPECT_EQ(ring.dropped(), uint64_t{5});

  int out[8] = {};
  SL_EXPECT_EQ(ring.pop(out, 8), size_t{4});
  SL_EXPECT_EQ(ring.push(in, 1), size_t{1});  // 腾出空间后恢复
  SL_EXPECT_EQ(ring.pop(out, 1), size_t{1});
  ring.clear();
  SL_EXPECT_EQ(ring.dropped(), uint64_t{0});
  SL_EXPECT(ring.empty());
}

SL_TEST(SpscRing_multithread_1m_samples) {
  constexpr uint32_t kTotal = 1000000;
  constexpr size_t kChunk = 64;
  sublens::SpscRing<uint32_t, 1024> ring;
  std::atomic<uint32_t> short_pushes{0};
  uint64_t mismatches = 0;  // 只有消费者线程写

  std::thread producer([&] {
    uint32_t chunk[kChunk];
    uint32_t next = 0;
    while (next < kTotal) {
      const uint32_t want = std::min<uint32_t>(kChunk, kTotal - next);
      size_t space = ring.space();
      while (space == 0) {
        std::this_thread::yield();  // 消费者会腾出空间
        space = ring.space();
      }
      // 用 space() 限制本次写入量: ring_buffer.hpp 的契约是"没写进去的元素都计入
      // dropped", 所以生产者必须自己按可写空间裁剪, 否则 retry 也会被记成丢弃。
      // 这样 1M 样本跑完 dropped() 必须严格为 0, 顺带验证 space() 的实现。
      const size_t n = std::min<size_t>(want, space);
      for (size_t i = 0; i < n; ++i) chunk[i] = next + static_cast<uint32_t>(i);
      if (ring.push(chunk, n) != n) {
        short_pushes.fetch_add(1, std::memory_order_relaxed);
      }
      next += static_cast<uint32_t>(n);
    }
  });

  uint32_t buffer[kChunk];
  uint32_t received = 0;
  while (received < kTotal) {
    const size_t got = ring.pop(buffer, kChunk);
    for (size_t i = 0; i < got; ++i) {
      if (buffer[i] != received + i) ++mismatches;
    }
    received += static_cast<uint32_t>(got);
    if (got == 0) std::this_thread::yield();
  }
  producer.join();

  SL_EXPECT_EQ(short_pushes.load(), uint32_t{0});
  SL_EXPECT_EQ(mismatches, uint64_t{0});
  SL_EXPECT_EQ(received, kTotal);
  SL_EXPECT_EQ(ring.dropped(), uint64_t{0});
  SL_EXPECT(ring.empty());
}

// ---------------------------------------------------------------------------
// ScriptedAudioSource: 时间线对齐
// ---------------------------------------------------------------------------
namespace {

/// 逐句检查: 语音起点前一帧、句内所有帧、语音末帧后一帧的能量。
/// 帧边界误差必须 < 20ms, 也就是首帧/末帧完全落在脚本区间内。
void check_script_alignment(const sublens::ConversationScript& script) {
  sublens::ScriptedAudioSource::Config cfg;
  cfg.script = &script;
  cfg.realtime_pacing = false;

  sublens::ScriptedAudioSource src(cfg);
  SL_EXPECT(src.start());
  SL_EXPECT_EQ(std::string(src.name()), std::string("scripted-audio"));

  int64_t total_samples = 0;
  const std::vector<double> dbs = read_all_frames_db(src, &total_samples);

  SL_EXPECT_EQ(total_samples, static_cast<int64_t>(script.total_ms()) * kSamplesPerMs);
  SL_EXPECT_EQ(static_cast<int64_t>(dbs.size()),
               static_cast<int64_t>(script.total_ms()) / kFrameMs);

  for (const auto& u : script.utterances) {
    const int first = u.start_ms / kFrameMs;
    const int last = (u.start_ms + u.speech_ms() - 1) / kFrameMs;
    SL_EXPECT(first >= 0);
    SL_EXPECT(last < static_cast<int>(dbs.size()));

    if (first > 0) SL_EXPECT(dbs[static_cast<size_t>(first - 1)] < -30.0);
    for (int f = first; f <= last; ++f) {
      // 句内不应出现能量深坑: 否则 VAD 会在句子中间误判句末
      SL_EXPECT(dbs[static_cast<size_t>(f)] > kSpeechFrameDb);
    }
    if (last + 1 < static_cast<int>(dbs.size())) {
      SL_EXPECT(dbs[static_cast<size_t>(last + 1)] < -30.0);
    }
  }

  std::vector<int16_t> buf(kFrameSamples);
  SL_EXPECT_EQ(src.read(buf.data(), kFrameSamples), -1);  // EOF 之后一直 -1
  SL_EXPECT_EQ(src.read(buf.data(), kFrameSamples), -1);
  src.stop();
  SL_EXPECT_EQ(src.read(buf.data(), kFrameSamples), -1);
}

}  // namespace

SL_TEST(ScriptedAudioSource_timeline_en2zh) {
  check_script_alignment(sublens::demo_scenario_en2zh());
}

SL_TEST(ScriptedAudioSource_timeline_zh2en) {
  check_script_alignment(sublens::demo_scenario_zh2en());
}

SL_TEST(ScriptedAudioSource_is_deterministic) {
  sublens::ScriptedAudioSource::Config cfg;
  cfg.script = &sublens::demo_scenario_en2zh();
  cfg.realtime_pacing = false;

  sublens::ScriptedAudioSource a(cfg);
  sublens::ScriptedAudioSource b(cfg);
  SL_EXPECT(a.start());
  SL_EXPECT(b.start());

  std::vector<int16_t> buf_a(kFrameSamples);
  std::vector<int16_t> buf_b(kFrameSamples);
  int compared = 0;
  while (compared < 1000) {
    const int na = a.read(buf_a.data(), kFrameSamples);
    const int nb = b.read(buf_b.data(), kFrameSamples);
    SL_EXPECT_EQ(na, nb);
    if (na <= 0) break;
    SL_EXPECT(std::equal(buf_a.begin(), buf_a.begin() + na, buf_b.begin()));
    compared += na;
  }
  SL_EXPECT(compared >= 1000);
}

SL_TEST(ScriptedAudioSource_realtime_pacing) {
  sublens::ScriptedAudioSource::Config cfg;
  cfg.script = &sublens::demo_scenario_en2zh();
  cfg.realtime_pacing = true;

  sublens::ScriptedAudioSource src(cfg);
  SL_EXPECT(src.start());
  const sublens::TimePoint t0 = sublens::SteadyClock::now();
  std::vector<int16_t> buf(kFrameSamples);
  for (int i = 0; i < 5; ++i) SL_EXPECT_EQ(src.read(buf.data(), kFrameSamples), kFrameSamples);
  const int64_t elapsed = sublens::elapsed_ms(t0);
  // 5 帧 = 100ms; 允许调度抖动, 但不能"瞬间跑完"
  SL_EXPECT(elapsed >= 70);
  SL_EXPECT(elapsed <= 600);
}

SL_TEST(ScriptedAudioSource_rejects_null_script) {
  sublens::ScriptedAudioSource src(sublens::ScriptedAudioSource::Config{});
  SL_EXPECT(!src.start());
}

// ---------------------------------------------------------------------------
// SilenceSource
// ---------------------------------------------------------------------------
SL_TEST(SilenceSource_is_zero_and_ends) {
  sublens::SilenceSource src(100);  // 100ms = 1600 样本
  SL_EXPECT_EQ(std::string(src.name()), std::string("silence"));
  SL_EXPECT(src.start());

  std::vector<int16_t> buf(kFrameSamples);
  int64_t total = 0;
  bool all_zero = true;
  for (;;) {
    const int n = src.read(buf.data(), kFrameSamples);
    if (n < 0) break;
    SL_EXPECT_EQ(n, kFrameSamples);
    for (int i = 0; i < n; ++i) all_zero = all_zero && (buf[static_cast<size_t>(i)] == 0);
    total += n;
  }
  SL_EXPECT_EQ(total, int64_t{1600});
  SL_EXPECT(all_zero);
  SL_EXPECT_EQ(src.read(buf.data(), kFrameSamples), -1);
}

// ---------------------------------------------------------------------------
// WavFileSource
// ---------------------------------------------------------------------------
SL_TEST(WavFileSource_mono_roundtrip) {
  const std::string path = temp_wav_path("mono");
  constexpr int kCount = 1600;  // 5 帧
  std::vector<int16_t> samples(kCount);
  for (int i = 0; i < kCount; ++i) samples[static_cast<size_t>(i)] = static_cast<int16_t>((i * 37) % 2001 - 1000);
  write_wav(path, 1, kSampleRate, 16, samples);

  auto src = sublens::WavFileSource::open(path);
  SL_EXPECT(src != nullptr);
  if (src) {
    SL_EXPECT_EQ(std::string(src->name()), std::string("wav-file"));
    SL_EXPECT(src->start());

    std::vector<int16_t> got;
    std::vector<int16_t> buf(kFrameSamples);
    for (;;) {
      const int n = src->read(buf.data(), kFrameSamples);
      if (n < 0) break;
      SL_EXPECT_EQ(n, kFrameSamples);
      got.insert(got.end(), buf.begin(), buf.begin() + n);
    }
    SL_EXPECT_EQ(got.size(), samples.size());
    SL_EXPECT(std::equal(got.begin(), got.end(), samples.begin()));
    SL_EXPECT_EQ(src->read(buf.data(), kFrameSamples), -1);
  }
  remove_file(path);
}

SL_TEST(WavFileSource_stereo_uses_left_channel) {
  const std::string path = temp_wav_path("stereo");
  constexpr int kCount = 640;  // 2 帧
  std::vector<int16_t> left(kCount);
  std::vector<int16_t> interleaved;
  interleaved.reserve(static_cast<size_t>(kCount) * 2);
  for (int i = 0; i < kCount; ++i) {
    left[static_cast<size_t>(i)] = static_cast<int16_t>((i * 13) % 1501 - 750);
    interleaved.push_back(left[static_cast<size_t>(i)]);
    interleaved.push_back(static_cast<int16_t>(-left[static_cast<size_t>(i)]));  // 右声道反相
  }
  write_wav(path, 2, kSampleRate, 16, interleaved);

  auto src = sublens::WavFileSource::open(path);
  SL_EXPECT(src != nullptr);
  if (src) {
    SL_EXPECT(src->start());
    std::vector<int16_t> got;
    std::vector<int16_t> buf(kFrameSamples);
    for (;;) {
      const int n = src->read(buf.data(), kFrameSamples);
      if (n < 0) break;
      got.insert(got.end(), buf.begin(), buf.begin() + n);
    }
    SL_EXPECT_EQ(got.size(), left.size());
    SL_EXPECT(std::equal(got.begin(), got.end(), left.begin()));
  }
  remove_file(path);
}

SL_TEST(WavFileSource_rejects_unsupported) {
  std::vector<int16_t> samples(320, 1000);

  const std::string eight_bit = temp_wav_path("eight_bit");
  write_wav(eight_bit, 1, kSampleRate, 8, samples);
  SL_EXPECT(sublens::WavFileSource::open(eight_bit) == nullptr);
  remove_file(eight_bit);

  const std::string wrong_rate = temp_wav_path("wrong_rate");
  write_wav(wrong_rate, 1, 44100, 16, samples);
  SL_EXPECT(sublens::WavFileSource::open(wrong_rate) == nullptr);
  remove_file(wrong_rate);

  const std::string three_channels = temp_wav_path("three_channels");
  write_wav(three_channels, 3, kSampleRate, 16, samples);
  SL_EXPECT(sublens::WavFileSource::open(three_channels) == nullptr);
  remove_file(three_channels);

  // 不存在 / 不是 RIFF 的文件
  SL_EXPECT(sublens::WavFileSource::open("no_such_file_for_sublens_test.wav") == nullptr);
  const std::string garbage = temp_wav_path("garbage");
  {
    std::ofstream f(garbage, std::ios::binary);
    f.write("this is definitely not a wav file", 33);
  }
  SL_EXPECT(sublens::WavFileSource::open(garbage) == nullptr);
  remove_file(garbage);
}
