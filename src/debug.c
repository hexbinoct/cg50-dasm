/* debug — DASM's on-device debugger: UBC breakpoints, single step, the register screen.
 * See debug.h for the design; stage 1 breaks only on DASM's own code (dbg_selftest). */
#include <gint/display.h>
#include <gint/keyboard.h>
#include <gint/ubc.h>
#include <gint/mpu/ubc.h>
#include <gint/mpu/power.h>
#include <stdio.h>
#include <string.h>
#include "debug.h"
#include "sh4dec.h"
#include "ui.h"

#define UBC   SH7305_UBC
#define POWER SH7305_POWER

extern char srom;       /* linker script _srom: size of the add-in's code + rodata at 0x00300000 */

/* ------------------------------------------------------------------ state */

enum { M_RUN, M_STEP, M_STEP_OVER };     /* why channel 1 is armed */

static struct {
    void *old_dbr;          /* DBR / CBCR.UBDE as dbg_init() found them */
    int old_ubde;
    uint32_t bp;            /* channel 0: the breakpoint (break before) */
    int bp_on;
    int mode;               /* M_*: what the next channel-1 break means */
    gdb_cpu_state_t st;     /* the state at the current / last stop */
    gdb_cpu_state_t prev;   /* the state when the previous stop was left (changed registers) */
    int have_prev;
    int nbreaks, nsteps;    /* stops on the breakpoint / after a step */
} D;

/* ------------------------------------------------------------------ the self-test target */

/* dbg_test_target(a, n) = a*n + 7, by a loop, with delay slots to step through
 * (bf/s, rts); also leaves r2 = a<<2 and MACL = a*n. Written in assembly so the
 * instructions (and the registers they touch) are known exactly. Needs n >= 1. */
int dbg_test_target(int a, int n);
extern char dbg_test_target_end[];
__asm__(
    "   .pushsection .text\n"
    "   .align  2\n"
    "   .global _dbg_test_target\n"
    "_dbg_test_target:\n"
    "   mul.l   r4, r5\n"
    "   mov     r4, r2\n"
    "   shll2   r2\n"
    "   mov     #0, r0\n"
    "   mov     r5, r1\n"
    "1: dt      r1\n"
    "   bf/s    1b\n"
    "   add     r4, r0\n"
    "   rts\n"
    "   add     #7, r0\n"
    "   .global _dbg_test_target_end\n"
    "_dbg_test_target_end:\n"
    "   .popsection\n");

#define TEST_A 0x1234
#define TEST_N 3
#define TEST_R (TEST_A * TEST_N + 7)       /* 0x36a3 */

/* ------------------------------------------------------------------ UBC */

/* Our break entry (debug_dbh.S): it stops both channels while the debugger runs and writes
 * dbg_cbr[] back to CBR0/CBR1 just before the rte. */
void dbg_dbh(void);
extern volatile uint32_t dbg_cbr[2];

/* CBR: instruction fetch (ID = 01), read cycle (RW = 01), any size, no ASID / match-flag
 * condition, enabled. CRR: bit 13 always 1, PCB (bit 1) = break after, BIE (bit 0). */
#define CBR_ON      0x00000013u
#define CRR(after)  (0x2000u | ((after) ? 2u : 0u) | 1u)

/* Outside the break handler: program a channel in the hardware directly. */
static void hw_set(int ch, uint32_t addr, int after) {
    if (ch == 0) { UBC.CBR0.lword = 0; UBC.CAR0 = addr; UBC.CAMR0 = 0; UBC.CRR0.lword = CRR(after); UBC.CBR0.lword = CBR_ON; }
    else { UBC.CBR1.lword = 0; UBC.CAR1 = addr; UBC.CAMR1 = 0; UBC.CRR1.lword = CRR(after); UBC.CBR1.lword = CBR_ON; }
    (void)UBC.CBR1.lword;                /* read back: the writes are done before we go on */
}

static void hw_off(void) {
    UBC.CBR0.lword = 0;
    UBC.CBR1.lword = 0;
    (void)UBC.CBR1.lword;
}

/* Inside the break handler: both channels are stopped (CE = 0) by dbg_dbh, so CAR/CRR can be
 * written now; the CBR values only go to the hardware when dbg_dbh returns (SR.BL = 1). The
 * handler never enables a channel itself, so it can't break on its own code. */
static void want(int ch, uint32_t addr, int after) {
    if (ch == 0) { UBC.CAR0 = addr; UBC.CAMR0 = 0; UBC.CRR0.lword = CRR(after); }
    else { UBC.CAR1 = addr; UBC.CAMR1 = 0; UBC.CRR1.lword = CRR(after); }
    dbg_cbr[ch] = CBR_ON;
}

