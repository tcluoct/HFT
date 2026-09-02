#pragma once
// plat/rep/win_paths.hpp — UTF-8 <-> UTF-16 path conversion.
//
// plat::Config reads the .cfg file as raw bytes (it's UTF-8, undecoded) and
// hands back std::string values verbatim. The access report's own template
// path is Chinese by the regulator's own naming convention
// (附件95-3：外部系统接入报告（程序化）.docx), so a config-supplied path can
// contain non-ASCII text. Building std::filesystem::path (or any Win32 call)
// from that std::string via the *narrow* APIs would transcode through the
// process's active code page — not necessarily UTF-8 — and silently mangle
// the path. Every filesystem path this tool touches goes through
// path_from_utf8() instead, which converts explicitly via MultiByteToWideChar
// and builds the path from the wide string (exact, no code-page guessing).

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <string>

namespace plat::rep {

inline std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  const int n =
      MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

inline std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr,
                                     0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                       nullptr);
  return s;
}

inline std::filesystem::path path_from_utf8(const std::string& s) {
  return std::filesystem::path(utf8_to_wide(s));
}

}  // namespace plat::rep

#endif  // _WIN32
