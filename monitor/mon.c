/* mon.c — DASM's resident debugger monitor (stages 2 and 3 of the on-device debugger).
 *
 * A freestanding blob linked at 0x8C4E0000 (the DRAM nobody else uses), entered through the UBC:
 * DBR points at mon_entry (entry.S), which saves the target's whole CPU state into mon_regs and
 * calls mon_main here; a software breakpoint (trapa #0xC3) comes in through the hook block and
 * mon_trap the same way. Everything runs with SR.BL = 1: no interrupts, no breaks, and no
 * exceptions allowed (one would reset the calculator), so this code
 *   - never calls the OS or gint (the target owns the machine: at its first instruction that is
 *     the OS's world, later maybe the target's own gint world);
 *   - only touches P1/P2/P4 addresses: target memory is read through a manual translation
 *     (a snapshot of the UTLB, then the OS 3.60 add-in page table for code pages the UTLB
 *     doesn't hold) and only if the physical address is flash or RAM;
 *   - drives the hardware directly: the R61524 LCD (0xB4000000, RS = PFC 0xA405013C bit 4),
 *     the key-scan unit's data words (0xA44B0000), the UBC (0xFF200000).
 *
 * Breakpoints (up to NBP):
 *   - software, in the target's add-in code (0x00300000-0x004FFFFF, OS 3.60 page table): the
 *     4 KB code page is copied to RAM (MON_PAGES), the OS's page table pointed at the copy and
 *     the TLB entries dropped, so the OS maps the copy on the next miss; while the target runs a
 *     trapa #0xC3 replaces the instruction. The trap reaches the monitor through the hook block
 *     (entry.S, at MON_HOOK): the target's VBR if that was the OS's, else (a gint target, VBR in
 *     RAM) gint's exception entry is patched to jump into it: its copy at VBR+0x100, and its
 *     source in the target's code, which gint's kernel copies there when it starts. Not in a
 *     delay slot (a trapa there is an illegal slot instruction) and not in code that runs with
 *     SR.BL = 1 (an exception there resets the calculator);
 *   - hardware, UBC channel 0 (break BEFORE): one, anywhere (OS code, delay slots, RAM);
 * and one watchpoint, also channel 0 (operand access: read, write or both, any size within the
 * aligned long word), so it excludes the hardware breakpoint. Channel 1 = single step (break
 * AFTER the instruction at PC) or step over (break BEFORE the instruction after a call's delay
 * slot). The patches are only in place while the target runs: every stop removes them, every
 * resume puts them back (leaving out a breakpoint at PC, which is first stepped over).
 * Why we stopped is read from the match flags (CCMFR) captured at entry and the trap flag;
 * S.ch1 only says what channel 1 was armed for, and it lives in the monitor's own memory.
 *
 * Keys: F1 step, F2 step over (calls), F3 toggle a breakpoint at the cursor line, F4 go to an
 * address (hex: 0-9, F1-F6 = A-F), F5 memory view / disassembly, F6 set / clear the watchpoint,
 * UP/DOWN move the cursor (LEFT/RIGHT a page), EXE continue (steps over a breakpoint at PC
 * first), EXIT detach (all breakpoints removed, the target runs on), S<->D the theme. */
#include <stdint.h>
#include "mon_abi.h"
#include "theme.h"
#include "../src/sh4dec.h"
#include "../src/font_aa.h"

#define R32(a) (*(volatile uint32_t *)(a))
#define R16(a) (*(volatile uint16_t *)(a))
#define R8(a)  (*(volatile uint8_t *)(a))

/* ------------------------------------------------------------------ shared with entry.S */

mon_regs_t mon_regs;            /* the target's CPU state (entry.S saves and restores it) */
uint32_t mon_cbr[2];            /* CBR0 / CBR1 to write just before the rte */
uint32_t mon_nstops;            /* stops shown so far (tests read it through the header) */
uint32_t mon_via_trap;          /* 1: entered through a software breakpoint (mon_trap) */
uint8_t mon_stack[16384] __attribute__((aligned(8)));
void mon_utlb_read(uint32_t *dst);
void mon_p2_w32(uint32_t addr, uint32_t val);
void mon_trap(void);
void mon_main(void);
extern const uint8_t mon_hook[], mon_hook_end[];

_Static_assert(sizeof(mon_regs_t) == 124, "entry.S assumes 124 bytes of saved state");

/* ------------------------------------------------------------------ freestanding helpers */

/* gcc may emit calls to these for struct copies; built with -fno-tree-loop-distribute-patterns
 * so the loops don't turn into calls to themselves */
