/// @file wav_file_source.cpp  16-bit PCM WAV 文件音源
///
/// 用途: 把真实录音接进流水线做回归测试 —— 合成音频能测逻辑, 但测不出
/// 真实底噪、唇齿音、房间混响对 VAD 的影响。
///
/// 格式约束(超出范围直接 open() 失败, 不做隐式重采样/位深转换):
///   - RIFF/WAVE, PCM(format=1), 16-bit;
///   - 采样率必须 16 kHz: 静默重采样会掩盖采样率配置错误, 也让延迟统计失真;
///   - 1 或 2 声道。
///
/// 双声道取左声道: 16k 立体声录音绝大多数是"麦克风 L / 参考 R",
/// 左声道才是目标说话人; 取平均会在两声道反相时把语音抵消掉。

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

#include "sublens/audio_source.hpp"

namespace sublens {
namespace {

constexpr int kBytesPerSample = 2;  ///< 16-bit
constexpr int kWavHeaderBytes = 44;
constexpr size_t kMaxChannels = 2;
/// 一帧最多需要的字节数(双声道 16-bit)
constexpr size_t kFrameBytes = static_cast<size_t>(kFrameSamples) * kMaxChannels * kBytesPerSample;

uint16_t load_u16_le(const unsigned char* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t load_u32_le(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

struct WavFileSource::Impl {
  std::ifstream file;
  int64_t data_offset = 0;  ///< data chunk 第一个字节的文件偏移
  int64_t data_bytes = 0;   ///< data chunk 字节数
  int64_t bytes_read = 0;   ///< 已经消费的 data 字节数
  int channels = 1;
  bool running = false;

  // 逐帧缓存: 保证 read() 的帧边界和采集线程一致
  std::array<int16_t, kFrameSamples> frame{};
  int frame_len = 0;
  int frame_pos = 0;

  bool read_exact(void* dst, size_t n) {
    file.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    return file.gcount() == static_cast<std::streamsize>(n);
  }

  /// 跳过 n 字节(RIFF 块是偶数字节对齐的, 奇数长度要额外跳 1 字节)
  bool skip_chunk(uint32_t size) {
    file.seekg(static_cast<std::streamoff>(size + (size & 1u)), std::ios::cur);
    return file.good();
  }

  bool parse(const std::string& path) {
    file.open(path, std::ios::binary);
    if (!file) return false;

    unsigned char riff[12] = {};
    if (!read_exact(riff, sizeof(riff))) return false;
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
      return false;
    }

    bool have_fmt = false;
    while (!have_fmt || data_bytes == 0) {
      unsigned char chunk[8] = {};
      if (!read_exact(chunk, sizeof(chunk))) return false;
      const uint32_t size = load_u32_le(chunk + 4);

      if (std::memcmp(chunk, "fmt ", 4) == 0) {
        if (have_fmt || size < 16) return false;
        unsigned char fmt[16] = {};
        if (!read_exact(fmt, sizeof(fmt))) return false;
        if (size > 16 && !skip_chunk(size - 16)) return false;
        if (size < 16) return false;

        const uint16_t audio_format = load_u16_le(fmt);       // 1 = PCM
        const uint16_t channel_count = load_u16_le(fmt + 2);
        const uint32_t sample_rate = load_u32_le(fmt + 4);
        const uint16_t bits = load_u16_le(fmt + 14);
        if (audio_format != 1 || bits != 16 || sample_rate != kSampleRate ||
            channel_count < 1 || channel_count > kMaxChannels) {
          return false;  // 不做隐式转换, 让调用方明确知道素材不合规
        }
        channels = static_cast<int>(channel_count);
        have_fmt = true;
      } else if (std::memcmp(chunk, "data", 4) == 0) {
        if (!have_fmt) return false;  // 规范要求 fmt 在 data 之前
        data_offset = static_cast<int64_t>(file.tellg());
        data_bytes = static_cast<int64_t>(size);
        break;
      } else {
        if (!skip_chunk(size)) return false;
      }
    }
    return data_bytes > 0;
  }

  /// 从文件读一帧到 frame, 返回样本数; 0 表示 EOF。
  int load_next_frame() {
    while (true) {
      const int64_t remain = data_bytes - bytes_read;
      if (remain <= 0) return 0;
      // 一帧最多 kFrameSamples 个样本: 不按"最大双声道字节数"算 want,
      // 否则 mono 文件会一次读进 640 个样本, 写穿 320 槽的缓存。
      const int64_t frame_bytes = static_cast<int64_t>(channels) * kBytesPerSample;
      const int64_t max_bytes = static_cast<int64_t>(kFrameSamples) * frame_bytes;
      const int64_t want = std::min(max_bytes, remain);

      std::array<unsigned char, kFrameBytes> raw{};
      file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(want));
      const std::streamsize got = file.gcount();
      const int samples =
          std::min(static_cast<int>(got / frame_bytes), kFrameSamples);
      if (samples <= 0) return 0;

      // 双声道只取左声道(见文件头注释)
      for (int i = 0; i < samples; ++i) {
        const unsigned char* p = raw.data() + static_cast<size_t>(i) * frame_bytes;
        frame[static_cast<size_t>(i)] = static_cast<int16_t>(load_u16_le(p));
      }
      bytes_read += static_cast<int64_t>(samples) * frame_bytes;
      frame_len = samples;
      frame_pos = 0;
      return samples;
    }
  }
};

std::unique_ptr<WavFileSource> WavFileSource::open(const std::string& path) {
  auto impl = std::make_unique<Impl>();
  if (!impl->parse(path)) return nullptr;
  auto source = std::make_unique<WavFileSource>();
  source->impl_ = std::move(impl);
  return source;
}

WavFileSource::~WavFileSource() = default;

bool WavFileSource::start() {
  if (!impl_) return false;
  impl_->file.clear();
  impl_->file.seekg(static_cast<std::streamoff>(impl_->data_offset));
  if (!impl_->file.good()) return false;
  impl_->bytes_read = 0;
  impl_->frame_len = 0;
  impl_->frame_pos = 0;
  impl_->running = true;
  return true;
}

void WavFileSource::stop() {
  if (impl_) impl_->running = false;
}

int WavFileSource::read(int16_t* out, int max_samples) {
  if (!impl_ || !out || max_samples <= 0) return 0;
  if (!impl_->running) return -1;

  int written = 0;
  while (written < max_samples) {
    if (impl_->frame_pos >= impl_->frame_len) {
      if (impl_->load_next_frame() == 0) {
        return written > 0 ? written : -1;
      }
    }
    const int n = std::min(max_samples - written, impl_->frame_len - impl_->frame_pos);
    std::copy_n(impl_->frame.data() + impl_->frame_pos, n, out + written);
    impl_->frame_pos += n;
    written += n;
  }
  return written;
}

}  // namespace sublens