static void want_off(int ch) { dbg_cbr[ch] = 0; }

int dbg_init(void) {
    D.old_dbr = ubc_getDBR();
    D.old_ubde = UBC.CBCR.UBDE;
    /* Calling gint's UBC driver (here and in dbg_done) links drv_ubc, which powers the module
     * on when DASM starts; never touch the CPG ourselves. */
    if (POWER.MSTPCR0.UDB) return -1;
    ubc_disable_channel(0);
    ubc_disable_channel(1);
    D.bp_on = 0; D.mode = M_RUN;
    ubc_setDBR((void *)dbg_dbh);
    UBC.CBCR.UBDE = 1;
    return 0;
}

void dbg_done(void) {
    D.bp_on = 0;                         /* not D.mode: the handler owns it (this code can be stepped) */
    if (POWER.MSTPCR0.UDB) return;
    ubc_disable_channel(0);
    ubc_disable_channel(1);
    UBC.CBCR.UBDE = D.old_ubde;
    ubc_setDBR(D.old_dbr);
}

void dbg_break(uint32_t addr) {
    D.bp = addr; D.bp_on = 1;
    hw_set(0, addr, 0);
}

void dbg_clear(void) {
    D.bp_on = 0;                         /* not D.mode: the handler owns it (this code can be stepped) */
    hw_off();
}

/* Resume with one instruction executed: channel 1 breaks after the instruction at PC.
 * Channel 0 is off meanwhile (a break-before on PC itself would stop us again at once);
 * the next break re-arms it. */
static void arm_step(const gdb_cpu_state_t *s, int mode) {
    want_off(0);
    want(1, s->reg.pc, 1);
    D.mode = mode;
}

/* ------------------------------------------------------------------ memory for the decoder */

static int mem_ok(uint32_t a, int size) {
    uint32_t e = a + (uint32_t)size;
    if (a >= 0x00300000 && e <= 0x00300000 + (uint32_t)&srom) return 1;   /* our code */
    if (a >= 0x08100000 && e <= 0x08180000) return 1;                       /* add-in RAM */
    if (a >= 0x80000000 && e <= 0x82000000) return 1;                       /* flash, P1 */
    return 0;
}

static int mem_rd(void *ctx, uint32_t a, int size, uint32_t *out) {
    (void)ctx;
    if (!mem_ok(a, size)) return 0;
    const volatile uint8_t *p = (const volatile uint8_t *)a;
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v = (v << 8) | p[i];
    *out = v;
    return 1;
}

/* ------------------------------------------------------------------ register screen */

#define COLS (DWIDTH / UI_CW)          /* 56 */
#define ROW_P 12                       /* register row pitch */
#define STAT_Y (DHEIGHT - UI_CH - 1)

static void txt(int col, int y, int color, const char *s) { aa_text(col * UI_CW, y, color, s, COLS - col); }

/* "name+0xN" for the addresses we know, "" otherwise */
static void where(uint32_t a, char *out, int cap) {
    uint32_t f = (uint32_t)dbg_test_target, e = (uint32_t)dbg_test_target_end;
    if (a >= f && a < e) snprintf(out, cap, a == f ? "dbg_test_target" : "dbg_test_target+0x%lx", (unsigned long)(a - f));
    else if (a == e) snprintf(out, cap, "dbg_test_target_end");
    else out[0] = 0;
}

static void reg(int col, int y, const char *name, uint32_t v, int changed) {
    char t[16];
    txt(col, y, C_DIM, name);
    snprintf(t, sizeof t, "%08lx", (unsigned long)v);
    txt(col + 5, y, changed ? C_BR : C_TEXT, t);
}

static int insn_color(const sh4_insn_t *in) {
    switch (in->kind) {
    case SH4_K_BRANCH: case SH4_K_JUMP_REG: return C_BR;
    case SH4_K_CALL: case SH4_K_CALL_REG: return C_CALL;
    case SH4_K_RET: return C_RET;
    default: return C_TEXT;
    }
}

