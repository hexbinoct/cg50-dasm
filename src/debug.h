/* debug — DASM's on-device debugger, built on the SH7305 User Break Controller (UBC).
 *
 * Stage 1 (this file): DASM stops on a hardware breakpoint in its own code and shows the CPU
 * state. The break enters our own entry stub dbg_dbh (debug_dbh.S; DBR points at it between
 * dbg_init and dbg_done), modelled on gint's ubc_dbh: it saves the whole CPU state on the stack
 * as a gdb_cpu_state_t, re-enables interrupts in register bank 0 (so the handler can draw and
 * read keys with gint), calls dbg_on_break, restores the (possibly modified) state and resumes
 * at state.pc (the saved SPC; changing state.r15 has no effect, the stack pointer is restored
 * by the pops). Unlike gint's, it stops both channels on entry and re-enables the ones the
 * handler asked for only after SR.BL = 1 again, so the debugger can never break on its own
 * code (a step into a function the handler also calls used to recurse until the stack ran
 * through the OS RAM and the calculator reset). Inside the handler, never enable a channel
 * directly: use want() / want_off() in debug.c.
 *
 * Channel use: channel 0 = the user breakpoint (break BEFORE the instruction), channel 1 =
 * single step (break AFTER the instruction at the current PC, as gint's GDB stub does).
 * A break-before triggers again when we return to it, so continuing from the breakpoint's own
 * address first steps over it silently on channel 1, then re-arms channel 0.
 *
 * Caveat (later stages): a gint world switch (BFile, the OS menu, ...) powers the UBC off and on
 * again (drv_ubc hpoweroff / hpoweron), which clears CE on both channels and points DBR back at
 * gint's ubc_dbh, so breakpoints set before such a call are lost.
 *
 * Register screen keys: EXE continue · F1 single step · EXIT clear the breakpoints and run on.
 * The self-test is started by the sin key (ALPHA: D) in any DASM view (main.c).
 */
#ifndef DEBUG_H
#define DEBUG_H
#include <stdint.h>

/* Install the break handler and make sure breaks reach it (DBR, CBCR.UBDE). 0 = ready,
 * -1 = the UBC module is powered off (MSTPCR0.UDB). */
int dbg_init(void);
/* Clear both channels and put DBR / UBDE back the way dbg_init() found them. */
void dbg_done(void);

/* Set / clear the breakpoint (channel 0, break before the instruction at [addr]). */
void dbg_break(uint32_t addr);
void dbg_clear(void);

/* Self-test: break at the first instruction of a known function inside DASM, show the
 * registers (step / continue from there), then check the function returned the right value. */
void dbg_selftest(void);

#endif
