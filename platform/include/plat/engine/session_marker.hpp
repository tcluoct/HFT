#pragma once
// A pending reconciliation record is durable BEFORE any order can be sent.
// Never overwrite or automatically clear a prior session's unresolved record.
#include <filesystem>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace plat {
class SessionMarker {
 public:
  explicit SessionMarker(std::string path) : path_(std::move(path)) {}
  bool begin() {
    if (begun_) return true;
    std::error_code ec;
    if (std::filesystem::exists(path_, ec)) {
      prior_ = true;
      begun_ = std::filesystem::is_regular_file(path_, ec) && !ec;
      return begun_;
    }
    if (ec) return false;
    constexpr char text[] = "Reconciliation pending. If this survives a session, review broker orders, "
                            "positions and journal; stop the daemon before removing and restarting.\n";
#ifdef _WIN32
    HANDLE f = CreateFileA(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, text, sizeof(text) - 1, &written, nullptr) &&
                    written == sizeof(text) - 1 && FlushFileBuffers(f);
    const bool closed = CloseHandle(f) != 0;
    begun_ = ok && closed;
#else
    const int f = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (f < 0) return false;
    const bool ok = ::write(f, text, sizeof(text) - 1) == sizeof(text) - 1 && ::fsync(f) == 0;
    const bool closed = ::close(f) == 0;
    auto parent = std::filesystem::path(path_).parent_path();
    if (parent.empty()) parent = ".";
    const int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    const bool synced = dir >= 0 && ::fsync(dir) == 0;
    if (dir >= 0) ::close(dir);
    begun_ = ok && closed && synced;
#endif
    return begun_;
  }
  [[nodiscard]] bool preexisting() const { return prior_; }
  bool complete(bool clean) {
    if (!begun_ || prior_ || !clean) return false;
    std::error_code ec;
    return std::filesystem::remove(path_, ec) && !ec;
  }
 private:
  std::string path_;
  bool prior_ = false, begun_ = false;
};
} // namespace plat
