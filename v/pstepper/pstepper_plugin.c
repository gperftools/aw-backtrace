// SPDX-License-Identifier: 0BSD
/*
 * pstepper QEMU plugin: implements the imagined kernel-side of pstepper.h
 * for linux-user qemu. Guest target is picked at qemu_plugin_install() time
 * from info->target_name -- x86_64 and aarch64 are supported. The plugin
 * .so is host-native and target-neutral: one build serves whichever
 * qemu-<target> loads it (on an aarch64 host you can and do run both
 * qemu-aarch64 and qemu-x86_64 against the same .so), so target selection
 * is a runtime branch, never a host #ifdef.
 *
 * Almost everything here is target-neutral: the [PC][SP] staging, the
 * resume-marker-by-address protocol, and the in_trap machine below. Only
 * two things vary by target -- the stack-pointer register name ("rsp" vs
 * "sp"), and, on aarch64, recognizing the exclusive-access instruction
 * classes so an LL/SC region can be stepped over (see "aarch64 LL/SC"
 * below).
 *
 * v2 design: the plugin does almost nothing per trap. It never marshals a
 * register file or touches eflags/cc_op itself -- see pstepper.h for the
 * full design rationale (this replaces an earlier version that fabricated
 * a full ucontext_t, including eflags, directly in the plugin).
 *
 * REQUIRES a QEMU carrying the precise-state patch to i386_tr_insn_start()
 * (target/i386/tcg/translate.c): when a plugin is loaded, EIP and cc_op are
 * materialized into env at every instruction boundary.
 *
 * This used to be `-one-insn-per-tb` on the command line instead, which
 * bought the same guarantee by brute force. x86 TCG tracks condition codes
 * (ZF/CF/SF/OF/PF/AF) lazily via cc_op/cc_src/cc_dst, which the translator
 * treats as *symbolic* state within a translation block -- real stores to
 * env->cc_op only happen where the translator has a reason to emit one, not
 * after every instruction of a multi-instruction TB. cc_src/cc_dst, by
 * contrast, are written eagerly. So an upcall taken mid-TB left env holding
 * fresh cc_src/cc_dst against a cc_op belonging to some earlier instruction,
 * and our trampoline's `pushfq` -- real hardware, always right *once it
 * runs*, but running in a freshly-jumped-to block after a hard
 * cpu_loop_exit -- captured garbage. Observed as a conditional branch
 * dependent on a `cmp` two instructions prior looping forever.
 *
 * Forcing one instruction per TB fixed that and cost two orders of
 * magnitude: it applies to *all* guest code, including the upcall handler,
 * which for a consumer like backtrace-comparer outnumbers the code being
 * observed by ~10^4:1. With the state synced at each instruction boundary
 * instead, the flag is unnecessary and the trampoline and handler run as
 * ordinary chained, multi-instruction TBs.
 *
 * The second half of that cost was the per-instruction callback itself,
 * which fired for every instruction of the trampoline and handler only to be
 * dropped by the in_trap check. That is now a scoreboard-gated conditional
 * callback -- see GATE_* below -- so while a trap is parked QEMU evaluates a
 * load/compare inline and calls nothing.
 *
 * ---------------------------------------------------------------------------
 * aarch64 LL/SC: x86 atomics are a single `lock` instruction, but aarch64
 * compiles them to `ldxr / <op> / stxr / cbnz` retry loops guarded by a
 * hardware exclusive monitor. QEMU emulates that monitor as an
 * address+value pair. It survives an upcall taken *at* the ldxr (before the
 * ldxr executes there is no reservation yet to disturb). It does NOT
 * reliably survive an upcall taken *between* a stepped ldxr and its stxr:
 * any syscall the handler chain makes clears the emulated reservation, the
 * stxr then fails on every retry, and the loop livelocks (verified
 * empirically -- a plain getpid() in the gap is enough).
 *
 * So on aarch64 the plugin always steps *over* an exclusive region. From a
 * stepped ldxr{,p}/ldaxr{,p} until its closing stxr/stlxr/stxp/stlxp/clrex
 * -- or until the PC leaves a small window around the ldxr, whichever comes
 * first -- no upcalls are delivered and the region runs unstepped at full
 * speed. Consequence for the consumer: it sees one upcall at the ldxr per
 * retry iteration and nothing between the ldxr and the stxr. For a
 * backtrace-style consumer the backtrace is identical across the whole
 * region, so this is invisible in practice; it is still a real difference
 * from x86 (where every instruction of a `lock cmpxchg` retry loop is
 * stepped) and is called out here for that reason.
 *
 * The window is a backstop, not the primary close condition: a budget that
 * expired *mid-region* would resume stepping between the ldxr and stxr --
 * exactly the configuration that livelocks -- so it is deliberately a
 * generous PC range (LL/SC sequences are short and straight-line) and the
 * stxr/clrex match is what normally closes the region.
 * ---------------------------------------------------------------------------
 *
 * Per-vCPU state is a small machine around one flag, in_trap, meaning "a
 * trap is parked" -- true from a genuine trap until the trapped instruction
 * finally retires after the upcall resumes. Per instruction, for a vCPU
 * with stepping armed:
 *   - If this instruction's address is the guest's registered resume marker
 *     (an otherwise-invalid instruction inside pstepper_trampoline -- `ud2`
 *     on x86_64, `brk`/`udf` on aarch64 -- recognized purely by address,
 *     see pstepper.h for why not by opcode bytes), the trampoline epilogue
 *     has finished: read the parked [PC][SP] pair back off the top of the
 *     upcall stack and install them as the new PC/SP -- our own minimal
 *     analogue of Intel UINTR's `uiret`. in_trap stays set.
 *   - Else if in_trap is set and this is the exact (PC, SP) we just steered
 *     back to, the trapped instruction is about to re-run for real: let it
 *     through once and clear in_trap. (The callback fires *before* an
 *     instruction executes, so without this the resumed instruction would
 *     just re-trap forever.)
 *   - Else if in_trap is set, we are inside the upcall -- trampoline,
 *     handler, a clone-bootstrapped thread's pre-resume bootstrap, or a
 *     guest signal handler QEMU interposed while the trap was in flight.
 *     Do nothing; let it run. Stepping any of it would reuse the fixed trap
 *     slot at upcall_stack_top-16 and lose the parked trap's resume state.
 *   - Else: a genuine trap, delivering a UXI. Write the trapped
 *     instruction's own address and the guest's current SP as two words at
 *     upcall_stack_top-16, point SP there, set in_trap, and jump to the
 *     registered trampoline.
 *
 * There is deliberately no "is SP inside the upcall stack range" check;
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
#include "pstepper_abi.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "qemu-plugin.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* Guest target, resolved once in qemu_plugin_install() from
 * info->target_name. Independent of the host arch this plugin was built
 * for. */
