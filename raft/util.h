#pragma once
#include <sys/select.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

namespace raft {
namespace util {
using TimePoint = decltype(std::chrono::high_resolution_clock::now());
using std::chrono::microseconds;
using std::chrono::milliseconds;
class Timer {
 public:
  Timer() : start_time_point_(std::chrono::high_resolution_clock::now()) {}
  ~Timer() = default;

  void Reset() { start_time_point_ = std::chrono::high_resolution_clock::now(); }

  int64_t ElapseMicroseconds() const {
    auto time_now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<microseconds>(time_now - start_time_point_).count();
  }

  int64_t ElapseMilliseconds() const {
    auto time_now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<milliseconds>(time_now - start_time_point_).count();
  }

 private:
  using TimePoint = decltype(std::chrono::high_resolution_clock::now());
  TimePoint start_time_point_;
};

enum LogMsgType { kRPC = 1, kRaft = 2, kEc = 3 };

enum LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

#ifndef LOG_LEVEL
#define LOG_LEVEL INFO
#endif

class Logger {
  // Enable debug messages or not. False means all types
  // of messages will be ignored
  // static const bool debugFlag = ENABLE_RAFT_LOG;
#ifdef ENABLE_LOG
  static const bool debugFlag = true;
#else
  static const bool debugFlag = false;
#endif

  // On debugFlag = true, enable RPC related messages
  static const bool debugRPCFlag = false;

  // On debugRaftFlag = true, enable Raft logic related messages
  static const bool debugRaftFlag = true;

  // On debugECFlag = true, enable EC logic related messages, including
  // the parameter k, m change; the # of live servers change and so on
  static const bool debugECFlag = true;

 public:
  Logger() : startTimePoint_(std::chrono::steady_clock::now()) {}

  void Debug(LogMsgType type, const char *fmt, ...);
  void LogWithLevel(LogLevel level, LogMsgType type, const char *fmt, ...);

  // Reset the start timepoint of this debugger
  void Reset();

 private:
  decltype(std::chrono::steady_clock::now()) startTimePoint_;
  char buf[512]{};
};

struct PerfCounter {
  using TimePoint = decltype(std::chrono::high_resolution_clock::now());
  virtual std::string ToString() const = 0;
  virtual void Record() = 0;
};

class PerfLogger {
 public:
  PerfLogger(const std::string &perf_file_path) { file_ = new std::ofstream(perf_file_path); }

 public:
  void Report(const PerfCounter *perf_counter) { *file_ << perf_counter->ToString() << "\n"; }

 private:
  std::ofstream *file_;
};

struct AppendEntriesRPCPerfCounter final : public PerfCounter {
  // Default constructor
  AppendEntriesRPCPerfCounter(uint64_t size)
      : start_time(std::chrono::high_resolution_clock::now()), transfer_size(size), pass_time(0) {}

  AppendEntriesRPCPerfCounter(const AppendEntriesRPCPerfCounter &rhs)
      : start_time(rhs.start_time), transfer_size(rhs.transfer_size), pass_time(0) {}

  TimePoint start_time;
  uint64_t transfer_size;
  uint64_t pass_time;

  std::string ToString() const override {
    char buf[512];
    sprintf(buf, "[AppendEntriesPerfRPCCounter: transfer_size(%lu) time(%lu us)]",
            this->transfer_size, this->pass_time);
    return std::string(buf);
  }

  void Record() override {
    auto end = std::chrono::high_resolution_clock::now();
    pass_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start_time).count();
  }
};

struct PersistencePerfCounter final : public PerfCounter {
  // Default constructor
  PersistencePerfCounter(uint64_t size)
      : start_time(std::chrono::high_resolution_clock::now()), persist_size(size), pass_time(0) {}

  TimePoint start_time;
  uint64_t persist_size;
  uint64_t pass_time;

