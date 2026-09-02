#pragma once
// plat/core/crash.hpp — post-mortem crash tracing for the long-running
// binaries (platrec, platd live/shards). Yesterday's lesson: a process that
// dies with only an exit code costs a day of guessing; a process that dies
// with a named reason, a symbolized stack, and a minidump costs ten minutes
// in the debugger.
//
// install_crash_handlers(app) hooks every way a process can die abnormally:
//   · hardware faults / SEH  (access violation, illegal instruction, stack
//     overflow, int divide, /GS fastfail …)   — SetUnhandledExceptionFilter
//   · uncaught C++ exceptions                 — std::set_terminate
//   · abort() (assert, CRT hard errors)       — SIGABRT
//   · pure virtual call / bad CRT parameter   — _set_purecall_handler etc.
// On POSIX the same coverage comes from sigaction(SEGV/BUS/ILL/FPE/ABRT).
//
// Every path writes through plat::logf — which run_* has already teed into
// the session's UNBUFFERED log file (plat/core/log.hpp), so the post-mortem
// is on disk before the process is allowed to die. On Windows it also drops
// a minidump (<app>_crash_<date>_<pid>.dmp) loadable in VS/WinDbg, then
// exits with the exception code so a supervisor (platd --fleet) can report
// the same named reason from the outside via exception_code_name().
//
// The handlers allocate nothing and take no locks of ours; DbgHelp is
// initialized lazily inside the (already-dying) process. A reentrancy gate
// makes a crash inside the crash handler terminate instead of recursing.

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "plat/core/log.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// dbghelp.h needs windows.h first; link is via pragma so no build changes.
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <execinfo.h>
#include <unistd.h>
#endif

namespace plat {

// Name a Windows exception / NTSTATUS exit code. Also used by the fleet
// supervisor to decode a dead shard's exit code into the same words the
// shard's own log used. Returns nullptr for codes that aren't crashes.
inline const char* exception_code_name(unsigned long code) {
  switch (code) {
    case 0xC0000005: return "ACCESS_VIOLATION";
    case 0xC000001D: return "ILLEGAL_INSTRUCTION";
    case 0xC0000025: return "NONCONTINUABLE_EXCEPTION";
    case 0xC000008C: return "ARRAY_BOUNDS_EXCEEDED";
    case 0xC000008E: return "FLT_DIVIDE_BY_ZERO";
    case 0xC0000090: return "FLT_INVALID_OPERATION";
    case 0xC0000091: return "FLT_OVERFLOW";
    case 0xC0000094: return "INT_DIVIDE_BY_ZERO";
    case 0xC0000095: return "INT_OVERFLOW";
    case 0xC0000096: return "PRIV_INSTRUCTION";
    case 0xC00000FD: return "STACK_OVERFLOW";
    case 0xC0000374: return "HEAP_CORRUPTION";
    case 0xC0000409: return "FASTFAIL/STACK_BUFFER_OVERRUN";
    case 0xC0000420: return "ASSERTION_FAILURE";
    case 0xE06D7363: return "UNCAUGHT_CPP_EXCEPTION";
    case 0x80000003: return "BREAKPOINT";
    case 3:          return "ABORT";
    default:         return nullptr;
  }
}

namespace crash_detail {

inline const char*& app_tag() {
  static const char* tag = "plat";
  return tag;
}

// One thread gets to run the post-mortem; a fault inside it must not recurse.
inline bool enter_once() {
  static std::atomic<int> entered{0};
  return entered.exchange(1) == 0;
}

#ifdef _WIN32

// Symbolize and log the stack of `ctx` (the FAULTING context — not the
// handler's own frames). With PDBs beside the exe this gives function +
// file:line; without, module+offset still locates the fault after the fact.
inline void log_stack(CONTEXT* ctx) {
  const HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(proc, nullptr, TRUE);

  STACKFRAME64 fr{};
  fr.AddrPC.Mode = fr.AddrFrame.Mode = fr.AddrStack.Mode = AddrModeFlat;
#if defined(_M_X64)
  const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
  fr.AddrPC.Offset = ctx->Rip;
  fr.AddrFrame.Offset = ctx->Rbp;
  fr.AddrStack.Offset = ctx->Rsp;
#elif defined(_M_ARM64)
  const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
  fr.AddrPC.Offset = ctx->Pc;
  fr.AddrFrame.Offset = ctx->Fp;
  fr.AddrStack.Offset = ctx->Sp;
#endif

  CONTEXT walk = *ctx;  // StackWalk64 mutates the context it walks
  for (int i = 0; i < 64; ++i) {
    if (!StackWalk64(machine, proc, GetCurrentThread(), &fr, &walk, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
      break;
    if (fr.AddrPC.Offset == 0) break;

    char sym_buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp64 = 0;

    char mod[MAX_PATH] = "?";
    const DWORD64 base = SymGetModuleBase64(proc, fr.AddrPC.Offset);
    if (base) {
      GetModuleFileNameA(reinterpret_cast<HMODULE>(base), mod, sizeof(mod));
      const char* slash = mod;
      for (const char* p = mod; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p + 1;
      if (slash != mod) std::memmove(mod, slash, std::strlen(slash) + 1);
    }

    if (SymFromAddr(proc, fr.AddrPC.Offset, &disp64, sym)) {
      IMAGEHLP_LINE64 line{};
      line.SizeOfStruct = sizeof(line);
      DWORD disp32 = 0;
      if (SymGetLineFromAddr64(proc, fr.AddrPC.Offset, &disp32, &line))
        logf("[crash]   #%02d %s!%s+0x%llx  (%s:%lu)\n", i, mod, sym->Name,
             static_cast<unsigned long long>(disp64), line.FileName, line.LineNumber);
      else
        logf("[crash]   #%02d %s!%s+0x%llx\n", i, mod, sym->Name,
             static_cast<unsigned long long>(disp64));
    } else if (base) {
      logf("[crash]   #%02d %s+0x%llx\n", i, mod,
           static_cast<unsigned long long>(fr.AddrPC.Offset - base));
    } else {
      logf("[crash]   #%02d 0x%llx\n", i,
           static_cast<unsigned long long>(fr.AddrPC.Offset));
    }
  }
}

// Drop <app>_crash_<yyyymmdd_hhmmss>_<pid>.dmp beside the log: open it in
// VS/WinDbg for full state at the faulting instruction.
inline void write_minidump(EXCEPTION_POINTERS* xp) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char path[256];
  std::snprintf(path, sizeof(path), "%s_crash_%04u%02u%02u_%02u%02u%02u_%lu.dmp", app_tag(),
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                GetCurrentProcessId());
  const HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    logf("[crash] minidump: cannot create %s (err %lu)\n", path, GetLastError());
    return;
  }
  MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), xp, FALSE};
  const auto type = static_cast<MINIDUMP_TYPE>(
      MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
      MiniDumpWithUnloadedModules);
  const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                    xp ? &mei : nullptr, nullptr, nullptr);
  CloseHandle(file);
  logf("[crash] minidump %s -> %s\n", ok ? "written" : "FAILED", path);
}

