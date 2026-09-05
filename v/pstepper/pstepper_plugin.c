/*
 * pstepper QEMU plugin: implements the imagined kernel-side of pstepper.h
 * for qemu-x86_64 (linux-user).
 *
 * v2 design: the plugin does almost nothing per trap. It never marshals a
 * register file or touches eflags/cc_op itself -- see pstepper.h for the
 * full design rationale (this replaces an earlier version that fabricated
 * a full ucontext_t, including eflags, directly in the plugin).
 *
 * REQUIRES `-one-insn-per-tb` on the qemu-x86_64 command line.
 *
 * This is a more fundamental requirement than it was in the old design, and
 * survives even though this plugin itself never touches eflags: x86 TCG
 * tracks condition codes (ZF/CF/SF/OF/PF/AF) lazily via cc_op/cc_src/cc_dst,
 * which the translator treats as *symbolic* state within a translation
 * block -- real stores to env->cc_op/cc_src/cc_dst only happen at specific
 * sync points, generally a TB's own natural end, not after every
 * instruction inside a multi-instruction TB. Our trampoline's own `pushfq`
 * (real hardware, always correct *once it runs*) is executed as the first
 * instruction of a freshly-jumped-to translation block reached via a hard
 * cpu_loop_exit -- and if the instruction whose flags it wants to capture
 * was itself mid-TB rather than at a TB boundary, cc_op may not have been
 * spilled to env yet, so pushfq observes stale state. Forcing one
 * instruction per TB guarantees the spill happens after every single
 * instruction, which is exactly the granularity pstepper needs to trap on
 * anyway, so the added TB-exit overhead is immaterial. Confirmed empirically
 * against a floating-point loop: without this flag a conditional branch
 * dependent on a `cmp` two instructions prior read the wrong flags and
 * looped forever; with it, the same run matches a native (non-QEMU) result
 * bit-for-bit.
 *
 * Per-vCPU state is a small machine around one flag, in_trap, meaning "a
 * trap is parked" -- true from a genuine trap until the trapped instruction
 * finally retires after the upcall resumes. Per instruction, for a vCPU
 * with stepping armed:
 *   - If this instruction's address is the guest's registered resume marker
 *     (a `ud2` inside pstepper_trampoline, recognized purely by address --
 *     see pstepper.h for why not by opcode bytes), the trampoline epilogue
 *     has finished: read the parked [RIP][RSP] pair back off the top of the
 *     upcall stack and install them as the new RIP/RSP -- our own minimal
 *     analogue of Intel UINTR's `uiret`. in_trap stays set.
 *   - Else if in_trap is set and this is the exact (RIP, RSP) we just
 *     steered back to, the trapped instruction is about to re-run for real:
 *     let it through once and clear in_trap. (The callback fires *before* an
 *     instruction executes, so without this the resumed instruction would
 *     just re-trap forever.)
 *   - Else if in_trap is set, we are inside the upcall -- trampoline,
 *     handler, a clone-bootstrapped thread's pre-resume bootstrap, or a
 *     guest signal handler QEMU interposed while the trap was in flight.
 *     Do nothing; let it run. Stepping any of it would reuse the fixed trap
 *     slot at upcall_stack_top-16 and lose the parked trap's resume state.
 *   - Else: a genuine trap, delivering a UXI. Write the trapped
 *     instruction's own address and the guest's current RSP as two words at
 *     upcall_stack_top-16, point RSP there, set in_trap, and jump to the
 *     registered trampoline.
 *
 * There is deliberately no "is RSP inside the upcall stack range" check;
 * in_trap alone gates the paused state. A thread the clone hook bootstraps
 * arms itself (CMD_ENABLE) while already running on its upcall stack and
 * about to resume through a pre-built frame, so it passes resuming=1 and the
 * plugin starts it with in_trap already set.
 *
 * Known gap: a siglongjmp out of an interposed signal handler returns to
 * neither the parked trap nor the trampoline, leaving in_trap stuck and
 * stepping wedged for that thread until its next CMD_ENABLE. The
 * hardware-single-step design this replaces loses stepping there too
 * (longjmp does not restore RFLAGS.TF), so it is not a regression -- and
 * rare enough in practice to accept.
 *
 * This plugin only implements part (a) (steppable execution). The
 * thread-creation and thread-exit hooks live entirely in the guest-side
 * plumbing (pstepper.c); the plugin needs no knowledge of them.
 */
#include "pstepper.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>

#include "qemu-plugin.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/*
 * Single choke point for every "pstepper cannot run like this" failure.
 * Aborting is the simplest thing for now; if a softer "warn and just don't
 * step" mode is ever wanted, this is the one place that needs to change.
 */
__attribute__((noreturn))
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[pstepper] fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

