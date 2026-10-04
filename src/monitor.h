/* monitor — DASM's side of the resident debugger monitor (stage 2 of the on-device debugger).
 *
 * The monitor (../monitor/, embedded as monitor.bin) is a freestanding blob that lives in the
 * DRAM nobody else uses (0x8C4E0000+) and is entered through the UBC with SR.BL = 1, so it can
 * stop *another* add-in: DASM installs it, then, from inside the OS world (gint_world_switch),
 * powers the UBC on, points DBR at the monitor, arms channel 0 BREAK BEFORE virtual 0x00300000
 * and opens the OS's MAIN MENU. The next add-in the user opens stops at its first instruction
 * with the monitor's screen (registers, disassembly; F1 step, F2 step over, F3 breakpoint, EXE
 * continue, EXIT detach). If the user comes back to DASM instead, the call returns and DASM
 * disarms everything.
 *
 * Key: F2 in the file picker. */
#ifndef MONITOR_H
#define MONITOR_H

/* Install the monitor, explain, arm, open the MAIN MENU. Returns (only if the user re-entered
 * DASM or cancelled) with a one-line status in msg. */
void mon_debug_next(char *msg, int cap);

#endif
