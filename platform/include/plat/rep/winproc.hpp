#pragma once
// plat/rep/winproc.hpp — run one PowerShell command and block for it.
//
// docx_fill.hpp needs to open and repack the .docx zip container. Rather
// than vendor a DEFLATE codec into a tool whose entire job is "never corrupt
// the document a regulator opens", it shells out to Expand-Archive /
// Compress-Archive — built into every Windows box since PowerShell 5.0,
// backed by .NET's System.IO.Compression.ZipFile. That trade lowers
// correctness risk (battle-tested implementation, not a hand-rolled inflate)
// at the cost of one subprocess call; for a report generated a few times a
// year, not the trading hot path, that's the right side of the trade.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace plat::rep {

// Escapes `s` for a PowerShell single-quoted literal: doubling an embedded
// single quote is the only escape a PS '...' string needs.
inline std::wstring ps_quote_w(const std::wstring& s) {
  std::wstring out = L"'";
  for (wchar_t c : s) {
    if (c == L'\'')
      out += L"''";
    else
      out += c;
  }
  out += L"'";
  return out;
}

// Runs `powershell -NoProfile -NonInteractive -Command <script>` and blocks
// until it exits. Returns the process exit code, or -1 if it could not be
// launched at all.
inline int run_powershell(const std::wstring& script) {
  std::wstring cmd =
      L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command " + script;
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
    return -1;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return static_cast<int>(code);
}

}  // namespace plat::rep

#endif  // _WIN32
