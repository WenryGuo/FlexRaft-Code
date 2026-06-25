#include "util.h"

#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <string>

namespace raft {
namespace util {

void Logger::Debug(LogMsgType type, const char *fmt, ...) {
  if (!debugFlag) {
    return;
  }

  switch (type) {
    case kRPC:
      if (debugRPCFlag) break;
    case kRaft:
      if (debugRaftFlag) break;
    case kEc:
      if (debugECFlag) break;
    default:
      return;
  }

  auto now = std::chrono::steady_clock::now();
  // Log in a granularity of 0.1ms
  auto elaps = std::chrono::duration_cast<std::chrono::microseconds>(now - startTimePoint_) / 100;

  va_list vaList;
  va_start(vaList, fmt);
  vsprintf(buf, fmt, vaList);
  va_end(vaList);

  std::string str(buf);
  str = std::to_string(static_cast<int>(elaps.count())) + " " + str;

  std::cout << str << std::endl;
}

void Logger::LogWithLevel(LogLevel level, LogMsgType type, const char *fmt, ...) {
  if (!debugFlag) {
    return;
  }

  switch (type) {
    case kRPC:
      if (debugRPCFlag) break;
    case kRaft:
      if (debugRaftFlag) break;
    case kEc:
      if (debugECFlag) break;
    default:
      return;
  }

  auto now = std::chrono::steady_clock::now();
  auto elaps = std::chrono::duration_cast<std::chrono::microseconds>(now - startTimePoint_) / 100;

  const char* level_str = "DEBUG";
  if (level == INFO) level_str = "INFO ";
  else if (level == WARN) level_str = "WARN ";
  else if (level == ERROR) level_str = "ERROR";

  va_list vaList;
  va_start(vaList, fmt);
  vsprintf(buf, fmt, vaList);
  va_end(vaList);

  std::string str(buf);
  str = std::to_string(static_cast<int>(elaps.count())) + " [" + level_str + "] " + str;

  std::cout << str << std::endl;
}

Logger *LoggerInstance() {
  static Logger logger;
  return &logger;
}

PerfLogger *PerfLoggerInstance() {
  static PerfLogger perf_logger("/tmp/perf.log");
  return &perf_logger;
}

}  // namespace util
}  // namespace raft
