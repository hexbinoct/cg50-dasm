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
	.long	2			/* version (MON_VERSION) */
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

/* A software breakpoint (trapa #0xC3) reaches here from the hook block below with SPC = the
   instruction after the trapa; r0/r1 of bank 1 are already used (exception scratch, as in the
   OS's and gint's own handlers). Flag it, then save the state like a UBC break. */
	.align	2
	.global	_mon_trap
_mon_trap:
	mov.l	.via, r0
	mov	#1, r1
	bra	_mon_entry
	mov.l	r1, @r0

	.align	2
.via:		.long	_mon_via_trap

/* The hook block: copied to MON_HOOK (mon_abi.h) by mon.c, run from there. It is the target's
   VBR while software breakpoints are set and the target's own VBR is the OS's (in ROM), and
   stays so if a gint target saved it as "the OS's VBR". It forwards every exception and
   interrupt to the VBR it replaced, except the monitor's trapa. It only uses r0/r1 of bank 1
   (scratch at exception entry for the OS's and gint's handlers too) and absolute addresses
   into its own data words (+4 the replaced VBR, +8 where a breakpoint trap goes, 0 = nowhere),
   so the copy works at MON_HOOK, and gint's patched exception entry (+0x700) also lands here. */
#define HOOK	0x8C7F0000
	.section .text.hook, "ax"	/* its own input section: .org counts from _mon_hook */
	.align	4
	.global	_mon_hook, _mon_hook_end
_mon_hook:
	.long	0x484F4F4B		/* +0 "HOOK" */
	.long	0			/* +4 the replaced VBR (mon.c) */
	.long	0			/* +8 the trap entry (mon.c) */

	/* +0x100: general exceptions. Ours = EXPEVT 0x160 (trapa) and TRA = 0xC3 << 2. */
	.org	0x100
	mov.l	h1_expevt, r1
	mov.l	@r1, r0
	mov.w	h1_c160, r1
	cmp/eq	r1, r0
	bf	1f
	mov.l	h1_tra, r1
	mov.l	@r1, r0
	mov.w	h1_c30c, r1
	cmp/eq	r1, r0
	bf	1f
	mov.l	h1_trap, r0
	mov.l	@r0, r0
	tst	r0, r0
	bt	1f
	jmp	@r0
	nop
1:	mov.l	h1_orig, r0
	mov.l	@r0, r0
	mov.w	h1_100, r1
	add	r1, r0
	jmp	@r0
	nop
h1_c160: .word	0x160
h1_c30c: .word	0xC3 << 2
h1_100:	.word	0x100
	.align	2
h1_expevt: .long 0xFF000024
h1_tra:	.long	0xFF000020
h1_trap: .long	HOOK + 8
h1_orig: .long	HOOK + 4

	/* +0x400: TLB misses */
	.org	0x400
	mov.l	h4_orig, r0
	mov.l	@r0, r0
	mov.w	h4_400, r1
	add	r1, r0
	jmp	@r0
	nop
h4_400:	.word	0x400
	.align	2
h4_orig: .long	HOOK + 4

	/* +0x600: interrupts */
	.org	0x600
	mov.l	h6_orig, r0
	mov.l	@r0, r0
	mov.w	h6_600, r1
	add	r1, r0
	jmp	@r0
	nop
h6_600:	.word	0x600
	.align	2
h6_orig: .long	HOOK + 4

	/* +0x700: gint's exception entry (its copy at VBR+0x100, or its source in the target's
	   code) starts with a jump here instead of its first 6 instructions. Ours: to the trap
	   entry; anything else: those 6 instructions, then on at VBR+0x10C. */
	.org	0x700
	mov.l	h7_expevt, r1
	mov.l	@r1, r0
	mov.w	h7_c160, r1
	cmp/eq	r1, r0
	bf	1f
	mov.l	h7_tra, r1
	mov.l	@r1, r0
	mov.w	h7_c30c, r1
	cmp/eq	r1, r0
	bf	1f
	mov.l	h7_trap, r0
	mov.l	@r0, r0
	tst	r0, r0
	bt	1f
	jmp	@r0
	nop
1:	sts.l	pr, @-r15
	stc.l	gbr, @-r15
	sts.l	mach, @-r15
	sts.l	macl, @-r15
	mov.l	r8, @-r15
	mov.l	r9, @-r15
	stc	vbr, r0
	mov.w	h7_10c, r1
	add	r1, r0
	jmp	@r0
	nop
h7_c160: .word	0x160
h7_c30c: .word	0xC3 << 2
h7_10c:	.word	0x10C
	.align	2
h7_expevt: .long 0xFF000024
h7_tra:	.long	0xFF000020
h7_trap: .long	HOOK + 8
	.align	4
_mon_hook_end:

	.text

/* mon_p2_w32(addr, value): one store done by code in P2, for the memory-mapped TLB arrays and
   CCR (the cache control register), which only P2 code may write. mon.c calls it through its
   P2 alias; the nops keep the next P1 instructions clear of the write (the manual asks for a
   gap before an access the change affects). */
	.align	4
	.global	_mon_p2_w32
_mon_p2_w32:
	mov.l	r5, @r4
	mov.l	@r4, r0
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	rts
	nop

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
