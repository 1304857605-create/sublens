/// @file log.cpp  极简日志实现(输出到 stderr, 线程安全)
#include "sublens/common.hpp"

#include <cstdio>

namespace sublens {
namespace {

std::mutex g_log_mutex;
LogLevel g_log_level = LogLevel::kInfo;

const char* level_name(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "DBG";
    case LogLevel::kInfo: return "INF";
    case LogLevel::kWarn: return "WRN";
    case LogLevel::kError: return "ERR";
  }
  return "???";
}

}  // namespace

void set_log_level(LogLevel level) { g_log_level = level; }

LogLevel log_level() { return g_log_level; }

void log_write(LogLevel level, const std::string& msg) {
  if (level < g_log_level) return;
  static const int64_t t0 = now_ms();
  std::lock_guard<std::mutex> guard(g_log_mutex);
  std::fprintf(stderr, "[%6lld ms][%s] %s\n", static_cast<long long>(now_ms() - t0),
               level_name(level), msg.c_str());
  std::fflush(stderr);
}

}  // namespace sublens
