#pragma once
// plat/engine/site_state.hpp — Site's deterministic state fingerprint and
// snapshot serialization, out-of-line (included by site.hpp; never include
// directly). Recovery contract (§11.1/§16): every stateful component hashes
// and serializes in a fixed order, so a snapshot + Source-suffix replay ends
// hash-equal to the uninterrupted run.

#include "plat/engine/site.hpp"

namespace plat {

template <OrderGateway Gateway, AnyStrategy... Strats>
std::uint64_t Site<Gateway, Strats...>::state_hash() const {
  StateHash h;
  h.mix(oms_.state_hash());
  h.mix(pnl_.state_hash());
  h.mix(comp_.state_hash());
  h.mix(risk_.state_hash());
  for (const InstrumentId i : cfg_.hosted) {
    if (has_fut_[i]) h.mix(fut_books_[i].state_hash());
    if (stock_books_[i]) h.mix(stock_books_[i]->state_hash());
  }
  std::apply(
      [&](const auto&... bank) {
        (([&] {
           for (const auto& s : bank) h.mix(s.state_hash());
         }()),
         ...);
      },
      strats_);
  h.mix(next_order_seq_);
  return h.value();
}

template <OrderGateway Gateway, AnyStrategy... Strats>
void Site<Gateway, Strats...>::save_state(std::vector<std::uint8_t>& out) const {
  append_pod(out, stream_.next_seq());
  append_pod(out, next_order_seq_);
  append_pod(out, cur_ts_);
  save_blob(out, [this](std::vector<std::uint8_t>& b) { oms_.save(b); });
  save_blob(out, [this](std::vector<std::uint8_t>& b) { pnl_.save(b); });
  save_blob(out, [this](std::vector<std::uint8_t>& b) { comp_.save(b); });
  save_blob(out, [this](std::vector<std::uint8_t>& b) { risk_.save(b); });
  for (const InstrumentId i : cfg_.hosted) {
    if (has_fut_[i])
      save_blob(out, [&](std::vector<std::uint8_t>& b) { fut_books_[i].save(b); });
    if (stock_books_[i])
      save_blob(out, [&](std::vector<std::uint8_t>& b) { stock_books_[i]->save(b); });
  }
  // Per-bank instance count first, so a snapshot loaded against a different
  // strategy composition skips cleanly instead of misaligning every blob.
  std::apply(
      [&](const auto&... bank) {
        (([&] {
           append_pod(out, static_cast<std::uint64_t>(bank.size()));
           for (const auto& s : bank)
             save_blob(out, [&](std::vector<std::uint8_t>& b) { s.save(b); });
         }()),
         ...);
      },
      strats_);
}

template <OrderGateway Gateway, AnyStrategy... Strats>
void Site<Gateway, Strats...>::load_state(const std::vector<std::uint8_t>& in) {
  std::size_t off = 0;
  std::uint64_t next_seq = 0;
  off = read_pod(in, off, next_seq);
  stream_.set_next_seq(next_seq);
  off = read_pod(in, off, next_order_seq_);
  off = read_pod(in, off, cur_ts_);
  off = load_blob(in, off, [this](const std::uint8_t* p, std::size_t n) { oms_.load(p, n); });
  off = load_blob(in, off, [this](const std::uint8_t* p, std::size_t n) { pnl_.load(p, n); });
  off = load_blob(in, off, [this](const std::uint8_t* p, std::size_t n) { comp_.load(p, n); });
  off = load_blob(in, off, [this](const std::uint8_t* p, std::size_t n) { risk_.load(p, n); });
  for (const InstrumentId i : cfg_.hosted) {
    if (has_fut_[i])
      off = load_blob(in, off,
                      [&](const std::uint8_t* p, std::size_t n) { fut_books_[i].load(p, n); });
    if (stock_books_[i])
      off = load_blob(in, off,
                      [&](const std::uint8_t* p, std::size_t n) { stock_books_[i]->load(p, n); });
  }
  std::apply(
      [&](auto&... bank) {
        (([&] {
           std::uint64_t n = 0;
           off = read_pod(in, off, n);
           for (std::uint64_t i = 0; i < n; ++i) {
             auto* dst = i < bank.size() ? &bank[i] : nullptr;
             off = load_blob(in, off, [&](const std::uint8_t* p, std::size_t sz) {
               if (dst) dst->load(p, sz);
             });
           }
         }()),
         ...);
      },
      strats_);
}

template <OrderGateway Gateway, AnyStrategy... Strats>
template <typename T>
void Site<Gateway, Strats...>::append_pod(std::vector<std::uint8_t>& out, const T& v) {
  const auto* b = reinterpret_cast<const std::uint8_t*>(&v);
  out.insert(out.end(), b, b + sizeof(T));
}

template <OrderGateway Gateway, AnyStrategy... Strats>
template <typename T>
std::size_t Site<Gateway, Strats...>::read_pod(const std::vector<std::uint8_t>& in,
                                               std::size_t off, T& v) {
  if (off + sizeof(T) <= in.size()) std::memcpy(&v, in.data() + off, sizeof(T));
  return off + sizeof(T);
}

template <OrderGateway Gateway, AnyStrategy... Strats>
template <typename F>
void Site<Gateway, Strats...>::save_blob(std::vector<std::uint8_t>& out, F&& fill) {
  std::vector<std::uint8_t> b;
  fill(b);
  append_pod(out, static_cast<std::uint64_t>(b.size()));
  out.insert(out.end(), b.begin(), b.end());
}

template <OrderGateway Gateway, AnyStrategy... Strats>
template <typename F>
std::size_t Site<Gateway, Strats...>::load_blob(const std::vector<std::uint8_t>& in,
                                                std::size_t off, F&& use) {
  std::uint64_t n = 0;
  off = read_pod(in, off, n);
  if (off + n <= in.size()) use(in.data() + off, static_cast<std::size_t>(n));
  return off + static_cast<std::size_t>(n);
}

}  // namespace plat
