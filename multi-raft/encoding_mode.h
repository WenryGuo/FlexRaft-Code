#pragma once
// encoding_mode.h — multi-raft 写入路径的三种编码模式
//
// 三种模式（N = 2F + 1）：
//   kRsF  : RS(F+1, F)        total = 2F+1 = N        1 frag/node  commit=2F+1
//   kRs3F : RS(F+1, 3F+1)     total = 4F+2 = 2N       2 frag/node  commit=ceil((3F+1)/2)
//   kLrc  : LRC(F+1, l=2, r=2N-k-l)
//                              total = 2N             2 frag/node  commit=ceil((3F+1)/2)
//
// 选择方式：bench_server_multiraft.cc 的 --encoding=RS_F|RS_3F|LRC gflag。
// 同一二进制内三条路径都编译进去，运行时按 mode 分支。

#include <cstdint>
#include <string>

namespace multiraft {

enum class EncodingMode : uint8_t {
  kRsF  = 0,  // RS(F+1, F)
  kRs3F = 1,  // RS(F+1, 3F+1)
  kLrc  = 2,  // LRC(F+1, l, 2N-k-l)
};

inline const char* EncodingModeName(EncodingMode m) {
  switch (m) {
    case EncodingMode::kRsF:  return "RS_F";
    case EncodingMode::kRs3F: return "RS_3F";
    case EncodingMode::kLrc:  return "LRC";
  }
  return "?";
}

// Parse a CLI string ("RS_F" / "RS_3F" / "LRC", case-insensitive) into an
// EncodingMode. Returns false on unrecognized input.
inline bool ParseEncodingMode(const std::string& s, EncodingMode* out) {
  std::string up;
  up.reserve(s.size());
  for (char c : s) up.push_back(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
  if (up == "RS_F"  || up == "RSF")  { *out = EncodingMode::kRsF;  return true; }
  if (up == "RS_3F" || up == "RS3F") { *out = EncodingMode::kRs3F; return true; }
  if (up == "LRC")                   { *out = EncodingMode::kLrc;  return true; }
  return false;
}

}  // namespace multiraft