inline void post_mortem(const char* how, EXCEPTION_POINTERS* xp, unsigned long exit_code) {
  logf("[crash] ================ FATAL: %s ================\n", how);
  logf("[crash] thread %lu, pid %lu\n", GetCurrentThreadId(), GetCurrentProcessId());
  if (xp) log_stack(xp->ContextRecord);
  write_minidump(xp);
  logf("[crash] terminating with code 0x%08lx\n", exit_code);
  TerminateProcess(GetCurrentProcess(), exit_code);
}

// An uncaught C++ exception reaches the filter as SEH 0xE06D7363 (it never
// gets to std::terminate), with the thrown object at ExceptionInformation[1].
// If that object derives from std::exception, the virtual what() names the
// failure ("invalid stoll argument" beats a bare code). A thrown non-class
// (or a torn record) faults — the __except swallows it and the caller falls
// back to the generic line. No C++ unwinding in here (MSVC C2712 rule).
inline bool fmt_cpp_exception(char* how, std::size_t n, const EXCEPTION_RECORD& er) {
  __try {
    const auto* ex = reinterpret_cast<const std::exception*>(er.ExceptionInformation[1]);
    return std::snprintf(how, n, "uncaught C++ exception: %s", ex->what()) > 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

inline LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* xp) {
  if (!enter_once()) TerminateProcess(GetCurrentProcess(), 0xC0000409u);
  const auto& er = *xp->ExceptionRecord;
  const unsigned long code = er.ExceptionCode;
  const char* name = exception_code_name(code);
  char how[256];
  if (code == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
    const char* op = er.ExceptionInformation[0] == 0   ? "reading"
                     : er.ExceptionInformation[0] == 1 ? "writing"
                                                       : "executing";
    std::snprintf(how, sizeof(how), "ACCESS_VIOLATION %s address 0x%llx (ip 0x%llx)", op,
                  static_cast<unsigned long long>(er.ExceptionInformation[1]),
                  reinterpret_cast<unsigned long long>(er.ExceptionAddress));
  } else if (code == 0xE06D7363u && er.NumberParameters >= 2 &&
             er.ExceptionInformation[0] == 0x19930520 &&  // MSVC throw magic
             fmt_cpp_exception(how, sizeof(how), er)) {
    // `how` holds "uncaught C++ exception: <what()>"
  } else {
    std::snprintf(how, sizeof(how), "exception 0x%08lx%s%s at ip 0x%llx", code,
                  name ? " " : "", name ? name : "",
                  reinterpret_cast<unsigned long long>(er.ExceptionAddress));
  }
  post_mortem(how, xp, code);
  return EXCEPTION_EXECUTE_HANDLER;  // unreachable
}

// terminate / abort / purecall arrive without an EXCEPTION_POINTERS; capture
// the CURRENT context so the stack shows who called them.
inline void die_here(const char* how, unsigned long exit_code) {
  if (!enter_once()) TerminateProcess(GetCurrentProcess(), exit_code);
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  RtlCaptureContext(&ctx);
  EXCEPTION_RECORD er{};
  er.ExceptionCode = exit_code;
#if defined(_M_X64)
  er.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip);
#elif defined(_M_ARM64)
  er.ExceptionAddress = reinterpret_cast<void*>(ctx.Pc);
#endif
  EXCEPTION_POINTERS xp{&er, &ctx};
  post_mortem(how, &xp, exit_code);
}

