/* entry.S — the resident monitor's header, UBC break entry and UTLB reader.
 *
 * The monitor runs its whole time with SR.BL = 1: no interrupt is accepted and no break can
 * match, so it can never break on itself (stage 1's lesson), and it must never cause an
 * exception either (an exception while BL = 1 resets the calculator). It touches only P1/P2/P4
 * addresses and its own memory at 0x8C4E0000+.
 *
 * At a break (DBR = mon_entry): SR.MD = RB = BL = 1, SSR = the target's SR, SPC = where it
 * stopped, SGR = its r15. mon_entry saves the whole CPU state into mon_regs (mon_abi.h order),
 * switches to the monitor's own stack, calls mon_main (mon.c), then writes mon_cbr[] to CBR0 /
 * CBR1 (the channels mon_main asked for: their CAR/CRR are already set while CE = 0), restores
 * everything and returns to the target with rte. */

	.section .text.header, "ax"
	.align	4
	.global	_mon_header
_mon_header:
	.long	0x4D4F4E31		/* magic "MON1" */
	.long	1			/* version */
	.long	_mon_entry		/* DBR target */
	.long	_mon_image_end
	.long	_mon_bss_end
	.long	_mon_regs
	.long	_mon_nstops
	.long	0

	.text
	.align	4
	.global	_mon_entry
_mon_entry:
	/* register bank 1 is current: r0-r7 here are bank 1, r0_bank-r7_bank are bank 0. The
	   target's r15 is in SGR, so r15 is free to point at the save area. */
	mov.l	.regs_end, r15
	sts.l	macl, @-r15
	sts.l	mach, @-r15
	stc.l	vbr, @-r15
	stc.l	gbr, @-r15
	sts.l	pr, @-r15
	stc.l	ssr, @-r15
	stc.l	spc, @-r15
	stc.l	sgr, @-r15
	mov.l	r14, @-r15
	mov.l	r13, @-r15
	mov.l	r12, @-r15
	mov.l	r11, @-r15
	mov.l	r10, @-r15
	mov.l	r9, @-r15
	mov.l	r8, @-r15
	mov.l	r7, @-r15
	mov.l	r6, @-r15
	mov.l	r5, @-r15
	mov.l	r4, @-r15
	mov.l	r3, @-r15
	mov.l	r2, @-r15
	mov.l	r1, @-r15
	mov.l	r0, @-r15
	stc.l	r7_bank, @-r15
	stc.l	r6_bank, @-r15
	stc.l	r5_bank, @-r15
	stc.l	r4_bank, @-r15
	stc.l	r3_bank, @-r15
	stc.l	r2_bank, @-r15
	stc.l	r1_bank, @-r15
	stc.l	r0_bank, @-r15

	/* the monitor's own stack; SR stays as the break left it (MD = RB = BL = 1) */
	mov.l	.stack_top, r15
	mov.l	.main, r0
	jsr	@r0
	nop

	/* the channels mon_main asked for (CE last, after everything else is set) */
	mov.l	.ubc, r1
	mov.l	.cbr, r2
	mov.l	@r2, r0
	mov.l	r0, @r1			/* CBR0 */
	mov.l	@(4,r2), r0
	mov.l	r0, @(0x20,r1)		/* CBR1 */
	mov.l	@(0x20,r1), r0		/* read back: the writes are done before the rte */

	mov.l	.regs, r15
	ldc.l	@r15+, r0_bank
	ldc.l	@r15+, r1_bank
	ldc.l	@r15+, r2_bank
	ldc.l	@r15+, r3_bank
	ldc.l	@r15+, r4_bank
	ldc.l	@r15+, r5_bank
	ldc.l	@r15+, r6_bank
	ldc.l	@r15+, r7_bank
	mov.l	@r15+, r0
	mov.l	@r15+, r1
	mov.l	@r15+, r2
	mov.l	@r15+, r3
	mov.l	@r15+, r4
	mov.l	@r15+, r5
	mov.l	@r15+, r6
	mov.l	@r15+, r7
	mov.l	@r15+, r8
	mov.l	@r15+, r9
	mov.l	@r15+, r10
	mov.l	@r15+, r11
	mov.l	@r15+, r12
	mov.l	@r15+, r13
	mov.l	@r15+, r14
	add	#4, r15			/* r15 last (below) */
	ldc.l	@r15+, spc
	ldc.l	@r15+, ssr
	lds.l	@r15+, pr
	ldc.l	@r15+, gbr
	ldc.l	@r15+, vbr
	lds.l	@r15+, mach
	lds.l	@r15+, macl
	add	#-32, r15		/* back to the saved r15 */
	mov.l	@r15, r15
	rte
	nop

	.align	2
.regs:		.long	_mon_regs
.regs_end:	.long	_mon_regs + 124
.stack_top:	.long	_mon_stack + 16384
.main:		.long	_mon_main
.ubc:		.long	0xff200000
.cbr:		.long	_mon_cbr

/* mon_utlb_read(uint32_t dst[128]): the 64 UTLB entries as (address array, data array 1)
 * pairs. The memory-mapped TLB arrays may only be accessed by a program in P2, so mon.c calls
 * this through its P2 alias (+0x20000000): it is position-independent (PC-relative literals
 * only). Address array 0xF6000000 + (i << 8): VPN 31:10, D 9, V 8, ASID 7:0; data array 1
 * 0xF7000000 + (i << 8): PPN 28:10, V 8, SZ1 7, PR 6:5, SZ0 4, C 3, D 2, SH 1, WT 0. */
	.align	4
	.global	_mon_utlb_read
_mon_utlb_read:
	mov.l	.f6, r1
	mov.l	.f7, r2
	mov	#64, r3
	mov.w	.step, r5
1:	mov.l	@r1, r0
	mov.l	r0, @r4
	mov.l	@r2, r0
	mov.l	r0, @(4,r4)
	add	#8, r4
	add	r5, r1
	dt	r3
	bf/s	1b
	add	r5, r2
	rts
	nop
.step:		.word	0x100
	.align	2
.f6:		.long	0xf6000000
.f7:		.long	0xf7000000
