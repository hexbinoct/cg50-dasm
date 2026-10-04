/* mon_abi — what DASM and the resident debugger monitor (monitor/) agree on.
 *
 * The monitor is a freestanding blob linked at MON_BASE, in the DRAM that neither the OS nor
 * any add-in uses (physical 0x0C4E0000-0x0C7FFFFF, measured in the emulator). DASM copies
 * monitor.bin there, zeroes the rest of its memory (image_end .. bss_end), points DBR at
 * `entry` and arms UBC channel 0 on the next add-in's first instruction (src/monitor.c). */
#ifndef MON_ABI_H
#define MON_ABI_H
#include <stdint.h>

#define MON_BASE    0x8C4E0000u     /* P1; the same RAM is 0xAC4E0000 through P2 (uncached) */
#define MON_LIMIT   0x8C800000u     /* end of the free DRAM */
#define MON_MAGIC   0x4D4F4E31u     /* "MON1" */
#define MON_VERSION 1u

/* At MON_BASE (monitor/entry.S). */
typedef struct {
    uint32_t magic;         /* MON_MAGIC */
    uint32_t version;       /* MON_VERSION */
    uint32_t entry;         /* the UBC break entry: DBR points here */
    uint32_t image_end;     /* end of the image (monitor.bin covers MON_BASE .. image_end) */
    uint32_t bss_end;       /* end of all the monitor's memory (DASM zeroes image_end .. bss_end) */
    uint32_t regs;          /* address of the target's saved registers (mon_regs_t) */
    uint32_t stops;         /* address of the stop counter (uint32_t; tests read it) */
    uint32_t reserved;
} mon_header_t;

/* The target's CPU state, saved by the entry stub (entry.S) in this order. */
typedef struct {
    uint32_t b0[8];         /* r0-r7 of register bank 0 */
    uint32_t b1[8];         /* r0-r7 of register bank 1 */
    uint32_t r8_14[7];      /* r8-r14 */
    uint32_t r15;           /* = SGR at the break */
    uint32_t spc, ssr, pr, gbr, vbr, mach, macl;
} mon_regs_t;

#endif
