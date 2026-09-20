# AGENT.md — orientation for coding agents

Where things live and why they are the way they are. Deliberately not a
substitute for reading the code: when this file describes a mechanism it says
*why it exists*, not how it is spelled. Grep for a named symbol rather than
trusting a description that looks stale.

`README.md` is the *user's* introduction — what the library does, why, and how
to call it. It is also where the design rationale now lives; this file is the
part that only matters once you are editing the code.

## 1. What this project is

A from-scratch, **async-signal-safe backtrace library for profilers** ("aw" =
the prefix on all public symbols). Unwinds from `.eh_frame` / `.eh_frame_hdr`
CFI parsed by hand, with no libunwind/libgcc dependency.

The contract:

* Never crash, never deadlock, never allocate on the sampling path. An
  occasional *wrong* backtrace is acceptable; crashing is not.
* Only PC, SP and FP are unwound. Full DWARF register-state unwinding is
  deliberately absent — the bet is that essentially all real frames are either
  fixed-offset-from-SP or RBP-framed.
* Anything outside that model is an explicit, reported error, never a silent
  guess.
* Modern Linux, glibc 2.35+ (`_dl_find_object`). x86-64 is where the testing
  is; aarch64 builds, passes the basic tests, and has both its own
  `GuessUnwindInfo` (§4.9) and the fast path (§4.8). The comparer's
  machine-dependent half is split out behind `ComparerArch` (§4.6) and an
  aarch64 implementation of it exists, but nothing has run it yet — so the
  differential sweep (§4.10) is still aarch64's only working oracle. riscv
  and 32-bit eventually.

## 2. Build and test

**Two build systems that build different things.** Neither is a superset.

### Bazel — the library and the test suite

bzlmod, one top-level package, module `aw-backtrace`.

```
bazel test ...:all
bazel test -c opt ...:all        # NDEBUG: drops assert(), keeps CHECK()
./test-all-cfg.rb                # gcc/clang x dbg/opt sweep, all four green
ci/run-stepped.rb                # the tagged tests inside qemu, actually stepping
```

`test-all-cfg.rb` is now a one-liner around `ci/run-bazel.rb`, so the sweep you
run by hand and the one CI runs are the same commands; `--cc=` / `--mode=` pick
one configuration out of the four. The `ci/` scripts are their own part of §2.

Traps worth knowing before you fight the build:

* **`-std=c++20 -fno-exceptions -fno-rtti` live in `BUILD.bazel`'s `CXXOPTS`,
  per target, on purpose.** A project that consumes this module never reads our
  `.bazelrc`, so anything we need has to travel with the targets. `cxxopts`
  rather than `copts` so it stays off the C compiles (`aw-addrcheck.c`). The
  public header is deliberately standard-agnostic — it compiles as C99 and as
  C++11 and up — so a consumer on an older standard can still include it.
* **All internal headers are one target, `:headers`** (a `glob(["*.h"])`).
  Every target that compiles C++ deps `:headers` and `#include`s whatever it
  needs — there are no per-header libraries and no dependency-layering to keep
  in sync. `:headers` carries no compile options (headers only), so it leaks
  nothing into a consumer; the `.cc` implementations still have their own
  targets. Two things stay out of the glob: `aw-addrcheck.h` (its own `:aw-
  addrcheck` C target, `exclude`d) and the public header (`:aw-backtrace-hdr`,
  shipped under `aw-backtrace/` via `strip_include_prefix`).
* **`.bazelrc` is for *this* workspace only.** It forces `-std=c++20` across the
  whole graph so abseil matches what `//perf-convert` compiles against. It does
  nothing for anyone depending on us.
* **`.bazelversion` pins 9.2.0.** If you ever re-introduce an override
  (`local_path_override` / `git_override`), note that `dev_dependency` belongs
  on the `bazel_dep` line only — 9.2.0 rejects it on the override and every
  `bazel` invocation dies. (This is why `v/mini-x86-int` is a plain in-tree
  package rather than an override.)
* **GNU ld intermittently segfaults linking this package.** Worked around with
  `features = ["-supports_start_end_lib"]` on the targets that hit it — a
  toolchain interaction, not a code bug. Copy the line onto any new binary
  target that starts crashing `ld`.
* **No libatomic, and keep it that way.** Nothing wider than 8 bytes is
  atomic. `UnwindInfoCache` used 16-byte atomics until libatomic on aarch64
  turned out to implement them with a lock table (§4.4); on x86-64 they cost a
  libatomic call per step anyway, unless built with `-mavx -mcx16`.
