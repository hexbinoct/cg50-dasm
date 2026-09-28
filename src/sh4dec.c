#include "sh4dec.h"

/* ---- tiny formatter (no printf: keeps the decoder fast and libc-free) ---- */
typedef struct { char *p; char *end; char *start; } buf_t;

static void put_c(buf_t *b, char c) { if (b->p < b->end) *b->p++ = c; }
static void put_s(buf_t *b, const char *s) { while (*s) put_c(b, *s++); }
static void put_hex(buf_t *b, uint32_t v, int digits) {
    /* digits = 0: minimal width */
    char tmp[9]; int n = 0;
    do { tmp[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v || n < digits);
    while (n) put_c(b, tmp[--n]);
}
static void put_dec(buf_t *b, int32_t v) {
    char tmp[12]; int n = 0; uint32_t u;
    if (v < 0) { put_c(b, '-'); u = (uint32_t)(-v); } else u = (uint32_t)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) put_c(b, tmp[--n]);
}
static void put_reg(buf_t *b, int r) { put_c(b, 'r'); put_dec(b, r); }
static void put_freg(buf_t *b, const char *pfx, int r) { put_s(b, pfx); put_dec(b, r); }

/* "@(0xNN,rM)" */
static void put_disp_reg(buf_t *b, uint32_t disp, int r) {
    put_s(b, "@(0x"); put_hex(b, disp, 0); put_c(b, ','); put_reg(b, r); put_c(b, ')');
}
static void put_disp_name(buf_t *b, uint32_t disp, const char *name) {
    put_s(b, "@(0x"); put_hex(b, disp, 0); put_c(b, ','); put_s(b, name); put_c(b, ')');
}
/* "op rM,rN" */
static void op_mn(buf_t *b, const char *op, int m, int n) {
    put_s(b, op); put_c(b, ' '); put_reg(b, m); put_c(b, ','); put_reg(b, n);
}
/* "op rN" */
static void op_n(buf_t *b, const char *op, int n) { put_s(b, op); put_c(b, ' '); put_reg(b, n); }
/* "op @rN" */
static void op_at_n(buf_t *b, const char *op, int n) { put_s(b, op); put_s(b, " @"); put_reg(b, n); }
/* "op rM,@rN" with optional predecrement */
static void op_store(buf_t *b, const char *op, int m, int n, int predec) {
    put_s(b, op); put_c(b, ' '); put_reg(b, m); put_s(b, predec ? ",@-" : ",@"); put_reg(b, n);
}
/* "op @rM,rN" with optional postincrement */
static void op_load(buf_t *b, const char *op, int m, int n, int postinc) {
    put_s(b, op); put_s(b, " @"); put_reg(b, m); if (postinc) put_c(b, '+'); put_c(b, ','); put_reg(b, n);
}
static void op_sys_to_reg(buf_t *b, const char *op, const char *sys, int n) {  /* "sts pr,rN" */
    put_s(b, op); put_c(b, ' '); put_s(b, sys); put_c(b, ','); put_reg(b, n);
}
static void op_reg_to_sys(buf_t *b, const char *op, int m, const char *sys) {  /* "lds rM,pr" */
    put_s(b, op); put_c(b, ' '); put_reg(b, m); put_c(b, ','); put_s(b, sys);
}
static void op_sys_store(buf_t *b, const char *op, const char *sys, int n) {  /* "sts.l pr,@-rN" */
    put_s(b, op); put_c(b, ' '); put_s(b, sys); put_s(b, ",@-"); put_reg(b, n);
}
static void op_sys_load(buf_t *b, const char *op, int m, const char *sys) {   /* "lds.l @rM+,pr" */
    put_s(b, op); put_s(b, " @"); put_reg(b, m); put_s(b, "+,"); put_s(b, sys);
}
static void word(buf_t *b, uint16_t x) { put_s(b, ".word 0x"); put_hex(b, x, 4); }

static const char *ctrl_name(int k) {
    /* k = bits 7..4 of the 0x0n?2 / 0x4n?e family: 0 sr 1 gbr 2 vbr 3 ssr 4 spc */
    static const char *t[] = { "sr", "gbr", "vbr", "ssr", "spc" };
    return (k >= 0 && k <= 4) ? t[k] : 0;
}

