#pragma once
// Named shared-memory segments: the substrate the bus lives in.
//
// Windows: pagefile-backed CreateFileMapping under the "Local\" (per-session)
// namespace; the segment exists while at least one handle is open. POSIX:
// shm_open + mmap; the creator unlinks the name on destruction. Both zero-
// fill new pages, which the bus protocol relies on (stamp 0 == never
// written).
//
// create() is exclusive: if the name already exists it fails rather than
// silently attaching to a stale bus from a crashed run — the one shared-
// memory failure mode that produces silent garbage instead of an error.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "hft/common/platform.hpp"

#if !defined(_WIN32)
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace sb {

class SharedSegment {
 public:
  SharedSegment() = default;
  SharedSegment(const SharedSegment&) = delete;
  SharedSegment& operator=(const SharedSegment&) = delete;

  SharedSegment(SharedSegment&& o) noexcept { swap(o); }
  SharedSegment& operator=(SharedSegment&& o) noexcept {
    if (this != &o) {
      close();
      swap(o);
    }
    return *this;
  }

  ~SharedSegment() { close(); }

  // Create a new zero-initialized segment. Fails if the name already exists.
  static SharedSegment create(const std::string& name, std::size_t bytes) {
    return make(name, bytes, /*exclusive_create=*/true);
  }

  // Attach to a segment another process (or thread) created.
  static SharedSegment open(const std::string& name, std::size_t bytes) {
    return make(name, bytes, /*exclusive_create=*/false);
  }

  [[nodiscard]] bool valid() const noexcept { return mem_ != nullptr; }
  [[nodiscard]] void* data() const noexcept { return mem_; }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_; }

  void close() noexcept {
#if defined(_WIN32)
    if (mem_) ::UnmapViewOfFile(mem_);
    if (handle_) ::CloseHandle(handle_);
    handle_ = nullptr;
#else
    if (mem_) ::munmap(mem_, bytes_);
    if (owner_ && !name_.empty()) ::shm_unlink(name_.c_str());
#endif
    mem_ = nullptr;
    bytes_ = 0;
    owner_ = false;
    name_.clear();
  }

 private:
  static SharedSegment make(const std::string& name, std::size_t bytes, bool exclusive_create) {
    SharedSegment s;
#if defined(_WIN32)
    const std::string full = "Local\\sb." + name;
    HANDLE h = nullptr;
    if (exclusive_create) {
      h = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                               static_cast<DWORD>(bytes >> 32),
                               static_cast<DWORD>(bytes & 0xFFFFFFFFu), full.c_str());
      if (h && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(h);
        return s;
      }
    } else {
      h = ::OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, full.c_str());
    }
    if (!h) return s;
    void* mem = ::MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (!mem) {
      ::CloseHandle(h);
      return s;
    }
    s.handle_ = h;
    s.mem_ = mem;
#else
    const std::string full = "/sb." + name;
    const int flags = exclusive_create ? (O_CREAT | O_EXCL | O_RDWR) : O_RDWR;
    const int fd = ::shm_open(full.c_str(), flags, 0600);
    if (fd < 0) return s;
    if (exclusive_create && ::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
      ::close(fd);
      ::shm_unlink(full.c_str());
      return s;
    }
    void* mem = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);  // the mapping keeps the segment alive
    if (mem == MAP_FAILED) {
      if (exclusive_create) ::shm_unlink(full.c_str());
      return s;
    }
    s.mem_ = mem;
    s.name_ = full;
#endif
    s.bytes_ = bytes;
    s.owner_ = exclusive_create;
    return s;
  }

  void swap(SharedSegment& o) noexcept {
    std::swap(mem_, o.mem_);
    std::swap(bytes_, o.bytes_);
    std::swap(owner_, o.owner_);
    std::swap(name_, o.name_);
#if defined(_WIN32)
    std::swap(handle_, o.handle_);
#endif
  }

  void* mem_ = nullptr;
  std::size_t bytes_ = 0;
  bool owner_ = false;
  std::string name_;
#if defined(_WIN32)
  HANDLE handle_ = nullptr;
#endif
};

}  // namespace sb