typedef struct {
    bool stepping_enabled;
    bool in_trap;           /* a trap is parked: from a genuine trap until
                             * the trapped instruction retires after resume */
    uint64_t trampoline_pc;
    uint64_t resume_marker_pc;
    uint64_t upcall_stack_top;  /* 16-byte aligned; trap slot is top - 16 */
    uint64_t resume_pc;     /* 0, or the exact PC we are steering back to */
    uint64_t resume_rsp;    /* the RSP that PC must resume with */

    struct qemu_plugin_register *reg_rsp;
} VcpuState;

#define MAX_VCPUS 256
static VcpuState g_vcpus[MAX_VCPUS];

static VcpuState *state_for(unsigned int vcpu_index)
{
    g_assert(vcpu_index < MAX_VCPUS);
    return &g_vcpus[vcpu_index];
}

static struct qemu_plugin_register *find_reg(const char *name)
{
    GArray *regs = qemu_plugin_get_registers();
    struct qemu_plugin_register *found = NULL;
    bool matched = false;
    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (strcmp(d->name, name) == 0) {
            found = d->handle;
            matched = true;
            break;
        }
    }
    if (!matched) {
        die("register '%s' not found\n", name);
    }
    g_array_free(regs, TRUE);
    return found;
}

static GByteArray* scratch_u64() {
    static __thread GByteArray* buf;
    if (!buf) {
	buf = g_byte_array_sized_new(8);
    }
    g_byte_array_set_size(buf, 0);
    return buf;
}

/*
 * These four helpers run on every single trapped instruction (at minimum to
 * check the current RSP against the upcall-stack range), so a fresh
 * heap-allocated GByteArray per call is hot-path malloc/free churn -- with
 * two or more vCPUs stepping concurrently, contention on the host
 * allocator's locks made this catastrophically slow (not a hang: the guest
 * PC was still visibly advancing under gdb, just ~1000x slower than usual).
 * Each helper instead reuses one thread-local buffer, which is safe since a
 * plugin callback for a given vCPU always runs on that vCPU's own host
 * thread. The buffer's length is reset before every use, since the
 * underlying gdb register-read path appends rather than overwrites.
 */
static uint64_t read_reg64(struct qemu_plugin_register *reg)
{
    GByteArray *buf = scratch_u64();
    qemu_plugin_read_register(reg, buf);
    g_assert(buf->len == 8);
    uint64_t v;
    memcpy(&v, buf->data, 8);
    return v;
}

static void write_reg64(struct qemu_plugin_register *reg, uint64_t v)
{
    GByteArray *buf = scratch_u64();
    g_byte_array_append(buf, (const guint8 *)&v, 8);
    bool ok = qemu_plugin_write_register(reg, buf);
    g_assert(ok); (void)ok;
}

static uint64_t read_mem64(uint64_t vaddr)
{
    GByteArray *buf = scratch_u64();
    if (!qemu_plugin_read_memory_vaddr(vaddr, buf, 8)) {
        die("failed to read memory at 0x%" PRIx64 "\n", vaddr);
    }
    uint64_t v;
    memcpy(&v, buf->data, 8);
    return v;
}

static void write_mem64(uint64_t vaddr, uint64_t v)
{
    GByteArray *buf = scratch_u64();
    g_byte_array_append(buf, (const guint8 *)&v, 8);
    if (!qemu_plugin_write_memory_vaddr(vaddr, buf)) {
        die("failed to write memory at 0x%" PRIx64 "\n", vaddr);
    }
}