  std::string ToString() const override {
    char buf[512];
    sprintf(buf, "[PersistencePerfCounter: persist_size(%lu) time(%lu us)]", this->persist_size,
            this->pass_time);
    return std::string(buf);
  }

  void Record() override {
    auto end = std::chrono::high_resolution_clock::now();
    pass_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start_time).count();
  }
};

struct RaftAppendEntriesProcessPerfCounter final : public PerfCounter {
  TimePoint start_time;
  uint64_t process_size;
  uint64_t pass_time;

  RaftAppendEntriesProcessPerfCounter(uint64_t size)
      : start_time(std::chrono::high_resolution_clock::now()), process_size(size), pass_time(0) {}

  std::string ToString() const override {
    char buf[512];
    sprintf(buf,
            "[RaftAppendEntriesProcessPerfCounter: process_size(%lu) "
            "time(%lu us)]",
            this->process_size, this->pass_time);
    return std::string(buf);
  }

  void Record() override {
    auto end = std::chrono::high_resolution_clock::now();
    pass_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start_time).count();
  }
};

struct EncodingEntryPerfCounter final : public PerfCounter {
  TimePoint start_time;
  // uint64_t encoding_size;
  int encoding_k, encoding_m;
  uint64_t pass_time;

  EncodingEntryPerfCounter(int k, int m)
      : start_time(std::chrono::high_resolution_clock::now()), encoding_k(k), encoding_m(m) {}

  std::string ToString() const override {
    char buf[512];
    sprintf(buf,
            "[EncodingEntryPerfCounter]: encoding_parameters(k=%d m=%d) "
            "time(%lu us)]",
            this->encoding_k, this->encoding_m, this->pass_time);
    return std::string(buf);
  }

  void Record() override {
    auto end = std::chrono::high_resolution_clock::now();
    pass_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start_time).count();
  }
};

inline std::string MakeKey(uint64_t key_id, size_t key_size) {
  std::string key = "key" + std::to_string(key_id);
  key.append(key_size - key.size(), '0');
  return key;
}

inline std::string MakeValue(uint64_t value_id, size_t value_size) {
  std::string value = "value" + std::to_string(value_id);
  value.append(value_size - value.size(), '0');
  return value;
}

inline TimePoint NowTime() { return std::chrono::high_resolution_clock::now(); }

inline int64_t DurationToMicros(TimePoint start, TimePoint end) {
  return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

// Use singleton to access the global-only logger
Logger *LoggerInstance();
PerfLogger *PerfLoggerInstance();
}  // namespace util

#define LOG(msg_type, format, ...)                  \
  {                                                 \
    auto logger = raft::util::LoggerInstance();     \
    logger->Debug(msg_type, format, ##__VA_ARGS__); \
  }

#define LOG_IF_LEVEL(level, msg_type, format, ...)                    \
  {                                                                   \
    if (level >= LOG_LEVEL) {                                        \
      auto logger = raft::util::LoggerInstance();                     \
      logger->LogWithLevel(level, msg_type, format, ##__VA_ARGS__);   \
    }                                                                 \
  }

#define LOG_DEBUG(msg_type, format, ...)   LOG_IF_LEVEL(raft::util::DEBUG, msg_type, format, ##__VA_ARGS__)
#define LOG_INFO(msg_type, format, ...)    LOG_IF_LEVEL(raft::util::INFO, msg_type, format, ##__VA_ARGS__)
#define LOG_WARN(msg_type, format, ...)    LOG_IF_LEVEL(raft::util::WARN, msg_type, format, ##__VA_ARGS__)
#define LOG_ERROR(msg_type, format, ...)   LOG_IF_LEVEL(raft::util::ERROR, msg_type, format, ##__VA_ARGS__)

#define PERF_LOG(perf_counter)                           \
  {                                                      \
    auto perf_logger = raft::util::PerfLoggerInstance(); \
    perf_logger->Report(perf_counter);                   \
  }
}  // namespace raft