typedef enum {
	TARGET_X86_64,
	TARGET_AARCH64,
} Target;
static Target g_target;

/*
 * Instruction class, as far as the exclusive-monitor step-over cares (see
 * the "aarch64 LL/SC" block in the file header). Every x86 instruction is
 * INSN_NORMAL -- the classification only ever runs for an aarch64 guest.
 */
typedef enum {
	INSN_NORMAL,
	INSN_LOAD_EXCL,	 /* ldxr{,b,h,p} / ldaxr{,b,h,p} -- arms the monitor */
	INSN_STORE_EXCL, /* stxr{,b,h,p} / stlxr{,b,h,p} -- closes the monitor */
	INSN_CLREX,	 /* clrex -- also closes the monitor */
} InsnKind;

/*
 * Classify one 32-bit A64 instruction word. Only the exclusive-access
 * family matters:
 *
 *   Load/store exclusive group: bits[29:24] == 0b001000.
 *     bit[23] (o2) == 0  -> exclusive (arms/uses the monitor)
 *                    == 1 -> plain acquire/release ordering, no monitor
 *     bit[22] (L)  == 1  -> load  (ldxr...)   == 0 -> store (stxr...)
 *     bit[21] (o1)       -> 0 single / 1 pair; irrelevant to us
 *     bits[31:30] (size) -> B/H/W/X; irrelevant to us
 *   so LOAD_EXCL  = (w & 0x3fc00000) == 0x08400000
 *      STORE_EXCL = (w & 0x3fc00000) == 0x08000000
 *
 *   CLREX: 1101 0101 0000 0011 0011 CRm 0101 1111, CRm is an unused imm.
 *      CLREX      = (w & 0xfffff0ff) == 0xd503305f
 *
 * The guest is little-endian aarch64 and so is the host, so the word read
 * out of guest memory by qemu_plugin_insn_data() is already in native order.
 */