void *memcpy(void *d, const void *s, unsigned n);
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n) {
    uint8_t *p = d; const uint8_t *q = s;
    while (n--) *p++ = *q++;
    return d;
}
void *memset(void *d, int c, unsigned n) {
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

static inline void synco(void) { __asm__ volatile(".word 0x00ab" ::: "memory"); }
static inline void ocbwb(uint32_t a) { __asm__ volatile("ocbwb @%0" :: "r"(a) : "memory"); }

static char *s_str(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *s_hex(char *p, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) *p++ = "0123456789abcdef"[(v >> (4 * i)) & 15];
    *p = 0;
    return p;
}
static char *s_dec(char *p, uint32_t v) {
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}
static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* ------------------------------------------------------------------ state across stops */

enum { CH1_NONE, CH1_STEP, CH1_OVER, CH1_RESUME };
enum { W_ENTRY, W_BREAK, W_STEP, W_OVER, W_WATCH, W_OTHER };

#define NBP 16
#define NPATCH (NBP + 12)
#define TRAP_OP 0xC3C3u                 /* trapa #0xC3: a software breakpoint */
#define NONE 0xFFFFFFFFu

typedef struct { uint32_t addr; uint8_t on, hw; } bp_t;

static struct {
    int started;                /* 0 until the first stop (DASM zeroes the monitor's memory) */
    int ch1;                    /* CH1_*: what channel 1 was armed for */
    bp_t bp[NBP];
    uint32_t wp;                /* channel 0 as a watchpoint: address, mode 1 read 2 write 3 both */
    int wp_rw;                  /* 0 = no watchpoint */
    /* code pages copied to MON_PAGES: virtual page, the OS's page-table entry before us */
    struct { uint32_t va, orig; } pg[MON_NPAGES];
    int npg;
    /* the words written while the target runs (removed at every stop) */
    struct { uint32_t p1; uint16_t old, new; } patch[NPATCH];
    int npatch;
    uint32_t gint_src;          /* gint's exception entry in the target's code (0 = none) */
    int gint_src_done;          /* searched for it */
    mon_regs_t prev;            /* the registers at the previous stop (changed = highlighted) */
    int have_prev;
} S;

static const char *msg;         /* one line for the status bar until the next key */

/* ------------------------------------------------------------------ UBC */

#define UBC_CBR(c)  R32(0xFF200000u + 0x20u * (c))
#define UBC_CRR(c)  R32(0xFF200004u + 0x20u * (c))
#define UBC_CAR(c)  R32(0xFF200008u + 0x20u * (c))
#define UBC_CAMR(c) R32(0xFF20000Cu + 0x20u * (c))
#define UBC_CCMFR   R32(0xFF200600u)

/* CBR: instruction fetch (ID = 01), read (RW = 01), any size and ASID, CE. CRR: bit 13 always 1,
 * PCB (bit 1) = break after, BIE (bit 0). */
#define CBR_ON      0x00000013u
#define CRR(after)  (0x2000u | ((after) ? 2u : 0u) | 1u)

/* The channels are stopped (CE = 0) while the monitor runs; CAR/CRR are written now, CBR (CE)
 * by entry.S after mon_main returns. */
static void want(int ch, uint32_t addr, int after) {
    UBC_CAR(ch) = addr;
    UBC_CAMR(ch) = 0;
    UBC_CRR(ch) = CRR(after);
    mon_cbr[ch] = CBR_ON;
}

/* channel 0 as the watchpoint: operand access (ID = 10), RW = S.wp_rw, any size; the low two
 * address bits are left out, so any access within the long word matches. The break comes after
 * the accessing instruction. */
static void want_watch(void) {
    UBC_CAR(0) = S.wp;
    UBC_CAMR(0) = 3;
    UBC_CRR(0) = CRR(1);
    mon_cbr[0] = 0x21u | ((uint32_t)S.wp_rw << 1);
}

static int hw_bp(void) {                            /* index of the hardware breakpoint, or -1 */
    for (int i = 0; i < NBP; i++) if (S.bp[i].on && S.bp[i].hw) return i;
    return -1;
}

static int bp_at(uint32_t a) {                      /* index of a breakpoint at a, or -1 */
    for (int i = 0; i < NBP; i++) if (S.bp[i].on && S.bp[i].addr == a) return i;
    return -1;
}

/* channel 0 for running on: the watchpoint, or the hardware breakpoint unless it is at skip */
static void want_ch0(uint32_t skip) {
    int h = hw_bp();
    if (S.wp_rw) want_watch();
    else if (h >= 0 && S.bp[h].addr != skip) want(0, S.bp[h].addr, 0);
}

/* ------------------------------------------------------------------ target memory */

#define MMUCR R32(0xFF000010u)
#define PTEH  R32(0xFF000000u)
#define CCR   R32(0xFF00001Cu)
#define MSTPCR0 R32(0xA4150030u)
#define PAGE_TABLE 0x8C04CF0Cu  /* OS 3.60: add-in code pages, one word per 4 KB from 0x00300000 */

static uint32_t utlb[128];      /* (address array, data array) per entry, read at each stop */
static uint32_t mmucr, asid;
static int tgt_md;              /* the target runs privileged (SSR.MD) */
static int os_tbl;              /* the OS 3.60 add-in page table can be used */

/* OS 3.60: its TLB-miss handler 0x8002c918 maps add-in code pages from the table 0x8c04cf0c
 * (literal at 0x8002ca9c, mask 0x1fffffff at 0x8002caa0). Anything else: no fallback. */
static int os360(void) {
    return R32(0x8002CA9Cu) == PAGE_TABLE && R32(0x8002CAA0u) == 0x1FFFFFFFu && R16(0x8002C918u) == 0x2FE6u;
}

static int phys_ok(uint32_t p) { return p < 0x02000000u || (p >= 0x0C000000u && p < 0x0C800000u); }

static int is_code(uint32_t va) { return va >= 0x00300000u && va < 0x00500000u; }
static volatile uint32_t *tbl(uint32_t va) { return (volatile uint32_t *)(PAGE_TABLE + ((va - 0x00300000u) >> 12) * 4); }

static const uint32_t pmask[4] = { 0x3FFu, 0xFFFu, 0xFFFFu, 0xFFFFFu };
static uint32_t utlb_mask(int i) { uint32_t d = utlb[2 * i + 1]; return pmask[((d >> 6) & 2) | ((d >> 4) & 1)]; }

/* Virtual address -> a P1 address that reads the same byte (cached, so data the target has in
 * the operand cache is seen). 0 = not readable. */
static int xlate(uint32_t va, uint32_t *out) {
    uint32_t p = 0;
    if (va >= 0x80000000u) {
        if (va >= 0xC0000000u) return 0;            /* P3 / P4: never */
        p = va & 0x1FFFFFFFu;
    } else if (!(mmucr & 1)) {
        p = va & 0x1FFFFFFFu;
    } else {
        int hit = 0;
        for (int i = 0; i < 64 && !hit; i++) {
            uint32_t a = utlb[2 * i], d = utlb[2 * i + 1];
            if (!(d & 0x100)) continue;
            uint32_t m = utlb_mask(i);
            if ((va ^ a) & ~m & 0xFFFFFC00u) continue;
            if (!(d & 2) && !((mmucr & 0x100) && tgt_md) && (a & 0xFF) != asid) continue;
            p = (d & 0x1FFFFC00u & ~m) | (va & m);
            hit = 1;
        }
        if (!hit) {
            if (!os_tbl || !is_code(va)) return 0;
            uint32_t e = *tbl(va);
            if (!e) return 0;
            p = (e & 0x1FFFF000u) | (va & 0xFFFu);
        }
    }
    if (!phys_ok(p)) return 0;
    *out = 0x80000000u | p;
    return 1;
}

static int rd(void *ctx, uint32_t a, int size, uint32_t *out) {
    (void)ctx;
    uint32_t v = 0, p;
    for (int i = 0; i < size; i++) {
        if (!xlate(a + (uint32_t)i, &p)) return 0;
        v = (v << 8) | R8(p);
    }
    *out = v;
    return 1;
}

/* ------------------------------------------------------------------ patching the target */

/* Stores the TLB arrays and CCR need are done by entry.S's P2 helper. */
static void p2w(uint32_t addr, uint32_t val) {
    ((void (*)(uint32_t, uint32_t))((uint32_t)mon_p2_w32 | 0x20000000u))(addr, val);
}

/* The instruction cache, all of it (CCR.ICI): after any code change, before the target runs. */
static void ici(void) { p2w(0xFF00001Cu, CCR | 0x800u); }

/* Drop the TLB entries that map any part of the 4 KB page at va (the UTLB ones overlapping it,
 * and the whole ITLB, which refills itself from the UTLB): the OS maps the page again from its
 * page table on the next miss. */
static void tlb_drop(uint32_t va) {
    for (int i = 0; i < 64; i++) {
        uint32_t a = utlb[2 * i], m = utlb_mask(i);
        if (!(utlb[2 * i + 1] & 0x100)) continue;
        uint32_t lo = a & ~m & 0xFFFFFC00u, hi = lo | m;
        if (hi < va || lo > va + 0xFFFu) continue;
        p2w(0xF6000000u | ((uint32_t)i << 8), a & ~0x100u);
        utlb[2 * i] = a & ~0x100u;
        utlb[2 * i + 1] &= ~0x100u;
    }
    for (int i = 0; i < 4; i++) p2w(0xF2000000u | ((uint32_t)i << 8), 0);
}

static uint32_t pg_copy(int k) { return MON_PAGES + 4096u * (uint32_t)k; }
static uint32_t pg_entry(int k) { return (S.pg[k].orig & ~0x1FFFF000u) | (pg_copy(k) & 0x1FFFF000u); }

/* The RAM copy of the code page holding va (made and mapped if needed): its P1 address for va,
 * or 0 (not add-in code, no page table, no room). */
static uint32_t code_p1(uint32_t va) {
    uint32_t page = va & ~0xFFFu;
    if (!os_tbl || !is_code(va)) return 0;
    for (int k = 0; k < S.npg; k++)
        if (S.pg[k].va == page) return pg_copy(k) + (va & 0xFFFu);
    uint32_t e = *tbl(va);
    if (!e || !phys_ok(e & 0x1FFFF000u) || S.npg >= MON_NPAGES) return 0;
    int k = S.npg++;
    uint32_t src = 0x80000000u | (e & 0x1FFFF000u), dst = pg_copy(k);
    for (uint32_t i = 0; i < 4096; i += 4) R32(dst + i) = R32(src + i);
    for (uint32_t i = 0; i < 4096; i += 32) ocbwb(dst + i);
    S.pg[k].va = page;
    S.pg[k].orig = e;
    *tbl(va) = pg_entry(k);
    tlb_drop(page);
    return dst + (va & 0xFFFu);
}

/* The page copies are ours only while the OS's table still points at them: a new add-in (the
 * target ended and another one started) has a new table. Then everything about the old one goes. */
static void pages_check(void) {
    for (int k = 0; k < S.npg; k++) {
        if (os_tbl && *tbl(S.pg[k].va) == pg_entry(k)) continue;
        S.npg = 0;
        S.gint_src = 0;
        S.gint_src_done = 0;
        for (int i = 0; i < NBP; i++) if (!S.bp[i].hw) S.bp[i].on = 0;
        msg = "another add-in runs now: software breakpoints cleared";
        return;
    }
}

static void poke16(uint32_t p1, uint16_t v) {
    if (S.npatch >= NPATCH) return;
    S.patch[S.npatch].p1 = p1;
    S.patch[S.npatch].old = R16(p1);
    S.patch[S.npatch].new = v;
    S.npatch++;
    R16(p1) = v;
    ocbwb(p1);
}

static void unpatch_all(void) {
    if (!S.npatch) return;
    while (S.npatch) {
        S.npatch--;
        uint32_t p = S.patch[S.npatch].p1;
        if (R16(p) == S.patch[S.npatch].new) { R16(p) = S.patch[S.npatch].old; ocbwb(p); }
    }
    ici();
}

/* gint's exception entry starts with these 6 instructions (gint 2.x kernel/exch.s: sts.l pr,
 * stc.l gbr, sts.l mach, sts.l macl, mov.l r8, mov.l r9, all @-r15), then mov.l .gint,r0;
 * mov.l @r0,r0; tst #1,r0. The patch: mov.l @(4,pc),r0; jmp @r0; nop; nop; .long hook+0x700
 * (the hook block runs the 6 instructions itself and comes back at +12). */
static const uint16_t gint_pro[9] = { 0x4F22, 0x4F13, 0x4F02, 0x4F12, 0x2F86, 0x2F96, 0xD000, 0x6002, 0xC801 };
#define GINT_HOOK (MON_HOOK + 0x700u)

static int gint_is(uint32_t p1) {                   /* 0 no, 1 gint's entry, 2 already patched */
    if (p1 & 3) return 0;                           /* (a misaligned R32 would reset the calculator) */
    if (R16(p1) == 0xD001 && R16(p1 + 2) == 0x402B && R32(p1 + 8) == GINT_HOOK) return 2;
    for (int i = 0; i < 9; i++)
        if ((R16(p1 + 2u * i) & (i == 6 ? 0xFF00u : 0xFFFFu)) != gint_pro[i]) return 0;
    return 1;
}

static void gint_patch(uint32_t p1) {
    if ((p1 & 3) || gint_is(p1) != 1) return;
    poke16(p1, 0xD001);
    poke16(p1 + 2, 0x402B);
    poke16(p1 + 4, 0x0009);
    poke16(p1 + 6, 0x0009);
    poke16(p1 + 8, (uint16_t)(GINT_HOOK >> 16));
    poke16(p1 + 10, (uint16_t)GINT_HOOK);
}

static void gint_unpatch(uint32_t p1) {             /* a patch gint copied itself (not in S.patch) */
    if (gint_is(p1) != 2) return;
    for (int i = 0; i < 6; i++) { R16(p1 + 2u * i) = gint_pro[i]; }
    for (int i = 0; i < 12; i += 4) ocbwb(p1 + (uint32_t)i);
}

/* gint's exception entry in the target's code: 16-byte aligned near the start (0x00300010 in
 * every gint 2.x add-in seen); 0 = none (not a gint add-in). */
static uint32_t gint_find(void) {
    for (uint32_t va = 0x00300000u; va < 0x00310000u; va += 16) {
        uint32_t p;
        if ((va & 0xFFFu) > 0xFE0u || !xlate(va, &p)) continue;   /* 18 bytes within the page */
        if (gint_is(p) == 1) return va;
    }
    return 0;
}

#define HOOK_ORIG R32(MON_HOOK + 4)
#define HOOK_TRAP R32(MON_HOOK + 8)

static void hook_install(void) {
    uint32_t n = (uint32_t)(mon_hook_end - mon_hook);
    uint32_t orig = R32(MON_HOOK) == 0x484F4F4Bu ? HOOK_ORIG : 0;
    for (uint32_t i = 0; i < n; i += 4) R32(MON_HOOK + i) = *(const uint32_t *)(mon_hook + i);
    HOOK_ORIG = orig;
    HOOK_TRAP = (uint32_t)mon_trap;
    for (uint32_t i = 0; i < n; i += 32) ocbwb(MON_HOOK + i);
}

static int is_ram_vbr(uint32_t v) { return v >= 0x8C000000u && v < 0x8C800000u && v != MON_HOOK; }

/* Put the software breakpoints in (all but the one at skip), with the way for their trap into
 * the monitor. Returns 0 if a breakpoint could not be placed (message set). */
static int patch_all(uint32_t skip) {
    int sw = 0, ok = 1;
    for (int i = 0; i < NBP; i++) if (S.bp[i].on && !S.bp[i].hw) sw = 1;
    if (!sw) return 1;
    hook_install();
    uint32_t v = mon_regs.vbr;
    if (is_ram_vbr(v)) {                            /* the target's own (gint's) */
        if (gint_is(v + 0x100) == 1) gint_patch(v + 0x100);
        else if (gint_is(v + 0x100) != 2) { msg = "unknown exception handler: software breakpoints may crash"; ok = 0; }
    } else if (v != MON_HOOK) {                     /* the OS's: replaced by the hook block */
        HOOK_ORIG = v;
        mon_regs.vbr = MON_HOOK;
    }
    if (!S.gint_src_done) { S.gint_src = gint_find(); S.gint_src_done = 1; }
    if (S.gint_src) {
        uint32_t p = code_p1(S.gint_src);
        if (p) gint_patch(p);
    }
    for (int i = 0; i < NBP; i++) {
        if (!S.bp[i].on || S.bp[i].hw || S.bp[i].addr == skip) continue;
        uint32_t p = code_p1(S.bp[i].addr);
        if (p) poke16(p, TRAP_OP);
        else { S.bp[i].on = 0; msg = "a breakpoint's code page could not be copied"; ok = 0; }
    }
    ici();
    return ok;
}

/* Detach: the page table as it was, the TLB entries of our copies dropped, the VBR back. The
 * hook block stays (a gint target may have saved it as the OS's VBR) but sends traps nowhere. */
static void restore_all(void) {
    for (int k = 0; k < S.npg; k++) {
        if (os_tbl && *tbl(S.pg[k].va) == pg_entry(k)) *tbl(S.pg[k].va) = S.pg[k].orig;
        tlb_drop(S.pg[k].va);
    }
    S.npg = 0;
    if (mon_regs.vbr == MON_HOOK && HOOK_ORIG) mon_regs.vbr = HOOK_ORIG;
    if (is_ram_vbr(mon_regs.vbr)) gint_unpatch(mon_regs.vbr + 0x100);
    if (R32(MON_HOOK) == 0x484F4F4Bu) { HOOK_TRAP = 0; ocbwb(MON_HOOK + 8); }
    for (int i = 0; i < NBP; i++) S.bp[i].on = 0;
    S.wp_rw = 0;
    ici();
}

/* ------------------------------------------------------------------ display */

#define W 396
#define H 224
#define CW AA_CW                /* 7 */
#define CH AA_CH                /* 11 */
#define COLS (W / CW)           /* 56 */

static uint16_t fb[W * H];

/* DASM's colours, the current theme of theme.h (S<->D switches; mon_main loads it per stop) */
static const uint16_t *pal = theme_pal[0];
#define C_BG      pal[P_BG]
#define C_TITLE   pal[P_TITLE]
#define C_ADDR    pal[P_ADDR]
#define C_HEXW    pal[P_HEXW]
#define C_TEXT    pal[P_TEXT]
#define C_BR      pal[P_BR]
#define C_CALL    pal[P_CALL]
#define C_RET     pal[P_RET]
#define C_LIT     pal[P_LIT]
#define C_SLOT    pal[P_SLOT]
#define C_CUR     pal[P_CUR]
#define C_PCBAR   pal[P_PCBAR]
#define C_STATUS  pal[P_STATUS]
#define C_FKEY    pal[P_FKEY]
#define C_KEY     pal[P_KEY]
#define C_DIM     pal[P_DIM]
#define C_SEP     pal[P_SEP]
#define C_DATA    pal[P_DATA]
#define C_ASCII   pal[P_ASCII]

static void fill(int x0, int y0, int x1, int y1, uint16_t c) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (y1 >= H) y1 = H - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) fb[y * W + x] = c;
}

