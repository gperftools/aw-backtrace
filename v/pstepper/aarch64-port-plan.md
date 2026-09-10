# pstepper: aarch64 port — scope and analysis

Status: **analysis only, nothing implemented.** This is the planning note for
porting pstepper (currently x86-64 only) to `qemu-aarch64` linux-user. A
separate pass is needed for RISC-V (sketched at the end).

The high-level design is unchanged — see the root `README.md` for the
four-part architecture. This document is only about what is *not* a
mechanical translation.

---

## Direct answer: the [PC][SP] pair stays in memory, LR is untouched

Nothing about aarch64 changes the resume mechanism. The plugin does **not**
put a return address in LR (X30) or anywhere else — X30 is live
general-purpose guest state at an arbitrary instruction boundary, and
clobbering it would corrupt the stepped program. So, exactly as on x86-64:

- The plugin stages `[trapped_PC][trapped_SP]` at `upcall_stack_top - 16`,
  points SP there, and `qemu_plugin_set_pc(trampoline)`.
- The trampoline saves X30 into the frame as **just another GPR** and
  restores it in the epilogue.
- At the resume marker, X30 already holds the guest value again, SP points
  at the floating `[PC][SP]` pair, and the plugin reads those two words,
  writes SP, and `set_pc`s to the trapped PC.

LR is never a special case. The x86 `[RIP][RSP]` staging ports 1:1.

---

## Non-trivial items, ranked by risk

### 1. LL/SC single-stepping (LDXR/STXR) — the main unknown

x86-64 atomics are `lock cmpxchg`: one instruction, no monitor. aarch64
atomics compile to `ldxr / <op> / stxr / cbnz` retry loops. On **real
hardware**, any exception taken between LDXR and STXR clears the exclusive
monitor, so STXR always fails and single-stepping livelocks — a known
aarch64 debugger problem (gdb steps over the whole sequence atomically).

The open question is whether **QEMU's emulated monitor** avoids this. QEMU
linux-user implements the monitor as an address+value comparison
(`env->exclusive_addr` / `exclusive_val`), not an interruption flag — so
trapping between LDXR and STXR, running the entire trampoline + handler
(which never touches that address or `exclusive_addr`), then resuming
*should* leave STXR able to match and succeed.

- **Believed OK, must be verified early.** Test: a contended
  `__atomic_fetch_add` loop (2+ threads) single-stepped, assert it
  terminates and the sum is correct.
- **If it livelocks**, mitigation: the plugin recognizes `ld[a]xr{,p}` at
  the trap PC and defers the upcall until execution passes the matching
  `st[l]xr`. Bounded but fiddly, and slightly bends the "plugin knows
  nothing guest-specific" rule (it would be recognizing an instruction
  class, not guest semantics, so arguably still fine).

### 2. `-one-insn-per-tb` — probably not load-bearing, keep it anyway

The x86 requirement exists purely because x86 TCG tracks condition codes
lazily (`cc_op`/`cc_src`/`cc_dst` are only spilled to `env` at TB
boundaries), so a trampoline `pushfq` entered mid-stream via `cpu_loop_exit`
reads stale flags.

**aarch64 TCG updates NF/ZF/CF/VF eagerly, inline per instruction**, so
`mrs nzcv` after a mid-TB exit should see coherent flags. The classic x86
failure (a conditional branch depending on a `cmp` two instructions prior
reading frozen flags) should simply not reproduce.

- Recommendation: keep the flag as a safety hammer for initial bring-up —
  it is free, since pstepper traps every instruction regardless — then test
  whether it can be dropped.
- The `qemu_plugin_tb_n_insns(tb) > 1` detection in `vcpu_tb_trans` ports
  as-is if the requirement is kept.