static InsnKind classify_a64(uint32_t w)
{
	if ((w & 0x3fc00000u) == 0x08400000u) {
		return INSN_LOAD_EXCL;
	}
	if ((w & 0x3fc00000u) == 0x08000000u) {
		return INSN_STORE_EXCL;
	}
	if ((w & 0xfffff0ffu) == 0xd503305fu) {
		return INSN_CLREX;
	}
	return INSN_NORMAL;
}

/*
 * How far past the arming ldxr the step-over stays in effect if no closing
 * stxr/clrex is seen (a signal handler QEMU interposes mid-region, or code
 * that abandons a reservation without clrex). LL/SC retry loops are a
 * handful of straight-line instructions, so this is comfortably generous;
 * see the file header for why a mid-region expiry must not step.
 */
#define EXCL_WINDOW_BYTES 1024

/*
 * Single choke point for every "pstepper cannot run like this" failure.
 * Aborting is the simplest thing for now; if a softer "warn and just don't
 * step" mode is ever wanted, this is the one place that needs to change.
 */
__attribute__((noreturn)) static void die(const char *fmt, ...)
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
	bool in_trap; /* a trap is parked: from a genuine trap until
		       * the trapped instruction retires after resume */
	uint64_t trampoline_pc;
	uint64_t resume_marker_pc;
	uint64_t upcall_stack_top; /* 64-byte aligned; trap slot is top - 16 */
	uint64_t resume_pc;	   /* 0, or the exact PC we are steering back to */
	uint64_t resume_sp;	   /* the SP that PC must resume with */

	/* aarch64 exclusive-monitor step-over (see the "aarch64 LL/SC" block in
	 * the file header). Never set for an x86_64 guest. */
	bool mon_armed;	  /* stepping over an exclusive region right now */
	uint64_t excl_pc; /* PC of the ldxr that opened the current region */

	struct qemu_plugin_register *reg_sp;
} VcpuState;

#define MAX_VCPUS 256
static VcpuState g_vcpus[MAX_VCPUS];

/*
 * The upcall gate: one per-vCPU scoreboard word, compared inline by QEMU
 * against each instruction's registered immediate, so that instructions
 * which would only be dropped again never reach a callback at all.
 *
 * Every ordinary instruction registers with immediate GATE_ARMED, the resume
 * marker with GATE_PARKED, and the condition is `gate >= imm`:
 *
 *   GATE_OFF     nothing is delivered  (stepping disabled)
 *   GATE_PARKED  only the resume marker is delivered (trampoline, handler,
 *                a clone bootstrap, or a guest signal handler QEMU
 *                interposed while the trap was in flight -- all the code
 *                the in_trap check used to drop one callback at a time)
 *   GATE_ARMED   everything is delivered
 *
 * in_trap survives alongside this: the gate says what QEMU should bother
 * calling us for, in_trap is what case (2) below reads to decide that a
 * stepped guest instruction is genuinely retiring. They are not the same
 * predicate -- the gate is already back to GATE_ARMED at the resume marker,
 * while in_trap stays set until the trapped instruction actually runs.
 */
enum {
	GATE_OFF = 0,
	GATE_PARKED = 1,
	GATE_ARMED = 2,
};

static struct qemu_plugin_scoreboard *g_gate_score;
static qemu_plugin_u64 g_gate;