static void vcpu_insn_exec(unsigned int vcpu_index, void *userdata)
{
    uint64_t vaddr = (uint64_t)userdata;
    VcpuState *st = state_for(vcpu_index);

    if (!st->stepping_enabled) {
        return;
    }

    /*
     * (1) The trampoline epilogue reached its `ud2`. Pop the parked trapped
     * RIP/RSP back off the top of the upcall stack and steer there. in_trap
     * stays set (cleared in (2)) so a signal QEMU delivers in the gap before
     * that instruction runs is not mistaken for it.
     */
    if (vaddr == st->resume_marker_pc) {
        uint64_t rsp = read_reg64(st->reg_rsp);
        st->resume_pc = read_mem64(rsp);
        st->resume_rsp = read_mem64(rsp + 8);
        st->in_trap = true;
        write_reg64(st->reg_rsp, st->resume_rsp);
        qemu_plugin_set_pc(st->resume_pc); /* noreturn */
        g_assert_not_reached();
    }

    /*
     * (2) The instruction we have been steering back to is about to run with
     * the RSP it is meant to have -- let it retire once, unintercepted, and
     * drop back to plain stepping. The RSP check rejects a signal handler
     * that re-enters the same code at the same PC while the real resume is
     * still pending.
     */
    if (st->resume_pc && vaddr == st->resume_pc &&
        read_reg64(st->reg_rsp) == st->resume_rsp) {
        st->resume_pc = 0;
        st->in_trap = false;
        return;
    }

    /*
     * (3) A trap is parked: trampoline, handler, a clone-bootstrapped
     * thread's pre-resume bootstrap, or a guest signal handler QEMU
     * interposed while the trap was in flight. Let it all run
     * unintercepted -- see the file header comment.
     */
    if (st->in_trap) {
        return;
    }

    /*
     * (4) Genuine trap. Stage [RIP][RSP] at the top of the upcall stack,
     * point RSP there, set in_trap, and enter the trampoline.
     */
    uint64_t rsp = read_reg64(st->reg_rsp);
    uint64_t slot = st->upcall_stack_top - 16;
    write_mem64(slot, vaddr);
    write_mem64(slot + 8, rsp);
    st->in_trap = true;
    st->resume_pc = 0;
    write_reg64(st->reg_rsp, slot);
    qemu_plugin_set_pc(st->trampoline_pc); /* noreturn */
    g_assert_not_reached();
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *userdata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    /*
     * -one-insn-per-tb is a hard requirement (see the file header comment),
     * but it can't be checked in qemu_plugin_install(): qemu_info_t doesn't
     * expose it, and install()'s argv is only this plugin's own `-plugin
     * file.so,...` arguments, not QEMU's global command line. Detect it
     * empirically instead: under -one-insn-per-tb, literally every
     * translation block contains exactly one instruction, so any TB with
     * more than one proves the flag is missing. Real guest startup code
     * guarantees we see one almost immediately, well before the guest ever
     * gets to call pstepper_enable().
     */
    if (n > 1) {
        die("saw a translation block with %zu instructions; pstepper "
            "requires qemu-x86_64 to be run with -one-insn-per-tb (see the "
            "header comment in this file for why)", n);
    }

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t vaddr = qemu_plugin_insn_vaddr(insn);
        qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_insn_exec,
                                                QEMU_PLUGIN_CB_RW_REGS_PC,
                                                (void *)vaddr);
    }
}

static bool vcpu_syscall_filter(unsigned int vcpu_index, int64_t num,
                                 uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6,
                                 uint64_t a7, uint64_t a8, int64_t *sysret,
                                 void *userdata)
{
    if ((uint64_t)num != PSTEPPER_SYSCALL_NR) {
        return false;
    }

    VcpuState *st = state_for(vcpu_index);

    switch (a1) {
    case PSTEPPER_CMD_ENABLE:
        /* a2 = trampoline PC, a3 = upcall stack top (16-byte aligned),
         * a4 = resuming flag, a5 = resume marker PC. */
        if (a3 & 15) {
            die("vcpu%u: upcall stack top 0x%" PRIx64 " is not 16-byte "
                "aligned", vcpu_index, a3);
        }
        st->reg_rsp = find_reg("rsp");
        st->trampoline_pc = a2;
        st->upcall_stack_top = a3;
        st->resume_marker_pc = a5;
        st->stepping_enabled = true;
        st->in_trap = (a4 != 0);
        st->resume_pc = 0;
        st->resume_rsp = 0;
        fprintf(stderr, "[pstepper] vcpu%u: stepping enabled, trampoline="
                "0x%" PRIx64 " upcall_stack_top=0x%" PRIx64 " resuming=%"
                PRIu64 " resume_marker=0x%" PRIx64 "\n", vcpu_index, a2, a3,
                a4, a5);
        *sysret = 0;
        return true;

    case PSTEPPER_CMD_DISABLE:
        st->stepping_enabled = false;
        st->in_trap = false;
        st->resume_pc = 0;
        st->resume_rsp = 0;
        fprintf(stderr, "[pstepper] vcpu%u: stepping disabled\n",
                vcpu_index);
        *sysret = 0;
        return true;

    default:
        die("vcpu%u: unknown cmd %" PRIu64 "\n", vcpu_index, a1);
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                            const qemu_info_t *info,
                                            int argc, char **argv)
{
    /*
     * pstepper's trampoline is hand-written x86-64 asm (see pstepper.h) and
     * its whole design assumes linux-user (a guest process's own address
     * space, not a full emulated machine) -- neither is remotely checked
     * by the compiler, since info->target_name is the QEMU *guest* target,
     * completely independent of whatever architecture this plugin itself
     * was compiled for (the host machine's). Confirm both explicitly rather
     * than silently misbehaving on a mismatch; a future ARM64/RISC-V port
     * would branch on target_name here instead of just rejecting it.
     */
    if (info->system_emulation) {
        die("requires linux-user mode (qemu-<arch>), not full system "
            "emulation");
    }
    if (strcmp(info->target_name, "x86_64") != 0) {
        die("only supports the x86_64 guest target right now (saw '%s')",
            info->target_name);
    }

    qemu_plugin_register_vcpu_syscall_filter_cb(id, vcpu_syscall_filter,
                                                 NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
