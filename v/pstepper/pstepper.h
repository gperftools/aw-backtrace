/*
 * pstepper: shared ABI between guest programs and the pstepper QEMU plugin.
 *
 * This header defines the "magic syscall" interface that stands in for the
 * imagined kernel facility: a per-thread single-step trap that delivers a
 * UXI (User eXecution Interrupt) to a registered handler at every
 * instruction boundary. UXI is pstepper's own term, deliberately distinct
 * from a POSIX "signal" -- it's synchronous and tied to instruction
 * boundaries rather than async and tied to arbitrary points, and the two
 * should never be conflated when reading this code or talking about it.
 * Delivering a UXI makes an upcall into the registered handler; that upcall
 * runs on a dedicated "upcall stack" (registered at PSTEPPER_CMD_ENABLE
 * time) -- pstepper's analogue of a POSIX sigaltstack, again named
 * distinctly on purpose. Guest code (and only guest code -- the consumer,
 * e.g. a future backtrace-comparer port, is meant to be written against
 * this header and have zero awareness that a QEMU plugin implements it)
 * issues these via a raw `syscall` instruction with rax =
 * PSTEPPER_SYSCALL_NR.
 *
 * Design (v2): the plugin itself does almost nothing. On a trap it writes
 * two words -- the trapped instruction's original RIP and RSP -- onto the
 * guest's registered upcall stack, points RSP there, and jumps to a
 * guest-side trampoline (pstepper_trampoline, in pstepper_trampoline.S).
 * From that point on it's ordinary guest code, running at full speed and
 * using real hardware instructions (push/mov/xsave/pushfq) to save and
 * restore its own state -- the plugin never marshals a register file or
 * touches eflags itself. Resuming is a custom, otherwise-invalid
 * instruction (ud2) that the plugin recognizes by *address* (it only ever
 * legitimately appears at one place: inside our own trampoline) and
 * interprets as "pop two words for RIP/RSP off the current stack and
 * jump" -- our own minimal analogue of Intel UINTR's `uiret`.
 *
 * The plugin tracks one flag per vCPU, "trap parked", spanning a genuine
 * trap through to the trapped instruction retiring after resume. While it is
 * set -- trampoline, handler, and any guest signal handler QEMU interposes
 * in that window -- nothing is re-intercepted. No RSP-range bookkeeping is
 * involved; the plugin is told only the aligned top of the upcall stack.
 *
 * This header is the guest-facing ABI only: types, constants, and the
 * entry points a guest program actually calls. The trampoline itself lives
 * in pstepper_trampoline.S (hand-written asm; see pstepper_frame.h for the
 * stack frame layout it and pstepper.c both agree on), and the
 * higher-level plumbing (arming/disarming, dispatching to the registered
 * handler) lives in pstepper.c.
 */
#ifndef PSTEPPER_H_
#define PSTEPPER_H_

#include <stddef.h>
#include <stdint.h>

/*
 * For gregset_t's REG_* indices, which the _Static_assert block below pins
 * pstepper_gpregs_t's field order to. This is glibc's x86-64
 * <ucontext.h>: the REG_* enum lives behind __USE_GNU, so every
 * translation unit that includes this header must be built with
 * -D_GNU_SOURCE (the pstepper build sets it in GUEST_CFLAGS / PLUGIN_CFLAGS).
 */
#include <ucontext.h>

/*
 * ASCII "PSTE" -- comfortably outside the real x86-64 syscall table (which
 * tops out a bit above 500), so it can never collide with a real syscall.
 * Must fit in a 32-bit int: linux-user's do_syscall() takes `num` as a
 * plain `int`, so anything wider gets silently truncated before a plugin
 * ever sees it.
 */
#define PSTEPPER_SYSCALL_NR 0x50535445ULL

