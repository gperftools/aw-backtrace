/* -*- Mode: asm -*- */
/*
 * Machine-specific half of with-exit.h. See that header for what this
 * implements and why. Architectures without an implementation here are
 * handled by with-exit-generic.cc instead.
 */

/*
 * bool aw_backtrace_run_raw(void (*fn)(ExitCookie, void*), void* data);
 *
 * SysV amd64: fn in %rdi, data in %rsi, return value in %al.
 *
 * We save the full callee-saved set into our own frame and hand our
 * %rbp to fn as the cookie. aw_backtrace_exit_raw restores %rsp from
 * that cookie, pops the same set back and executes *our* ret. So both
 * exit paths honour the ABI contract this function makes with its
 * caller, and from the compiler's point of view this is an ordinary
 * function that returns exactly once. That is the entire point: no
 * returns_twice, so no clobbered-local rules for our callers.
 *
 * Frame we build, all offsets from %rbp (which is the cookie):
 *
 *    +8  return address
 *     0  saved %rbp
 *    -8  %rbx     -16 %r12    -24 %r13    -32 %r14    -40 %r15
 *   -48  shadow stack pointer at entry, or 0 if shadow stacks are off
 *
 */
#if __x86_64__

#if AW_HIDDEN_VISIBILITY
#define HIDE(s) .hidden s
#else
#define HIDE(s)
#endif
	.text
	.p2align 4
	.globl	aw_backtrace_run_raw
        HIDE(aw_backtrace_run_raw)
	.type	aw_backtrace_run_raw, @function
aw_backtrace_run_raw:
	.cfi_startproc
	endbr64
	pushq	%rbp
	.cfi_def_cfa_offset 16
	movq	%rsp, %rbp
	.cfi_def_cfa %rbp, 16
	.cfi_offset %rbp, -16

	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	.cfi_offset %rbx, -24
	.cfi_offset %r12, -32
	.cfi_offset %r13, -40
	.cfi_offset %r14, -48
	.cfi_offset %r15, -56

	/* RDSSP is in the hint-nop space, so it is safe to execute blind:
	 * with shadow stacks off it leaves %rax alone and we record 0,
	 * which is how aw_backtrace_exit_raw knows to skip its half. Note
	 * that only call/ret move SSP, so taking this after the pushes is
	 * no different from taking it first. */
	xorl	%eax, %eax
	rdsspq	%rax

	pushq	%rax

	movq	%rdi, %rax		/* fn */
	movq	%rbp, %rdi		/* cookie = this frame's %rbp */
	call	*%rax			/* fn(cookie, data); data still in %rsi */

	/* Nothing to do for the shadow stack here: this path returns
	 * through the call above, so SSP is already back where it was. */
	addq	$8, %rsp
	/* NOTE: cfa is anchored to rbp and red-zone rules let us keep original cfi_offsets for those */
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	.cfi_def_cfa %rsp, 8
	xorl	%eax, %eax		/* false: fn returned normally */
	ret
	.cfi_endproc
	.size	aw_backtrace_run_raw, .-aw_backtrace_run_raw

/*
 * void aw_backtrace_exit_raw(ExitCookie cookie);  [[noreturn]]
 *
 * cookie (i.e. the %rbp of the aw_backtrace_run_raw frame we are
 * returning to) arrives in %rdi. Everything except %rdi is scratch:
 * we are about to reload the callee-saved set from that frame anyway.
 *
 * Note: the CFI below deliberately describes the *destination* frame rather
 * than our own because we're effectively returning from that original stack frame.
 */
	.p2align 4
	.globl	aw_backtrace_exit_raw
        HIDE(aw_backtrace_exit_raw)
	.type	aw_backtrace_exit_raw, @function
aw_backtrace_exit_raw:
	.cfi_startproc
	endbr64
	movq	%rdi, %rbp

	.cfi_def_cfa %rbp, 16
	.cfi_offset %rbp, -16
	.cfi_offset %rbx, -24
	.cfi_offset %r12, -32
	.cfi_offset %r13, -40
	.cfi_offset %r14, -48
	.cfi_offset %r15, -56

	/* Shadow stack. Our ret below returns past every frame between
	 * here and aw_backtrace_run_raw, so the matching shadow stack
	 * entries have to go first or that ret takes a #CP.
	 *
	 * INCSSP is the only way to move SSP -- there is no write-SSP
	 * instruction, and RSTORSSP wants a restore token we could only
	 * write with WRSS, which is an opt-in that hands the process
	 * write access to its own shadow stack. INCSSP consumes just the
	 * low byte of its operand, i.e. 255 entries at a time, hence the
	 * loop: a deep unwind will exceed that.
	 *
	 * Unlike RDSSP, INCSSP is not in the nop space and faults when
	 * shadow stacks are off, so the zero snapshot must gate it.
	 */
	movq	-48(%rdi), %rax		/* SSP snapshot taken by run_raw */
	testq	%rax, %rax
	je	.Lno_shstk
	xorl	%ecx, %ecx
	rdsspq	%rcx			/* our own SSP */
	subq	%rcx, %rax		/* bytes to discard; shadow stacks grow down */
	jbe	.Lno_shstk
	shrq	$3, %rax		/* bytes -> entries */
.Lshstk_loop:
	cmpq	$255, %rax
	jbe	.Lshstk_last
	movl	$255, %ecx
	incsspq	%rcx
	subq	$255, %rax
	jmp	.Lshstk_loop
.Lshstk_last:
	incsspq	%rax
.Lno_shstk:

	subq	$40, %rdi		/* 40 is 56 - 16: the %r15 slot */
	movq	%rdi, %rsp		/* the SSP slot sits below it, so unchanged */

	/* NOTE: cfa is stable and red-zone rules let us keep cfi_offsets for those */
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	.cfi_def_cfa %rsp, 8
	movl	$1, %eax		/* true: we exited non-locally */
	ret				/* returns out of aw_backtrace_run_raw */
	.cfi_endproc
	.size	aw_backtrace_exit_raw, .-aw_backtrace_exit_raw

/*
 * Tell the linker that we support SHSTK and IBT
 */
	.section ".note.gnu.property", "a"
	.p2align 3
	.long	1f - 0f			/* name length */
	.long	4f - 1f			/* data length */
	.long	5			/* NT_GNU_PROPERTY_TYPE_0 */
0:	.asciz	"GNU"
1:	.p2align 3
	.long	0xc0000002		/* GNU_PROPERTY_X86_FEATURE_1_AND */
	.long	3f - 2f
2:	.long	3			/* bit 0 IBT, bit 1 SHSTK */
3:	.p2align 3
4:

	.section .note.GNU-stack,"",@progbits
#endif  // __x86_64__
