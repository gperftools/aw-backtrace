/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
// Portable fallback implementation of the with-exit.h primitives.
#include "with-exit.h"

#if !__x86_64__ || FORCED_WITH_EXIT_GENERIC

#include <setjmp.h>

using aw_backtrace_internal::ExitCookie;

namespace {
// jmp_buf is a little odd, so lets wrap with struct to keep
// everything sane and simple
struct RunFrame {
  jmp_buf buf;
};
}  // namespace

extern "C" {

__attribute__((noinline)) bool aw_backtrace_run_raw(void (*fn)(ExitCookie, void*), void* data) {
  RunFrame frame;
  if (_setjmp(frame.buf) != 0) {
    return true;
  }
  fn(ExitCookie{reinterpret_cast<uintptr_t>(&frame)}, data);
  return false;
}

__attribute__((noinline)) [[noreturn]] void aw_backtrace_exit_raw(ExitCookie cookie) {
  _longjmp(reinterpret_cast<RunFrame*>(cookie.data)->buf, 1);
}

}  // extern "C"

#endif  // !__x86_64__ || FORCED_WITH_EXIT_GENERIC