* **`v/mini-x86-int` is "vendored" only mechanically** — co-developed, same
  author, no upstream. A fix belonging in the stepper goes in the stepper.
  Its `BUILD.bazel` targets carry `includes = ["."]` so the sources' `base/…`
  quote-includes (and dependents' bare `#include "sim_stepper.h"`) resolve the
  way they did when the directory was its own module root.
* `:aw-backtrace` is the only public target and the only public header
  (`include/aw-backtrace/aw-backtrace.h`, via `strip_include_prefix`).
* `with-exit.h` is header-only: `WithExit::Run`/`Exit` are a thin
  `_setjmp`/`_longjmp` wrapper.

### genbuild.rb / ninja — the LD_PRELOAD comparer

`genbuild.rb` is a self-contained generator (`build!` at the top is the part you
edit). **Edit `genbuild.rb`, never `build.ninja`.**

```
./genbuild.rb ninja                          # regenerate if stale, then build
./genbuild.rb ninja CC=clang CXX=clang++     # ENV overrides; re-execs itself
```

Defaults are `-ggdb3 -O2 -DNDEBUG -Wall -Wextra`, plus `-march=haswell` on
x86-64 (`ARCH` comes from `$CXX -dumpmachine`; aarch64 gets no `-march`). So an
NDEBUG build that assumes at least Haswell — don't copy the `.so` to an older
x86 machine, it will `SIGILL`. Four artifacts, none of which Bazel builds:
`backtrace-comparer.so`, `sym_helper_bin`, and `cjm`/`cjm0`.

`ARCH` also picks which `v/pstepper` slice is compiled, so the model is
cross-capable, but the `sym_helper` blob is still objcopied with the *host*
`objdump`/`objcopy` (`BFD_ARCH`) — a cross build therefore links an
x86 blob into an aarch64 `.so` and fails. Native builds on either arch are
fine.

### `ci/` — what CI runs, and how to run it here

`.github/workflows/ci.yml` is deliberately thin: every step is a ruby script
under `ci/`, so the same thing runs on a GitHub runner, under `act` locally
(`.actrc` pins the runner image), or straight on a developer box. Nothing in
the yaml is worth debugging through a push cycle. Two jobs: the four-way
`bazel` sweep (one matrix job per configuration) and `stepped`.

* `ci/build-qemu.rb` — pinned upstream qemu (commit in the script) plus every
  patch in `ci/qemu-patches/`, built linux-user-only into `~/qemu`. **The
  patch there is not optional**: upstream's i386 translator only materializes
  EIP and the lazy `cc_op` condition codes where it has a reason to, so a
  plugin that reads registers or redirects execution at an arbitrary
  instruction boundary sees stale EFLAGS — and three of `v/pstepper`'s tests
  segfault. The workflow's cache key is a hash of the script *and* the
  patches, so bumping either rebuilds.
* `ci/check-pstepper.rb` — `v/pstepper`'s own suite under that qemu. Run first
  in the job: seconds, no bazel, and if stepping itself is broken every
  stepped test below fails less legibly.
* `ci/install-bazelisk.rb` — `bazel` from the latest prebuilt bazelisk
  release (curl), into `~/.local/bin`. `.bazelversion` then decides the release.
* `ci/run-bazel.rb` — the sweep. `test-all-cfg.rb` is a one-liner around it.
  It skips tests tagged `legacy` — today exactly `v/mini-x86-int`'s three,
  see §3 for why — and `--with-legacy` puts them back.
* `ci/run-stepped.rb` — builds the plugin with `v/pstepper`'s ninja, then
  `bazel test --config=stepped --run_under=ci/qemu-stepper.rb <qemu> <plugin>`.

**Why `--run_under` and not a bazel target:** the emulator and the plugin are
built outside bazel (the plugin needs glib and qemu's plugin header), so
neither is a bazel input — which is also why the config carries
`--nocache_test_results`. A cached PASS would otherwise outlive a change to
either one. The config changes nothing about the build, so the binaries are
exactly what a plain `bazel test` produces; `tags = ["stepped"]` plus
`--test_tag_filters` is what picks the comparer-hosting tests out.

### Two things about test configurations

**Only `bazel test -c dbg` traps on unwinder diagnostics.** Non-`NDEBUG` builds
capture with `trap_diagnostics`, so a diagnostic aborts where it happened; `#if
defined(NDEBUG) || defined(BUILD_SO)` opts out, so the preloadable `.so` never
traps — it walks startup and libc code nobody will fix.

**A comparer mismatch is a test failure — but only under qemu.** The comparer
rides `v/pstepper`, so a plain `bazel test` gets `pstepper_enable FAILED:
Function not implemented`, passes, and compares nothing at all. That is still
every run of the default suite; what runs the tests *stepping* is
`--config=stepped`, i.e. `ci/run-stepped.rb` (§2, `ci/`), and §5 has the by-hand
invocation.

**The silent-pass hazard is what `AW_BT_REQUIRE_STEPPER=1` exists for.** The
stepped config sets it, and then a missing stepper, or a run that exits having
stepped zero instructions, is a hard failure (exit 91) instead of a green test
that compared nothing — and any unsuppressed diagnostic fails the run at exit
(exit 92) rather than only printing. It also *ignores* `AW_BT_DIAG`, so
acceleration stays on and every diagnostic gets reported before the verdict;
`AW_BT_DIAG` remains the interactive tool that stops at the first one. So
`aw-backtrace-test`'s own `setenv("AW_BT_DIAG", "0", 0)` — which otherwise
makes the first unsuppressed diagnostic raise `SIGTRAP`, in `-c opt` too — has
no effect under CI's config. (It also still sets `AW_BT_DIAG_VIA_CORE=1`, which
nothing reads any more.) Knobs in §5.
Cache counters only exist when `AW_BUMP_STATS_IN_PRODUCTION` is defined —
`genbuild.rb` defines it, Bazel does not.

## 3. Code map

### Core library

| file | what |
| --- | --- |
| `include/aw-backtrace/aw-backtrace.h` | the whole public API: `aw_backtrace_full`, `aw_backtrace`, plus `aw_backtrace_ext::DebugExtensionV0` (test/inspection surface, `TryGet()` may return null, not stable ABI) |
| `aw-backtrace.cc` | entry points, `UnwindLoop`, `UnwindLoopFastPath`, `StackAccess`, the `NoDiag`/`RuntimeDiag` policies, `FrameInfoCache`, `DebugExtensionV0` impl |
| `aw-backtrace-fastpath.h` | `TryFastFrameInfo` / `FastPathFrame` (§4.8): a single inlined pass over `.eh_frame_hdr`+FDE+CIE that handles the common frame shapes and bails (`Failure()`) on anything else. `ToFrameInfo` turns its result into a `FrameInfo` |
| `backtrace-core.{h,cc}` | the *policy* half of CFI lookup: a visitor turning decoded CFI into a `FrameInfo`. No `.eh_frame` parsing, no arch cases, no `is_leaf` |
| `eh-frame-reader.h` | the *decoder* half: `.eh_frame_hdr` search, CIE/FDE parsing, the CFI opcode loop. Visitor-templated, knows nothing above it |
| `backtrace-drap.h` | x86-64 only. Second minimal visitor for gcc's DRAP prologues (§4.3) |
| `aw-structs.h` | the unwind model: `CfaRule`, `RegisterRule`, `FrameInfo`, `Cursor`, `CompressedFrameInfo` |
| `aw-arch.h`, `aw-arch-x86_64.h`, `aw-arch-aarch64.h` | per-arch `struct Arch` with a fixed static interface. Adding an arch means implementing exactly that set |
| `unwind-info-cache.h` | `UnwindInfoCache`: fixed-size, bucketed, per-bucket try-lock seqlock, second-chance eviction (§4.4) |
| `aw-addrcheck.{h,c}` | async-signal-safe `/proc/self/maps` querying, `PROCMAP_QUERY` or snapshot+bsearch. Validates addresses before dereferencing |
| `with-exit.h` | `WithExit::Run`/`Exit` — header-only `_setjmp`/`_longjmp` wrapper hiding the returns-twice. **Nothing runs on the way out**, no destructors |
| `check.h` | `CHECK()`, unconditional (not `NDEBUG`-gated). For differential self-checks, never the capture path |
| `dwarf-constants.h`, `utils.h`, `simple-counter.h`, `static_storage.h`, `function_ref.h` | constants and small utilities lifted from gperftools/tcmalloc |

### Reference / comparison implementations

* `simple-fp-backtrace.{h,cc}` — pure frame-pointer walker, the passing baseline.

### Test / tooling

* **`backtrace-comparer.cc` — the best test asset in the repo.**
  Single-steps the whole process via `v/pstepper`, maintains a shadow
  call stack from calls and returns, and compares the capture against
  it *at every instruction*. Needs no expected values. Known-bogus
  spots (`_dl_fixup`, `call_init`, `__run_exit_handlers`) are
  suppressed by symbolizing the top frames; matches are cached, and an
  unsuppressed mismatch registers its own top frame so each location
  reports once. §4.6 before trusting a quiet run. The file itself is
  arch-neutral: per-thread state lives in one `ThreadState`, and
  everything machine-dependent is behind `ComparerArch`.
* `comparer-arch.h`, `comparer-arch-x86_64.h`, `comparer-arch-aarch64.h` —
  that seam: a per-arch `struct ComparerArch` with a fixed static interface,
  same shape as the unwinder's `struct Arch` and deliberately sharing no code
  with it (§4.6).
* **`backtrace-comparer.so`** (`-DBUILD_SO`, ninja only) — the same
  thing as an LD_PRELOAD object, which is what points the differential
  machine at arbitrary already-built binaries. Interposes
  `sigaction` so the target can't take over the signals the comparer
  itself answers on, `unsetenv`s `LD_PRELOAD` so it doesn't follow into
  children, exports almost nothing (version script). Usage in §5.
* `aw-backtrace-test.cc` — main end-to-end test. Captures from a
  SIGILL handler (so the top frame is a real signal frame with a
  `ucontext`) and checks that walking *through* the signal frame
  without one lands in the same place — the direct test of
  `Arch::IsSignalFrame`. Also hosts the DRAP fixtures. Compiled twice,
  the second time as `simple-backtrace-test` (`BT_USE_SIMPLE`).
* `amd64-leaf-test.cc` — hand-written asm with deliberate CFI shapes,
  compared against glibc `backtrace()`. The minimized repro vehicle
  for leaf/epilogue issues, and the gdb-friendly binary (no comparer).
  Also holds the jump-through-null test (§4.1): `call *%rax` with `%rax == 0`,
  caught by a SIGSEGV handler that captures and `siglongjmp`s out.
* `arm64-leaf-test.cc` — the aarch64 companion, and the only test of §4.9. Its
  fixtures are asm *without* `.cfi_startproc`, so no FDE is emitted and every
  step out of them comes from the guess; glibc `backtrace()` is printed but
  cannot be the reference, since it gives up on exactly these frames. Pins the
  stale-x30 self-correction and the jump-through-null case (`blr` on a zeroed
  register), and documents the one shape the guess gets wrong. Also never
  starts the comparer, so it is the gdb-friendly binary on aarch64. Note the
  `paciasp` fixture is inert without FEAT_PAuth.
* `aw-backtrace-skip-test.cc` — the `skip` argument of `aw_backtrace`: skipping
  N frames must equal a full capture with N dropped, and the returned count is
  what was actually filled.
* `amd64-drap-test.{c,s}` — gcc 16 `-mforce-drap` output supplying
  `minimal_drap`; the pre-gcc-16 shape lives on as `minimal_drap_2` in
  `aw-backtrace-test.cc`.
* **`fastpath-sweep-test.cc` — the fast path's only real oracle on
  aarch64.**  Sweeps every FDE of every module the test process has
  mapped, asks both `DoUnwindLookup` and `TryFastFrameInfo` about four
  pcs in each, and requires that wherever the fast path answers it
  answers *exactly* what the full decoder does. Needs no stepping
  machinery, so unlike the comparer it runs on both arches. clang on
  aarch64 is deliberately bypassing the strict comparison due to known
  issue (§4.8).  Mismatches must be zero regardless; the coverage
  percentage is only a loose floor, since the corpus is whatever
  happens to be loaded and therefore toolchain-dependent. §4.10 for
  the one divergence it deliberately tolerates.
* `eh-frame-reader-test.cc` — the only place the decoder runs without a real
  process image. `Optionalize()` turns a reader `Fail()` into `std::nullopt`, so
  "must be rejected" is an `EXPECT_EQ`. Stops at the byte-level helpers.
* `symbolize-backtrace-test.cc` — installs a **`SIGCHLD` handler that
  `abort()`s** first thing in `main`: a healthy symbolizer daemon must never
  deliver one to its owner (§4.5). Also the repeated-PC regression test and
  `DumpStackTraceWithClosedStdio` (symbolizes with 0/1/2 closed).
* `comparer-longjmp.cc` → `cjm`/`cjm0` — the first in-tree program written to
  make the comparer *fail*: `_longjmp` out of deep recursion, then a frame
  pointer deliberately off by one bit. **Asserts nothing** — an eyeball vehicle.
  So does `recursion-test.cc`, a jump-table-heavy benchmark built three ways
  (aw/libgcc/fp) to compare speed — the numbers in README's Benchmark section
  come from it.
* `lua-test.cc` — the comparer over the Lua compiler, for varied real codegen.
* `aw-addrcheck-test.c`, `bench-addrcheck.c` — unit test / microbenchmark. The
  test `#undef NDEBUG`s at the top because it asserts for effect.
* `symbolize-backtrace.{h,cc}` + `sym-helper.cc` — async-signal-safe
  symbolization; `sym-helper` is embedded as a blob and spawned once per
  process image as a daemon that keeps an `addr2line` per binary and answers
  requests over a socketpair. **Diagnostics and tests only, never the capture
  path.** The spawn and the protocol are §4.5.
* `v/pstepper/` — co-developed sibling, and what the comparer rides on now: a
  per-instruction upcall facility shaped like an imagined kernel feature
  (a "UXI" delivered on a dedicated upcall stack), faked underneath by a QEMU
  TCG plugin. The comparer is written against `pstepper.h` alone and knows
  nothing about QEMU; `pstepper_enable` returns `ENOSYS` when no plugin is
  loaded. Its own `README.md` has the four-layer design. The "acceleration"
  hack runs the handler as *host* code when guest and host arch match, which
  is why a `raise()` from a diagnostic behaves differently with it on (§5).
* `v/mini-x86-int/` — the previous, x86-only stepper: partial x86-64
  interpreter + `sim_stepper`. **No `ptrace` anywhere in it** — stepping is
  `EFLAGS.TF` plus an `SA_ONSTACK` `SIGTRAP` handler, i.e. the process steps
  itself, and the interpreter is a fast path that simulates runs of
  instructions to avoid a trap each. §4.7. Nothing in the comparer or in
  `genbuild.rb` uses it any more; its own Bazel tests still run, but they are
  tagged `legacy` and CI skips them. The reason is worth knowing before
  trusting this directory: the interpreter runs *real libc code*, so it is
  sensitive to which libc it meets, and `test-signal-disable` segfaults
  against Ubuntu 24.04's glibc 2.39 while passing on a Debian trixie box.
  That is an unexplored interpreter coverage gap, not an environment
  restriction — it reproduces with seccomp off, on identical hardware.

### Fuzzing and offline tooling

* `fuzz/` — libFuzzer target for the fast path (`TryFastFrameInfo`), fed real
  `.eh_frame_hdr`+`.eh_frame` blobs from `fuzz/data/` plus byte mutations. The
  `FuzzXlate` accessor is the reason `Access` / `TryFastFrameInfo` are templated
  on an `Xlate` at all (production is `IdentityXlate` and compiles away); it
  emits `__sanitizer_cov_trace_cmp4` so mutations aim at bytes the decoder
  actually reads. `fuzz/README.md` has the input layout and how to run it.
  Built by `fuzz/build.sh` (clang, ASan+UBSan), not by Bazel.
* `perf-convert/` — offline `perf.data` converter: reads a `--call-graph dwarf`
  recording and rewrites the raw stack dumps as plain callchains. Uses
  `//:aw-fastpath` (the header-only fast-path decoder) and none of the
  in-process unwinder, and links abseil freely — it is neither signal-safe nor
  dependency-free, deliberately. `//:aw-fastpath` is visible only to this
  package, since header-only targets can't carry `CXXOPTS` into a consumer's
  compile. `perf-convert/compare.rb` diffs its output against `perf`'s own.

### Docs

* `doc/amd64-drap-problem.adoc` — why the DRAP handling exists. The only
  long-form doc that survived the release cleanup.
* `TODO` — live task list.
* `LICENSE` is 0BSD; every source file carries an SPDX line.

## 4. How it works

### 4.1 The main loop

`aw_backtrace_full` (callback) / `aw_backtrace` (array) → `PrepareCursor()`
(from the `ucontext` for a signal capture, or from `__builtin_frame_address(0)`
for a direct call; both `NEVER_INLINE`, which matters for correctness) →
`UnwindLoopFastPath()`, which falls through to `UnwindLoop()`.

`is_leaf` really means **"pc came from a register file, not from an unwind
step"** — hence the public callback parameter being named `pc_before_insn`. It
is true for frame 0 of a `ucontext` capture and for a frame reached by stepping
through a signal trampoline. Non-leaf frames look up `pc - 1` and consult the
cache; leaf frames go straight to a fresh lookup. Getting this flag onto the
right frame is fiddlier than it looks: `UnwindLoop` threads `next_uc` /
`this_frame_uc` precisely so the flag belongs to the frame being *reported*
rather than the one being unwound *from*.

**A zero pc means two different things, and `is_leaf` is what tells them
apart.** From an unwind step it is the end of the chain — the outermost frame's
return-address slot is zeroed — and the walk stops. From a register file it is a
live jump through a null function pointer: the pc really is 0 and the caller is
still recoverable, because the branch recorded its return address before faulting
on the fetch at 0 — on x86-64 `call` pushed it, so `sp` points straight at it; on
aarch64 `blr` wrote x30. Either way `Arch::GuessUnwindInfo` finds it. Both loops
therefore break only on `pc == 0 && !is_leaf`, which is also what keeps
`lookup_pc`'s `- 1` from underflowing. Regression tests in `amd64-leaf-test.cc`
and `arm64-leaf-test.cc`; libgcc and libunwind both give up here, so there is no
reference implementation to diff against.

A lookup that comes back `kUndefinedRA` is **not** "nothing": the row covering
the pc carries an explicit `DW_CFA_undefined` on the return-address column, so
the frame has nothing to return to and the walk stops right there. Skipping the
chain below matters for more than tidiness — it is the frame that ends *every*
backtrace (`_start`), and the chain's first act is an `ExecutableBoundsFor`.
That call is answered from `LoadedSetHolder`'s executable-segment table (§4.4)
for anything in an initially-loaded module, and only falls through to
`aw-addrcheck` otherwise — where, without `PROCMAP_QUERY`, it is a snapshot and
parse of the whole of `/proc/self/maps`.

When the lookup produces nothing, a **fallback chain** runs in this
order: PLT detection (leaf only; includes PLT0, the lazy-binding
header, which on aarch64 moves sp by 16 before branching to the
resolver) → signal-trampoline byte match (at the trampoline's entry or
at its `syscall`, so a sample landing on that one instruction still
unwinds) → DRAP (x86-64, and only if the failure was a CFI expression)
→ refuse outright if it was a CFI expression →
`Arch::GuessUnwindInfo`. Everything in it that reads code bytes is
bounds-checked against `AddrChecker::ExecutableBoundsFor()`, the DRAP
`lea` sniff included — that one takes the bounds as a parameter and
falls back to trusting the unwind info when it can't read safely.

All stack reads go through `StackAccess` (alignment + containment in a stack
VMA). Two properties, both driven by comparer findings: a read outside the
cached bounds re-discovers them rather than failing, so an unwind can cross onto
or off a `sigaltstack`; and the TLS bounds cache is a seqlock, because a signal
handler can nest on the same thread and update it for a different VMA mid-write.

### 4.2 The CFI lookup, and how failure is reported

**Decoder/visitor split.** `eh-frame-reader.h` reads bytes and knows nothing
about this unwinder; `backtrace-core.cc` is the visitor that gives decoded
instructions meaning. **Callback arguments are the whole contract** — the
visitor is handed no reader state, so a new callback must be passed what it
needs rather than handed the state back.

**Diagnostics are flags, not an interface.** `DoUnwindLookup` returns
`kOk`/`kFail`/`kFailExpression` and takes a `DiagFlags` by value; reporting is a
single variadic `ReportError` that formats into a stack buffer and optionally
`write(2)`s or traps. `kFailExpression` is broken out because it is the one
failure the fallback chain can plausibly explain away; its message costs a
second full decode, so it is `if constexpr`-gated out of production entirely.

**Two flavours of failure, not symmetric.** *The reader gives up* (truncation,
unsupported encoding, unknown opcode, out-of-range address) via
`Decoder::Fail()`, which `WithExit::Exit`s out, **discarding every intervening
frame with no destructors** — so nothing in `eh-frame-reader.h` may own anything
by RAII. *The visitor gives up* by returning false, which stops the decode
normally. Either way the caller only sees an outcome code.

Unsupported things report and fail the frame rather than guessing, with a few
deliberate exceptions the code calls out: expressions on non-critical registers,
`DW_CFA_undefined` on RA (not a failure at all — it is reported as its own
outcome, `kUndefinedRA`, see §4.1), and unknown registers, so a future greg
expansion won't break unwinding.

**`DW_CFA_undefined` on RA is decided by the row, not by the instruction.**
`HandleUndefined` records `RegisterRule::Undefined()` and keeps decoding;
`DoLookup` converts a rule that survives into the row covering the pc. Bailing
out the moment the opcode goes by was harmless while the outcome fed the
fallback chain — a guess might still recover the frame — but it is not
row-accurate, and now that the outcome *stops the walk*, an Undefined in a row
preceding the pc's (with RA defined again by a later `DW_CFA_offset` /
`DW_CFA_restore`) would silently truncate the backtrace.

