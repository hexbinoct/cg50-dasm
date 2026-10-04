/* monitor_blob — the resident debugger monitor's image (monitor/, built as monitor.bin by
 * CMakeLists.txt) embedded into DASM, and the cache maintenance DASM needs to install it. */

	.section .rodata
	.align	5
	.global	_mon_blob
	.global	_mon_blob_end
_mon_blob:
	.incbin	"monitor.bin"
_mon_blob_end:

/* mon_cache_sync(start, end): for each 32-byte line of [start, end) (P1 addresses, 32-byte
 * aligned), write the operand cache line back to RAM (ocbwb) and invalidate the instruction
 * cache line (icbi, SH-4A), so code copied through the data side runs, also through P2. */
	.text
	.align	2
	.global	_mon_cache_sync
_mon_cache_sync:
1:	.word	0x04b3			/* ocbwb @r4 */
	.word	0x04e3			/* icbi  @r4 */
	add	#32, r4
	cmp/hs	r5, r4
	bf	1b
	.word	0x00ab			/* synco */
	rts
	nop
