/*
 * pstepper_trampoline: what the plugin jumps PC to on every trap -- the
 * start of a UXI upcall. Entry state, arranged by the plugin: RSP =
 * upcall_stack_top - 16, with [RSP] = trapped RIP and [RSP+8] = trapped RSP
 * (the guest's real stack pointer at the moment of the trap -- untouched,
 * never written to).
 *
 * Sequence:
 *   1. pushfq immediately -- captures the real, original eflags before our
 *      own subsequent instructions (e.g. the frame `sub`) have a chance to
 *      disturb it. This floating word, and the [RIP][RSP] pair already on
 *      the stack, sit *above* the frame we're about to open and are never
 *      touched again until the matching restore sequence at the end.
 *   2. Open the frame (sub) and save every GPR into it via plain `mov`
 *      (never affects flags or RSP -- safe regardless of ordering), then
 *      `fxsave` for FP/SSE/AVX state (real hardware, so no risk of the
 *      lazy-condition-code correctness trap we hit marshalling eflags
 *      through the plugin's register API in the old design -- see
 *      pstepper_project memory).
 *   3. Copy the floating RIP/RSP words into gpr.rip/gpr.rsp (so the
 *      handler can read them); the floating eflags word is left alone --
 *      untouched, ready to be reused as-is when we get to step 5, since
 *      nothing the handler or its callees do can reach memory *above* our
 *      frame (only below, as they push further).
 *   4. Call into the handler with a pointer to the frame's gpregs.
 *   5. On return: fxrstor, restore every GPR from the frame, close the
 *      frame (add), then `popfq` -- which both restores the real original
 *      eflags and advances RSP by 8, landing exactly on the still-intact
 *      [RIP][RSP] pair.
 *   6. Execute the resume marker (ud2). The plugin recognizes this
 *      specific address, reads [RSP]/[RSP+8], and applies them as the new
 *      guest RIP/RSP -- completing the resume atomically from the guest's
 *      point of view -- our own minimal analogue of Intel UINTR's `uiret`.
 *
 * No dedicated scratch/frame-pointer register is needed anywhere: RSP
 * itself is the addressing base throughout, and it only moves at the
 * pushfq/sub/add/popfq points accounted for above.
 */
#include "pstepper_frame.h"

/* x86-64 raw syscall numbers (asm/unistd_64.h) -- only needed here; the C
 * side (pstepper.c) uses glibc's own SYS_* from <sys/syscall.h> instead of
 * sharing these definitions. */
#define PSTEPPER_SYS_CLONE  56
#define PSTEPPER_SYS_MUNMAP 11
#define PSTEPPER_SYS_EXIT   60

	.text
	.global pstepper_trampoline
	.type pstepper_trampoline, @function
pstepper_trampoline:
	pushfq
	sub $PSTEPPER_FRAME_SIZE, %rsp
	mov %rax, PSTEPPER_OFF_RAX(%rsp)
	mov %rbx, PSTEPPER_OFF_RBX(%rsp)
	mov %rcx, PSTEPPER_OFF_RCX(%rsp)
	mov %rdx, PSTEPPER_OFF_RDX(%rsp)
	mov %rsi, PSTEPPER_OFF_RSI(%rsp)
	mov %rdi, PSTEPPER_OFF_RDI(%rsp)
	mov %rbp, PSTEPPER_OFF_RBP(%rsp)
	mov %r8,  PSTEPPER_OFF_R8(%rsp)
	mov %r9,  PSTEPPER_OFF_R9(%rsp)
	mov %r10, PSTEPPER_OFF_R10(%rsp)
	mov %r11, PSTEPPER_OFF_R11(%rsp)
	mov %r12, PSTEPPER_OFF_R12(%rsp)
	mov %r13, PSTEPPER_OFF_R13(%rsp)
	mov %r14, PSTEPPER_OFF_R14(%rsp)
	mov %r15, PSTEPPER_OFF_R15(%rsp)
	fxsave PSTEPPER_OFF_FXSAVE(%rsp)
	mov PSTEPPER_FRAME_SIZE+8(%rsp), %rax
	mov %rax, PSTEPPER_OFF_RIP(%rsp)
	mov PSTEPPER_FRAME_SIZE+16(%rsp), %rax
	mov %rax, PSTEPPER_OFF_RSP(%rsp)
	mov %rsp, %rdi
	call pstepper_invoke_handler
	/*
	 * pstepper_invoke_handler either returns normally (an ordinary step --
	 * fall through to the usual restore-and-resume below) or diverts
	 * internally and never returns at all -- e.g. for a trap that turned
	 * out to be a CLONE_THREAD syscall about to happen, which it handles
	 * entirely on the C side (see pstepper.c) before ever giving control
	 * back here. Either way RSP is unchanged since the call, so it's still
	 * the frame pointer.
	 */
