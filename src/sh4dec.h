/* sh4dec — SH-4 / SH-4A instruction decoder (big-endian), C port of re/sh4dis.py in the
 * casio-cg50 emulator (github.com/hexbinoct/casio-cg50), with the full FPU set and the SH-4A
 * extensions added.
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
    uint32_t target;    /* direct branch target */
    uint32_t lit_addr;  /* literal / mova address */
    uint32_t lit_val;   /* literal value (zero-extended) */
} sh4_insn_t;

/* Read [size] bytes (2 or 4) big-endian at [addr]; return 1 if readable. */
typedef int (*sh4_read_fn)(void *ctx, uint32_t addr, int size, uint32_t *out);

/* Decode [op] located at [pc]. Writes "mnemonic operands" (no comment) into
 * text[cap] and fills *info (may be NULL). Returns the text length. */
int sh4_decode(uint16_t op, uint32_t pc, sh4_read_fn read, void *ctx,
               sh4_insn_t *info, char *text, int cap);

#endif