static void draw_regs(const gdb_cpu_state_t *s, int why) {
    char t[80], w[32];
    const gdb_cpu_state_t *p = D.have_prev ? &D.prev : s;
    uint32_t pc = s->reg.pc;
    dclear(C_BG);

    /* title: why we stopped, where */
    drect(0, 0, DWIDTH - 1, UI_CH, C_TITLE);
    where(pc, w, sizeof w);
    snprintf(t, sizeof t, "%s  %08lx %s", why == M_STEP ? "step" : "BREAK", (unsigned long)pc, w);
    txt(0, 1, C_KEY, t);
    snprintf(t, sizeof t, "brk %d stp %d", D.nbreaks, D.nsteps);
    txt(COLS - (int)strlen(t), 1, C_TEXT, t);

    /* r0..r15 in 4 rows of 4 */
    int y = UI_CH + 3;
    static const char *rn[16] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
                                  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
    for (int i = 0; i < 16; i++)
        reg((i & 3) * 14, y + (i >> 2) * ROW_P, rn[i], s->regs[i], s->regs[i] != p->regs[i]);
    y += 4 * ROW_P + 2;
    reg(0, y, "pc", s->reg.pc, 0);
    reg(14, y, "pr", s->reg.pr, s->reg.pr != p->reg.pr);
    reg(28, y, "gbr", s->reg.gbr, s->reg.gbr != p->reg.gbr);
    reg(42, y, "vbr", s->reg.vbr, s->reg.vbr != p->reg.vbr);
    y += ROW_P;
    reg(0, y, "mach", s->reg.mach, s->reg.mach != p->reg.mach);
    reg(14, y, "macl", s->reg.macl, s->reg.macl != p->reg.macl);
    y += ROW_P;
    uint32_t sr = s->reg.sr;
    reg(0, y, "sr", sr, sr != p->reg.sr);
    snprintf(t, sizeof t, "T=%lu S=%lu Q=%lu M=%lu IMASK=%lu MD=%lu RB=%lu BL=%lu",
        sr & 1, (sr >> 1) & 1, (sr >> 8) & 1, (sr >> 9) & 1, (sr >> 4) & 15,
        (sr >> 30) & 1, (sr >> 29) & 1, (sr >> 28) & 1);
    txt(14, y, (sr & 1) != (p->reg.sr & 1) ? C_BR : C_LIT, t);
    y += ROW_P + 1;

    /* disassembly around PC: 3 before, PC, the rest after */
    drect(0, y - 2, DWIDTH - 1, y - 2, C_SEP);
    int rows = (STAT_Y - 1 - y) / UI_CH;
    uint32_t a0 = pc - 6, op;
    sh4_insn_t in;
    int prev_delay = 0;
    if (mem_rd(0, a0 - 2, 2, &op)) { sh4_decode((uint16_t)op, a0 - 2, mem_rd, 0, &in, t, sizeof t); prev_delay = in.delay; }
    for (int i = 0; i < rows; i++) {
        uint32_t a = a0 + 2u * (uint32_t)i;
        int yy = y + i * UI_CH;
        if (a == pc) drect(0, yy - 1, DWIDTH - 1, yy + UI_CH - 2, C_CUR);
        if (!mem_rd(0, a, 2, &op)) { prev_delay = 0; continue; }
        if (a == pc) txt(0, yy, C_KEY, ">");
        else if (D.bp_on && a == D.bp) txt(0, yy, C_RET, "*");
        snprintf(w, sizeof w, "%08lx", (unsigned long)a); txt(1, yy, C_ADDR, w);
        snprintf(w, sizeof w, "%04lx", (unsigned long)op); txt(10, yy, C_HEXW, w);
        sh4_decode((uint16_t)op, a, mem_rd, 0, &in, t, sizeof t);
        int col = 15 + (prev_delay ? 1 : 0);
        txt(col, yy, prev_delay ? C_SLOT : insn_color(&in), t);
        int ccol = 0;                    /* comment: literal value or a known branch target */
        if (in.kind == SH4_K_LITERAL && in.lit_ok) { snprintf(w, sizeof w, "=0x%08lx", (unsigned long)in.lit_val); ccol = C_LIT; }
        else if ((in.kind == SH4_K_BRANCH || in.kind == SH4_K_CALL) && (where(in.target, w, sizeof w), w[0])) ccol = C_NAME;
        if (ccol) {
            int end = col + (int)strlen(t) + 2, n = (int)strlen(w), cc = 38;
            if (cc + n > COLS) cc = COLS - n;    /* right-align a long one */
            if (cc < end) cc = end;
            txt(cc, yy, ccol, w);
        }
        prev_delay = in.delay;
    }

    /* keys */
    drect(0, STAT_Y - 1, DWIDTH - 1, DHEIGHT - 1, C_STATUS);
    drect(1, STAT_Y, 62, DHEIGHT - 1, C_RGB(6, 14, 24));
    aa_text(32 - 2 * UI_CW, STAT_Y + 1, C_KEY, "STEP", 4);
    txt(10, STAT_Y + 1, C_DIM, "EXE continue   EXIT clear breaks, run on");
    dupdate();
}