pstepper_trampoline_epilogue:
	fxrstor PSTEPPER_OFF_FXSAVE(%rsp)
	mov PSTEPPER_OFF_RAX(%rsp), %rax
	mov PSTEPPER_OFF_RBX(%rsp), %rbx
	mov PSTEPPER_OFF_RCX(%rsp), %rcx
	mov PSTEPPER_OFF_RDX(%rsp), %rdx
	mov PSTEPPER_OFF_RSI(%rsp), %rsi
	mov PSTEPPER_OFF_RDI(%rsp), %rdi
	mov PSTEPPER_OFF_RBP(%rsp), %rbp
	mov PSTEPPER_OFF_R8(%rsp),  %r8
	mov PSTEPPER_OFF_R9(%rsp),  %r9
	mov PSTEPPER_OFF_R10(%rsp), %r10
	mov PSTEPPER_OFF_R11(%rsp), %r11
	mov PSTEPPER_OFF_R12(%rsp), %r12
	mov PSTEPPER_OFF_R13(%rsp), %r13
	mov PSTEPPER_OFF_R14(%rsp), %r14
	mov PSTEPPER_OFF_R15(%rsp), %r15
	add $PSTEPPER_FRAME_SIZE, %rsp
	popfq
	.global pstepper_resume_marker
	.type pstepper_resume_marker, @function
pstepper_resume_marker:
	ud2
	.size pstepper_trampoline, . - pstepper_trampoline
	.size pstepper_resume_marker, . - pstepper_resume_marker

/*
 * pstepper_clone_helper(frame, continue_rip, child_upcall_base,
 * child_frame_base): a real (noreturn) call from
 * pstepper.c's pstepper_do_clone_intercept, which has already decided the
 * trapped instruction is a CLONE_THREAD clone(2) about to happen and done
 * all the pre-fork prep -- mmapping the child's own upcall stack, computing
 * continue_rip, and building the child a complete ready-to-resume
 * trampoline frame at child_frame_base on that stack -- *before* calling
 * here, i.e. before anything forks, while still safely single-threaded.
 * This issues the *real* clone itself and completes the trap for both
 * resulting threads.
 *
 * Why the fork/syscall itself still can't be portable C calling a normal
 * clone()/syscall() wrapper: when a real child_stack is given (as it is
 * here -- the guest's own original 2nd clone(2) argument, read below), the
 * CHILD resumes with RSP already pointing at that stack, not the caller's.
 * A normal C function's own trailing `ret` pops whatever's at the *current*
 * RSP -- for the child that's uninitialized memory on a stack that never
 * had a return address pushed onto it, so it would jump into the void
 * immediately. The raw syscall has to be issued directly here, with the
 * branch on its result immediately following in the same, uninterrupted
 * instruction stream.
 *
 * Race note: after the syscall the parent touches only its own frame,
 * sequentially (same as any ordinary trap); the child touches the parent's
 * frame not at all. Everything the child needs is either in its own
 * private registers (copied wholesale at fork) or in the frame the caller
 * pre-built on the child's *own* upcall stack.
 */
	.global pstepper_clone_helper
	.type pstepper_clone_helper, @function
pstepper_clone_helper:
	mov %rdi, %r15   /* parent frame pointer (parent branch only) */
	mov %rsi, %r12   /* continue_rip, already +2'd (parent branch only) */
	mov %rdx, %r13   /* child_upcall_base (child branch only) */
	mov %rcx, %rbx   /* child_frame_base (child branch only) */

	mov PSTEPPER_OFF_RDI(%r15), %rdi  /* raw clone(2) args: flags */
	mov PSTEPPER_OFF_RSI(%r15), %rsi  /* child_stack -- the child branch no
	                                    * longer needs it (baked into the
	                                    * pre-built frame), but the real
	                                    * syscall still does */
	mov PSTEPPER_OFF_RDX(%r15), %rdx  /* parent_tid */
	mov PSTEPPER_OFF_R10(%r15), %r10  /* child_tid */
	mov PSTEPPER_OFF_R8(%r15),  %r8   /* tls */
	mov $PSTEPPER_SYS_CLONE, %eax
	syscall
	test %rax, %rax
	jz pstepper_clone_child
	/*
	 * Parent: rax is the real return value (child tid, or a negative
	 * errno). Store it where the trapped instruction's own RAX would have
	 * ended up, and set the trapped instruction's own saved RIP to
	 * continue_rip (already past the 2-byte `syscall` opcode we
	 * intercepted -- otherwise resuming lands *on* it and executes a
	 * second, real, unwanted clone). With both set, falling into the
	 * ordinary epilogue resumes the parent exactly as if its own original
	 * clone instruction had run for real.
	 *
	 * The RIP that actually matters here is the *floating* word above the
	 * frame (at FRAME_SIZE+8(%r15)) -- the one the epilogue's popfq/ud2
	 * resume sequence reads back out. PSTEPPER_OFF_RIP inside the frame is
	 * only an informational copy for the handler to read; the epilogue
	 * never looks at it again, so fixing up that copy alone silently does
	 * nothing and resume lands squarely back on the syscall opcode.
	 */
	mov %rax, PSTEPPER_OFF_RAX(%r15)
	mov %r12, PSTEPPER_FRAME_SIZE+8(%r15)
	mov %r15, %rsp
	jmp pstepper_trampoline_epilogue
