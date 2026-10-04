/* mon.ld — the resident monitor, linked to run at 0x8C4E0000 (P1, the free DRAM). The header
 * (entry.S) comes first; monitor.bin is MON_BASE .. _mon_image_end, the .bss after it is zeroed
 * by DASM when it installs the monitor. */
ENTRY(_mon_header)

MEMORY
{
	ram (rwx) : ORIGIN = 0x8C4E0000, LENGTH = 0x320000
}

SECTIONS
{
	.text : {
		KEEP(*(.text.header))
		*(.text .text.*)
	} > ram

	.rodata : {
		*(.rodata .rodata.*)
	} > ram

	.data : {
		*(.data .data.*)
		*(.sdata .sdata.*)
		. = ALIGN(32);
		_mon_image_end = .;
	} > ram

	.bss (NOLOAD) : {
		*(.bss .bss.* .sbss .sbss.* COMMON)
		. = ALIGN(32);
		_mon_bss_end = .;
	} > ram

	/DISCARD/ : {
		*(.comment) *(.eh_frame) *(.debug*) *(.note*)
	}
}