/* ------------------------------------------------------------------ the break handler */

/* Called by dbg_dbh (debug_dbh.S) for every UBC break: bank 0, interrupts on, both
 * channels stopped until it returns. */
void dbg_on_break(gdb_cpu_state_t *s);
void dbg_on_break(gdb_cpu_state_t *s) {
    int why = D.mode;
    D.mode = M_RUN;
    want_off(1);
    if (D.bp_on) want(0, D.bp, 0); else want_off(0);
    if (why == M_STEP_OVER && !(D.bp_on && s->reg.pc == D.bp))
        return;                          /* stepped off the breakpoint for a continue: run on */
    if (why == M_STEP) D.nsteps++; else D.nbreaks++;
    D.st = *s;

    for (;;) {
        draw_regs(s, why == M_STEP ? M_STEP : M_RUN);
        /* no GETKEY_MENU / POWEROFF: no world switch from inside a break */
        key_event_t ev = getkey_opt(GETKEY_NONE, 0);
        if (ev.key == KEY_EXE) {
            if (D.bp_on && s->reg.pc == D.bp) arm_step(s, M_STEP_OVER);
            break;
        }
        if (ev.key == KEY_F1) { arm_step(s, M_STEP); break; }
        if (ev.key == KEY_EXIT) { D.bp_on = 0; want_off(0); want_off(1); break; }
    }
    D.prev = *s;
    D.have_prev = 1;
}

/* ------------------------------------------------------------------ self-test */

void dbg_selftest(void) {
    char t[80];
    int ok = dbg_init() == 0, r = 0;
    D.nbreaks = D.nsteps = 0;
    D.have_prev = 0;
    if (ok) {
        dbg_break((uint32_t)dbg_test_target);
        r = dbg_test_target(TEST_A, TEST_N);
        dbg_clear();
    }
    uint32_t dbr = (uint32_t)ubc_getDBR(), cbcr = UBC.CBCR.lword;
    dbg_done();

    dclear(C_BG);
    drect(0, 0, DWIDTH - 1, UI_CH, C_TITLE);
    txt(0, 1, C_KEY, "DASM debugger  self-test (UBC)");
    int y = UI_CH + 6;
    if (!ok) {
        txt(1, y, C_RET, "The UBC is powered off (MSTPCR0.UDB = 1): no breaks."); y += ROW_P;
    } else {
        snprintf(t, sizeof t, "dbg_test_target(0x%x, %d) returned 0x%08lx", TEST_A, TEST_N, (unsigned long)r);
        txt(1, y, C_TEXT, t); y += ROW_P;
        int good = r == TEST_R;
        snprintf(t, sizeof t, "expected 0x%08lx: %s", (unsigned long)TEST_R, good ? "OK, execution resumed correctly" : "WRONG");
        txt(1, y, good ? C_LIT : C_RET, t); y += ROW_P;
        snprintf(t, sizeof t, "stops: %d on the breakpoint, %d single steps", D.nbreaks, D.nsteps);
        txt(1, y, D.nbreaks ? C_TEXT : C_RET, t); y += ROW_P;
        if (!D.nbreaks) { txt(1, y, C_RET, "no break happened: the UBC did not stop the CPU"); y += ROW_P; }
    }
    y += ROW_P;
    snprintf(t, sizeof t, "target 0x%08lx  break entry dbg_dbh 0x%08lx", (unsigned long)(uint32_t)dbg_test_target, (unsigned long)(uint32_t)dbg_dbh);
    txt(1, y, C_DIM, t); y += ROW_P;
    snprintf(t, sizeof t, "during the test: DBR 0x%08lx  CBCR 0x%08lx", (unsigned long)dbr, (unsigned long)cbcr);
    txt(1, y, C_DIM, t); y += ROW_P;
    snprintf(t, sizeof t, "found at start:  DBR 0x%08lx  UBDE %d", (unsigned long)(uint32_t)D.old_dbr, D.old_ubde);
    txt(1, y, C_DIM, t); y += ROW_P;
    y += ROW_P;
    txt(1, y, C_DIM, "On a stop: EXE continue, F1 step, EXIT clear + run on.");
    y += ROW_P;
    txt(1, y, C_KEY, "Press any key.");
    dupdate();
    getkey();
}