pstepper_clone_child:
	/*
	 * Child: a brand new thread, still on its real thread stack (child_stack,
	 * from the clone `newsp` arg). Park RSP just below the pre-built frame at
	 * %rbx before calling bootstrap, so bootstrap's scratch writes land
	 * neither on the otherwise-pristine child stack nor on the frame (it only
	 * ever writes below its entry RSP). The plugin traps none of this: the
	 * child vCPU is not armed until bootstrap's own PSTEPPER_CMD_ENABLE, which
	 * passes resuming=1 -- stepping then starts already "trap parked", so the
	 * bootstrap tail and the shared epilogue run unintercepted through to
	 * continue_rip. The child's real thread stack top is reinstalled as RSP
	 * by the epilogue, from the frame's floating rsp word.
	 */
	lea -64(%rbx), %rsp
	and $~15, %rsp
	mov %r13, %rdi   /* pstepper_child_bootstrap arg1: upcall_stack_base */
	mov %rbx, %rsi   /* arg2: child_frame_base */
	call pstepper_child_bootstrap
	ud2 /* unreachable: pstepper_child_bootstrap never returns */
	.size pstepper_clone_helper, . - pstepper_clone_helper

/*
 * pstepper_resume_via_frame(frame_base): resume guest execution through a
 * ready-made trampoline frame -- point RSP at it and fall straight into the
 * shared epilogue, which restores the full GPR + FP + eflags file from the
 * frame and hands the resume marker the [rip][rsp] pair staged just above
 * it. Never returns.
 *
 * Used only for a clone-bootstrapped child's very first resume: its frame
 * is built by pstepper_do_clone_intercept (pre-fork -- a verbatim copy of
 * the parent's, with rax forced to 0), not by the trampoline. This replaces
 * the old pstepper_resume_at(rip, rsp, rax): with a real frame, rax and
 * everything else come from the frame exactly as on the ordinary per-trap
 * path, so the child no longer needs its resume state passed as loose args.
 * Entered with RSP already inside the child's upcall stack (see
 * pstepper_clone_child), so neither instruction here traps.
 */
	.global pstepper_resume_via_frame
	.type pstepper_resume_via_frame, @function
pstepper_resume_via_frame:
	mov %rdi, %rsp
	jmp pstepper_trampoline_epilogue
	.size pstepper_resume_via_frame, . - pstepper_resume_via_frame

/*
 * pstepper_thread_exit(resume_rsp, upcall_base, upcall_size, status): the
 * noreturn tail of pstepper_do_exit_intercept (see pstepper.c). Entered
 * still standing on this thread's upcall stack, with stepping already
 * DISABLEd -- so nothing re-traps into the trampoline no matter where RSP
 * goes from here. Switch onto the thread's real stack (its value at the
 * exit(2) syscall site), unmap the now-unused upcall stack -- which we
 * couldn't do while still on it -- then issue the real per-thread exit(2).
 *
 * Only raw syscalls and register moves past the stack switch: nothing is
 * pushed, so the real stack just needs to hold a valid RSP, not headroom.
 * `and $~15` because a syscall site's alignment isn't otherwise guaranteed.
 */
	.global pstepper_thread_exit
	.type pstepper_thread_exit, @function
pstepper_thread_exit:
	mov %rcx, %r12          /* status -- syscall clobbers rcx; stash it */
	and $~15, %rdi
	mov %rdi, %rsp          /* off the upcall stack, onto the real one */
	mov %rsi, %rdi          /* munmap(upcall_base, upcall_size) */
	mov %rdx, %rsi
	mov $PSTEPPER_SYS_MUNMAP, %eax
	syscall
	mov %r12, %rdi          /* exit(status) */
	mov $PSTEPPER_SYS_EXIT, %eax
	syscall
	ud2                     /* unreachable: exit(2) never returns */
	.size pstepper_thread_exit, . - pstepper_thread_exit

/* Mark the stack non-executable -- absent this, some toolchains default an
 * object with no .note.GNU-stack section to an executable-stack binary. */
#if defined(__linux__) && defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif
