/* debug_dbh — DASM's own UBC break entry (DBR points here while the debugger runs).
 *
 * The structure of gint's ubc_dbh (src/ubc/ubc.S), with one difference that matters: the
 * debugger must never break on its own code. gint's handler runs the C code with SR.BL = 0,
 * so the UBC is live there, and a step channel armed on a function the handler also calls
 * (e.g. gint's ubc_disable_channel) recursed until the stack ran through the OS RAM. Here:
 *   1. on entry (SR.BL = 1: no break can fire) CBR0/CBR1 are saved to dbg_cbr[] and both
 *      channels are stopped (CE = 0), and CCMFR is cleared;
 *   2. the C handler (dbg_on_break, debug.c) sees the CPU state as a gdb_cpu_state_t, with
 *      interrupts on in register bank 0, and only *records* the channel setup it wants: CAR/CRR
 *      written while CE = 0, the final CBR values in dbg_cbr[];
 *   3. on exit, once SR.BL = 1 again, dbg_cbr[] goes back to CBR0/CBR1 and rte resumes the
 *      target with its own SR.
 * UBC registers: CBR0 +0x00, CBR1 +0x20, CCMFR +0x600 (base 0xff200000). */

	.text
	.align	2
	.global	_dbg_dbh
_dbg_dbh:
	/* breaking where register bank 1 is in use is not supported (as in gint) */
	stc	ssr, r0
	mov.l	.sr_rb1, r1
	tst	r0, r1
	bf	.panic

	/* stop both channels; remember what they were */
	mov.l	.ubc, r1
	mov.l	.cbr, r2
	mov	#-2, r3			/* ~CE */
	mov.l	@r1, r0			/* CBR0 */
	mov.l	r0, @r2
	and	r3, r0
	mov.l	r0, @r1
	mov.l	@(0x20,r1), r0		/* CBR1 */
	mov.l	r0, @(4,r2)
	and	r3, r0
	mov.l	r0, @(0x20,r1)
	mov.w	.ccmfr, r0
	mov	#0, r3
	mov.l	r3, @(r0,r1)		/* clear the match flags */
	mov.l	@(r0,r1), r3		/* read back: the writes are done */

	/* the CPU state, in gdb_cpu_state_t order (r0 at the lowest address) */
	stc.l	ssr,  @-r15
	sts.l	macl, @-r15
	sts.l	mach, @-r15
	stc.l	vbr,  @-r15
	stc.l	gbr,  @-r15
	sts.l	pr,   @-r15
	stc.l	spc,  @-r15
	stc.l	sgr,  @-r15
	mov.l	r14,  @-r15
	mov.l	r13,  @-r15
	mov.l	r12,  @-r15
	mov.l	r11,  @-r15
	mov.l	r10,  @-r15
	mov.l	r9,   @-r15
	mov.l	r8,   @-r15
	stc.l	r7_bank, @-r15
	stc.l	r6_bank, @-r15
	stc.l	r5_bank, @-r15
	stc.l	r4_bank, @-r15
	stc.l	r3_bank, @-r15
	stc.l	r2_bank, @-r15
	stc.l	r1_bank, @-r15
	stc.l	r0_bank, @-r15

	/* bank 0, interrupts on (keyboard, display); the original SR stays in r8 */
	stc	sr, r8
	mov	r8, r1
	mov.l	.sr_mask, r0
	and	r0, r1
	ldc	r1, sr

	mov	r15, r4
	mov.l	.handler, r0
	jsr	@r0
	nop

	/* SR.BL = 1 again (and bank 1): nothing can break from here to the rte */
	ldc	r8, sr

	/* the channels the handler asked for */
	mov.l	.ubc, r1
	mov.l	.cbr, r2
	mov.l	@r2, r0
	mov.l	r0, @r1			/* CBR0 */
	mov.l	@(4,r2), r0
	mov.l	r0, @(0x20,r1)		/* CBR1 */
	mov.l	@(0x20,r1), r0		/* read back */

	ldc.l	@r15+, r0_bank
	ldc.l	@r15+, r1_bank
	ldc.l	@r15+, r2_bank
	ldc.l	@r15+, r3_bank
	ldc.l	@r15+, r4_bank
	ldc.l	@r15+, r5_bank
	ldc.l	@r15+, r6_bank
	ldc.l	@r15+, r7_bank
	mov.l	@r15+, r8
	mov.l	@r15+, r9
	mov.l	@r15+, r10
	mov.l	@r15+, r11
	mov.l	@r15+, r12
	mov.l	@r15+, r13
	mov.l	@r15+, r14
	ldc.l	@r15+, sgr
	ldc.l	@r15+, spc
	lds.l	@r15+, pr
	ldc.l	@r15+, gbr
	ldc.l	@r15+, vbr
	lds.l	@r15+, mach
	lds.l	@r15+, macl
	ldc.l	@r15+, ssr
	rte
	nop

.panic:
	stc	sr, r1
	mov.l	.sr_mask, r0
	and	r0, r1
	ldc	r1, sr
	mov.l	.panic_code, r4
	mov.l	.panic_fn, r0
	mov.l	@r0, r0
	jmp	@r0
	nop

.ccmfr:		.word	0x600
	.align	2
.ubc:		.long	0xff200000
.cbr:		.long	_dbg_cbr
.handler:	.long	_dbg_on_break
.sr_rb1:	.long	(1 << 29)
.sr_mask:	.long	~0x300000f0	/* IMASK = 0, BL = 0, RB = 0 */
.panic_code:	.long	0x10a0		/* gint's code for a UBC break in bank 1 */
.panic_fn:	.long	_gint_exc_panic

	.data
	.align	2
	.global	_dbg_cbr
_dbg_cbr:	.long	0, 0