**The asymmetry above is a live trap when editing `backtrace-core.cc`.** The
reader exits non-locally; the visitor does not. So a visitor helper that detects
a problem can only *return* the failure, and a caller that forgets to propagate
it turns an explicit error into a silent wrong answer — which is exactly how an
invalid `DW_CFA_register` operand once became `%rax`. `NarrowOffset` and
`NarrowReg` are therefore `[[nodiscard]]`: forgetting is a compile error.

The reader also rejects a few things up front that would otherwise produce
plausible-looking nonsense rather than a failure: a `code_align` of 0 (every
`advance_loc` becomes a no-op, so the row search never advances and the frame
gets built from every row in the FDE), a `data_align` of 0 (every offset becomes
0, putting RA at CFA+0), and either of them beyond ±16. `AdvanceLoc` and
`OffsetWithDataAlign` check the multiplications for overflow.

`LocateEHFrame` returns null — failing the frame — when `_dl_find_object` can't
place the `.eh_frame_hdr` it just handed us. There is no "assume everything is
readable" fallback: that would be the one unbounded read in the library.

### 4.3 DRAP (x86-64 only)

gcc's stack-realigning prologues produce unwind info the three-register model
cannot follow, and in places it is outright missing — read
`doc/amd64-drap-problem.adoc` first. The handling is a **second decode of the
same FDE** with a visitor tracking a four-state machine that maps directly onto
a new `Cursor`. Two of the four states read `%r10`, for them unwind is leaf-only.