int sh4_decode(uint16_t x, uint32_t pc, sh4_read_fn read, void *ctx,
               sh4_insn_t *info, char *text, int cap)
{
    buf_t b; b.p = text; b.start = text; b.end = text + cap - 1;
    sh4_insn_t tmp; sh4_insn_t *I = info ? info : &tmp;
    I->kind = SH4_K_NONE; I->delay = 0; I->reg = 0; I->lit_size = 0; I->lit_ok = 0;
    I->target = 0; I->lit_addr = 0; I->lit_val = 0;

    int n = (x >> 8) & 0xF, m = (x >> 4) & 0xF, d4 = x & 0xF, d8 = x & 0xFF, d12 = x & 0xFFF;
    int32_t simm8 = (int8_t)d8;
    int hi = x >> 12;

    switch (hi) {
    case 0x0:
        switch (d4) {
        case 0x2:
            if (m & 8) { put_s(&b, "stc r"); put_dec(&b, m & 7); put_s(&b, "_bank,"); put_reg(&b, n); break; }
            if (ctrl_name(m)) { op_sys_to_reg(&b, "stc", ctrl_name(m), n); break; }
            goto bad;
        case 0x3:
            switch (m) {
            case 0x0: op_n(&b, "bsrf", n); I->kind = SH4_K_CALL_REG; I->reg = n; I->delay = 1; break;
            case 0x2: op_n(&b, "braf", n); I->kind = SH4_K_JUMP_REG; I->reg = n; I->delay = 1; break;
            case 0x6: put_s(&b, "movli.l @"); put_reg(&b, n); put_s(&b, ",r0"); break;
            case 0x7: put_s(&b, "movco.l r0,@"); put_reg(&b, n); break;
            case 0x8: op_at_n(&b, "pref", n); break;
            case 0x9: op_at_n(&b, "ocbi", n); break;
            case 0xa: op_at_n(&b, "ocbp", n); break;
            case 0xb: op_at_n(&b, "ocbwb", n); break;
            case 0xc: put_s(&b, "movca.l r0,@"); put_reg(&b, n); break;
            case 0xd: op_at_n(&b, "prefi", n); break;
            case 0xe: op_at_n(&b, "icbi", n); break;
            default: goto bad;
            }
            break;
        case 0x4: put_s(&b, "mov.b "); put_reg(&b, m); put_s(&b, ",@(r0,"); put_reg(&b, n); put_c(&b, ')'); break;
        case 0x5: put_s(&b, "mov.w "); put_reg(&b, m); put_s(&b, ",@(r0,"); put_reg(&b, n); put_c(&b, ')'); break;
        case 0x6: put_s(&b, "mov.l "); put_reg(&b, m); put_s(&b, ",@(r0,"); put_reg(&b, n); put_c(&b, ')'); break;
        case 0x7: op_mn(&b, "mul.l", m, n); break;
        case 0x8:
            if (n != 0) goto bad;
            switch (m) {
            case 0: put_s(&b, "clrt"); break;
            case 1: put_s(&b, "sett"); break;
            case 2: put_s(&b, "clrmac"); break;
            case 3: put_s(&b, "ldtlb"); break;
            case 4: put_s(&b, "clrs"); break;
            case 5: put_s(&b, "sets"); break;
            default: goto bad;
            }
            break;
        case 0x9:
            if (x == 0x0009) { put_s(&b, "nop"); break; }
            if (x == 0x0019) { put_s(&b, "div0u"); break; }
            if (m == 2) { op_n(&b, "movt", n); break; }
            goto bad;
        case 0xa:
            switch (m) {
            case 0x0: op_sys_to_reg(&b, "sts", "mach", n); break;
            case 0x1: op_sys_to_reg(&b, "sts", "macl", n); break;
            case 0x2: op_sys_to_reg(&b, "sts", "pr", n); break;
            case 0x3: op_sys_to_reg(&b, "stc", "sgr", n); break;
            case 0x5: op_sys_to_reg(&b, "sts", "fpul", n); break;
            case 0x6: op_sys_to_reg(&b, "sts", "fpscr", n); break;
            case 0xf: op_sys_to_reg(&b, "stc", "dbr", n); break;
            default: goto bad;
            }
            break;
        case 0xb:
            if (x == 0x000b) { put_s(&b, "rts"); I->kind = SH4_K_RET; I->delay = 1; break; }
            if (x == 0x001b) { put_s(&b, "sleep"); break; }
            if (x == 0x002b) { put_s(&b, "rte"); I->kind = SH4_K_RET; I->delay = 1; break; }
            if (x == 0x00ab) { put_s(&b, "synco"); break; }
            goto bad;
        case 0xc: put_s(&b, "mov.b @(r0,"); put_reg(&b, m); put_s(&b, "),"); put_reg(&b, n); break;
        case 0xd: put_s(&b, "mov.w @(r0,"); put_reg(&b, m); put_s(&b, "),"); put_reg(&b, n); break;
        case 0xe: put_s(&b, "mov.l @(r0,"); put_reg(&b, m); put_s(&b, "),"); put_reg(&b, n); break;
        case 0xf: put_s(&b, "mac.l @"); put_reg(&b, m); put_s(&b, "+,@"); put_reg(&b, n); put_c(&b, '+'); break;
        default: goto bad;
        }
        break;

    case 0x1: put_s(&b, "mov.l "); put_reg(&b, m); put_c(&b, ','); put_disp_reg(&b, d4 * 4, n); break;

    case 0x2:
        switch (d4) {
        case 0x0: op_store(&b, "mov.b", m, n, 0); break;
        case 0x1: op_store(&b, "mov.w", m, n, 0); break;
        case 0x2: op_store(&b, "mov.l", m, n, 0); break;
        case 0x4: op_store(&b, "mov.b", m, n, 1); break;
        case 0x5: op_store(&b, "mov.w", m, n, 1); break;
        case 0x6: op_store(&b, "mov.l", m, n, 1); break;
        case 0x7: op_mn(&b, "div0s", m, n); break;
        case 0x8: op_mn(&b, "tst", m, n); break;
        case 0x9: op_mn(&b, "and", m, n); break;
        case 0xa: op_mn(&b, "xor", m, n); break;
        case 0xb: op_mn(&b, "or", m, n); break;
        case 0xc: op_mn(&b, "cmp/str", m, n); break;
        case 0xd: op_mn(&b, "xtrct", m, n); break;
        case 0xe: op_mn(&b, "mulu.w", m, n); break;
        case 0xf: op_mn(&b, "muls.w", m, n); break;
        default: goto bad;
        }
        break;

    case 0x3:
        switch (d4) {
        case 0x0: op_mn(&b, "cmp/eq", m, n); break;
        case 0x2: op_mn(&b, "cmp/hs", m, n); break;
        case 0x3: op_mn(&b, "cmp/ge", m, n); break;
        case 0x4: op_mn(&b, "div1", m, n); break;
        case 0x5: op_mn(&b, "dmulu.l", m, n); break;
        case 0x6: op_mn(&b, "cmp/hi", m, n); break;
        case 0x7: op_mn(&b, "cmp/gt", m, n); break;
        case 0x8: op_mn(&b, "sub", m, n); break;
        case 0xa: op_mn(&b, "subc", m, n); break;
        case 0xb: op_mn(&b, "subv", m, n); break;
        case 0xc: op_mn(&b, "add", m, n); break;
        case 0xd: op_mn(&b, "dmuls.l", m, n); break;
        case 0xe: op_mn(&b, "addc", m, n); break;
        case 0xf: op_mn(&b, "addv", m, n); break;
        default: goto bad;
        }
        break;

    case 0x4:
        switch (d4) {
        case 0xc: op_mn(&b, "shad", m, n); break;
        case 0xd: op_mn(&b, "shld", m, n); break;
        case 0xf: put_s(&b, "mac.w @"); put_reg(&b, m); put_s(&b, "+,@"); put_reg(&b, n); put_c(&b, '+'); break;
        case 0x3:   /* stc.l xxx,@-rn */
            if (m & 8) { put_s(&b, "stc.l r"); put_dec(&b, m & 7); put_s(&b, "_bank,@-"); put_reg(&b, n); break; }
            if (ctrl_name(m)) { op_sys_store(&b, "stc.l", ctrl_name(m), n); break; }
            goto bad;
        case 0x7:   /* ldc.l @rn+,xxx */
            if (m & 8) { put_s(&b, "ldc.l @"); put_reg(&b, n); put_s(&b, "+,r"); put_dec(&b, m & 7); put_s(&b, "_bank"); break; }
            if (ctrl_name(m)) { op_sys_load(&b, "ldc.l", n, ctrl_name(m)); break; }
            goto bad;
        case 0xe:   /* ldc rn,xxx */
            if (m & 8) { put_s(&b, "ldc "); put_reg(&b, n); put_s(&b, ",r"); put_dec(&b, m & 7); put_s(&b, "_bank"); break; }
            if (ctrl_name(m)) { op_reg_to_sys(&b, "ldc", n, ctrl_name(m)); break; }
            goto bad;
        case 0x0:
            switch (m) {
            case 0: op_n(&b, "shll", n); break;
            case 1: op_n(&b, "dt", n); break;
            case 2: op_n(&b, "shal", n); break;
            default: goto bad;
            }
            break;
        case 0x1:
            switch (m) {
            case 0: op_n(&b, "shlr", n); break;
            case 1: op_n(&b, "cmp/pz", n); break;
            case 2: op_n(&b, "shar", n); break;
            default: goto bad;
            }
            break;
        case 0x2:   /* sts.l / stc.l @-rn */
            switch (m) {
            case 0x0: op_sys_store(&b, "sts.l", "mach", n); break;
            case 0x1: op_sys_store(&b, "sts.l", "macl", n); break;
            case 0x2: op_sys_store(&b, "sts.l", "pr", n); break;
            case 0x3: op_sys_store(&b, "stc.l", "sgr", n); break;
            case 0x5: op_sys_store(&b, "sts.l", "fpul", n); break;
            case 0x6: op_sys_store(&b, "sts.l", "fpscr", n); break;
            case 0xf: op_sys_store(&b, "stc.l", "dbr", n); break;
            default: goto bad;
            }
            break;
        case 0x4:
            switch (m) {
            case 0: op_n(&b, "rotl", n); break;
            case 2: op_n(&b, "rotcl", n); break;
            default: goto bad;
            }
            break;
        case 0x5:
            switch (m) {
            case 0: op_n(&b, "rotr", n); break;
            case 1: op_n(&b, "cmp/pl", n); break;
            case 2: op_n(&b, "rotcr", n); break;
            default: goto bad;
            }
            break;
        case 0x6:   /* lds.l / ldc.l @rn+ */
            switch (m) {
            case 0x0: op_sys_load(&b, "lds.l", n, "mach"); break;
            case 0x1: op_sys_load(&b, "lds.l", n, "macl"); break;
            case 0x2: op_sys_load(&b, "lds.l", n, "pr"); break;
            case 0x3: op_sys_load(&b, "ldc.l", n, "sgr"); break;
            case 0x5: op_sys_load(&b, "lds.l", n, "fpul"); break;
            case 0x6: op_sys_load(&b, "lds.l", n, "fpscr"); break;
            case 0xf: op_sys_load(&b, "ldc.l", n, "dbr"); break;
            default: goto bad;
            }
            break;
        case 0x8:
            switch (m) {
            case 0: op_n(&b, "shll2", n); break;
            case 1: op_n(&b, "shll8", n); break;
            case 2: op_n(&b, "shll16", n); break;
            default: goto bad;
            }
            break;
        case 0x9:
            switch (m) {
            case 0x0: op_n(&b, "shlr2", n); break;
            case 0x1: op_n(&b, "shlr8", n); break;
            case 0x2: op_n(&b, "shlr16", n); break;
            case 0xa: put_s(&b, "movua.l @"); put_reg(&b, n); put_s(&b, ",r0"); break;
            case 0xe: put_s(&b, "movua.l @"); put_reg(&b, n); put_s(&b, "+,r0"); break;
            default: goto bad;
            }
            break;
        case 0xa:   /* lds / ldc rn,xxx */
            switch (m) {
            case 0x0: op_reg_to_sys(&b, "lds", n, "mach"); break;
            case 0x1: op_reg_to_sys(&b, "lds", n, "macl"); break;
            case 0x2: op_reg_to_sys(&b, "lds", n, "pr"); break;
            case 0x3: op_reg_to_sys(&b, "ldc", n, "sgr"); break;
            case 0x5: op_reg_to_sys(&b, "lds", n, "fpul"); break;
            case 0x6: op_reg_to_sys(&b, "lds", n, "fpscr"); break;
            case 0xf: op_reg_to_sys(&b, "ldc", n, "dbr"); break;
            default: goto bad;
            }
            break;
        case 0xb:
            switch (m) {
            case 0: op_at_n(&b, "jsr", n); I->kind = SH4_K_CALL_REG; I->reg = n; I->delay = 1; break;
            case 1: op_at_n(&b, "tas.b", n); break;
            case 2: op_at_n(&b, "jmp", n); I->kind = SH4_K_JUMP_REG; I->reg = n; I->delay = 1; break;
            default: goto bad;
            }
            break;
        default: goto bad;
        }
        break;

    case 0x5: put_s(&b, "mov.l "); put_disp_reg(&b, d4 * 4, m); put_c(&b, ','); put_reg(&b, n); break;

    case 0x6:
        switch (d4) {
        case 0x0: op_load(&b, "mov.b", m, n, 0); break;
        case 0x1: op_load(&b, "mov.w", m, n, 0); break;
        case 0x2: op_load(&b, "mov.l", m, n, 0); break;
        case 0x3: op_mn(&b, "mov", m, n); break;
        case 0x4: op_load(&b, "mov.b", m, n, 1); break;
        case 0x5: op_load(&b, "mov.w", m, n, 1); break;
        case 0x6: op_load(&b, "mov.l", m, n, 1); break;
        case 0x7: op_mn(&b, "not", m, n); break;
        case 0x8: op_mn(&b, "swap.b", m, n); break;
        case 0x9: op_mn(&b, "swap.w", m, n); break;
        case 0xa: op_mn(&b, "negc", m, n); break;
        case 0xb: op_mn(&b, "neg", m, n); break;
        case 0xc: op_mn(&b, "extu.b", m, n); break;
        case 0xd: op_mn(&b, "extu.w", m, n); break;
        case 0xe: op_mn(&b, "exts.b", m, n); break;
        case 0xf: op_mn(&b, "exts.w", m, n); break;
        }
        break;

    case 0x7: put_s(&b, "add #"); put_dec(&b, simm8); put_c(&b, ','); put_reg(&b, n); break;

    case 0x8:
        switch (n) {
        case 0x0: put_s(&b, "mov.b r0,"); put_disp_reg(&b, d4, m); break;
        case 0x1: put_s(&b, "mov.w r0,"); put_disp_reg(&b, d4 * 2, m); break;
        case 0x4: put_s(&b, "mov.b "); put_disp_reg(&b, d4, m); put_s(&b, ",r0"); break;
        case 0x5: put_s(&b, "mov.w "); put_disp_reg(&b, d4 * 2, m); put_s(&b, ",r0"); break;
        case 0x8: put_s(&b, "cmp/eq #"); put_dec(&b, simm8); put_s(&b, ",r0"); break;
        case 0x9: case 0xb: case 0xd: case 0xf: {
            static const char *nm[] = { "bt", "bf", "bt/s", "bf/s" };
            const char *s = nm[(n - 9) / 2];
            uint32_t t = pc + 4 + (uint32_t)(simm8 * 2);
            put_s(&b, s); put_s(&b, " 0x"); put_hex(&b, t, 8);
            I->kind = SH4_K_BRANCH; I->target = t; I->delay = (n >= 0xd);
            break;
        }
        default: goto bad;
        }
        break;

    case 0x9: {
        uint32_t ea = pc + 4 + (uint32_t)d8 * 2;
        put_s(&b, "mov.w "); put_disp_name(&b, d8 * 2, "pc"); put_c(&b, ','); put_reg(&b, n);
        I->kind = SH4_K_LITERAL; I->reg = n; I->lit_addr = ea; I->lit_size = 2;
        if (read && read(ctx, ea, 2, &I->lit_val)) I->lit_ok = 1;
        break;
    }
    case 0xa: case 0xb: {
        int32_t disp = (d12 & 0x800) ? (int32_t)d12 - 0x1000 : (int32_t)d12;
        uint32_t t = pc + 4 + (uint32_t)(disp * 2);
        put_s(&b, hi == 0xa ? "bra 0x" : "bsr 0x"); put_hex(&b, t, 8);
        I->kind = hi == 0xa ? SH4_K_BRANCH : SH4_K_CALL; I->target = t; I->delay = 1;
        break;
    }
    case 0xc:
        switch (n) {
        case 0x0: put_s(&b, "mov.b r0,"); put_disp_name(&b, d8, "gbr"); break;
        case 0x1: put_s(&b, "mov.w r0,"); put_disp_name(&b, d8 * 2, "gbr"); break;
        case 0x2: put_s(&b, "mov.l r0,"); put_disp_name(&b, d8 * 4, "gbr"); break;
        case 0x3: put_s(&b, "trapa #0x"); put_hex(&b, d8, 0); I->kind = SH4_K_TRAPA; break;
        case 0x4: put_s(&b, "mov.b "); put_disp_name(&b, d8, "gbr"); put_s(&b, ",r0"); break;
        case 0x5: put_s(&b, "mov.w "); put_disp_name(&b, d8 * 2, "gbr"); put_s(&b, ",r0"); break;
        case 0x6: put_s(&b, "mov.l "); put_disp_name(&b, d8 * 4, "gbr"); put_s(&b, ",r0"); break;
        case 0x7: {
            uint32_t ea = (pc & ~3u) + 4 + (uint32_t)d8 * 4;
            put_s(&b, "mova "); put_disp_name(&b, d8 * 4, "pc"); put_s(&b, ",r0");
            I->kind = SH4_K_MOVA; I->lit_addr = ea; I->reg = 0;
            break;
        }
        case 0x8: put_s(&b, "tst #0x"); put_hex(&b, d8, 0); put_s(&b, ",r0"); break;
        case 0x9: put_s(&b, "and #0x"); put_hex(&b, d8, 0); put_s(&b, ",r0"); break;
        case 0xa: put_s(&b, "xor #0x"); put_hex(&b, d8, 0); put_s(&b, ",r0"); break;
        case 0xb: put_s(&b, "or #0x"); put_hex(&b, d8, 0); put_s(&b, ",r0"); break;
        case 0xc: put_s(&b, "tst.b #0x"); put_hex(&b, d8, 0); put_s(&b, ",@(r0,gbr)"); break;
        case 0xd: put_s(&b, "and.b #0x"); put_hex(&b, d8, 0); put_s(&b, ",@(r0,gbr)"); break;
        case 0xe: put_s(&b, "xor.b #0x"); put_hex(&b, d8, 0); put_s(&b, ",@(r0,gbr)"); break;
        case 0xf: put_s(&b, "or.b #0x"); put_hex(&b, d8, 0); put_s(&b, ",@(r0,gbr)"); break;
        }
        break;

    case 0xd: {
        uint32_t ea = (pc & ~3u) + 4 + (uint32_t)d8 * 4;
        put_s(&b, "mov.l "); put_disp_name(&b, d8 * 4, "pc"); put_c(&b, ','); put_reg(&b, n);
        I->kind = SH4_K_LITERAL; I->reg = n; I->lit_addr = ea; I->lit_size = 4;
        if (read && read(ctx, ea, 4, &I->lit_val)) I->lit_ok = 1;
        break;
    }
    case 0xe: put_s(&b, "mov #"); put_dec(&b, simm8); put_c(&b, ','); put_reg(&b, n); break;

    case 0xf:
        /* FPU. FPSCR.SZ/PR are unknown statically: single-precision names, as objdump. */
        switch (d4) {
        case 0x0: put_s(&b, "fadd "); put_freg(&b, "fr", m); put_c(&b, ','); put_freg(&b, "fr", n); break;
        case 0x1: put_s(&b, "fsub "); put_freg(&b, "fr", m); put_c(&b, ','); put_freg(&b, "fr", n); break;
        case 0x2: put_s(&b, "fmul "); put_freg(&b, "fr", m); put_c(&b, ','); put_freg(&b, "fr", n); break;
        case 0x3: put_s(&b, "fdiv "); put_freg(&b, "fr", m); put_c(&b, ','); put_freg(&b, "fr", n); break;
        case 0x4: put_s(&b, "fcmp/eq "); put_freg(&b, "fr", m); put_c(&b, ','); put_freg(&b, "fr", n); break;
        case 0x5: put_s(&b, "fcmp/gt "); put_freg(&b, "fr", m); put_c(&b, ','); put_freg(&b, "fr", n); break;
        case 0x6: put_s(&b, "fmov.s @(r0,"); put_reg(&b, m); put_s(&b, "),"); put_freg(&b, "fr", n); break;
        case 0x7: put_s(&b, "fmov.s "); put_freg(&b, "fr", m); put_s(&b, ",@(r0,"); put_reg(&b, n); put_c(&b, ')'); break;
        case 0x8: put_s(&b, "fmov.s @"); put_reg(&b, m); put_c(&b, ','); put_freg(&b, "fr", n); break;
        case 0x9: put_s(&b, "fmov.s @"); put_reg(&b, m); put_s(&b, "+,"); put_freg(&b, "fr", n); break;
        case 0xa: put_s(&b, "fmov.s "); put_freg(&b, "fr", m); put_s(&b, ",@"); put_reg(&b, n); break;
        case 0xb: put_s(&b, "fmov.s "); put_freg(&b, "fr", m); put_s(&b, ",@-"); put_reg(&b, n); break;
        case 0xc: put_s(&b, "fmov "); put_freg(&b, "fr", m); put_c(&b, ','); put_freg(&b, "fr", n); break;
        case 0xe: put_s(&b, "fmac fr0,"); put_freg(&b, "fr", m); put_c(&b, ','); put_freg(&b, "fr", n); break;
        case 0xd:
            switch (m) {
            case 0x0: put_s(&b, "fsts fpul,"); put_freg(&b, "fr", n); break;
            case 0x1: put_s(&b, "flds "); put_freg(&b, "fr", n); put_s(&b, ",fpul"); break;
            case 0x2: put_s(&b, "float fpul,"); put_freg(&b, "fr", n); break;
            case 0x3: put_s(&b, "ftrc "); put_freg(&b, "fr", n); put_s(&b, ",fpul"); break;
            case 0x4: put_s(&b, "fneg "); put_freg(&b, "fr", n); break;
            case 0x5: put_s(&b, "fabs "); put_freg(&b, "fr", n); break;
            case 0x6: put_s(&b, "fsqrt "); put_freg(&b, "fr", n); break;
            case 0x7: put_s(&b, "fsrra "); put_freg(&b, "fr", n); break;
            case 0x8: put_s(&b, "fldi0 "); put_freg(&b, "fr", n); break;
            case 0x9: put_s(&b, "fldi1 "); put_freg(&b, "fr", n); break;
            case 0xa: if (n & 1) goto bad; put_s(&b, "fcnvsd fpul,"); put_freg(&b, "dr", n); break;
            case 0xb: if (n & 1) goto bad; put_s(&b, "fcnvds "); put_freg(&b, "dr", n); put_s(&b, ",fpul"); break;
            case 0xe: put_s(&b, "fipr fv"); put_dec(&b, (n & 3) * 4); put_s(&b, ",fv"); put_dec(&b, (n >> 2) * 4); break;
            case 0xf:
                if ((n & 3) == 1) { put_s(&b, "ftrv xmtrx,fv"); put_dec(&b, (n >> 2) * 4); break; }
                if (x == 0xfbfd) { put_s(&b, "frchg"); break; }
                if (x == 0xf3fd) { put_s(&b, "fschg"); break; }
                if (x == 0xf7fd) { put_s(&b, "fpchg"); break; }
                if ((n & 1) == 0) { put_s(&b, "fsca fpul,"); put_freg(&b, "dr", n); break; }
                goto bad;
            default: goto bad;
            }
            break;
        default: goto bad;
        }
        break;
    }
    goto done;
bad:
    b.p = b.start; word(&b, x);
done:
    *b.p = 0;
    return (int)(b.p - b.start);
}