enum pstepper_cmd {
    /*
     * arg1 = trampoline entry PC (pstepper_trampoline)
     * arg2 = upcall stack top (guest vaddr): the high end of the upcall
     *        stack, 64-byte aligned (the trampoline's xsave area alignment
     *        derives from this -- see pstepper_frame.h). The trampoline is
     *        entered with RSP = top - 16 and the trapped [RIP][RSP] pair
     *        staged there. Only the top matters to the plugin -- it never
     *        needs the base or the size (the guest side keeps those, for
     *        munmap).
     * arg3 = resuming: 0 for a normal arm from ordinary code; nonzero if the
     *        calling thread is already running on the upcall stack and is
     *        about to resume through a pre-built trampoline frame (only the
     *        clone hook's bootstrap does this). When nonzero the plugin
     *        starts the vCPU in its "trap parked" state, so the bootstrap
     *        tail and first resume are not treated as steppable.
     * arg4 = resume marker PC (pstepper_resume_marker, the `ud2` inside the
     *        trampoline) -- the plugin recognizes execution reaching this
     *        exact address as "pop [RSP]/[RSP+8] into RIP/RSP and resume",
     *        rather than as a real illegal-instruction fault.
     *
     * Arms single-stepping for the calling thread. The upcall stack must be
     * sized generously -- as with a POSIX sigaltstack, the entire handler
     * call chain (trampoline, the user handler, anything it calls) runs on
     * it.
     */
    PSTEPPER_CMD_ENABLE = 1,

    /* Disarms single-stepping for the calling thread. */
    PSTEPPER_CMD_DISABLE = 3,
};

/* The (currently only) kind of UXI, handed to the handler in its first
 * argument -- shaped like a real sa_sigaction(int sig, siginfo_t *info,
 * void *ucontext) handler for familiarity, but this is a UXI kind, not a
 * signal number. The second argument (siginfo_t*) is always NULL -- nothing
 * populates it in this first cut. The third argument is a "fake"-ish
 * ucontext_t (see pstepper_handler_fn below). */
#define PSTEPPER_UXI_STEP 1

/*
 * Raw syscall, args in rdi/rsi/rdx/r10/r8 order -- i.e. cmd lands in the
 * filter callback's a1, arg1 in a2, arg2 in a3, arg3 in a4, arg4 in a5. See
 * qemu_plugin_vcpu_syscall_filter_cb_t: num is the syscall number (rax),
 * a1 is rdi, a2 is rsi, a3 is rdx, a4 is r10, a5 is r8.
 */
static inline uint64_t pstepper_syscall(uint64_t cmd, uint64_t arg1,
                                         uint64_t arg2, uint64_t arg3,
                                         uint64_t arg4)
{
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = arg3;
    register uint64_t r8 __asm__("r8") = arg4;
    __asm__ volatile("syscall"
                      : "=a"(ret)
                      : "a"(PSTEPPER_SYSCALL_NR), "D"(cmd), "S"(arg1),
                        "d"(arg2), "r"(r10), "r"(r8)
                      : "rcx", "r11", "memory");
    return ret;
}

/*
 * General-purpose register snapshot handed to the pstepper handler. This is
 * pstepper's own minimal, portable context -- only the integer registers,
 * since that is all a backtracer ever needs. FP + full vector state (via
 * xsave/xrstor) and eflags are transparently preserved by the trampoline
 * around the handler call (so a real program's floating-point/SIMD state is
 * never disturbed), but never exposed here. There is no promise that edits
 * made to this struct are reflected back on resume.
 *
 * The field order deliberately mirrors x86-64 glibc's gregset_t -- the
 * REG_* indices in <sys/ucontext.h> (REG_R8=0 .. REG_R15, REG_RDI, REG_RSI,
 * REG_RBP, REG_RBX, REG_RDX, REG_RAX, REG_RCX, REG_RSP, REG_RIP), and the
 * _Static_assert block below build-checks that against the real header. The
 * struct still isn't a ucontext_t (no signal mask, no FP pointer, no
 * segment/error words); the point of the correspondence is only that
 * synthesizing a "fake"-ish ucontext_t from this snapshot is a straight
 * index mapping rather than a field-by-field shuffle.
 */
typedef struct pstepper_gpregs {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx;
    uint64_t rsp;
    uint64_t rip;
} pstepper_gpregs_t;

/*
 * gregset_t is greg_t[__NGREG]; each pstepper_gpregs_t field must sit at the
 * byte offset its REG_* index would land at in that array, so a fake
 * ucontext_t is `memcpy(uc->uc_mcontext.gregs, regs, sizeof *regs)` on the
 * covered prefix. greg_t is `long long`, i.e. 8 bytes == our uint64_t.
 */
