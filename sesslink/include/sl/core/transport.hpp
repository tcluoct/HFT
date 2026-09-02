#pragma once
// The byte-transport seam. Sessions are written against this concept, not
// against sockets — the same state machine runs over real Winsock TCP
// (net/tcp.hpp) in the benchmark and over a deterministic in-memory pipe
// (sim/mem_link.hpp) in the tests, where every partial read, coalesced
// frame, and mid-frame connection kill is scripted and repeatable. This is
// the testability trick that makes recovery logic provable: the hard cases
// are not races you hope to hit, they are inputs you construct.

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace sl {

// recv_some/send_some contract: >0 bytes moved; kWouldBlock = nothing now,
// try again; kClosed = peer gone (FIN, RST, or simulated crash). Both are
// non-blocking always — sessions busy-poll, they never park in the kernel.
inline constexpr std::ptrdiff_t kWouldBlock = -1;
inline constexpr std::ptrdiff_t kClosed = -2;

template <typename T>
concept ByteTransport = requires(T t, std::uint8_t* r, const std::uint8_t* w, std::size_t n) {
  { t.recv_some(r, n) } -> std::same_as<std::ptrdiff_t>;
  { t.send_some(w, n) } -> std::same_as<std::ptrdiff_t>;
};

}  // namespace sl