The test bites because of `drap_test_trampoline`, an RSP-framed naked caller: a
DRAP prologue looks like an ordinary frame, so the naive guess gets RIP and RBP
right and only **RSP** wrong — invisible unless the caller is RSP-framed. Both
shapes are covered (gcc 16's, with the `.cfi_restore 6`, and every gcc before
it). Verified, not assumed: making the DRAP call unreachable fails the test.

### 4.4 Caching

**One cache, non-leaf only** — leaf frames always take a fresh
lookup. We also only cache for addresses in modules present at
constructor time, because there is no invalidation. Those modules are
assumed to be permanently loaded (not unloadable). Cache
implementation is two layers: `FrameInfoCache` is policy (compression,
cacheability) and `UnwindInfoCache`, which is storage and eviction
and knows nothing about the unwinder.

**`LoadedSetHolder` is what says which addresses those are**, and it does
double duty. Its constructor records the *executable* `PT_LOAD` segments of
every module `dl_iterate_phdr` reports, one entry each, sorted. Executable
segments are enough because every address anyone looks it up with is a pc; and
a single segment, unlike the union of a module's `PT_LOADs` it used to record,
is a contiguous mapping with no `PROT_NONE` alignment holes — which is what
makes the same table a safe answer to `ExecutableBoundsFor` (§4.1), instead of
a `/proc/self/maps` snapshot per call on kernels without `PROCMAP_QUERY`. The
recorded extent is the segment's own, not the page-rounded mapping: tighter can
only make a byte matcher decline, never read past what the object declared.
Anything not in the table — JIT, post-constructor `dlopen`, bare anonymous exec
mappings — still goes to `aw-addrcheck`.

**A seqlock per bucket, and nothing wider than 64 bits.** An entry is two
`atomic<uint64_t>`s: key+flags, and the compressed `FrameInfo` `bit_cast` to a
word. The seqs are a table of their own, so a bucket stays exactly 128
bytes, 128-aligned (two cache lines), at the price of a third line per lookup.
A seq is a `uint32_t`: native everywhere, 32-bit targets included, and its
wraparound is harmless (parity survives it; ABA takes 2³¹ writes to one bucket
inside one read).
It used to be one 16-byte atomic, with no lock at all; that died because
libatomic on aarch64 — not even an old one — implements 16-byte atomics with a
lock table, which is neither fast nor async-signal-safe.

**Nobody ever waits on the lock**, because a signal handler can interrupt a
writer holding it and then look something up on the same thread. A `Put`
try-locks and drops the insert if it can't get the bucket; a `Lookup` that sees
the bucket locked is a miss, and it retries only when a writer came *and went*
while it read. Only a hit is validated, since a miss is always acceptable. The
one unlocked write is a hit clearing its entry's `pending_eviction` bit (an
atomic `fetch_and`); readers ignore that bit, so it needs no seqlock. Eviction
is second-chance: an insert marks the other entries in its bucket, a hit clears
its own mark, and `Put` prefers free, then marked, then random.
`dropped`/`retries` in `PrintStats` count the two ways the lock can cost
something.

`CompressedFrameInfo` still carries an explicit `unused` member rather than a
padding hole, and `static_assert(std::has_unique_object_representations_v<...>)`
enforces it. Nothing compares it bitwise any more, but `bit_cast` of a padding
byte is still indeterminate.

### 4.5 The symbolizer daemon

Diagnostics-only code, documented here because it is the one place where
*process*-level details carry the correctness argument: the comparer preloads
into arbitrary host processes, and a symbolizer that spawns visible children
perturbs whatever process management the host is doing.

**One persistent `sym-helper` per process image, spawned lazily and talked to
over a mailbox.** The first symbolization (`EnsureHelperOwner`) spawns the
daemon and keeps one end of an `AF_UNIX` `SOCK_SEQPACKET` socketpair; every
request after that is a single datagram on it carrying two descriptors via
`SCM_RIGHTS`:

* a memfd holding the request — the addresses plus the caller's own
  `/proc/self/maps` snapshot — which the daemon truncates and rewrites in place
  with a flat binary response the caller simply `mmap`s;
* the write end of a pipe made fresh for this request, which carries no data:
  its close is the "response committed" signal. A daemon that dies mid-request
  closes it too, which is why the caller checks `kSymResponseMagic` before
  trusting the memfd.