inline void on_terminate() {
  char how[192] = "std::terminate (uncaught exception, unknown type)";
  if (auto ex = std::current_exception()) {
    try {
      std::rethrow_exception(ex);
    } catch (const std::exception& e) {
      std::snprintf(how, sizeof(how), "std::terminate — uncaught std::exception: %s", e.what());
    } catch (...) {
    }
  } else {
    std::snprintf(how, sizeof(how), "std::terminate (no active exception)");
  }
  die_here(how, 0xE06D7363u);
}

inline void on_abort(int) { die_here("abort() called (assert/CRT hard error)", 3); }
inline void on_purecall() { die_here("pure virtual function call", 0xC0000025u); }
inline void on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned,
                                 std::uintptr_t) {
  die_here("CRT invalid parameter", 0xC000000Du);
}

#else  // ---- POSIX: signal handlers + glibc backtrace, then default re-raise

inline void posix_handler(int sig, siginfo_t* si, void*) {
  if (!enter_once()) _exit(128 + sig);
  logf("[crash] ================ FATAL: signal %d (%s), fault addr %p ================\n", sig,
       strsignal(sig), si ? si->si_addr : nullptr);
  void* frames[64];
  const int n = backtrace(frames, 64);
  backtrace_symbols_fd(frames, n, STDERR_FILENO);          // async-signal-safe
  if (std::FILE* f = log_file_slot()) {
    fflush(f);
    backtrace_symbols_fd(frames, n, fileno(f));            // same trace into the log
  }
  logf("[crash] re-raising for core dump, exit %d\n", 128 + sig);
  signal(sig, SIG_DFL);
  raise(sig);
}

#endif

}  // namespace crash_detail

// Install every abnormal-death hook. Call FIRST in main(), before threads
// exist, with the binary's tag (used to name the minidump). The hooks are
// process-wide, so faults on CTP api threads and the writer/journal threads
// are traced identically.
inline void install_crash_handlers(const char* app) {
  crash_detail::app_tag() = app;
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);  // die here, not in WerFault
  SetUnhandledExceptionFilter(&crash_detail::unhandled_filter);
  std::set_terminate(&crash_detail::on_terminate);
  std::signal(SIGABRT, &crash_detail::on_abort);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // our handler, not WER's
  _set_purecall_handler(&crash_detail::on_purecall);
  _set_invalid_parameter_handler(&crash_detail::on_invalid_parameter);
#else
  struct sigaction sa{};
  sa.sa_sigaction = &crash_detail::posix_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
  std::set_terminate([] {
    logf("[crash] std::terminate — uncaught exception\n");
    std::abort();  // falls into the SIGABRT handler above for the backtrace
  });
#endif
}

}  // namespace plat
