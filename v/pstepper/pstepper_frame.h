/*
 * pstepper_trampoline's stack frame layout -- internal to the pstepper
 * implementation, not part of the guest-facing ABI in pstepper.h.
 *
 * Shared between pstepper_trampoline.S (which addresses fields by these
 * plain numeric offsets -- a .S file is only preprocessed, never compiled,
 * so it can use a #define directly but could never evaluate offsetof()
 * itself) and pstepper.c (which cross-checks them against the real struct
 * layout via _Static_assert, so a layout change that isn't mirrored here
 * fails the build instead of silently corrupting register state at
 * runtime). The __ASSEMBLER__ guard is the standard idiom for this split --
 * gcc predefines it while preprocessing a .S file for the assembler.
 */
#ifndef PSTEPPER_FRAME_H_
#define PSTEPPER_FRAME_H_

#define PSTEPPER_OFF_RAX     0
#define PSTEPPER_OFF_RBX     8
#define PSTEPPER_OFF_RCX     16
#define PSTEPPER_OFF_RDX     24
#define PSTEPPER_OFF_RSI     32
#define PSTEPPER_OFF_RDI     40
#define PSTEPPER_OFF_RBP     48
#define PSTEPPER_OFF_R8      56
#define PSTEPPER_OFF_R9      64
#define PSTEPPER_OFF_R10     72
#define PSTEPPER_OFF_R11     80
#define PSTEPPER_OFF_R12     88
#define PSTEPPER_OFF_R13     96
#define PSTEPPER_OFF_R14     104
#define PSTEPPER_OFF_R15     112
#define PSTEPPER_OFF_RIP     120
#define PSTEPPER_OFF_RSP     128
#define PSTEPPER_OFF_FXSAVE  144

/*
 * Bytes reserved on the stack for the frame (the trampoline's `sub`/`add`
 * amount) -- deliberately NOT sizeof(pstepper_frame_t): a struct containing
 * a 16-aligned member is itself forced to a size that's a multiple of 16,
 * but we need frame_size % 16 == 8. Why: the trampoline's very first
 * instruction is `pushfq` (8 bytes, done before touching RSP for the frame
 * itself, so the *original* eflags is captured before anything else can
 * disturb it), and entry RSP is 16-aligned (see pstepper_enable() in
 * pstepper.c). So after pushfq (+8) and reserving this frame, RSP must land
 * back on a 16-byte boundary for the frame's internal `call
 * pstepper_invoke_handler` to be SysV-legal. Picking a frame size
 * independent of sizeof() (just required to be big enough) sidesteps the
 * compiler's forced alignment padding entirely.
 */
#define PSTEPPER_FRAME_SIZE 664

#ifndef __ASSEMBLER__

#include <stddef.h>

#include "pstepper.h"

typedef struct pstepper_frame {
    pstepper_gpregs_t gpr;
    /* Padding so fxsave_area lands on a 16-byte boundary -- fxsave/fxrstor
     * fault on a misaligned operand. */
    unsigned char _pad_to_fxsave[8];
    unsigned char fxsave_area[512] __attribute__((aligned(16)));
} pstepper_frame_t;

_Static_assert(offsetof(pstepper_frame_t, gpr.rax) == PSTEPPER_OFF_RAX, "offset drift: rax");
_Static_assert(offsetof(pstepper_frame_t, gpr.rbx) == PSTEPPER_OFF_RBX, "offset drift: rbx");
_Static_assert(offsetof(pstepper_frame_t, gpr.rcx) == PSTEPPER_OFF_RCX, "offset drift: rcx");
_Static_assert(offsetof(pstepper_frame_t, gpr.rdx) == PSTEPPER_OFF_RDX, "offset drift: rdx");
_Static_assert(offsetof(pstepper_frame_t, gpr.rsi) == PSTEPPER_OFF_RSI, "offset drift: rsi");
_Static_assert(offsetof(pstepper_frame_t, gpr.rdi) == PSTEPPER_OFF_RDI, "offset drift: rdi");
_Static_assert(offsetof(pstepper_frame_t, gpr.rbp) == PSTEPPER_OFF_RBP, "offset drift: rbp");
_Static_assert(offsetof(pstepper_frame_t, gpr.r8)  == PSTEPPER_OFF_R8,  "offset drift: r8");
_Static_assert(offsetof(pstepper_frame_t, gpr.r9)  == PSTEPPER_OFF_R9,  "offset drift: r9");
_Static_assert(offsetof(pstepper_frame_t, gpr.r10) == PSTEPPER_OFF_R10, "offset drift: r10");
_Static_assert(offsetof(pstepper_frame_t, gpr.r11) == PSTEPPER_OFF_R11, "offset drift: r11");
_Static_assert(offsetof(pstepper_frame_t, gpr.r12) == PSTEPPER_OFF_R12, "offset drift: r12");
_Static_assert(offsetof(pstepper_frame_t, gpr.r13) == PSTEPPER_OFF_R13, "offset drift: r13");
_Static_assert(offsetof(pstepper_frame_t, gpr.r14) == PSTEPPER_OFF_R14, "offset drift: r14");
_Static_assert(offsetof(pstepper_frame_t, gpr.r15) == PSTEPPER_OFF_R15, "offset drift: r15");
_Static_assert(offsetof(pstepper_frame_t, gpr.rip) == PSTEPPER_OFF_RIP, "offset drift: rip");
_Static_assert(offsetof(pstepper_frame_t, gpr.rsp) == PSTEPPER_OFF_RSP, "offset drift: rsp");
_Static_assert(offsetof(pstepper_frame_t, fxsave_area) == PSTEPPER_OFF_FXSAVE,
               "offset drift: fxsave_area");
_Static_assert(offsetof(pstepper_frame_t, fxsave_area) % 16 == 0,
               "fxsave_area must be 16-byte aligned");
_Static_assert(PSTEPPER_FRAME_SIZE >= sizeof(pstepper_frame_t),
               "frame size constant must fit the real struct");
_Static_assert(PSTEPPER_FRAME_SIZE % 16 == 8,
               "frame size + the leading pushfq must total a 16-byte "
               "multiple, for the internal call's stack alignment");

#endif /* __ASSEMBLER__ */

#endif /* PSTEPPER_FRAME_H_ */