The daemon serves one request at a time and keeps one `addr2line` per binary
alive across requests. Because each request's state is private to its own
memfd and pipe, and `sendmsg` is atomic per datagram, concurrent callers —
threads, or a forked child sharing the inherited mailbox — need no locking. The
one serialized thing is the spawn itself, under a CAS spinlock with every signal
blocked.

The maps snapshot being the *caller's* view is deliberate: under qemu-user it is
the guest view QEMU synthesizes, which is what makes guest addresses resolve.

**The spawn is a plain `vfork` + `execve`**, of the embedded blob written to a
memfd and run as `/proc/self/fd/N`, with all signals blocked around it; the
child restores an empty mask before it execs. Two details matter:
* **Only `PATH` is passed on**, so `LD_PRELOAD` — the comparer — does not follow
  into the daemon.
* **The mailbox is `CLOEXEC`**, so a child that forks without exec shares its
  parent's daemon, while an exec'd child is a fresh image that spawns its own.

**`SIGCHLD` is a tradeoff now, not an absolute.** Healthy operation produces
none for the host: the `addr2line`s are the daemon's children, reaped by it with
`SIGCHLD` ignored, and the daemon itself exits only on mailbox EOF, i.e. after
every holder of the owner's end is gone, so its exit reaches init or a
subreaper, not the host. What is accepted is exactly one ordinary `SIGCHLD` if
the daemon dies while its owner is alive — in practice an `execve` failure right
at spawn. `symbolize-backtrace-test`'s abort-on-`SIGCHLD` handler pins the
healthy case.

**Descriptors 0/1/2 remain the recurring hazard.** A target may close its
standard descriptors — glibc's `close_stdout` atexit handler does — so the
socketpair, memfd and pipe created on the caller's side can land in that range.
The daemon absorbs it at a single point: before anything else it moves its
inherited mailbox descriptor to 16 or above, then reopens 0/1/2 (onto
`/dev/null`, or stderr onto `/dev/tty` in debug builds), so request descriptors
never arrive where a standard one is expected. `DumpStackTraceWithClosedStdio`
is the regression test, with stdin both open and closed — but it runs after the
daemon already exists, so it covers the per-request memfd and pipe, not the
spawn-time case where the daemon's own end of the mailbox is the descriptor at
risk. The spawn code this
replaced had two such bugs and neither was loud — one crashed on a `CHECK`, the
other made every dump come back silently **empty** — so a report with no body
means look here first, and any new descriptor handed across should be assumed to
have the same problem.

**A failed request still produces frames.** When the daemon cannot answer — dead,
never spawned, or died mid-request — `ReportUnsymbolized` stands in for it: each
pc with its module and ELF vaddr from `_dl_find_object` (the main executable
shows up as the literal `/proc/self/exe`), no function or line, plus one
`sym-helper request failed` line on stderr. The daemon is not respawned, so once
it is gone every later dump in the process looks like that.
`symbolize-backtrace-test`'s last case shuts the mailbox down through
`test_only_symbolize_backtrace_kill_daemon` — a real `sendmsg` failure, and EOF
for the daemon, which exits — then checks that frame 0's pc and vaddr match what
the live daemon printed.

**The daemon closes every descriptor it inherited** except its mailbox and the
freshly reopened 0/1/2. It lives as long as its owner, so any host descriptor
that lacked `CLOEXEC` would otherwise stay open with it — and if that is a
pipe's write end, the host's reader never sees EOF and a make or shell pipeline
hangs.

### 4.6 The comparer's shadow stack, and its arch seam

**Everything machine-dependent is behind `ComparerArch`** (`comparer-arch.h`
and its two per-arch headers), and there are exactly two things in it:
recognizing the instruction at the trapped pc as a call or a return and saying
what a retired call put where, and recognizing a signal delivery in the frame
the kernel built. The rest of `backtrace-comparer.cc` — the shadow stack, the
resync rule, the suppression cache, the comparison — is arch-neutral, and the
one piece of per-thread state is `ThreadState`.

**It deliberately shares no code with the unwinder's `struct Arch`.** The
comparer is an independent oracle: if it borrowed the unwinder's idea of what
a signal frame looks like, a wrong answer in `Arch::IsSignalFrame` would agree
with itself and the test would pass.

**On aarch64 the model is identical and the reason is different.** An entry
retires at `sp + 8` on x86-64 because `call` pushed the return address and
`ret` pops it; on aarch64 `bl` writes x30 and touches no stack, so a callee is
entered with the *caller's* sp and a well-behaved `ret` is reached with that
same sp — an entry retires at exactly the sp the call was made with. The
corollary is the same trap §4.9 describes for `GuessUnwindInfo`: `*sp` at a
callee's entry belongs to the caller and very often holds the caller's own
spilled x30, so x86-64's "the word at `*sp` is the return address" must not be
ported. The aarch64 signal frame has no `pretcode` either — the trampoline's
address arrives in x30, so the handler's own `ret` is what reaches it.

Before touching `BacktraceBuffer`:

* **Entries carry the SP their frame will return to**, so one `ret` can retire
  several at once — which is what a `longjmp` leaves behind, having restored
  `%rsp` with no `ret`s at all. The reverse case (shadow stack shorter than the
  capture) is accepted outright: the comparer can start mid-stack.
* **`Pop` can drain the stack entirely, and an empty shadow stack passes
  everything**, so one mis-modelled return turns the comparer off until calls
  refill it. Deliberate — it is what makes `longjmp` survivable — but it traded
  a noisy failure mode for a quiet one. `return_buffer_pop_skips` in
  `PrintStats` is the instrument; a big jump means go looking.
* **Suppression is also a cache, and it cuts both ways.** The table is checked
  against the top of the *shadow stack* as well as the capture, so a
  once-reported pc that later appears as a return address silently skips that
  comparison too; and it saturates at 256 entries (saying so once).
* **The suppressions need glibc's debug info installed**, which is easy to
  miss because a dev box usually has it. They match by symbol *name*
  (`_dl_fixup`, `call_init`, `__run_exit_handlers`), and two also match a
  source filename — none of which is in glibc's dynamic symbol table. Without
  debug info addr2line answers with the nearest exported symbol instead
  (`_dl_fatal_printf` for the lazy-binding path on Ubuntu 24.04), every
  suppression misses, and a lazy-binding truncation is reported as a real
  mismatch. `ci/install-deps.rb` installs `libc6-dbg` for exactly this; a
  report whose frames all read `(:0)` is the symptom.
