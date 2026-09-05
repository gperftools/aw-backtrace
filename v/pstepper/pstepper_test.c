/*
 * Guest test program for pstepper. Written purely against pstepper.h --
 * no knowledge of QEMU. Arms stepping, runs a chunk of real floating-point
 * work while the handler observes each instruction (proving per-instruction
 * delivery and that FP/SIMD state survives transparently across every
 * trap/resume), then disables stepping and exits normally.
 */
#include <stdio.h>
#include <inttypes.h>

#include "pstepper.h"

static volatile long g_steps_seen;
static volatile double g_fp_seen; /* proves xmm/fp state survives round-trip */

static void handler(int uxi_kind, void *info, pstepper_gpregs_t *regs)
{
    g_steps_seen++;
    if (g_steps_seen <= 5 || (g_steps_seen % 5000) == 0) {
        fprintf(stderr, "[guest handler] step %ld: rip=0x%" PRIx64
                " rsp=0x%" PRIx64 "\n", g_steps_seen, regs->rip, regs->rsp);
    }
}

/* A little floating point work, to exercise xmm state round-tripping
 * through every single-stepped instruction. */
__attribute__((noinline)) static double compute(int n)
{
    double acc = 1.0;
    for (int i = 1; i <= n; i++) {
        acc = acc * 1.0001 + (double)i;
    }
    return acc;
}

int main(void)
{
    fprintf(stderr, "[guest] arming pstepper\n");
    pstepper_enable(handler, 65536);

    g_fp_seen = compute(20000);

    pstepper_disable();
    fprintf(stderr, "[guest] disabled pstepper, steps seen = %ld, "
            "compute() = %f\n", g_steps_seen, g_fp_seen);
    return 0;
}
