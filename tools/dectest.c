/* Host harness for the decoder: disassemble a range of an image file and print
 * "addr: word   text[    ; =0xLITERAL]" in the exact layout of re/sh4dis.py so
 * tools/verify_decoder.py can diff the two.  Built with the host gcc (Docker).
 *   dectest <image> <base_vaddr> <start> <end>       (hex accepted with 0x) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/sh4dec.h"

static unsigned char *img; static size_t img_len; static uint32_t base;

static int rd(void *ctx, uint32_t addr, int size, uint32_t *out) {
    (void)ctx;
    uint32_t off = addr - base;
    if (addr < base || off + (uint32_t)size > img_len) return 0;
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v = (v << 8) | img[off + i];
    *out = v; return 1;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: dectest image base start end\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); img_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    img = malloc(img_len); if (fread(img, 1, img_len, f) != img_len) return 1; fclose(f);
    base = (uint32_t)strtoul(argv[2], 0, 0);
    uint32_t start = (uint32_t)strtoul(argv[3], 0, 0), end = (uint32_t)strtoul(argv[4], 0, 0);
    char text[96]; sh4_insn_t in;
    static char line[160];
    for (uint32_t pc = start; pc < end; pc += 2) {
        uint32_t op;
        if (!rd(0, pc, 2, &op)) break;
        sh4_decode((uint16_t)op, pc, rd, 0, &in, text, sizeof text);
        int n = snprintf(line, sizeof line, "%08x: %04x   %s", pc, op, text);
        if (in.kind == SH4_K_LITERAL) {
            if (in.lit_ok) snprintf(line + n, sizeof line - n, "    ; =0x%0*x", in.lit_size * 2, in.lit_val);
            else snprintf(line + n, sizeof line - n, "    ; =0x?");
        }
        puts(line);
    }
    return 0;
}