* **Signal delivery is modelled explicitly**, because it is the one control
  transfer that inserts frames without a `call`: the kernel builds an
  `rt_sigframe` and jumps at the handler. `TryRecognizeSignalEntry` spots that
  frame at the new sp — no trampoline byte matching and nothing borrowed from
  the unwinder, so the comparer stays an independent oracle for
  `Arch::IsSignalFrame` — and pushes the two entries it accounts for, the
  interrupted pc and the trampoline return address. `rt_sigreturn` is then just
  a `ret`, and so is the handler's own. The interrupted entry keeps a pointer to
  its `rt_sigframe` and re-reads the pc from it at every comparison, since a
  handler may edit the context it resumes through (`aw-backtrace-test`'s does).
  Note a pending call or return ought to be resolved against the *interrupted*
  register file, not the current one — `SignalDelivery` carries it and
  `EntryForRetiredCall` takes the pair for exactly that, but
  `ResolvePendingTransfer` still uses the live one, since the case has never
  been caught happening and guessing wrong corrupts the shadow stack for good.
  None of this was reachable from the old hardware-TF stepper, which never got
  a callback inside a handler (§4.7) — so that stepper never actually compared
  the SIGILL capture `aw-backtrace-test` exists to produce; `v/pstepper` does.

### 4.7 Why the *old* stepper redirects syscalls through a trampoline

This is `v/mini-x86-int`, which the comparer no longer uses — kept because the
hole it describes is what motivated moving to `v/pstepper`, and because the
sibling still has to work.

`EFLAGS.TF` stepping **cannot observe the instruction after a `syscall`**:
`SYSCALL` clears `TF` in hardware, the kernel leaves via `IRET`, and `IRET`
restoring `TF` re-arms stepping only for the instruction *after* it, so two
instructions retire between two callbacks. The kernel closes this hole only for
ptrace, which we don't use. Rather than model it in the comparer, the handler
points `rip` at a private `syscall; nop; …` trampoline and restores the real
resume address on the next trap; the load-bearing details (restore before the
`!active` early return, strict lower bound, `clone`/`clone3` excluded because a
new thread's fresh TLS would send it to `rip` 0) are commented at the site.

**The bigger hole this does not close: signal handlers are invisible**, because
the kernel clears `TF` on handler entry. A coverage gap rather than a
correctness one — a handler that returns normally is invisible but balanced.
Note the sigcontext handed to the target has `TF=1` in it, so a handler that
normalizes eflags silently kills stepping.

### 4.8 The fast path

`aw-backtrace-fastpath.h` is a single `ALWAYS_INLINE` function that reads
`.eh_frame_hdr`, binary-searches the FDE, and decodes just enough of the FDE+CIE
CFI to produce a `FastPathFrame` (SP/FP-relative CFA, optional `%rbp` spill
slot, "end of chain"). It hard-codes the encodings the sane toolchains actually
emit (`eh_frame_hdr` enc `0x1b`/`0x03`/`0x3b`, aug `z...`, the CIE alignment
factors pinned per-arch via `Arch::kCodeAlign` / `kDataAlign` — `1`/`-8` on
x86-64, `4`/`-8` on aarch64) and returns `FastPathFrame::Failure()` — not a
wrong answer — for anything outside that. It never reports a diagnostic and owns
nothing; the bounds math (`Access`, `kSmallBump`, `kSlop`) is what keeps a
malformed `.eh_frame` from walking off the section.

`Access` and `TryFastFrameInfo` are templated on an `Xlate` policy that maps a
decoder pointer to the bytes behind it. Production is `IdentityXlate` (a plain
dereference, compiles away); `fuzz/` swaps in one that redirects reads and
reports accessed offsets. Every raw read the decoder issues goes through
`Access::internal_ptr_as` for this. The struct-header reads (`hdr`, `cie_hdr`,
…) route their *first* touch through it and then the caller re-reads fields
off the returned pointer, so a translating accessor sees one call per struct,
not per field.

`UnwindLoopFastPath` (in `aw-backtrace.cc`) is the loop around it: cache lookup
(non-leaf only) → `TryFastFrameInfo` → `FastPathFrame::ToFrameInfo` → the same
cursor step as `UnwindLoop`. On *any* miss it tail-calls `UnwindLoop` with
`skip_first_callback=true` — so the fast path is a pure accelerator, never a
behaviour change. Neither loop counts frames; both return void and the caller
counts if it cares (`aw_backtrace` returns what it actually stored). Getting
that arithmetic wrong across the handoff was easier than getting it right.

**Every `goto fallback` sits upstream of the cursor commit.** The unwind step
reads into `next_fp`/`next_pc` and only writes `cursor` once every read has
succeeded, and `uc` is cleared after that — so when the fast path hands off,
`UnwindLoop` recomputes the same `is_leaf` and `lookup_pc` and continues from a
cursor that still describes the frame the fast path already reported. Committing
`cursor.fp` early used to leave the two halves describing different frames.

**What is per-arch, and what the encoding buys.** Everything the decoder
assumes about the target lives in `Arch` (`kCodeAlign`, `kDataAlign`,
`kInitialCFAOffset`, the register numbers) rather than in `#if`s. Three of
those matter because aarch64 broke assumptions x86-64 had let stand, or —
for the alignment factors — because its toolchains disagree:

* **A zero CFA offset is real.** aarch64's architectural CFA is `sp+0` — the
  whole CIE program is `def_cfa sp, 0`, and `DW_CFA_def_cfa_offset: 0` closes
  every epilogue — so `cfa_offset == 0` cannot double as "invalid" the way it
  did on x86-64. `FastPathFrame`'s flags therefore live in the bottom three
  bits of `fp_offset`, which are dead anyway (spill offsets are 8-aligned):
  `kFlagFPBased`, `kFlagValid`, `kFlagEndOfChain`. That keeps the struct at 8
  bytes — one register, so `remember_state`/`restore_state` are register moves
  — and it keeps tags out of `cfa_offset`, which makes `DW_CFA_def_cfa_offset`
  (the most frequent state-mutating opcode by a wide margin) a plain store
  instead of a read-modify-write, and lets `ToFrameInfo` use `cfa_offset`
  unmasked on both the SP and FP branches. The `static_assert`s on size and
  `has_unique_object_representations` are what hold that in place.
* **`ra_offset` is live, and zero means "the architectural default".**
  Not "at CFA-8" — `Arch::ResetFrameInfo` supplies the real rule, which is
  CFA-8 on x86-64 and *live in x30* on aarch64. That is what makes
  `DW_CFA_offset`/`DW_CFA_restore` on the RA column arch-neutral.
* **The CIE alignment factors are pinned per-arch, to one pair.** The four
  CIE bytes (`0`, `code_align`, `data_align`, `ret_reg`) are `memcmp`'d
  against the single triple the arch's assembler emits — `1 / -8` on x86-64,
  `4 / -8` on aarch64 — never decoded as LEBs, so `kCodeAlign` and
  `kDataAlign` stay compile-time constants and the scaling constant-folds on
  both arches. aarch64 clang is the wrinkle: it emits `code_align 1 /
  data_align -4` at every `-O` level, which is legal but gratuitously
  different from what gas does, so clang-built objects miss the fast path and
  take the slow one (which decodes the factors and does not care). That is
  ~30 points of sweep coverage on a clang-heavy corpus, accepted on the bet
  that clang gets fixed; a `kHasAltCFIAlignment` branch that carried both
  conventions through the hot loop existed briefly and was removed as not
  worth the weight.

`DW_CFA_AARCH64_negate_ra_state` is accepted as a no-op, matching
`eh-frame-reader.h`. This is not optional on aarch64: gcc emits it as the
second instruction of nearly every FDE under `-mbranch-protection`, the distro
default, so rejecting it would have failed essentially all of glibc.

**The leaf frame needs RA-in-register, and that is a correctness requirement,
not an optimization.** aarch64's CIE leaves the return address in x30, so a
leaf — or the window before a prologue's `stp` / after an epilogue's `ldp` — is
`RegisterRule::InReg(30)`. `UnwindLoopFastPath` reads it out of the `ucontext`.
Bailing instead would bail on *frame 0* of every signal capture and hand the
whole walk to `UnwindLoop`, i.e. the fast path would never run at all on the
profiler captures it exists for. The step also runs `Arch::CleanReturnAddress`
on the recovered pc, as `UnwindLoop` does — identity on x86-64, PAC-stripping
on aarch64.

**It is on the diagnostics path too, not just production.** Both loops are
templated on the `NoDiag`/`RuntimeDiag` policy; `RuntimeDiag::use_fastpath()` /
`use_cache()` read `DiagOptions::disable_fastpath` / `disable_cache`. The
dispatch lives at the top of `UnwindLoopFastPath` itself, so it covers both
entry points and `DiagUnwindLoop` alike; for `NoDiag` on x86-64 it folds to a
compile-time `true` and the branch disappears, and on other arches the whole
fast-path body dead-codes out. Counters `fast_path_frames` /
`fast_path_cache_hits` / `fast_path_fallbacks` show in `PrintStats` (the
per-frame two only under `AW_BUMP_STATS_IN_PRODUCTION`, like the cache's own).

**The comparer cross-checks it every step.** `CaptureReferenceBacktrace` in
`backtrace-comparer.cc` re-captures with `disable_fastpath` + `disable_cache`
and `StepperCallback` requires an exact match against the normal capture before
it even consults the shadow stack — so a fast-path or cache bug is a test
failure with `--fast:` / `--ref:` dumps, independent of the shadow-stack
machinery. It is on by default and doubles the cost of a step, which is the
only reason `AW_BT_CROSS_CHECK=0` (§5) exists. One caveat on "independent":
`StepperCallback` returns early while a shadow-stack suppression is active, and
that early return sits *above* the cross-check, so a suppression switches this
off too until the frame it was set on is popped.

### 4.9 The aarch64 guess

`Arch::GuessUnwindInfo` is per-arch for a structural reason: **`bl` puts the
return address in x30, not on the stack**, so x86-64's central heuristic — "the
word at `*sp` looks like a pc, because `call` pushed it" — has no aarch64
counterpart and must not be ported. At the entry to a callee `*sp` still belongs
to the caller, and very often holds the caller's own spilled x30, so that guess
would silently report the *grandparent*. Two shapes are left:

* **x30 is live** — a leaf that never spilled it, the window before a prologue's
  `stp` and after an epilogue's `ldp`, PLT stubs, `blr` through null. Then
  CFA == sp and the answer is exactly `ResetFrameInfo`. Needs a register file,
  so leaf frames only. The register goes through `CleanReturnAddress` before
  anything looks at it, since FEAT_PAuth leaves a signed pointer there.
* **the AAPCS64 frame record at fp**, `{caller's x29, return address}`.

x86-64's third guess, the mid-prologue one, has no aarch64 analogue worth
writing: on a leaf we always have the register file, so x30 answers that window
with an exact CFA, and a non-leaf pc — being a return address — can never land
inside a prologue.

**x30 first, and the ordering is a damage argument, not a confidence one.**
Nothing distinguishes the two cases locally. x30 is only ever written by a call,
so a *stale* x30 still points just past a `bl` and looks exactly as valid as a
live one; and in the first case fp is the *caller's* record, which validates
just as well as our own would. But the mistakes are not symmetric. Preferring
x30 wrongly costs one spurious frame pointing back into the same function, and
only one — the next step is no longer a leaf, so it cannot consult x30 and walks
the same still-valid fp to the real caller. Preferring the record wrongly
deletes a caller permanently. Repeating a callee beats deleting a caller;
`arm64-leaf-test.cc` pins both halves.

**The frame-record walk takes the CFA from the caller's side.** The record sits
at the *bottom* of the frame in both gcc's and clang's layouts, so `fp + 16` is
only a lower bound on the CFA — the frame size, unknowable without unwind info,
is the gap. `*(fp)` is the caller's x29 instead, and gcc keeps `x29 == sp`
through the body of any function with a static frame, which is exactly when it
also leaves the CFA sp-based (~99% of FDE rows in gcc-built libraries are
`sp+N`; gcc switches to `x29+N` only where sp goes dynamic, and clang switches
always). So the rule is `cfa = DerefFpRel(0)`, `fp = MemFpRel(0)`,
`ra = MemFpRel(8)` — the same identity `InitializeUnwindForCaller` already
relies on. `RegisterRule::MemFpRel` for RA exists only for this; nothing in the
CFI decoder emits it. The shape is not representable in `CompressedFrameInfo`,
which is harmless since guesses are never cached, but it does mean guess results
cannot be made cacheable without widening it.

**It never reads code.** No prologue scanning, no "was the previous instruction
a `bl`" test on candidate pcs — deliberately, so the guess cannot fault on
execute-only text (FEAT_EPAN, `HWCAP2_EXECONLY`) and does not depend on
instruction patterns. A candidate pc must be nonzero, 4-aligned and in an
executable VMA; that is the whole check. The price is the case
`arm64-leaf-test.cc` documents rather than fixes: a CFI-less leaf that carves
scratch space off sp keeps x30 live, so the caller's pc comes back right, but
CFA == sp is then wrong by the size of that area and everything below the
recovered frame is garbage.

### 4.10 The differential sweep, and the one divergence it allows

The fast path's whole contract is *agree with `DoUnwindLookup`, or fail*, and
that is checkable without any of the machinery the comparer needs — which is
the point, because the comparer is x86-64 only. `fastpath-sweep-test.cc` walks
the `.eh_frame_hdr` search table of every loaded module for probe pcs, runs
both decoders, and demands an exact `FrameInfo` match. It is the reason the
aarch64 fast path can be trusted at all, and it is a better oracle for *this
component* than the comparer is, on either arch.

One comparison is deliberately not exact, and it is an agreement rather than a
divergence. `DW_CFA_undefined` on the RA column — the frames with nothing to
return to: process and thread entry points, `_dl_start_user`, `clone`'s child —
is reported by the fast path as `EndOfChain` and by the full decoder as
`kUndefinedRA` (§4.2), and both stop the walk. Since the slow path does not
return `kOk`, those probes are outside the coverage denominator; the sweep
counts them (single digits out of ~36k) rather than treating them as a
mismatch. The check requires *both* sides to say so, which is what stops it
excusing a fast path that invents an end of chain the FDE does not have.

This used to be a genuine asymmetry — the full decoder reported a plain `kFail`
and `UnwindLoop` carried on into the fallback chain, where `GuessUnwindInfo`
might manufacture one more frame — and the comparer never flagged it because
the guess also fails at `_start`. What eventually surfaced it was cost, not
correctness: a `/proc/self/maps` read on the last frame of every backtrace.
`EquivalentForUnwinding` in the sweep still carries the representation
exemption for two Undefined rules; it is now unreachable, kept as a backstop
should the outcome ever be routed back through `kOk`.

## 5. Conventions and gotchas

* C++20, 2-space indent, 120 cols, Google-ish (`.clang-format`). Emacs mode
  lines on nearly every file, SPDX line under them — keep both on new ones.
  clang-format has not been run across the tree recently and several files have
  drifted, so format the block you touched, not the file.
* Everything internal is in `namespace aw_backtrace_internal`; the public API is
  `extern "C"` and `aw_`-prefixed.
* **The capture path stays allocation-free, lock-free and signal-safe.**
  Anything that allocates happens at init or in test-only code. Statics on that
  path are `constinit`/trivially-constructible so they land in `.bss` with no
  initializer-ordering hazard — there are `static_assert`s; don't add a
  constructor to those types.
* Nothing reachable from `eh-frame-reader.h`'s failure path may own anything by
  RAII (§4.2).
* Byte-pattern matching (PLT shapes, sigreturn trampolines, the DRAP `lea`) is
  compiler/linker-version fragile by nature.
* **The `sym_helper` blob carries an explicit empty `.note.GNU-stack`**, added
  by the `objcopy` in both `BUILD.bazel` and `genbuild.rb`. An
  `objcopy -I binary` object has no such section, and a linker older than
  binutils 2.43 reads that as "this program wants an executable stack" — so
  everything linking the blob got `PT_GNU_STACK RWE`, with only a warning to
  show for it. Newer binutils ignores the missing note, which is why this
  survived so long.
* History was squashed at the first public release, so there is no pre-release
  archaeology to do. Infer intent from `TODO`, `README.md` and this file.
* **Debugging.** Run natively, nothing steps at all — `pstepper_enable` fails
  with `ENOSYS`, so every test is gdb-friendly, and `aw-backtrace-test`,
  `amd64-leaf-test` / `arm64-leaf-test` and `cjm0` are all fair game.
  `aw-backtrace-test --nocompare` skips starting it at all. Under qemu it is
  the comparer that fights gdb, and with acceleration on the handler
  runs as *host* code, so attach to qemu rather than to the guest — that is
  what `diag_all_text_sections` is for, as `add-symbol-file` arguments.
  `AW_BT_BREAK_AT=0x<addr>` (or `0x<addr>:<skips>`) no longer stops anything by
  itself: it runs a lone `nop` in `StepperCallback` when the stepped pc matches,
  for you to put a gdb breakpoint on, and it turns acceleration off so that
  breakpoint is in the process you are debugging rather than in qemu.
* **Comparer knobs, all read once in `StartBacktraceComparer`**:
  `AW_BT_REQUIRE_STEPPER=1` says this run is *supposed* to be stepping: no
  pstepper underneath, or an exit having stepped zero instructions, kills the
  process (exit 91) instead of passing silently, and any unsuppressed
  diagnostic fails it at exit (exit 92). It ignores `AW_BT_DIAG` — see §2 — so
  acceleration stays on and every diagnostic is reported before the verdict.
  `ci/run-stepped.rb` sets it; `cjm` under a stepper is the one in-tree
  program that makes it fire.
  `AW_BT_DIAG=N` means "act on the (N+1)-th unsuppressed diagnostic", where
  acting is `raise(SIGTRAP)` — for a debugger to catch, or for the default
  action to kill the process. The default is to report and keep going.
  `AW_BT_CROSS_CHECK=0` turns off the per-step fast-path/cache cross-check
  (§4.8), which is the only thing that doubles the cost of every step.
  `AW_BT_ACCEL=0`/`1` forces pstepper's acceleration off or on; setting
  `AW_BT_DIAG` or `AW_BT_BREAK_AT` turns it off by default, because with it on
  the handler runs as host code and a `raise` from there lands in qemu rather
  than in the target. There is no `AW_BT_DIAG_VIA_CORE` any more — a
  `SIGTRAP` nobody catches dumps core on its own.
* **Comparer diagnostics never go through stdio** — they are `write(2)`s to a
  private dup of stderr taken at startup and moved to fd 900+, because the
  comparer prints from inside the stepper callback and because the target may
  have closed or redirected 0/1/2. `AW_BT_DIAG_FILE=<path>` redirects them. Use
  `DiagPrintf`/`DiagWrite`, never `printf`/`fprintf`, anywhere in that file.
* **Running the comparer against an arbitrary binary:**

  ```
  qemu-x86_64 -plugin v/pstepper/pstepper_plugin.so \
      -E LD_PRELOAD=./backtrace-comparer.so some-program args...
  kill -USR1 <pid>   # cache stats + instructions stepped
  kill -USR2 <pid>   # dump the next captured backtrace
  ```

  **`-E` is load-bearing.** A bare `LD_PRELOAD=… qemu-x86_64 …` preloads the
  comparer into *qemu itself*, where it prints a plausible-looking banner and
  then `pstepper_enable FAILED: Function not implemented` — the guest never
  sees the preload at all, because the constructor `unsetenv`s it. `-E` sets
  the guest's environment instead. Everything is single-stepped, so expect it
  to be *very* slow. `AW_BT_DONT_DROP_PRELOAD=1` keeps the preload for children
  (usually you want it off).

  **Expect zero mismatches** on a small dynamically linked program — `/bin/true
  hello` and friends print three `added cached suppression` lines and nothing
  else. When a run isn't clean: **check the exit code**, not just the mismatch
  count, and **check that a report has a body** — a `Mismatch at 1` with no
  `--bad:`/`--good:` lines means symbolization returned nothing, which is a bug
  in the symbolizer daemon or its protocol (§4.5), not in the unwinder. Frames
  that all read `(:0)` next to a `sym-helper request failed` line on stderr are
  the other symbolizer failure: the daemon is gone and the dump fell back to raw
  addresses.

## 6. Known gaps

From `TODO`: a flags word instead of the callback's `pc_before_insn` bool
(including a signal-frame indicator); whether a signal frame should report
`pc_before_insn = true`; exposing whether a backtrace was
complete/reliable/guessed; finishing and making permanent the `-Wconversion`
cleanup; short-circuiting the walk into `_start`.

On `-Wconversion` specifically: `bazel build --copt=-Wconversion :aw-backtrace`
is down to two warnings, both `-Wsign-conversion` in `aw-addrcheck.c`. Everything else in the core is clean.

True of the tree but not in `TODO`:

* **aarch64 builds and passes the tests, but is not release-ready** — the
  comparer has never run there, and no CI. The old blocker is gone: the
  sim-stepper was x86-only and `PSTATE.SS` is EL1-only, so the self-stepping
  trick could not be ported, but `v/pstepper` steps from outside the guest and
  has an arm64 port. What is left is wiring, not design — `ComparerArch`
  (§4.6) has an aarch64 implementation that compiles and is otherwise
  completely unexercised, `genbuild.rb` has no cross-compile support, and
  `BUILD.bazel` still gates `:pstepper_plumbing` and `MAYBE_COMPARER` to
  x86-64. Flipping those is the next step, and until something has actually
  run, treat the aarch64 `ComparerArch` as a starting point rather than as
  working code. What aarch64 does have now:
  `GuessUnwindInfo` (§4.9) with `arm64-leaf-test.cc` behind it, the fast path
  (§4.8), and the differential sweep (§4.10) holding the latter honest at
  ~43k probes and ~99% coverage in a gcc build (60% under clang, by design —
  §4.8). `GuessUnwindInfo`'s accuracy is still
  unmeasured — see the guess-vs-CFI differ below, which is the same idea as
  the sweep applied to the other half. Still easy to break without noticing
  from an x86 box, since nothing in the default build compiles it;
  `aarch64-linux-gnu-g++ -c` on the core sources is a cheap smoke test (and
  `x86_64-linux-gnu-g++ -c` is the reverse one from an arm64 box).
* **No guess-vs-CFI differ.** The cheapest oracle available for §4.9, and it
  works on either arch: for every frame where `DoUnwindLookup` succeeded, also
  run `GuessUnwindInfo` and diff the resulting cursor step. That measures the
  false-positive rate and the CFA error over any workload, needs no stepping
  machinery at all, and is what should decide whether the residual cases in
  §4.9 are worth more code.
* **The comparer's own machinery is the fastest-moving code in the tree and
  almost none of it is under test.** `BacktraceBuffer`, the truncation rule,
  the return resynchronization and the `sigaction` interposer are validated
  only by "the tests still pass" and by eyeballing a preload run. Empirically not
  enough: three out-of-bounds reads plus both §4.5 descriptor bugs were
  unreachable from anything the tests assert on, and the descriptor ones each
  took a hand-run preload to find. `BacktraceBuffer` is the easy win — it needs
  nothing but `<span>`.
* **No dedicated test of the cache**, and no coverage of its *concurrency* at
  all. Single-threaded correctness is now checked at least: the comparer's
  §4.8 cross-check re-captures with `disable_cache` and diffs, so a stale or
  wrong entry fails a test.
* **CI covers the sweep and the stepped tests, not everything else.** §2's
  `ci/` section:
  four-configuration `bazel test` plus `ci/run-stepped.rb`, and pstepper's own
  suite under the pinned qemu. Not covered: the `genbuild.rb` artifacts
  (`backtrace-comparer.so`, `cjm`, `sym_helper_bin`) beyond what
  `ci/run-stepped.rb` builds, the preload path against arbitrary binaries, the
  fuzzer, aarch64, and any sanitizer configuration.
* No sanitizer configs. The fuzzer (§3) is a crash-only oracle so far;
  the full-decoder wrong-answer cross-check and a hand-built CIE/FDE blob target
  (`eh-frame-reader-test.cc` is the unit-level beachhead) are still open, and it
  is not wired into any automated run.
* Open in-code on the DRAP path: only apply the tail workaround when the main
  DRAP shape was seen.
* Making `DW_CFA_restore` spec-accurate — it restores the CIE's initial rule,
  we restore the architectural default and refuse the frame when the CIE set up
  something else. Nothing in the current corpus exercises the difference.
* `//perf-convert`'s `SelfTest.SweepOwnFDEs` is a threshold over the *test
  binary's own* FDEs, so its bound moves with the toolchain — it has already had
  to be widened once. A fixed input would be better. Now largely subsumed by
  `fastpath-sweep-test` (§4.10), which sweeps more and actually compares
  answers rather than counting failures.
* **`//perf-convert` is still `target_compatible_with` x86_64 only**, and the
  fast path was the reason — it links `//:aw-fastpath` and nothing else of the
  unwinder. That blocker is gone; what remains is checking `perf.data` parsing
  for its own arch assumptions.

### Loose ends the first release shipped with

* **Symbol hygiene is nearly done, and the remaining bits are test-only.** The
  release set (`:aw-backtrace` and its deps) exports twelve unmangled symbols,
  all `aw_`-prefixed, which is what makes the planned `tcmalloc_` renaming for
  gperftools a small bounded job. What is *not* prefixed —
  `simple-fp-backtrace`'s `simple_backtrace`, `symbolize-backtrace.h`'s types in
  the global namespace, `_binary_sym_helper_bin_*` — belongs to targets that
  `:aw-backtrace` does not depend on. `aw-addrcheck.h` declares a test-only
  ioctl-disable setter, but that header is internal (only
  `include/aw-backtrace/aw-backtrace.h` is exported), so it is not a public API
  wart.
* Naming mixes dashes and underscores (`function_ref.h`, `static_storage.h`,
  target `sym_helper_obj`…); new files go the dashed way.
* No `make install`, no pkg-config, no CMake. Bazel or vendoring, for now.
