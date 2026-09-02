#pragma once
// Read-only memory-mapped file.
//
// The read-side trick of every serious tick store: mapping makes the whole
// capture addressable memory backed by the OS page cache — random access
// (time-seeks) without read() syscalls, zero-copy decode straight out of the
// mapping, and a second replay of the same file runs entirely from RAM.
// FILE_FLAG_SEQUENTIAL_SCAN / MADV_SEQUENTIAL hints the prefetcher for the
// dominant access pattern.

#include <cstddef>
#include <cstdint>

#include "hft/common/platform.hpp"

#if !defined(_WIN32)
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace tl {

class MappedFile {
 public:
  MappedFile() = default;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  ~MappedFile() { close(); }

  bool open(const char* path) noexcept {
    close();
#if defined(_WIN32)
    file_ = ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!::GetFileSizeEx(file_, &sz) || sz.QuadPart <= 0) return false;
    size_ = static_cast<std::size_t>(sz.QuadPart);
    mapping_ = ::CreateFileMappingA(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_) return false;
    data_ = static_cast<const std::byte*>(::MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    return data_ != nullptr;
#else
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) return false;
    struct stat st{};
    if (::fstat(fd_, &st) != 0 || st.st_size <= 0) return false;
    size_ = static_cast<std::size_t>(st.st_size);
    void* p = ::mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) return false;
    ::madvise(p, size_, MADV_SEQUENTIAL);
    data_ = static_cast<const std::byte*>(p);
    return true;
#endif
  }

  void close() noexcept {
#if defined(_WIN32)
    if (data_) ::UnmapViewOfFile(data_);
    if (mapping_) ::CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) ::CloseHandle(file_);
    mapping_ = nullptr;
    file_ = INVALID_HANDLE_VALUE;
#else
    if (data_) ::munmap(const_cast<std::byte*>(data_), size_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
#endif
    data_ = nullptr;
    size_ = 0;
  }

  [[nodiscard]] const std::byte* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool is_open() const noexcept { return data_ != nullptr; }

 private:
  const std::byte* data_ = nullptr;
  std::size_t size_ = 0;
#if defined(_WIN32)
  HANDLE file_ = INVALID_HANDLE_VALUE;
  HANDLE mapping_ = nullptr;
#else
  int fd_ = -1;
#endif
};

}  // namespace tl
