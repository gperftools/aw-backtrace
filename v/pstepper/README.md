# pstepper — a portable per-instruction stepping facility

`pstepper` ("portable stepper") is an experiment in building per-instruction
execution interception the way it could exist — as a small, portable,
signal-like kernel facility — and then driving a real consumer against that
imagined API without the consumer knowing anything about how it is
implemented underneath.

Today the "kernel facility" is faked by a QEMU TCG plugin for
`qemu-x86_64` (linux-user). The eventual goal is to replace the
single-stepping machinery in the sibling `aw-backtrace` project, which
currently drives its per-instruction callback with real hardware
single-step traps plus a hand-rolled x86 instruction interpreter — brittle,
slow to maintain, and thoroughly x86-specific.

## The imagined API

A program asks the (imagined) kernel to start single-stepping the current
thread. From then on, at **every instruction boundary**, the kernel makes an
**upcall** into a registered handler, on a dedicated **upcall stack**, handing
it a snapshot of the thread's registers — exactly as if a synchronous,
per-instruction signal had been delivered. The handler looks at the state,
does whatever it wants (in our case: unwind the stack and cross-check it),
and returns; the kernel then resumes the interrupted instruction.

Deliberate terminology, to keep this from being mentally collapsed into
POSIX signals (which are asynchronous and tied to arbitrary points, not
instruction boundaries):

- **UXI** — "User eXecution Interrupt", the per-instruction event.
- **upcall** — delivering a UXI into the handler.
- **upcall stack** — the dedicated stack the upcall runs on (our analogue of
  `sigaltstack`).

There are also two thread-lifetime hooks in the same spirit — a
thread-creation hook (arm stepping on a new thread before its first
instruction runs) and a thread-exit hook (tear stepping down cleanly) — but
the core facility is the per-instruction upcall.

## The four parts

Everything in this repo is organized along a strict four-layer split. Each
layer knows only about the one below it through a narrow interface, and the
layering is load-bearing: it is what lets the top layer eventually run
unchanged against a real kernel instead of QEMU.

### 1. The plugin — `pstepper_plugin.c`

Stands in for the imagined kernel extension. It is a QEMU TCG plugin and
must stay ignorant of everything above it: no notion of threads, backtraces,
clones, or anything guest-specific beyond a single "magic syscall" ABI and
the generic per-instruction trap / resume-marker / upcall-stack mechanics.

On a trap it does almost nothing: it writes two words — the trapped
instruction's `RIP` and `RSP` — onto the guest's registered upcall stack,
points `RSP` there, and jumps to a guest-side trampoline. Resuming is a
single otherwise-invalid instruction the plugin recognizes purely by
address. It never marshals a register file and never touches CPU flags.

### 2. The upcall plumbing — `pstepper.{h,c}`, `pstepper_trampoline.S`, `pstepper_frame.h`

Guest-side code written *purely* against the imagined kernel API, with zero
QEMU awareness. It turns the raw magic-syscall / trampoline mechanics into
something that looks, to the layer above, like an ordinary signal-handler
invocation: it saves and restores the interrupted code's full register, FP,
and vector state around the handler call (using real hardware instructions),
builds the handler's context argument, and dispatches to the registered
handler.

The thread-creation and thread-exit hooks live here too — they are just
ordinary instructions that get trapped like any other and recognized in
plain guest code, so the plugin needs no knowledge of them.

In the eventual integration this layer is expected to be melded into
`aw-backtrace` as its own `.c` / `.S` files.

### 3. The consumer — the future backtrace-comparer

The actual per-instruction logic: a port of `aw-backtrace`'s
`backtrace-comparer` `StepperCallback`, which at every instruction unwinds
the stack with `aw_backtrace()` and cross-checks the result against a shadow
call stack it maintains by watching `call` / `ret` instructions go by.

This layer must stay completely agnostic of UXIs, upcalls, and QEMU. It just
sees what looks like a signal handler receiving a `ucontext_t *` and reads
registers out of it. Not yet ported into this repo.

### 4. The program being stepped

The ordinary target program, running normally, unaware it is being
observed one instruction at a time. In this repo, the guest test
programs play this role.

See `./genbuild.rb` for the build model and source-file header
comments for the detailed design of each piece.