_Static_assert(sizeof(uint64_t) == sizeof(greg_t), "greg_t is not 64-bit");
#define PSTEPPER_ASSERT_GREG(field, reg) \
    _Static_assert(offsetof(pstepper_gpregs_t, field) == (reg) * sizeof(greg_t), \
                   "pstepper_gpregs_t." #field " does not match gregset_t[" #reg "]")
PSTEPPER_ASSERT_GREG(r8,  REG_R8);
PSTEPPER_ASSERT_GREG(r9,  REG_R9);
PSTEPPER_ASSERT_GREG(r10, REG_R10);
PSTEPPER_ASSERT_GREG(r11, REG_R11);
PSTEPPER_ASSERT_GREG(r12, REG_R12);
PSTEPPER_ASSERT_GREG(r13, REG_R13);
PSTEPPER_ASSERT_GREG(r14, REG_R14);
PSTEPPER_ASSERT_GREG(r15, REG_R15);
PSTEPPER_ASSERT_GREG(rdi, REG_RDI);
PSTEPPER_ASSERT_GREG(rsi, REG_RSI);
PSTEPPER_ASSERT_GREG(rbp, REG_RBP);
PSTEPPER_ASSERT_GREG(rbx, REG_RBX);
PSTEPPER_ASSERT_GREG(rdx, REG_RDX);
PSTEPPER_ASSERT_GREG(rax, REG_RAX);
PSTEPPER_ASSERT_GREG(rcx, REG_RCX);
PSTEPPER_ASSERT_GREG(rsp, REG_RSP);
PSTEPPER_ASSERT_GREG(rip, REG_RIP);
#undef PSTEPPER_ASSERT_GREG
/* rip is the last field, and every field from REG_R8=0 up to it is covered
 * with no gaps -- so the struct is exactly a prefix of gregset_t. */
_Static_assert(sizeof(pstepper_gpregs_t) == (REG_RIP + 1) * sizeof(greg_t),
               "pstepper_gpregs_t is not a gap-free prefix of gregset_t");

/*
 * The registered upcall handler. Deliberately shaped like a POSIX
 * sa_sigaction handler so the consumer above (part 3 -- a future
 * backtrace-comparer port) can be written as if it were an ordinary signal
 * handler and stay unaware that a QEMU plugin, rather than a kernel,
 * delivers the context.
 *
 *   uxi_kind  -- PSTEPPER_UXI_* (not a signal number).
 *   info      -- always NULL for now (analogue of siginfo_t*).
 *   uc        -- a "fake"-ish ucontext_t. Only the integer register file is
 *                real: uc->uc_mcontext.gregs[REG_R8..REG_RIP] hold the
 *                trapped instruction's registers (RIP is the address of the
 *                instruction *about to* execute). Everything else is fake --
 *                gregs[REG_EFL] and the other scalar slots are zero,
 *                uc_mcontext.fpregs is NULL (no FP/SIMD state is exposed,
 *                though it is preserved transparently around the upcall),
 *                uc_stack / uc_sigmask / uc_flags / uc_link are zeroed. The
 *                conversion is one-way: edits the handler makes to uc are
 *                NOT reflected back when the trapped instruction resumes.
 */
typedef void (*pstepper_handler_fn)(int uxi_kind, void *info,
                                     ucontext_t *uc);

/* Defined in pstepper_trampoline.S. */
extern void pstepper_trampoline(void);
extern void pstepper_resume_marker(void);

/*
 * Defined in pstepper.c; called only from pstepper_trampoline.S once it has
 * built the gpregs snapshot. Always either returns normally (an ordinary
 * step -- the trampoline falls through to its usual restore-and-resume) or
 * diverts internally and never returns at all (e.g. when the trap turns out
 * to be a CLONE_THREAD syscall about to happen, handled entirely on this
 * side before the trampoline ever gets control back).
 */
void pstepper_invoke_handler(pstepper_gpregs_t *regs);

/*
 * Defined in pstepper.c. pstepper_enable() mmaps its own upcall stack of
 * upcall_stack_size bytes (remembered process-wide for the thread-creation
 * hook to reuse when bootstrapping later threads) and frees it again in
 * pstepper_disable().
 */
void pstepper_enable(pstepper_handler_fn handler, uint64_t upcall_stack_size);
void pstepper_disable(void);

#endif /* PSTEPPER_H_ */
