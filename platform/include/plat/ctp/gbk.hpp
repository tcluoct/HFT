#pragma once
// plat/ctp/gbk.hpp — CTP's text fields (ErrorMsg, instrument names) are GBK;
// the console and every log file here are UTF-8 (init_console_utf8), so a
// raw ErrorMsg like 资金不足 prints as mojibake. Cold path only: used when
// logging a CTP error, never on the order path.

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace plat::ctp {

inline std::string gbk_to_utf8(const char* gbk) {
  if (!gbk || !*gbk) return {};
#ifdef _WIN32
  const int wn = MultiByteToWideChar(936, 0, gbk, -1, nullptr, 0);
  if (wn <= 0) return gbk;
  std::wstring w(static_cast<std::size_t>(wn), L'\0');
  MultiByteToWideChar(936, 0, gbk, -1, w.data(), wn);
  const int un = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (un <= 0) return gbk;
  std::string u(static_cast<std::size_t>(un), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, u.data(), un, nullptr, nullptr);
  u.resize(static_cast<std::size_t>(un - 1));  // drop the terminator
  return u;
#else
  return gbk;  // libiconv not linked on the Linux build; pass through
#endif
}

}  // namespace plat::ctp
