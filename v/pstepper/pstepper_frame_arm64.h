// SPDX-License-Identifier: 0BSD
/*
 * pstepper_trampoline's stack frame layout (arm64) -- internal to the
 * pstepper implementation, not part of the guest-facing ABI in pstepper.h.
 *
 * Shared between pstepper_trampoline_arm64.S (which addresses fields by these
 * plain numeric offsets -- a .S file is only preprocessed, never compiled,
 * so it cannot evaluate offsetof() itself) and pstepper_arm64.c (which
 * cross-checks them against the real struct layout via _Static_assert, so a
 * layout change that isn't mirrored here fails the build instead of
 * silently corrupting register state at runtime). The __ASSEMBLER__ guard
 * is the standard idiom -- gcc predefines it while preprocessing a .S file.
 *
 * Unlike the amd64 frame there is no floating flags word: aarch64 has no
 * push-flags instruction, so NZCV is saved into the frame like any other
 * register (see the trampoline). Only the [PC][SP] pair the plugin stages
 * floats above the frame.
 *
 * FP/SIMD: NEON only -- V0..V31 + FPSR + FPCR, a fixed 528 bytes, no XSAVE
 * header, no runtime mask, no >16 alignment need. SVE upper bits / predicates
 * and SME state are NOT saved (documented gap; pstepper_arch_prepare() is
 * where a guard belongs).
 */
#ifndef PSTEPPER_FRAME_ARM64_H_
#define PSTEPPER_FRAME_ARM64_H_

/*
 * gpr area: exactly pstepper_gpregs_t == { x[31], sp, pc } (see
 * pstepper_arm64.h), the natural-order image of glibc mcontext_t's regs..pc.
 * x_n lives at PSTEPPER_OFF_X0 + 8*n.
 */
#define PSTEPPER_OFF_X0 0
#define PSTEPPER_OFF_SP 248   /* x[31] ends at 248 */
#define PSTEPPER_OFF_PC 256
#define PSTEPPER_OFF_NZCV 264 /* mrs/msr NZCV; low 32 bits meaningful */
#define PSTEPPER_OFF_VREGS 272 /* V0..V31, 16 bytes each -> 512 bytes */
#define PSTEPPER_OFF_FPSR 784  /* low 32 bits */
#define PSTEPPER_OFF_FPCR 792  /* low 32 bits */

/*
 * Bytes the trampoline sub/adds off SP for the frame. Must be a multiple of
 * 16 (AArch64 requires SP 16-aligned whenever it is used to access memory).
 * Entry SP = upcall_top - 16 is already 16-aligned (upcall_top is), so a
 * 16-multiple frame keeps every sp-relative access and the internal `bl`
 * legal with no realignment. 800 == sizeof(pstepper_frame_t); a touch of
 * headroom rounds it to 832.
 */
#define PSTEPPER_FRAME_SIZE 832

#ifndef __ASSEMBLER__

#include <stddef.h>

#include "pstepper.h"

typedef struct pstepper_frame {
	pstepper_gpregs_t gpr; /* x[31], sp, pc -- 264 bytes */
	uint64_t nzcv;
	unsigned char vregs[512] __attribute__((aligned(16))); /* V0..V31 */
	uint64_t fpsr;
	uint64_t fpcr;
} pstepper_frame_t;

_Static_assert(offsetof(pstepper_frame_t, gpr) == PSTEPPER_OFF_X0, "offset drift: gpr");
_Static_assert(offsetof(pstepper_frame_t, gpr.sp) == PSTEPPER_OFF_SP, "offset drift: sp");
_Static_assert(offsetof(pstepper_frame_t, gpr.pc) == PSTEPPER_OFF_PC, "offset drift: pc");
_Static_assert(offsetof(pstepper_frame_t, nzcv) == PSTEPPER_OFF_NZCV, "offset drift: nzcv");
_Static_assert(offsetof(pstepper_frame_t, vregs) == PSTEPPER_OFF_VREGS, "offset drift: vregs");
_Static_assert(offsetof(pstepper_frame_t, fpsr) == PSTEPPER_OFF_FPSR, "offset drift: fpsr");
_Static_assert(offsetof(pstepper_frame_t, fpcr) == PSTEPPER_OFF_FPCR, "offset drift: fpcr");
_Static_assert(PSTEPPER_OFF_VREGS % 16 == 0, "vregs must be 16-byte aligned within the frame");
_Static_assert(PSTEPPER_FRAME_SIZE % 16 == 0, "frame size must be a multiple of 16 (SP alignment)");
_Static_assert(PSTEPPER_FRAME_SIZE >= sizeof(pstepper_frame_t), "frame size constant must fit the real struct");

#endif /* __ASSEMBLER__ */

#endif /* PSTEPPER_FRAME_ARM64_H_ */
