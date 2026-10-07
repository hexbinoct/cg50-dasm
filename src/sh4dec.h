/* sh4dec — SH4AL-DSP instruction decoder (big-endian): the CPU of the fx-CG50's SH7305.
 * Started as a C port of re/sh4dis.py in the casio-cg50 emulator
 * (github.com/hexbinoct/casio-cg50); has the SH-4A extensions and the DSP set. The SH7305 has
 * no FPU: the 0xF--- space holds the DSP data transfers (16 bits) and the parallel-processing
 * instructions (32 bits, first word 0xF800-0xFBFF), and the 0x4n?A-style system-register slots
 * name the DSP registers (dsr a0 x0 x1 y0 y1, mod rs re) instead of fpul/fpscr.
 * Pure: no globals, no allocation; memory is reached through a read callback so
 * the same code serves the on-device viewer and the host verification harness. */
#ifndef SH4DEC_H
#define SH4DEC_H
#include <stdint.h>

enum sh4_kind {
    SH4_K_NONE = 0,
    SH4_K_BRANCH,     /* bra bt bf bt/s bf/s: direct target in .target */
    SH4_K_CALL,       /* bsr: direct target in .target */
    SH4_K_JUMP_REG,   /* jmp @rN braf rN: .reg */
    SH4_K_CALL_REG,   /* jsr @rN bsrf rN: .reg */
    SH4_K_RET,        /* rts rte */
    SH4_K_LITERAL,    /* mov.w/mov.l @(disp,pc),rN: .lit_addr .lit_size .lit_val .reg */
    SH4_K_MOVA,       /* mova @(disp,pc),r0: .lit_addr */
    SH4_K_TRAPA,
};

typedef struct {
    uint8_t  kind;      /* enum sh4_kind */
    uint8_t  delay;     /* 1 = the next instruction is this one's delay slot */
    uint8_t  reg;       /* register for *_REG kinds and LITERAL destination */
    uint8_t  lit_size;  /* 2 or 4 for LITERAL */
    uint8_t  lit_ok;    /* literal value was readable */
    uint8_t  len;       /* instruction length in bytes: 2, or 4 for a DSP parallel instruction */
    uint32_t target;    /* direct branch target */
    uint32_t lit_addr;  /* literal / mova address */
    uint32_t lit_val;   /* literal value (zero-extended) */
} sh4_insn_t;

/* Read [size] bytes (2 or 4) big-endian at [addr]; return 1 if readable. */
typedef int (*sh4_read_fn)(void *ctx, uint32_t addr, int size, uint32_t *out);

/* Length in bytes of the instruction whose first word is [op]: 4 for the DSP parallel-processing
 * instructions (0xF800-0xFBFF), else 2. */
static inline int sh4_insn_len(uint16_t op) { return (op & 0xFC00) == 0xF800 ? 4 : 2; }

/* Decode [op] located at [pc]. Writes "mnemonic operands" (no comment) into
 * text[cap] and fills *info (may be NULL). A 32-bit instruction's second word is fetched through
 * [read] at pc + 2 (without it, or if unreadable, the text is ".word 0xf8xx"). Returns the text
 * length. */
int sh4_decode(uint16_t op, uint32_t pc, sh4_read_fn read, void *ctx,
               sh4_insn_t *info, char *text, int cap);

#endif
