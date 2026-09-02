/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef WITH_EXIT_H_
#define WITH_EXIT_H_
#include <stdint.h>

#include <type_traits>

#include "function_ref.h"

// Non-local exits as they should have been. No "setjmp returns twice"
// complications.
//
// WithExit::Run(body) calls body with a cookie. Anything body reaches
// can hand that cookie back to WithExit::Exit to abandon everything
// down to (and including) the body and resume right after the Run
// call, which then reports true. If body just returns, Run reports
// false.
//
// Nothing runs on the way out. Every frame between Exit and Run is
// discarded without executing destructors, so no code inside body may
// rely on RAII for anything that matters.
namespace aw_backtrace_internal {

struct ExitCookie {
  uintptr_t data;

  bool operator==(const ExitCookie&) const = default;
};

inline constexpr ExitCookie kInvalidExit = ExitCookie{};

// The machine-specific pair the C++ API above is built from. See
// with-exit-amd64.S or with-exit-generic.cc
extern "C" {

// Calls fn(cookie, data). Returns false if fn returned normally, true
// if fn (or anything below it) passed cookie to aw_backtrace_exit_raw.
bool aw_backtrace_run_raw(void (*fn)(ExitCookie, void*), void* data);

// Discards every frame down to the aw_backtrace_run_raw call that
// produced cookie, making it return true.
[[noreturn]] void aw_backtrace_exit_raw(ExitCookie cookie);

}  // extern "C"

struct WithExit {
  // Returns false if body returned normally, true if it exited via Exit.
  static bool Run(FunctionRef<void(ExitCookie)> body) {
    return aw_backtrace_run_raw(body.fn, body.data);
  }

  [[noreturn]] static void Exit(ExitCookie cookie) {
    aw_backtrace_exit_raw(cookie);
  }
};

}  // namespace aw_backtrace_internal

#endif  // WITH_EXIT_H_
