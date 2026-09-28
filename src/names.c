#include "names.h"
#include "syscall_names.h"
#include <stdlib.h>

/* handler address -> index into syscall_names, sorted by address */
typedef struct { uint32_t addr; uint16_t idx; } hmap_t;
static hmap_t g_map[SYSCALL_NAME_COUNT];
static int g_count;
static uint32_t g_table;

static inline uint32_t rom32(uint32_t a) { return *(const volatile uint32_t *)a; }
static inline uint16_t rom16(uint32_t a) { return *(const volatile uint16_t *)a; }

static int cmp_hmap(const void *a, const void *b) {
    uint32_t x = ((const hmap_t *)a)->addr, y = ((const hmap_t *)b)->addr;
    return x < y ? -1 : x > y;
}

void names_init(void) {
    g_count = 0; g_table = 0;
    /* The trampoline at 0x80020070 starts with mov.l @(disp,pc),r2 = table base. */
    for (uint32_t a = SYSCALL_TRAMPOLINE; a < SYSCALL_TRAMPOLINE + 0x10; a += 2) {
        uint16_t op = rom16(a);
        if ((op & 0xFF00) == 0xD200) {
            uint32_t ea = (a & ~3u) + 4 + (op & 0xFF) * 4;
            uint32_t t = rom32(ea);
            if ((t >> 24) == 0x80 || (t >> 24) == 0xA0) g_table = t;
            break;
        }
    }
    if (!g_table) return;
    for (int i = 0; i < SYSCALL_NAME_COUNT; i++) {
        uint32_t h = rom32(g_table + (uint32_t)syscall_names[i].num * 4);
        if ((h >> 24) != 0x80 && (h >> 24) != 0xA0) continue;   /* not a ROM address */
        g_map[g_count].addr = h; g_map[g_count].idx = (uint16_t)i; g_count++;
    }
    qsort(g_map, (size_t)g_count, sizeof g_map[0], cmp_hmap);
}

uint32_t names_table_base(void) { return g_table; }
int names_count(void) { return g_count; }

const char *names_for_handler(uint32_t addr) {
    int lo = 0, hi = g_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (g_map[mid].addr == addr) {
            /* several numbers may share a handler: prefer the lowest index (first in file order) */
            while (mid > 0 && g_map[mid - 1].addr == addr) mid--;
            return syscall_names[g_map[mid].idx].name;
        }
        if (g_map[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

const char *names_for_syscall(uint32_t num) {
    int lo = 0, hi = SYSCALL_NAME_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (syscall_names[mid].num == num) return syscall_names[mid].name;
        if (syscall_names[mid].num < num) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int rd(void *ctx, uint32_t addr, int size, uint32_t *out) {
    return src_read((src_t *)ctx, addr, size, out);
}

/* Scan back from pc (exclusive) up to [max] instructions for the last write of
 * register r by a mov.l/mov.w @(disp,pc) or mov #imm; stop at a return or an
 * unconditional branch (a basic-block boundary, roughly). 1 = found -> *val. */
static int find_reg_value(src_t *s, uint32_t pc, int r, int max, uint32_t *val) {
    char txt[8];
    sh4_insn_t in;
    uint32_t a = pc;
    for (int i = 0; i < max; i++) {
        a -= 2;
        uint32_t op;
        if (!src_read(s, a, 2, &op)) return 0;
        sh4_decode((uint16_t)op, a, rd, s, &in, txt, sizeof txt);
        if (in.kind == SH4_K_LITERAL && in.reg == r) {
            if (!in.lit_ok) return 0;
            *val = in.lit_size == 2 ? (uint32_t)(int16_t)in.lit_val : in.lit_val;
            return 1;
        }
        if ((op & 0xF000) == 0xE000 && ((op >> 8) & 0xF) == (uint32_t)r) {   /* mov #imm,rN */
            *val = (uint32_t)(int32_t)(int8_t)(op & 0xFF);
            return 1;
        }
        /* other writers of rN we cannot follow: mov rM,rN / add / loads */
        if (((op & 0xF00F) == 0x6003 || (op & 0xF000) == 0x7000 || (op & 0xF000) == 0x5000 ||
             (op & 0xF00F) == 0x6002) && ((op >> 8) & 0xF) == (uint32_t)r)
            return 0;
        if (in.kind == SH4_K_RET) return 0;
        if (i > 0 && (in.kind == SH4_K_JUMP_REG || (in.kind == SH4_K_BRANCH && (op & 0xF000) == 0xA000)))
            return 0;
    }
    return 0;
}

int names_resolve_indirect(src_t *s, uint32_t pc, const sh4_insn_t *in,
                           uint32_t *target, const char **name) {
    *name = 0;
    if (in->kind != SH4_K_JUMP_REG && in->kind != SH4_K_CALL_REG) return 0;
    uint32_t v;
    /* the register may also be loaded in the delay slot */
    if (!find_reg_value(s, pc + 4, in->reg, 12, &v)) return 0;
    uint32_t op;
    if (src_read(s, pc, 2, &op) && ((op & 0xF0FF) == 0x0003 || (op & 0xF0FF) == 0x0023))
        v += pc + 4;                                     /* braf/bsrf: pc-relative */
    *target = v;
    if (v == SYSCALL_TRAMPOLINE) {
        uint32_t num;
        if (find_reg_value(s, pc + 4, 0, 12, &num)) {
            const char *nm = names_for_syscall(num & 0xFFFF);
            *name = nm;
            if (g_table) *target = rom32(g_table + (num & 0xFFFF) * 4);
            return 1;
        }
        *name = "syscall";
        return 1;
    }
    *name = names_for_handler(v);
    return 1;
}