/*
 * The resume marker's address, needed at *translation* time to give that one
 * instruction the lower gate immediate. Per-vCPU state can't serve: a TB is
 * translated once and shared. All threads run the same trampoline, so one
 * global is right; CMD_ENABLE rejects a second, different value.
 *
 * This is only correct because the trampoline cannot have been translated
 * before the first CMD_ENABLE -- nothing branches to it until a trap does.
 * If that ever stopped holding, the marker would be registered as an
 * ordinary instruction, never intercepted while parked, and the guest would
 * execute the bare ud2/brk: a loud SIGILL, not a silent wedge.
 */
static uint64_t g_resume_marker_pc;

static void set_gate(unsigned int vcpu_index, uint64_t value)
{
	qemu_plugin_u64_set(g_gate, vcpu_index, value);
}

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
		qemu_plugin_reg_descriptor *d = &g_array_index(regs, qemu_plugin_reg_descriptor, i);
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

static GByteArray *scratch_u64()
{
	static __thread GByteArray *buf;
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
	g_assert(ok);
	(void)ok;
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

static void do_insn_exec(unsigned int vcpu_index, uint64_t vaddr, InsnKind kind)
{
	VcpuState *st = state_for(vcpu_index);

	if (!st->stepping_enabled) {
		return;
	}

	/*
	 * (1) The trampoline epilogue reached its resume marker. Pop the parked
	 * trapped PC/SP back off the top of the upcall stack and steer there.
	 * in_trap stays set (cleared in (2)) so a signal QEMU delivers in the
	 * gap before that instruction runs is not mistaken for it.
	 */
	if (vaddr == st->resume_marker_pc) {
		uint64_t sp = read_reg64(st->reg_sp);
		st->resume_pc = read_mem64(sp);
		st->resume_sp = read_mem64(sp + 8);
		st->in_trap = true;
		set_gate(vcpu_index, GATE_ARMED);
		write_reg64(st->reg_sp, st->resume_sp);
		qemu_plugin_set_pc(st->resume_pc); /* noreturn */
		g_assert_not_reached();
	}

	/*
	 * (2) The instruction we have been steering back to is about to run
	 * with the SP it is meant to have -- let it retire once, unintercepted,
	 * and drop back to plain stepping. The SP check rejects a signal
	 * handler that re-enters the same code at the same PC while the real
	 * resume is still pending.
	 *
	 * This is also the one point that means "a stepped *guest* instruction
	 * is now really executing", so it is where an exclusive region is
	 * armed: if the instruction retiring here is the load half of an LL/SC
	 * pair, start stepping over everything until its store half (see (4)
	 * and the "aarch64 LL/SC" block in the file header). Case (3)
	 * passthrough never reaches here, so trampoline/handler ldxrs cannot
	 * arm the region -- exactly right.
	 */
	if (st->resume_pc && vaddr == st->resume_pc && read_reg64(st->reg_sp) == st->resume_sp) {
		st->resume_pc = 0;
		st->in_trap = false;
		if (kind == INSN_LOAD_EXCL) {
			st->mon_armed = true;
			st->excl_pc = vaddr;
		}
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
	 * Stepping over an aarch64 exclusive region (armed in (2)). Deliver no
	 * upcalls until the region closes:
	 *   - its store half (stxr/stlxr/stxp/stlxp) or a clrex: let that
	 *     instruction run unstepped too -- an upcall *at* it is just as
	 *     toxic as one in the middle -- then disarm;
	 *   - the PC wandering out of the window around the opening ldxr (a
	 *     signal handler, or code that drops the reservation without clrex):
	 *     disarm and fall through to a normal trap. The emulated monitor is
	 *     already gone in that case, so stepping from here just costs one
	 *     extra stxr retry, not a livelock.
	 */
	if (st->mon_armed) {
		int64_t off = (int64_t)(vaddr - st->excl_pc);
		if (off < -EXCL_WINDOW_BYTES || off > EXCL_WINDOW_BYTES) {
			/* PC left the region (interposed signal handler, or a
			 * reservation dropped without clrex). The window is dead
			 * regardless of what this instruction is -- a stxr this
			 * far away is not ours. Disarm and trap normally. */
			st->mon_armed = false;
			/* fall through to (4) */
		} else if (kind == INSN_STORE_EXCL || kind == INSN_CLREX) {
			st->mon_armed = false;
			return;
		} else {
			return;
		}
	}

	/*
	 * (4) Genuine trap. Stage [PC][SP] at the top of the upcall stack,
	 * point SP there, set in_trap, and enter the trampoline.
	 */
	uint64_t sp = read_reg64(st->reg_sp);
	uint64_t slot = st->upcall_stack_top - 16;
	write_mem64(slot, vaddr);
	write_mem64(slot + 8, sp);
	st->in_trap = true;
	st->resume_pc = 0;
	set_gate(vcpu_index, GATE_PARKED);
	write_reg64(st->reg_sp, slot);
	qemu_plugin_set_pc(st->trampoline_pc); /* noreturn */
	g_assert_not_reached();
}

/*
 * Per-instruction entry points. The bare vaddr is the callback's userdata;
 * the instruction class is baked into *which* wrapper vcpu_tb_trans
 * registered, so nothing has to be decoded again on the hot path. x86_64
 * only ever uses vcpu_insn_exec.
 */
static void vcpu_insn_exec(unsigned int vcpu_index, void *userdata)
{
	do_insn_exec(vcpu_index, (uint64_t)userdata, INSN_NORMAL);
}
static void vcpu_insn_exec_load_excl(unsigned int vcpu_index, void *userdata)
{
	do_insn_exec(vcpu_index, (uint64_t)userdata, INSN_LOAD_EXCL);
}
static void vcpu_insn_exec_store_excl(unsigned int vcpu_index, void *userdata)
{
	do_insn_exec(vcpu_index, (uint64_t)userdata, INSN_STORE_EXCL);
}
static void vcpu_insn_exec_clrex(unsigned int vcpu_index, void *userdata)
{
	do_insn_exec(vcpu_index, (uint64_t)userdata, INSN_CLREX);
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *userdata)
{
	size_t n = qemu_plugin_tb_n_insns(tb);

	for (size_t i = 0; i < n; i++) {
		struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
		uint64_t vaddr = qemu_plugin_insn_vaddr(insn);
		qemu_plugin_vcpu_udata_cb_t cb = vcpu_insn_exec;

		/*
		 * On aarch64, pick the class-specific entry point so the
		 * exclusive-region step-over in do_insn_exec() needs no
		 * per-instruction decoding. x86_64 always uses the plain one.
		 */
		if (g_target == TARGET_AARCH64) {
			uint32_t word = 0;
			if (qemu_plugin_insn_data(insn, &word, sizeof(word)) != sizeof(word)) {
				/* Every A64 instruction is a 4-byte word. If we
				 * ever can't read one, a real ldxr would silently
				 * misclassify as INSN_NORMAL and the LL/SC
				 * livelock would be back with no clue why -- so
				 * fail loudly instead. */
				die("could not read the 4-byte instruction word at 0x%" PRIx64, vaddr);
			}
			switch (classify_a64(word)) {
			case INSN_LOAD_EXCL:
				cb = vcpu_insn_exec_load_excl;
				break;
			case INSN_STORE_EXCL:
				cb = vcpu_insn_exec_store_excl;
				break;
			case INSN_CLREX:
				cb = vcpu_insn_exec_clrex;
				break;
			case INSN_NORMAL:
				break;
			}
		}

		/* See GATE_* above: the resume marker has to be delivered while
		 * a trap is parked, everything else only while armed. */
		uint64_t gate_min = (g_resume_marker_pc != 0 && vaddr == g_resume_marker_pc) ? GATE_PARKED : GATE_ARMED;

		qemu_plugin_register_vcpu_insn_exec_cond_cb(insn, cb, QEMU_PLUGIN_CB_RW_REGS_PC, QEMU_PLUGIN_COND_GE,
							   g_gate, gate_min, (void *)vaddr);
	}
}

static bool vcpu_syscall_filter(unsigned int vcpu_index, int64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
				uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, int64_t *sysret,
				void *userdata)
{
	if ((uint64_t)num != PSTEPPER_SYSCALL_NR) {
		return false;
	}

	VcpuState *st = state_for(vcpu_index);

	switch (a1) {
	case PSTEPPER_CMD_ENABLE: {
		/* a2 = trampoline PC, a3 = upcall stack top, a4 = resuming flag,
		 * a5 = resume marker PC. x86_64 wants the top 64-byte aligned
		 * (the trampoline's xsave area alignment derives from it);
		 * aarch64 only needs the AAPCS64 16. */
		uint64_t align_mask = (g_target == TARGET_X86_64) ? 63 : 15;
		if (a3 & align_mask) {
			die("vcpu%u: upcall stack top 0x%" PRIx64 " is not %" PRIu64 "-byte aligned", vcpu_index,
			    a3, align_mask + 1);
		}
		if (g_resume_marker_pc == 0) {
			g_resume_marker_pc = a5;
		} else if (g_resume_marker_pc != a5) {
			die("vcpu%u: resume marker moved from 0x%" PRIx64 " to 0x%" PRIx64
			    "; it is baked into translated code and must be process-wide",
			    vcpu_index, g_resume_marker_pc, a5);
		}
		st->reg_sp = find_reg((g_target == TARGET_X86_64) ? "rsp" : "sp");
		st->trampoline_pc = a2;
		st->upcall_stack_top = a3;
		st->resume_marker_pc = a5;
		st->stepping_enabled = true;
		st->in_trap = (a4 != 0);
		st->resume_pc = 0;
		st->resume_sp = 0;
		st->mon_armed = false;
		st->excl_pc = 0;
		/* A thread the clone hook bootstraps arms itself while already
		 * running on its upcall stack, i.e. already parked. */
		set_gate(vcpu_index, (a4 != 0) ? GATE_PARKED : GATE_ARMED);
		fprintf(stderr,
			"[pstepper] vcpu%u: stepping enabled, trampoline=0x%" PRIx64 " upcall_stack_top=0x%" PRIx64
			" resuming=%" PRIu64 " resume_marker=0x%" PRIx64 "\n",
			vcpu_index, a2, a3, a4, a5);
		*sysret = 0;
		return true;
	}

	case PSTEPPER_CMD_DISABLE:
		st->stepping_enabled = false;
		st->in_trap = false;
		st->resume_pc = 0;
		st->resume_sp = 0;
		st->mon_armed = false;
		st->excl_pc = 0;
		set_gate(vcpu_index, GATE_OFF);
		fprintf(stderr, "[pstepper] vcpu%u: stepping disabled\n", vcpu_index);
		*sysret = 0;
		return true;

	default:
		die("vcpu%u: unknown cmd %" PRIu64 "\n", vcpu_index, a1);
	}
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv)
{
	/*
	 * pstepper's trampoline is hand-written per-arch asm (see pstepper.h)
	 * and its whole design assumes linux-user (a guest process's own
	 * address space, not a full emulated machine). info->target_name is the
	 * QEMU *guest* target, completely independent of whatever architecture
	 * this plugin itself was compiled for (the host machine's) -- so this
	 * is where the guest arch is bound, at runtime, for the rest of the
	 * plugin to branch on. RISC-V would add another arm here.
	 */
	if (info->system_emulation) {
		die("requires linux-user mode (qemu-<arch>), not full system emulation");
	}
	if (strcmp(info->target_name, "x86_64") == 0) {
		g_target = TARGET_X86_64;
	} else if (strcmp(info->target_name, "aarch64") == 0) {
		g_target = TARGET_AARCH64;
	} else {
		die("unsupported guest target '%s' (supports x86_64, aarch64)", info->target_name);
	}

	g_gate_score = qemu_plugin_scoreboard_new(sizeof(uint64_t));
	g_gate = qemu_plugin_scoreboard_u64(g_gate_score);

	qemu_plugin_register_vcpu_syscall_filter_cb(id, vcpu_syscall_filter, NULL);
	qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
	return 0;
}