- Caveat: this analysis assumes aarch64 TCG has *no* other TB-boundary-only
  deferred state that a mid-TB `cpu_loop_exit` would leave incoherent. Not
  exhaustively checked. The LL/SC exclusive state (#1) is the one known
  candidate and is tracked separately.

### 3. Vector state — NEON is trivial, SVE is the analogue of x86's xsave mess

- **NEON**: save/restore V0–V31 + FPSR + FPCR with plain `stp q, q` × 16.
  Fixed ~528 bytes, no alignment constraint beyond 16, no header to zero,
  no runtime mask. This is *simpler* than x86 `xsave` — the easy part.
- **SVE**: Z0–Z31 / P0–P15 / FFR are **variable-size** (VL-dependent, up to
  ~9 KB at VL = 2048). Saving them needs `rdvl` plus a `HWCAP_SVE` /
  `ID_AA64PFR0_EL1.SVE` check — the direct mirror of x86's
  `xgetbv` + `CPUID.0Dh` dance — and a variable frame size fights the
  fixed-size-POD-`pstepper_frame_t` property the clone hook relies on
  (`*cf = *pframe`). Saving only Q0–Q31 covers the NEON ABI view but not
  SVE upper bits or predicates, so a glibc SVE-optimized routine
  (`__memcpy_sve`, etc.) running inside the handler could corrupt a
  mid-SVE-computation guest.
- **SME** (streaming mode, ZA tile): even worse — entering the handler may
  need to leave streaming mode / manage `PSTATE.SM`/`ZA`.

**Recommendation**: baseline = **NEON-only** save/restore. Same rationale as
"a backtracer never needs FP register values" — we only save V0–V31 so the
handler chain (which touches NEON via libc) does not wreck the common case.
Add:
- a documented gap for SVE upper bits / predicates,
- a `die()` (or warn) guard if SVE is detected in use, and
- a hard `die()` guard on `PSTATE.SM` / `PSTATE.ZA` for SME.

Full SVE fidelity is a follow-up project, not part of the baseline.

### 4. Flags: there is no `pushf` / `popf`

Use `mrs Xn, NZCV` / `msr NZCV, Xn`. NZCV is **not** callee-saved in
AAPCS64, and the guest can be trapped between `cmp` and `b.eq`, so the
trampoline must save and restore it — the same need as x86 eflags. Two
things are *easier* than x86:

- `sub` / `stp` do not affect flags (x86 needed `pushfq` literally first
  because `sub rsp` sets flags). So the trampoline can save X0/X1 first,
  then `mrs nzcv` into X0 — the ordering constraint is relaxed.
- NZCV lives **inside** the frame (there is no push mechanism forcing it to
  float above the frame the way x86 eflags does).

Epilogue discipline: nothing flag-setting between `msr nzcv` and the resume
marker (same rule as "nothing between `popfq` and `ud2`" on x86).

### 5. clone(2) argument order differs

aarch64 raw `clone` is `(flags, stack, parent_tid, tls, child_tid)` in
x0–x4. x86-64 is `(flags, stack, parent_tid, child_tid, tls)` in
rdi/rsi/rdx/r10/r8 — **tls and child_tid are swapped** (documented in
`man 2 clone`). So both the register mapping and the semantic slot order
change; the clone-helper asm and `pstepper_do_clone_intercept` need
arch-specific arg extraction.

- Parent PC advance is **+4** (svc is 4 bytes; all aarch64 instructions are
  fixed-width) instead of x86's +2. Simpler.
- glibc's aarch64 `__clone` child reads func/arg off the child stack
  (`ldp x1, x0, [sp], #16`), the same shape as x86's legacy `__clone`, so
  the "build the child a complete trampoline frame pre-fork, resume via
  `pstepper_resume_via_frame`" solution ports directly.
- clone3: verify, but almost certainly ENOSYS in QEMU linux-user for
  aarch64 too (clone3 is not arch-gated in `linux-user/syscall.c` — if it
  is missing for one target it is missing for all), so glibc keeps falling
  back to legacy `clone` and the existing punt holds.

### 6. ucontext conversion — actually simpler than x86

aarch64 glibc `mcontext_t`:

```c
typedef struct {
    unsigned long long fault_address;
    unsigned long long regs[31];   /* x0 .. x30, natural order */
    unsigned long long sp;
    unsigned long long pc;
    unsigned long long pstate;
    /* __reserved[]: FP state as an _aarch64_ctx TLV chain */
} mcontext_t;
```

No `REG_*` permutation like x86's `gregset_t`. Lay `pstepper_gpregs_t` out
to match from `regs[0]` onward and the conversion is a clean copy plus
sp/pc; `fault_address` and `pstate` are fake-zeroed (`pstate` could carry
NZCV later — same deliberate deferral as x86 leaving `REG_EFL` at 0). FP
state is an inline TLV chain, not a pointer, so leaving `__reserved` zeroed
(immediate chain terminator) is the exact analogue of x86's
`fpregs = NULL`.

**Confirmed sufficient for the real consumer**: `aw-arch-aarch64.h` only
reads `uc_mcontext.regs[]`, `.sp`, `.pc` (and `regs[29]` as the frame
pointer). Nothing reads `pstate` or the FP chain.

### 7. Resume marker

`UDF` (the all-zero word) or `BRK #imm`, recognized by **address** — the
direct analogue of `ud2`, with the same caveat that compilers emit a bare
`UDF` for `__builtin_trap()` / UBSan, so matching must be address-only, not
opcode-only. Intercepted by the insn_exec callback before it faults, exactly
as `ud2` is.

### 8. Magic syscall

`svc #0`, number in x8, args x0–x5. Needs a new inline-asm
`pstepper_syscall()`. `PSTEPPER_SYSCALL_NR = 0x50535445` is still safe (the
aarch64 generic syscall table tops out around 460; the value fits in a
signed 32-bit int, which is what `do_syscall` truncates to). Verify the
large number reaches the syscall-filter callback before ENOSYS — very likely
the same `do_syscall` path as x86.

### 9. Smaller items / verify during bring-up

- Plugin register handle: `find_reg("sp")` instead of `"rsp"`; PC is handled
  by the arch-neutral `qemu_plugin_set_pc`.
- BTI: if guest binaries are built with branch protection, make the
  trampoline's first instruction `bti c` / `bti jc` as cheap insurance, and
  check whether QEMU enforces BTI on a plugin `set_pc` at all (probably
  not).
- PAC: aw-backtrace's concern, not pstepper's — it already strips PAC bits
  in `CleanReturnAddress`. The trampoline saves/restores X30 verbatim, so a
  signed LR round-trips fine.
- MTE: the upcall stack is untagged `PROT_READ|PROT_WRITE` and the plugin
  sets an untagged SP, so untagged-access-to-untagged-memory is fine; note
  it if the guest runs under MTE.
- SP must be 16-byte aligned for every SP-relative access; keep
  `PSTEPPER_FRAME_SIZE % 16 == 0` (contrast x86's 64-byte `xsave`
  constraint — aarch64's is easier).
- Hook opcode check: read the 4-byte instruction word and compare
  `== 0xD4000001` (`svc #0`).
- Hardware single-step (`MDSCR_EL1.SS`) is irrelevant — pstepper is
  plugin-driven, and aarch64 HW single-step is EL1-privileged and unusable
  from linux-user anyway.

### 10. Build system

`genbuild.rb` needs cross-compilation support: the guest test programs are
cross-built (`aarch64-linux-gnu-gcc`) and run under `qemu-aarch64`, but the
**plugin `.so` stays host-native** (the host QEMU loads it). Likely a target
parameter or parallel build directories, plus a `QEMU_AARCH64` runner
variable alongside `QEMU_X86_64`. `-one-insn-per-tb` unchanged. Moderate.

---

## What ports trivially

- The plugin's `in_trap` state machine — pure arch-neutral logic.
- Every `qemu_plugin_*` callback API used (`insn_exec_cb`, `tb_trans_cb`,
  `syscall_filter_cb`, `set_pc`, register read/write).
- The four-part architecture and the layering rules.
- The `PSTEPPER_CMD_ENABLE` / `_DISABLE` command protocol and its args.
- The resume-marker-by-address concept.
- The clone / thread-exit hook *structure* — only the recognition
  predicates and arg extraction change.
- The "one-way, fake, gregs-only" ucontext contract.
- The self-checking-guest test pattern (native in-process reference,
  exit 0 == pass).

---

## Rough scope

| Area | Effort |
|---|---|
| Trampoline `.S` rewrite (GPRs + NZCV + NEON) | ~1 day, mechanical once the frame layout is fixed |
| `pstepper_frame.h` aarch64 offsets + `_Static_assert`s | few hours |
| Magic syscall + arm/disarm plumbing | few hours |
| clone / exit hooks (arg order, +4, opcode) | ~1 day |
| ucontext conversion | few hours (simpler than x86) |
| Plugin: register name, opcode consts, target check | few hours |
| Build: cross-compile support in genbuild.rb | ~1 day |
| **LL/SC verification (+ mitigation if needed)** | 1 day if it just works; **several days** if it livelocks |
| **SVE decision + guard** | few hours for NEON-only + guard; ~1 week for full SVE |
| genbuild multi-target + CI wiring | ~1 day |

A clean baseline — NEON-only, LL/SC works under emulation, `-one-insn-per-tb`
kept — is roughly **1 to 1.5 weeks**. The two variables that can blow that
up are LL/SC livelock (#1) and any requirement for full SVE fidelity (#3).

---

## RISC-V — separate analysis needed

Different enough to warrant its own pass. Initial flavor:

- **No condition flags at all** — comparisons feed directly into branch
  instructions. So there is no NZCV/eflags equivalent to save; the
  trampoline is simpler in that dimension.
- **`ecall`** for syscalls; number in `a7`, args `a0`–`a5`.
- **Compressed instructions (C extension)**: instructions are 2 *or* 4
  bytes, so the "advance PC past the trapped syscall" fixup and any
  opcode-length reasoning must read the instruction to know its width
  (`ecall` itself is always the 4-byte `0x00000073`, and `c.ebreak` vs
  `ebreak` matters for the marker).
- **A-extension LR/SC** has the same monitor concern as aarch64 #1, and the
  RISC-V ISA additionally constrains what may appear in an LR/SC loop body
  for forward-progress guarantees — so a "defer across the sequence"
  mitigation has a well-defined window.
- Marker: `EBREAK`, or a reserved/illegal encoding, recognized by address.
- FP/vector: F/D extensions are fixed 32 registers (easy); the V extension
  is scalable (VLEN-dependent) — same shape as aarch64 SVE, same
  NEON-only-equivalent baseline recommendation.
