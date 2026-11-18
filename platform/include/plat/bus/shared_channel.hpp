#pragma once

#include <atomic>
#include <cstdint>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

#include "sb/core/shm.hpp"

namespace plat {

// The name identifies a unique session or runner generation. The owner
// creates the mapping before launching readers and retains it while they run.
template <typename T, std::uint64_t LayoutVersion>
class SharedChannel {
  static_assert(LayoutVersion != 0);

 public:
  static SharedChannel create(const std::string& name, std::uint64_t generation) {
    auto segment = sb::SharedSegment::create(name, sizeof(Storage));
    if (!segment.valid()) return SharedChannel(sb::SharedSegment{});
    auto* storage = ::new (segment.data()) Storage{};
    storage->generation = generation;
    storage->version.store(LayoutVersion, std::memory_order_release);
    return SharedChannel(std::move(segment));
  }

  static SharedChannel open(const std::string& name, std::uint64_t generation) {
    auto segment = sb::SharedSegment::open(name, sizeof(Storage));
    if (segment.valid()) {
      auto* storage = static_cast<const Storage*>(segment.data());
      if (storage->version.load(std::memory_order_acquire) != LayoutVersion ||
          storage->generation != generation)
        segment.close();
    }
    return SharedChannel(std::move(segment));
  }

  [[nodiscard]] T* get() const noexcept {
    auto* storage = static_cast<Storage*>(segment_.data());
    return storage ? &storage->channel : nullptr;
  }

 private:
  struct Storage {
    std::atomic<std::uint64_t> version{0};
    std::uint64_t generation = 0;
    T channel;
  };
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  static_assert(std::is_trivially_destructible_v<T>);

  explicit SharedChannel(sb::SharedSegment segment) : segment_(std::move(segment)) {}

  sb::SharedSegment segment_;
};

}  // namespace plat
