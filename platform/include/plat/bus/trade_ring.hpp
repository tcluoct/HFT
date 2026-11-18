#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "hft/common/spsc_ring.hpp"
#include "plat/codec/msgs.hpp"

namespace plat {

// Counter-to-engine transport record. The engine assigns the canonical
// sequence and builds a journal Envelope after dequeueing.
struct TradeEvent {
  enum class Kind : std::uint8_t { Report, Session };

  union {
    ExecReport report;
    SessionStateEvt session;
  };
  Kind kind;

  TradeEvent() noexcept : report{}, kind(Kind::Report) {}
  explicit TradeEvent(const ExecReport& value) noexcept : report(value), kind(Kind::Report) {}
  explicit TradeEvent(const SessionStateEvt& value) noexcept
      : session(value), kind(Kind::Session) {}
};
static_assert(std::is_trivially_copyable_v<TradeEvent> && sizeof(TradeEvent) == 88);

// One engine consumer and one logical producer. An adapter with callbacks
// and local bounces from different threads must serialize its publish calls.
class TradeRing {
 public:
  static constexpr std::size_t kSlots = 1 << 12;
  enum class Poll : std::uint8_t { Empty, Event, Fault };

  void publish(const TradeEvent& event) noexcept {
    if (faulted_.load(std::memory_order_relaxed)) return;
    if (!queue_->try_push(event)) faulted_.store(true, std::memory_order_release);
  }

  [[nodiscard]] Poll poll(TradeEvent& out) noexcept {
    if (faulted_.load(std::memory_order_acquire)) return Poll::Fault;
    return queue_->try_pop(out) ? Poll::Event : Poll::Empty;
  }

 private:
  using Queue = hft::SpscRing<TradeEvent, kSlots>;
  std::unique_ptr<Queue> queue_ = std::make_unique<Queue>();
  std::atomic<bool> faulted_{false};
};

}  // namespace plat
