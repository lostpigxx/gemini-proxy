// The completion object shared by all IO backends.
// Design: docs/design/io-and-coroutines.md §1–§2.
#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <sys/socket.h>

namespace vkp::io {

enum class opcode : std::uint8_t {
  nop,      // schedule(): completes immediately via the ready queue
  accept,   // result: new fd (nonblocking, cloexec)
  connect,  // result: 0
  recv,     // result: bytes read (0 = peer closed)
  send,     // result: bytes written (partial writes allowed)
  timeout,  // sleep_for(): handled by the event loop's timer heap
};

// One in-flight operation. It lives inside the awaiter — i.e. inside the
// awaiting coroutine's frame — so its address is stable from submission to
// completion and backends/queues store raw pointers to it.
struct operation {
  opcode op = opcode::nop;
  int fd = -1;
  char* rbuf = nullptr;        // recv destination
  const char* wbuf = nullptr;  // send source
  std::size_t len = 0;
  sockaddr_storage addr = {};  // connect destination
  socklen_t addr_len = 0;
  std::chrono::nanoseconds duration{0};  // timeout only

  // Completion: >= 0 is bytes / new fd / 0, < 0 is -errno (io_uring CQE
  // convention; reactor backends translate to match).
  std::int32_t result = 0;
  std::coroutine_handle<> continuation;
  operation* next = nullptr;  // intrusive ready-queue link
};

}  // namespace vkp::io