static uint16_t blend565(uint16_t dst, int src, int a) {
    a += a >> 7;
    int r = dst >> 11, g = (dst >> 5) & 63, b = dst & 31;
    r += (((src >> 11) - r) * a) >> 8;
    g += ((((src >> 5) & 63) - g) * a) >> 8;
    b += (((src & 31) - b) * a) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* text at column col (7 px cells), pixel row y, blended over what is drawn */
static void text(int col, int y, int color, const char *s) {
    int x = col * CW;
    for (int i = 0; s[i] && col + i < COLS; i++, x += CW) {
        int c = (unsigned char)s[i];
        if (c == ' ') continue;
        if (c < 0x20 || c > 0x7e) c = '?';
        const uint8_t *g = font_aa[c - 0x20];
        for (int r = 0; r < CH; r++) {
            int yy = y + r;
            if (yy < 0 || yy >= H) continue;
            uint16_t *row = fb + yy * W;
            for (int k = 0; k < CW; k++) {
                int a = g[r * CW + k];
                if (a) row[x + k] = a >= 250 ? (uint16_t)color : blend565(row[x + k], color, a);
            }
        }
    }
}

/* The R61524: registers through the data port at 0xB4000000 with RS (PFC port 0xA405013C
 * bit 4) low for the index, high for data. As gint: window R210-R213 with H = 395 - x, address
 * R200/R201 = 0, entry mode R003 = ORG, V increment, H decrement. */
#define LCD  R16(0xB4000000u)
#define PRDR R8(0xA405013Cu)

static void lcd_sel(uint16_t reg) {
    PRDR &= (uint8_t)~0x10; synco();
    LCD = reg; synco();
    PRDR |= 0x10; synco();
}
static uint16_t lcd_get(uint16_t reg) { lcd_sel(reg); return LCD; }
static void lcd_set(uint16_t reg, uint16_t v) { lcd_sel(reg); LCD = v; }

static struct { uint16_t r003, win[4]; uint8_t rs; } L;

/* Wait for a DMA transfer into the LCD (DAR = area 5, 0x14000000) to finish: the target's
 * frame push may still be running. Bounded, so a stuck channel can't hang us. */
static void dma_wait_lcd(void) {
    if (MSTPCR0 & (1u << 21)) return;              /* DMAC stopped: nothing runs */
    static const uint32_t ch[6] = { 0xFE008020u, 0xFE008030u, 0xFE008040u, 0xFE008050u, 0xFE008070u, 0xFE008080u };
    for (int i = 0; i < 6; i++) {
        for (uint32_t n = 0; n < 50000000u; n++) {
            uint32_t chcr = R32(ch[i] + 12);
            if ((chcr & 3) != 1 || (R32(ch[i] + 4) & 0x1FFFFFFFu) != 0x14000000u) break;
        }
    }
}

static void lcd_begin(void) {
    dma_wait_lcd();
    L.rs = PRDR & 0x10;
    L.r003 = lcd_get(0x003);
    for (int i = 0; i < 4; i++) L.win[i] = lcd_get((uint16_t)(0x210 + i));
}

static void lcd_end(void) {
    lcd_set(0x003, L.r003);
    for (int i = 0; i < 4; i++) lcd_set((uint16_t)(0x210 + i), L.win[i]);
    lcd_set(0x200, 0);
    lcd_set(0x201, 0);
    lcd_sel(0x202);                                 /* GRAM write, as after a frame start */
    if (!L.rs) { PRDR &= (uint8_t)~0x10; synco(); }
}

static void lcd_push(void) {
    lcd_set(0x003, (uint16_t)((L.r003 & ~0xB8u) | 0xA0u));
    lcd_set(0x210, 0);
    lcd_set(0x211, W - 1);
    lcd_set(0x212, 0);
    lcd_set(0x213, H - 1);
    lcd_set(0x200, 0);
    lcd_set(0x201, 0);
    lcd_sel(0x202);
    for (int i = 0; i < W * H; i++) LCD = fb[i];
}

/* ------------------------------------------------------------------ keyboard */

/* The key-scan unit (0xA44B0000): six data words, word w = matrix columns 2w (low byte) and
 * 2w+1 (high byte), bit = row (0-based grid of re/KEYMAP.md in the emulator project).
 * The data words hold the LAST scan, and the unit doesn't scan while idle: in the OS's normal
 * mode the OS's keyboard interrupt drives it, and we run with interrupts blocked. Polling the
 * words as they were therefore saw the EXE that launched the target "held" forever (the first
 * real-calculator run hung there). So every poll is a synchronous scan done the OS's own way
 * (its sync scan 0x801E56FA, 3.60): unit off (mode 0x400, ctrl 0, PFC 0xA40501C6 = 0), ~1 ms,
 * ctrl 0x8000, flags cleared with IE 0, timing 0x8042/200/0/0xFFF/0xFF, PFC 0xFF, mode 0x800
 * (scan now), ~1 ms, wait for flag 1 (scan complete), read the words, unit off again. The
 * configuration is saved at the stop and written back before the target resumes (IE without
 * clearing flags: the OS then sees one more scan, with the keys as they are now). */
#define KSR(off)   R16(0xA44B0000u + (off))
#define KPFC       R8(0xA40501C6u)

#define K(row, col) ((row) << 4 | (col))
enum {
    K_F1 = K(6, 9), K_F2 = K(5, 9), K_F3 = K(4, 9), K_F4 = K(3, 9), K_F5 = K(2, 9), K_F6 = K(1, 9),
    K_UP = K(1, 8), K_DOWN = K(2, 7), K_LEFT = K(2, 8), K_RIGHT = K(1, 7),
    K_EXE = K(2, 1), K_EXIT = K(3, 7), K_FD = K(5, 5), K_DEL = K(3, 4),   /* K_FD: S<->D */
};
static const uint8_t k_digit[10] = {                /* keys 0-9 */
    K(6, 1), K(6, 2), K(5, 2), K(4, 2), K(6, 3), K(5, 3), K(4, 3), K(6, 4), K(5, 4), K(4, 4),
};
static const uint8_t k_fn[6] = { K_F1, K_F2, K_F3, K_F4, K_F5, K_F6 };

/* the hex digit a key types in an address prompt (0-9, F1-F6 = A-F), or -1 */
static int key_hex(int k) {
    for (int i = 0; i < 10; i++) if (k == k_digit[i]) return i;
    for (int i = 0; i < 6; i++) if (k == k_fn[i]) return 10 + i;
    return -1;
}

static struct { uint16_t ctrl, r0e, mode, ie, r16, r18, r1a, r1c; uint8_t pfc; } KS;
static uint16_t kw[6];                              /* the last scan */
static uint32_t kscans, kstuck;                     /* scans done / scans that never completed */

static void kdelay(void) {                          /* ~1 ms or more (peripheral-bus reads) */
    for (uint32_t n = 0; n < 20000u; n++) (void)R8(0xA413FEC0u);
}

static void kbd_begin(void) {
    KS.ctrl = KSR(0x0C); KS.r0e = KSR(0x0E); KS.mode = KSR(0x10); KS.ie = KSR(0x14) & 0xFF00u;
    KS.r16 = KSR(0x16); KS.r18 = KSR(0x18); KS.r1a = KSR(0x1A); KS.r1c = KSR(0x1C);
    KS.pfc = KPFC;
}

static void kbd_end(void) {
    KSR(0x10) = 0x400; KSR(0x0C) = 0; KPFC = 0;
    kdelay();
    KSR(0x0C) = KS.ctrl; KSR(0x0E) = KS.r0e; KSR(0x14) = KS.ie;   /* flags: 0 = left alone */
    KSR(0x16) = KS.r16; KSR(0x18) = KS.r18; KSR(0x1A) = KS.r1a; KSR(0x1C) = KS.r1c;
    KPFC = KS.pfc;
    KSR(0x10) = KS.mode;
}

static void kbd_scan(void) {
    KSR(0x10) = 0x400; KSR(0x0C) = 0; KPFC = 0;
    kdelay();
    KSR(0x0C) = 0x8000;
    KSR(0x14) = KSR(0x14) & 0xFFu;                  /* IE 0, clear the flags (write 1 to clear) */
    KSR(0x0E) = 0x8042; KSR(0x18) = 200; KSR(0x16) = 0; KPFC = 0xFF;
    KSR(0x1A) = 0xFFF; KSR(0x1C) = 0xFF;
    KSR(0x10) = 0x800;
    kdelay();
    uint32_t n = 0;
    while (!(KSR(0x14) & 2) && n < 3000000u) n++;
    if (n >= 3000000u) kstuck++;
    for (int i = 0; i < 6; i++) kw[i] = KSR(2 * i);
    KSR(0x10) = 0x400; KSR(0x0C) = 0; KPFC = 0;
    kscans++;
}

static int key_now(void) {                          /* scan; first key held, or -1 */
    kbd_scan();
    for (int w = 0; w < 6; w++) {
        uint32_t v = kw[w];
        if (!v) continue;
        for (int b = 0; b < 16; b++)
            if (v & (1u << b)) return K(b & 7, 2 * w + (b >> 3));
    }
    return -1;
}

static void heartbeat(void);                        /* the screen section: a live status corner */
static void status_corner(void);

static void wait_release(void) {                    /* 3 empty scans in a row */
    for (int up = 0; up < 3; ) {
        if (key_now() >= 0) { up = 0; heartbeat(); }
        else up++;
    }
}

/* Wait for a key seen on two scans in a row, then for its release; returns the key. Acting on
 * the release means the next call doesn't see the same press, and the key is up when the
 * target resumes (it would see it otherwise). */
static int getkey(void) {
    for (;;) {
        int k = key_now();
        if (k < 0) { heartbeat(); continue; }
        if (key_now() == k) { wait_release(); return k; }
    }
}

/* ------------------------------------------------------------------ the screen */

#define TITLE_H 12
#define ROW_P 12
#define DIS_Y 101
#define DIS_ROWS 10
#define STAT_Y (H - CH - 1)
#define MEM_BYTES 8                                 /* per memory-view row */

static uint32_t cur, top;                           /* disassembly cursor / first line */
static uint32_t maddr;                              /* memory view: the first row */
static int memview;                                 /* F5: the memory view instead of the code */
static const char *prompt_label;                    /* an address prompt in the status line */
static char prompt_buf[12];

static const char *why_name(int why) {
    switch (why) {
    case W_ENTRY: return "ENTRY";
    case W_BREAK: return "BREAKPOINT";
    case W_STEP: return "STEP";
    case W_OVER: return "STEP OVER";
    case W_WATCH: return "WATCHPOINT";
    default: return "BREAK";
    }
}

static void reg(int col, int y, const char *name, uint32_t v, int changed) {
    char t[12];
    text(col, y, C_DIM, name);
    s_hex(t, v, 8);
    text(col + 5, y, changed ? C_BR : C_TEXT, t);
}

static int insn_color(const sh4_insn_t *in) {
    switch (in->kind) {
    case SH4_K_BRANCH: case SH4_K_JUMP_REG: return C_BR;
    case SH4_K_CALL: case SH4_K_CALL_REG: return C_CALL;
    case SH4_K_RET: return C_RET;
    default: return C_TEXT;
    }
}

/* What the word at a is, from the instruction before it: 1 = a delay slot, 2 = the second word
 * of a 32-bit DSP instruction, 0 = an instruction of its own (as far as one word back tells). */
static int slot_kind(uint32_t a) {
    uint32_t op;
    sh4_insn_t in;
    char t[48];
    if (!rd(0, a - 2, 2, &op)) return 0;
    sh4_decode((uint16_t)op, a - 2, rd, 0, &in, t, sizeof t);
    return in.len == 4 ? 2 : in.delay ? 1 : 0;
}

static void draw_code(uint32_t pc) {
    sh4_insn_t in;
    uint32_t op;
    char t[80], *q;
    int prev_delay = 0, cont = 0;    /* cont: this row is the second word of a DSP instruction */
    if (rd(0, top - 2, 2, &op)) {
        sh4_decode((uint16_t)op, top - 2, rd, 0, &in, t, sizeof t);
        prev_delay = in.delay; cont = in.len == 4;
    }
    for (int i = 0; i < DIS_ROWS; i++) {
        uint32_t a = top + 2u * (uint32_t)i;
        int yy = DIS_Y + i * CH;
        if (a == cur) fill(0, yy - 1, W - 1, yy + CH - 2, C_CUR);
        else if (a == pc) fill(0, yy - 1, W - 1, yy + CH - 2, C_PCBAR);
        if (a == pc) text(0, yy, C_KEY, ">");
        int b = bp_at(a);
        if (b >= 0) text(1, yy, C_RET, S.bp[b].hw ? "#" : "*");
        s_hex(t, a, 8);
        text(2, yy, a == pc ? C_KEY : C_ADDR, t);
        if (!rd(0, a, 2, &op)) { text(11, yy, C_DIM, "----  (not mapped)"); prev_delay = cont = 0; continue; }
        s_hex(t, op, 4);
        text(11, yy, C_HEXW, t);
        if (cont) { text(16, yy, C_DIM, "  (cont.)"); prev_delay = cont = 0; continue; }
        sh4_decode((uint16_t)op, a, rd, 0, &in, t, sizeof t);
        int col = 16 + (prev_delay ? 1 : 0);
        text(col, yy, prev_delay ? C_SLOT : insn_color(&in), t);
        if (in.kind == SH4_K_LITERAL && in.lit_ok) {
            char c[16];
            q = s_str(c, "=0x");
            s_hex(q, in.lit_val, in.lit_size == 2 ? 4 : 8);
            int cc = col + s_len(t) + 2;
            if (cc < 42) cc = 42;
            if (cc + s_len(c) > COLS) cc = COLS - s_len(c);
            text(cc, yy, C_LIT, c);
        }
        prev_delay = in.delay;
        cont = in.len == 4;
    }
}

/* memory view: address, 8 bytes, the same as text; "--" = not readable; the watched word in
 * the breakpoint colour */
static void draw_mem(void) {
    char t[16];
    for (int i = 0; i < DIS_ROWS; i++) {
        uint32_t a = maddr + (uint32_t)(MEM_BYTES * i);
        int yy = DIS_Y + i * CH;
        if (i == 0) fill(0, yy - 1, W - 1, yy + CH - 2, C_CUR);
        s_hex(t, a, 8);
        text(1, yy, C_ADDR, t);
        for (int j = 0; j < MEM_BYTES; j++) {
            uint32_t v, b = a + (uint32_t)j;
            int col = 11 + 3 * j + (j >= 4 ? 1 : 0);
            int w = S.wp_rw && (b & ~3u) == (S.wp & ~3u);
            if (!rd(0, b, 1, &v)) { text(col, yy, C_DIM, "--"); text(48 + j, yy, C_DIM, "."); continue; }
            s_hex(t, v, 2);
            text(col, yy, w ? C_RET : C_DATA, t);
            t[0] = v >= 0x20 && v < 0x7F ? (char)v : '.';
            t[1] = 0;
            text(48 + j, yy, C_ASCII, t);
        }
    }
}

static void draw(int why) {
    const mon_regs_t *s = &mon_regs, *p = S.have_prev ? &S.prev : &mon_regs;
    uint32_t pc = s->spc, sr = s->ssr;
    char t[80], *q;

    fill(0, 0, W - 1, H - 1, C_BG);

    /* title: why we stopped; the breakpoints and the watchpoint on the right */
    fill(0, 0, W - 1, TITLE_H - 1, C_TITLE);
    q = s_str(t, "MONITOR  ");
    s_str(q, why_name(why));
    text(0, 1, C_KEY, t);
    int nbp = 0;
    for (int i = 0; i < NBP; i++) nbp += S.bp[i].on;
    q = s_dec(t, (uint32_t)nbp);
    q = s_str(q, " bp");
    if (S.wp_rw) {
        q = s_str(q, "  watch ");
        q = s_hex(q, S.wp, 8);
        s_str(q, S.wp_rw == 1 ? " r" : S.wp_rw == 2 ? " w" : " rw");
    }
    text(COLS - s_len(t), 1, nbp || S.wp_rw ? C_RET : C_DIM, t);

    /* r0-r15 (the bank SR.RB selects), then the control registers */
    const uint32_t *lo = (sr & (1u << 29)) ? s->b1 : s->b0, *plo = (sr & (1u << 29)) ? p->b1 : p->b0;
    static const char *rn[16] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
                                  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
    int y = TITLE_H + 2;
    for (int i = 0; i < 16; i++) {
        uint32_t v = i < 8 ? lo[i] : i < 15 ? s->r8_14[i - 8] : s->r15;
        uint32_t w = i < 8 ? plo[i] : i < 15 ? p->r8_14[i - 8] : p->r15;
        reg((i & 3) * 14, y + (i >> 2) * ROW_P, rn[i], v, v != w);
    }
    y += 4 * ROW_P;
    reg(0, y, "pc", pc, 0);
    reg(14, y, "pr", s->pr, s->pr != p->pr);
    reg(28, y, "gbr", s->gbr, s->gbr != p->gbr);
    reg(42, y, "vbr", s->vbr, s->vbr != p->vbr);
    y += ROW_P;
    reg(0, y, "mach", s->mach, s->mach != p->mach);
    reg(14, y, "macl", s->macl, s->macl != p->macl);
    reg(28, y, "sr", sr, sr != p->ssr);
    y += ROW_P;
    q = s_str(t, "T="); q = s_dec(q, sr & 1);
    q = s_str(q, " S="); q = s_dec(q, (sr >> 1) & 1);
    q = s_str(q, " Q="); q = s_dec(q, (sr >> 8) & 1);
    q = s_str(q, " M="); q = s_dec(q, (sr >> 9) & 1);
    q = s_str(q, " IMASK="); q = s_dec(q, (sr >> 4) & 15);
    q = s_str(q, " MD="); q = s_dec(q, (sr >> 30) & 1);
    q = s_str(q, " RB="); q = s_dec(q, (sr >> 29) & 1);
    q = s_str(q, " BL="); q = s_dec(q, (sr >> 28) & 1);
    q = s_str(q, " DSP="); s_dec(q, (sr >> 12) & 1);
    text(1, y, (sr & 1) != (p->ssr & 1) ? C_BR : C_LIT, t);

    fill(0, DIS_Y - 3, W - 1, DIS_Y - 3, C_SEP);
    if (memview) draw_mem(); else draw_code(pc);

    /* the status line: a prompt, a message, or the function keys */
    fill(0, STAT_Y - 1, W - 1, H - 1, C_STATUS);
    if (prompt_label) {
        q = s_str(t, prompt_label);
        q = s_str(q, prompt_buf);
        s_str(q, "_");
        text(0, STAT_Y, C_KEY, t);
        text(COLS - 20, STAT_Y, C_DIM, "EXE ok  EXIT cancel");
    } else if (msg) {
        text(0, STAT_Y, C_BR, msg);
    } else {
        static const char *fk[6] = { "step", "over", "brk", "goto", 0, "watch" };
        for (int i = 0; i < 6; i++) {
            const char *l = i == 4 ? (memview ? "code" : "mem") : fk[i];
            fill(i * 7 * CW + 1, STAT_Y - 1, i * 7 * CW + 6 * CW - 1, H - 1, C_FKEY);
            text(i * 7 + (6 - s_len(l)) / 2, STAT_Y, C_KEY, l);
        }
        status_corner();
    }
}

/* The live corner of the status line, refreshed while waiting for a key: the number of key
 * scans done ("!" if one never completed) and the first non-zero key data word, so a photo of
 * a stuck screen tells whether the monitor runs and what the key-scan unit reports. Only drawn
 * with the function keys (it sits right of them). */
static void status_corner(void) {
    if (prompt_label || msg) return;
    char t[24], *q;
    int w = 0;
    while (w < 6 && !kw[w]) w++;
    q = s_str(t, "k");
    q = s_dec(q, kscans % 1000u);
    if (kstuck) q = s_str(q, "!");
    if (w < 6) { *q++ = ' '; *q++ = (char)('0' + w); *q++ = '='; q = s_hex(q, kw[w], 4); *q = 0; }
    fill(42 * CW, STAT_Y - 1, W - 1, H - 1, C_STATUS);
    text(COLS - s_len(t), STAT_Y, C_DIM, t);
}

static void heartbeat(void) {
    static uint32_t hb;
    if ((++hb & 31) == 0 && !prompt_label && !msg) { status_corner(); lcd_push(); }
}

/* ------------------------------------------------------------------ commands */

static void cursor_to(uint32_t a) {
    cur = a;
    if (cur < top) top = cur;
    if (cur >= top + 2u * DIS_ROWS) top = cur - 2u * (DIS_ROWS - 1);
}

static void goto_code(uint32_t a) {
    a &= ~1u;
    top = a - 6;
    cur = a;
}

/* An address prompt in the status line: hex digits (0-9, F1-F6 = A-F), DEL, EXE, EXIT. The
 * suggestion is shown first and goes on the first digit. 1 = *val set. */
static int prompt(int why, const char *label, uint32_t *val) {
    int n = 8, fresh = 1;
    s_hex(prompt_buf, *val, 8);
    prompt_label = label;
    for (;;) {
        draw(why);
        lcd_push();
        int k = getkey(), h = key_hex(k);
        if (k == K_EXIT) { prompt_label = 0; return 0; }
        if (k == K_EXE && n) break;
        if (k == K_DEL && n) { prompt_buf[--n] = 0; fresh = 0; }
        else if (h >= 0) {
            if (fresh) n = 0;
            fresh = 0;
            if (n < 8) { prompt_buf[n++] = "0123456789abcdef"[h]; prompt_buf[n] = 0; }
        }
    }
    prompt_label = 0;
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        char c = prompt_buf[i];
        v = v << 4 | (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    *val = v;
    return 1;
}

/* The address the cursor line points at: a branch / call target or a literal's value, else the
 * line itself (the go-to and watchpoint suggestions). */
static uint32_t cursor_ref(void) {
    uint32_t op;
    sh4_insn_t in;
    char t[48];
    if (memview) return maddr;
    if (!rd(0, cur, 2, &op)) return cur;
    sh4_decode((uint16_t)op, cur, rd, 0, &in, t, sizeof t);
    if (in.kind == SH4_K_BRANCH || in.kind == SH4_K_CALL) return in.target;
    if (in.kind == SH4_K_LITERAL && in.lit_ok && in.lit_size == 4) return in.lit_val;
    if (in.kind == SH4_K_MOVA) return in.lit_addr;
    return cur;
}

/* F3: a breakpoint at the cursor line, or off if there is one. Software if it can be (add-in
 * code, not a delay slot), else the hardware one if channel 0 is free. */
static void toggle_bp(void) {
    uint32_t a = cur;
    int b = bp_at(a), free = -1;
    if (b >= 0) { S.bp[b].on = 0; return; }
    for (int i = 0; i < NBP; i++) if (!S.bp[i].on) { free = i; break; }
    if (free < 0) { msg = "16 breakpoints set already"; return; }
    int sk = slot_kind(a);
    if (sk == 2) { msg = "the second half of a 32-bit instruction"; return; }
    uint32_t p, op;
    int sw = sk == 0 && os_tbl && is_code(a) && (*tbl(a) != 0) && xlate(a, &p) && rd(0, a, 2, &op);
    if (!sw && (hw_bp() >= 0 || S.wp_rw)) {
        msg = S.wp_rw ? "only add-in code: the watchpoint has channel 0" : "only add-in code: channel 0 is in use";
        return;
    }
    S.bp[free].addr = a;
    S.bp[free].hw = (uint8_t)!sw;
    S.bp[free].on = 1;
    if (!sw) msg = sk == 1 ? "delay slot: the hardware breakpoint (channel 0)" : "the hardware breakpoint (channel 0)";
}

/* F6: the watchpoint (channel 0), or off */
static void set_watch(int why) {
    if (S.wp_rw) { S.wp_rw = 0; msg = "watchpoint off"; return; }
    if (hw_bp() >= 0) { msg = "the hardware breakpoint has channel 0"; return; }
    uint32_t a = cursor_ref();
    if (!prompt(why, "watch address: ", &a)) return;
    msg = "watch: 1 read  2 write  3 read or write  (EXIT cancel)";
    draw(why);
    lcd_push();
    msg = 0;
    for (;;) {
        int k = getkey();
        if (k == K_EXIT) return;
        for (int i = 1; i <= 3; i++)
            if (k == k_digit[i]) { S.wp = a; S.wp_rw = i; return; }
    }
}

/* Arm channel 1 to stop once the instruction at pc has run (a branch: with its delay slot):
 * a break AFTER it, except for sleep and trapa, which never take one (sleep halts until an
 * interrupt, trapa enters its handler with BL = 1): for those, a break BEFORE the next one. */
static void arm_step(uint32_t pc, int ch1) {
    uint32_t op;
    if (rd(0, pc, 2, &op) && (op == 0x001Bu || (op & 0xFF00u) == 0xC300u)) want(1, pc + 2, 0);
    else want(1, pc, 1);
    S.ch1 = ch1;
}

/* ------------------------------------------------------------------ the break handler */

void mon_main(void) {
    /* the match flags say why we are here; stop both channels (BL = 1 already blocks them) */
    uint32_t mf = UBC_CCMFR;
    UBC_CCMFR = 0;
    UBC_CBR(0) = 0;
    UBC_CBR(1) = 0;
    (void)UBC_CBR(1);
    mon_cbr[0] = mon_cbr[1] = 0;
    int trap = mon_via_trap;
    mon_via_trap = 0;
    msg = 0;

    /* the target's memory as it is now, without our patches */
    mmucr = MMUCR;
    asid = PTEH & 0xFF;
    tgt_md = (mon_regs.ssr >> 30) & 1;
    os_tbl = os360();
    ((void (*)(uint32_t *))((uint32_t)mon_utlb_read | 0x20000000u))(utlb);   /* from P2 */
    unpatch_all();
    pages_check();

    int ch1 = S.ch1, why;
    S.ch1 = CH1_NONE;
    if (trap) {                                     /* SPC is after the trapa: back onto it */
        int b = bp_at(mon_regs.spc - 2);
        if (b >= 0 && !S.bp[b].hw) mon_regs.spc -= 2;
    }
    uint32_t pc = mon_regs.spc;
    if (!S.started) { S.started = 1; why = W_ENTRY; }
    else if (trap) why = W_BREAK;
    else if (mf & 1) why = S.wp_rw ? W_WATCH : W_BREAK;
    else if (mf & 2) {
        if (ch1 == CH1_RESUME) {                    /* stepped off a breakpoint: run on */
            patch_all(NONE);
            want_ch0(NONE);
            return;
        }
        why = ch1 == CH1_OVER ? W_OVER : W_STEP;
    } else why = W_OTHER;

    /* without the key-scan unit there is no way to talk to the user: detach */
    if (MSTPCR0 & (1u << 6)) { restore_all(); return; }

    mon_nstops++;
    lcd_begin();
    kbd_begin();
    pal = theme_pal[theme_get()];
    memview = 0;
    goto_code(pc);
    int held = key_now() >= 0;                      /* a key held at the stop (EXE that launched) */
    draw(why);
    lcd_push();
    if (held) wait_release();
    int detach = 0;
    for (;;) {
        int k = getkey();
        msg = 0;
        if (k == K_UP) { if (memview) maddr -= MEM_BYTES; else cursor_to(cur - 2); }
        else if (k == K_DOWN) { if (memview) maddr += MEM_BYTES; else cursor_to(cur + 2); }
        else if (k == K_LEFT) { if (memview) maddr -= MEM_BYTES * DIS_ROWS; else { top -= 2u * DIS_ROWS; cur -= 2u * DIS_ROWS; } }
        else if (k == K_RIGHT) { if (memview) maddr += MEM_BYTES * DIS_ROWS; else { top += 2u * DIS_ROWS; cur += 2u * DIS_ROWS; } }
        else if (k == K_F3) { if (memview) msg = "breakpoints: in the code view (F5)"; else toggle_bp(); }
        else if (k == K_F4) {
            uint32_t a = cursor_ref();
            if (prompt(why, "go to: ", &a)) { if (memview) maddr = a; else goto_code(a); }
        }
        else if (k == K_F5) {
            if (!memview) maddr = cursor_ref();
            memview = !memview;
        }
        else if (k == K_F6) set_watch(why);
        else if (k == K_F1 || k == K_F2) {
            sh4_insn_t in;
            uint32_t op;
            char t[48];
            int call = 0;
            if (k == K_F2 && rd(0, pc, 2, &op)) {
                sh4_decode((uint16_t)op, pc, rd, 0, &in, t, sizeof t);
                call = in.kind == SH4_K_CALL || in.kind == SH4_K_CALL_REG;
            }
            if (call) {                             /* run the call: stop after its delay slot */
                want(1, pc + 4, 0);
                S.ch1 = CH1_OVER;
                patch_all(pc);
                want_ch0(pc);
            } else arm_step(pc, CH1_STEP);          /* one instruction: no breakpoints needed */
            break;
        }
        else if (k == K_EXE) {                      /* on a breakpoint: step off it first */
            if (bp_at(pc) >= 0) {
                arm_step(pc, CH1_RESUME);
                patch_all(pc);
                want_ch0(pc);
            } else {
                patch_all(NONE);
                want_ch0(NONE);
            }
            break;
        }
        else if (k == K_EXIT) { detach = 1; break; }
        else if (k == K_FD) {
            int light = pal == theme_pal[0];
            pal = theme_pal[light];
            theme_put(light);
        }
        draw(why);
        lcd_push();
    }
    if (detach) {
        restore_all();
        mon_cbr[0] = mon_cbr[1] = 0;
        S.ch1 = CH1_NONE;
    }
    memview = 0;
    const char *m = msg;
    draw(why);
    fill(0, STAT_Y - 1, W - 1, H - 1, C_STATUS);
    text(0, STAT_Y, m ? C_BR : C_DIM, m ? m : S.ch1 == CH1_STEP ? "stepping..." : S.ch1 == CH1_OVER ? "running the call..."
        : detach ? "detached: running on (the target redraws its own screen)" : "running (the target redraws its own screen)");
    lcd_push();
    kbd_end();
    lcd_end();
    S.prev = mon_regs;
    S.have_prev = 1;
}
