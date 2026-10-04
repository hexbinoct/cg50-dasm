/* monitor — DASM's side of the resident debugger monitor; see monitor.h and ../monitor/mon.c. */
#include <gint/gint.h>
#include <gint/display.h>
#include <gint/keyboard.h>
#include <gint/ubc.h>
#include <gint/mpu/ubc.h>
#include <gint/mpu/power.h>
#include <stdio.h>
#include <string.h>
#include "../monitor/mon_abi.h"
#include "monitor.h"
#include "ui.h"

#define UBC   SH7305_UBC
#define POWER SH7305_POWER
#define UDB   (1u << 17)            /* MSTPCR0: the UBC module is stopped */

extern const uint8_t mon_blob[], mon_blob_end[];      /* monitor_blob.S */
void mon_cache_sync(uint32_t start, uint32_t end);

static const mon_header_t *hdr(void) { return (const mon_header_t *)mon_blob; }

/* Copy the monitor to MON_BASE, zero the rest of its memory, push it out of the caches and
 * check it through P2 (uncached: what is really in RAM). NULL = ready. */
static const char *install(void) {
    const mon_header_t *h = hdr();
    uint32_t size = (uint32_t)(mon_blob_end - mon_blob);
    if (h->magic != MON_MAGIC || h->version != MON_VERSION) return "bad monitor header";
    if (size < sizeof *h || MON_BASE + size > h->image_end || h->image_end > h->bss_end || h->bss_end > (MON_THEME & ~0x20000000u)
        || h->entry < MON_BASE || h->entry >= MON_BASE + size)
        return "monitor sizes don't fit 0x8C4E0000-0x8C7FFFFF";
    memcpy((void *)MON_BASE, mon_blob, size);
    memset((void *)(MON_BASE + size), 0, h->bss_end - MON_BASE - size);
    mon_cache_sync(MON_BASE, (h->bss_end + 31) & ~31u);
    if (memcmp((const void *)(MON_BASE | 0x20000000u), mon_blob, size)) return "read-back of the monitor differs";
    return NULL;
}

/* ------------------------------------------------------------------ arming (OS world) */

static struct { uint32_t mstpcr0, dbr, cbcr; } A;

/* Runs inside gint_world_switch, i.e. with the OS's hardware state: gint stops the UBC when
 * it leaves to the OS (drv_ubc hpoweroff), so arming has to happen here. Verified in the
 * emulator (monitor_probe_test.go): from the MAIN MENU the OS launches an add-in without
 * touching UDB, DBR or the channels, and the break fires at its first instruction. */
static int arm_and_menu(void) {
    const mon_header_t *h = hdr();
    A.mstpcr0 = POWER.MSTPCR0.lword;
    A.dbr = (uint32_t)ubc_getDBR();
    POWER.MSTPCR0.lword = A.mstpcr0 & ~UDB;        /* the UBC module on (as gint's drv_ubc) */
    (void)POWER.MSTPCR0.lword;
    A.cbcr = UBC.CBCR.lword;
    UBC.CBR0.lword = 0;
    UBC.CBR1.lword = 0;
    UBC.CCMFR.lword = 0;
    ubc_setDBR((void *)h->entry);
    UBC.CBCR.lword = 1;                             /* UBDE: breaks go to DBR */
    UBC.CAR0 = 0x00300000;                          /* the next add-in's first instruction */
    UBC.CAMR0 = 0;
    UBC.CRR0.lword = 0x2001;                        /* break before, BIE */
    UBC.CBR0.lword = 0x13;                          /* instruction fetch, read, CE */
    (void)UBC.CBR0.lword;

    gint_osmenu_native();

    /* back in DASM: no add-in was started (one that was would have ended DASM) */
    UBC.CBR0.lword = 0;
    UBC.CBR1.lword = 0;
    UBC.CCMFR.lword = 0;
    UBC.CBCR.lword = A.cbcr;
    ubc_setDBR((void *)A.dbr);
    POWER.MSTPCR0.lword = (POWER.MSTPCR0.lword & ~UDB) | (A.mstpcr0 & UDB);
    (void)POWER.MSTPCR0.lword;
    return 0;
}

/* ------------------------------------------------------------------ the screen */

#define COLS (DWIDTH / UI_CW)

static int line(int y, int color, const char *s) {
    aa_text(UI_CW, y, color, s, COLS - 2);
    return y + UI_CH + 2;
}

void mon_debug_next(char *msg, int cap) {
    const char *err = install();
    const mon_header_t *h = hdr();
    char t[80];

    dclear(C_BG);
    drect(0, 0, DWIDTH - 1, UI_CH, C_TITLE);
    aa_text(0, 1, C_KEY, "DASM debugger  debug the next add-in", COLS);
    int y = UI_CH + 8;
    if (err) {
        snprintf(t, sizeof t, "Can't install the monitor: %s.", err);
        y = line(y, C_RET, t);
        y = line(y + 6, C_KEY, "Press any key.");
        dupdate();
        getkey();
        snprintf(msg, cap, "debugger: %s", err);
        return;
    }
    snprintf(t, sizeof t, "Monitor at 0x%08lx: %lu bytes, verified.",
        (unsigned long)MON_BASE, (unsigned long)(mon_blob_end - mon_blob));
    y = line(y, C_DIM, t);
    y += 4;
    y = line(y, C_TEXT, "EXE opens the MAIN MENU. Open an add-in from there:");
    y = line(y, C_TEXT, "it stops at its first instruction (0x00300000).");
    y += 4;
    y = line(y, C_LIT, "On a stop:  F1 step       F2 step over a call");
    y = line(y, C_LIT, "            F3 breakpoint at the cursor (UP/DOWN)");
    y = line(y, C_LIT, "            EXE continue  EXIT detach (runs on)");
    y = line(y, C_LIT, "            S<>D light / dark theme");
    y += 4;
    y = line(y, C_DIM, "Coming back to DASM instead disarms the debugger.");
    snprintf(t, sizeof t, "UBC break entry 0x%08lx", (unsigned long)h->entry);
    y = line(y, C_DIM, t);
    y += 6;
    line(y, C_KEY, "EXE: MAIN MENU     EXIT: cancel");
    dupdate();
    for (;;) {
        key_event_t ev = getkey();
        if (ev.key == KEY_EXE) break;
        if (ev.key == KEY_EXIT) { snprintf(msg, cap, "debugger: cancelled"); return; }
    }

    gint_world_switch(GINT_CALL(arm_and_menu));
    snprintf(msg, cap, "debugger disarmed: no add-in was started");
}
