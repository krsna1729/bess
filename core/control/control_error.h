// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_CONTROL_ERROR_H_
#define BESS_CONTROL_CONTROL_ERROR_H_

#include <expected>
#include <new>
#include <string>
#include <utility>

namespace bess {
namespace control {

// The control plane's single error model. The RPC adapter translates these to
// the legacy protobuf error fields; nothing inside the control plane inspects
// protobuf errors.
enum class ControlErrorCode {
  kInvalidArgument,
  kNotFound,
  kAlreadyExists,
  kConflict,
  kResourceBusy,
  kUnsupportedTransaction,
  // A failure the daemon could not get past (EIO, EAGAIN): UNAVAILABLE on the
  // wire, the code gRPC retry policies and generic clients retry.
  kResourceFailure,
  // A bounded resource is used up -- no free CPU for an automatic worker, a
  // message above the size limit, or memory (ENOMEM, a utils::MemoryRefused:
  // hugepages exhausted without --mem_fallback, or the memory limit,
  // D-198): RESOURCE_EXHAUSTED on the wire. Clients do not retry it: the same
  // request fails the same way until the operator frees or adds the resource.
  kResourceExhausted,
  kInternal,
};

struct ControlError {
  ControlErrorCode code = ControlErrorCode::kInternal;

  // errno-compatible value, carried out as ErrorDetail.errno_value on the
  // wire (0 for success, errno > 0 for failure).
  int err = 0;

  // Human-readable message, from the module or the control plane.
  std::string message;

  // Optional structured context, filled in as operations migrate to the
  // transactional engine.
  std::string object;
  std::string field;
};

// A control operation's outcome. [[nodiscard]]: dropping one would hide a
// failure (D-101). Otherwise std::expected's, except that it is not a
// std::expected specialization: and_then/or_else continuations must return a
// plain std::expected, a ControlResult<T> parameter does not deduce T from a
// plain std::expected, and two ControlResults do not compare with ==.
template <typename T>
class [[nodiscard]] ControlResult : public std::expected<T, ControlError> {
 public:
  using std::expected<T, ControlError>::expected;
  ControlResult(std::expected<T, ControlError> &&other)  // NOLINT: implicit
      : std::expected<T, ControlError>(std::move(other)) {}
  ControlResult(const std::expected<T, ControlError> &other)  // NOLINT: implicit
      : std::expected<T, ControlError>(other) {}
};

ControlErrorCode CodeFromErrno(int err);

// Builds an error from an errno value plus a printf-formatted message.
[[gnu::format(printf, 2, 3)]]
ControlError Err(int err, const char* fmt, ...);

// Builds an error whose message is strerror(err).
ControlError Errno(int err);

// D-198: memory a control operation asked for and did not get. A
// utils::MemoryRefused keeps its message (which names the bytes, the heap
// state and the flag that would allow a fallback); any other std::bad_alloc
// is "out of memory". kResourceExhausted, err ENOMEM.
ControlError OutOfMemory(const std::bad_alloc &e);

// Runs `f`, a control operation returning a ControlResult, and answers memory
// refused while it ran with OutOfMemory() instead of letting the exception
// leave the RPC handler. A catch is not a rollback: use it only around code
// that leaves nothing visible when it unwinds (the transaction engine's
// Reserve phase, D-172; a module's or port's Init, whose failure tears the
// object down) or whose caller undoes on the error it returns (ApplyPipeline
// aborts the transaction). Never on the packet path.
template <typename F>
auto RefuseOutOfMemory(F &&f) -> decltype(std::forward<F>(f)()) {
  try {
    return std::forward<F>(f)();
  } catch (const std::bad_alloc &e) {
    return std::unexpected(OutOfMemory(e));
  }
}

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_CONTROL_ERROR_H_
