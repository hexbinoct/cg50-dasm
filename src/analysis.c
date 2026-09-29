#include "analysis.h"
#include "names.h"
#include "sh4dec.h"
#include <stdlib.h>
#include <string.h>

#define OS_CODE 0x00C00000u   /* the OS image in the flash; storage memory follows */

/* ---------- reading: a one-page window over the source ---------- */

static src_t *S_;
static uint32_t lo, hi_end;            /* the code region */
static uint32_t pg_base = 1;
static const uint8_t *pg;

/* Any miss replaces the window: for a file the page is a cache slot the source may reuse. */
static const uint8_t *win(uint32_t a) {
    uint32_t p = a & ~(SRC_PAGE_SIZE - 1);
    if (p != pg_base) { pg = src_page(S_, a); pg_base = pg ? p : 1; }
    return pg ? pg + (a - p) : 0;
}
static int rd8(uint32_t a, uint32_t *v) {
    if (a < lo || a >= hi_end) return 0;
    const uint8_t *q = win(a); if (!q) return 0;
    *v = q[0]; return 1;
}
static int rd16(uint32_t a, uint32_t *v) {
    if (a < lo || a + 2 > hi_end || (a & 1)) return 0;
    const uint8_t *q = win(a); if (!q) return 0;
    *v = (uint32_t)q[0] << 8 | q[1]; return 1;
}
static int rd32(uint32_t a, uint32_t *v) {
    if (a < lo || a + 4 > hi_end || (a & 3)) return 0;
    const uint8_t *q = win(a); if (!q) return 0;
    *v = (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3]; return 1;
}
static inline int in_code(uint32_t a) { return a >= lo && a < hi_end && !(a & 1); }
static inline int32_t s8(uint32_t x) { return (int8_t)x; }
static inline int32_t s12(uint32_t x) { return (x & 0x800) ? (int32_t)x - 0x1000 : (int32_t)x; }

/* ---------- growable address lists ---------- */

typedef struct { uint32_t *v; int n, cap; } vec_t;
static int oom;

static void vsortuniq(vec_t *x);
static void vpush(vec_t *x, uint32_t a) {
    if (x->n == x->cap && x->n >= 1024) vsortuniq(x);     /* many call sites, few targets */
    if (x->n == x->cap) {
        int nc = x->cap ? x->cap * 2 : 256;
        uint32_t *p = realloc(x->v, (size_t)nc * sizeof *p);
        if (!p) { oom = 1; return; }
        x->v = p; x->cap = nc;
    }
    x->v[x->n++] = a;
}
static void vfree(vec_t *x) { free(x->v); x->v = 0; x->n = x->cap = 0; }
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static void vsortuniq(vec_t *x) {
    if (x->n < 2) return;
    qsort(x->v, (size_t)x->n, sizeof x->v[0], cmp_u32);
    int k = 1;
    for (int i = 1; i < x->n; i++) if (x->v[i] != x->v[k - 1]) x->v[k++] = x->v[i];
    x->n = k;
}

/* ---------- the function table ---------- */

static an_func_t *F;
static int nF, capF, nSorted, g_ready, g_partial;

static int cmp_func(const void *a, const void *b) {
    uint32_t x = ((const an_func_t *)a)->addr, y = ((const an_func_t *)b)->addr;
    return x < y ? -1 : x > y;
}
/* largest i < nSorted with F[i].addr <= a, or -1 */
static int idx_le(uint32_t a) {
    int lo_ = 0, hi_ = nSorted - 1, r = -1;
    while (lo_ <= hi_) {
        int mid = (lo_ + hi_) >> 1;
        if (F[mid].addr <= a) { r = mid; lo_ = mid + 1; } else hi_ = mid - 1;
    }
    return r;
}
int an_func_at(uint32_t a) { int i = idx_le(a); return (i >= 0 && F[i].addr == a) ? i : -1; }
int an_func_containing(uint32_t a) {
    int i = idx_le(a);
    return (i >= 0 && a < F[i].addr + F[i].size) ? i : -1;
}
int an_func_before(uint32_t a) { return idx_le(a); }
int an_funcs_ready(void) { return g_ready; }
int an_funcs_partial(void) { return g_partial; }
int an_funcs_count(void) { return nF; }
const an_func_t *an_func(int i) { return (i >= 0 && i < nF) ? &F[i] : 0; }

void an_funcs_free(void) {
    free(F); F = 0; nF = capF = nSorted = 0; g_ready = g_partial = 0;
}

/* The table is sized once from the region (the heap is split in two gint arenas that also hold
 * the VRAMs, so doubling a large block fails long before memory runs out), then grown by half. */
static void alloc_funcs(uint32_t region) {
    int est = (int)(region / 256);
    if (est < 256) est = 256;
    if (est > 24000) est = 24000;
    while (est >= 256 && !(F = malloc((size_t)est * sizeof *F))) est /= 2;
    capF = F ? est : 0;
}

static void add_func(uint32_t a, uint32_t code_end, uint32_t pool_end) {
    if (nF == capF) {
        int nc = capF ? capF + capF / 2 : 256;
        an_func_t *p = realloc(F, (size_t)nc * sizeof *p);
        if (!p) { oom = 1; return; }
        F = p; capF = nc;
    }
    uint32_t sz = code_end - a, pl = pool_end - a;
    F[nF].addr = a;
    F[nF].size = (uint16_t)(sz > 0xffff ? 0xffff : sz);
    F[nF].pool = (uint16_t)(pl > 0xffff ? 0xffff : pl);
    nF++;
}

/* ---------- exploring one function ---------- */

typedef struct { uint32_t a; uint16_t mask; uint32_t v[16]; } path_t;   /* v[r]: literal in r */
#define MAXPATHS 64
static path_t paths[MAXPATHS];
static uint8_t vis[0x10000 / 16];      /* one bit per instruction over 64 KB from the start */
static vec_t found;                    /* call targets for the next round */

static void push_path(int *np, uint32_t a, const path_t *from) {
    if (*np >= MAXPATHS) return;
    path_t *q = &paths[(*np)++];
    q->a = a;
    q->mask = from ? from->mask : 0;
    if (from) memcpy(q->v, from->v, sizeof q->v);
}

static void found_call(uint32_t t) {
    if (in_code(t) && an_func_at(t) < 0) vpush(&found, t);
}

/* Explore the function at [start]. Returns 1 if clean (no path ran into erased/zero words, off
 * the code, or - when only checking a candidate, !collect - into a word that is no instruction); *code_end = end of the last reachable instruction, *pool_end = end of its
 * literal pools / jump tables. [collect]: record call targets in `found`. */
static int explore(uint32_t start, uint32_t *code_end, uint32_t *pool_end, int collect) {
    uint32_t hi = start + 2, pool = start, maxoff = 0;
    int clean = 1, np = 0;
    push_path(&np, start, 0);
    while (np) {
        path_t P = paths[--np];
        uint32_t a = P.a;
        uint8_t imm[16];
        uint16_t immmask = 0;
        uint32_t mova = 0;
        int tl_reg = -1, tl_size = 0, tl_scale = 1, bound = -1;   /* gcc jump table state */
        for (;;) {
            if (a < start) break;                 /* shared code before the entry: not ours */
            if (!in_code(a) || a - start >= 0x10000) { clean = 0; break; }
            uint32_t off = (a - start) >> 1, op, t;
            if (vis[off >> 3] & (1u << (off & 7))) break;
            vis[off >> 3] |= (uint8_t)(1u << (off & 7));
            if (off > maxoff) maxoff = off;
            if (!rd16(a, &op) || op == 0xFFFF || op == 0x0000) { clean = 0; break; }
            if (!collect) {                              /* checking a candidate: real opcodes only */
                char txt[8];
                sh4_decode((uint16_t)op, a, 0, 0, 0, txt, sizeof txt);
                if (txt[0] == '.') { clean = 0; break; }
            }
            if (a + 2 > hi) hi = a + 2;
            int n = (op >> 8) & 15, m = (op >> 4) & 15, h = op >> 12;
            int stop = 0, call = 0;
            if ((op & 0xF000) == 0xD000) {                       /* mov.l @(disp,pc),Rn */
                uint32_t la = (a & ~3u) + 4 + (op & 0xFF) * 4, v;
                if (la + 4 > pool) pool = la + 4;
                if (rd32(la, &v)) { P.v[n] = v; P.mask |= (uint16_t)(1u << n); }
                else P.mask &= (uint16_t)~(1u << n);
                a += 2;
                continue;
            }
            if (h == 0x9) {                                       /* mov.w @(disp,pc),Rn */
                uint32_t la = a + 4 + (op & 0xFF) * 2;
                if (la + 2 > pool) pool = la + 2;
            } else if (h == 0xB) {                                /* bsr */
                t = a + 4 + (uint32_t)(s12(op & 0xFFF) * 2);
                if (collect) found_call(t);
                call = 1;
            } else if (h == 0xA) {                                /* bra */
                t = a + 4 + (uint32_t)(s12(op & 0xFFF) * 2);
                if (in_code(t)) push_path(&np, t, &P);
                stop = 1;
            } else if ((op & 0xF900) == 0x8900) {                 /* bt bf bt/s bf/s */
                t = a + 4 + (uint32_t)(s8(op & 0xFF) * 2);
                if (in_code(t)) push_path(&np, t, &P);
            } else if ((op & 0xF0FF) == 0x400B || (op & 0xF0FF) == 0x402B) {   /* jsr / jmp @Rn */
                if (collect && ((P.mask >> n) & 1)) found_call(P.v[n]);
                if ((op & 0xF0FF) == 0x402B) stop = 1; else call = 1;
            } else if ((op & 0xF0FF) == 0x0003) {                 /* bsrf Rn */
                call = 1;
            } else if ((op & 0xF0FF) == 0x0023) {                 /* braf Rn: a gcc jump table? */
                if (mova && tl_reg == n && bound >= 0 && bound < 256) {
                    for (int k = 0; k <= bound; k++) {
                        uint32_t e, ea = mova + (uint32_t)(k * tl_size);
                        int32_t d;
                        if (tl_size == 2) { if (!rd16(ea, &e)) break; d = (int16_t)e; }
                        else { if (!rd8(ea, &e)) break; d = (int8_t)e; }
                        t = a + 4 + (uint32_t)(d * tl_scale);
                        if (in_code(t)) push_path(&np, t, &P);
                    }
                    uint32_t te = mova + (uint32_t)((bound + 1) * tl_size);
                    if (te > pool) pool = te;
                }
                stop = 1;
            } else if (op == 0x000B || op == 0x002B) {            /* rts rte */
                stop = 1;
            } else if ((op & 0xFF00) == 0xC700) {                 /* mova @(disp,pc),r0 */
                mova = (a & ~3u) + 4 + (op & 0xFF) * 4;
            } else if (h == 0xE) {                                /* mov #imm,Rn */
                imm[n] = (uint8_t)op; immmask |= (uint16_t)(1u << n);
            } else if ((op & 0xF00F) == 0x3006) {                 /* cmp/hi Rm,Rn: index > max? */
                bound = ((immmask >> m) & 1) ? imm[m] : -1;
            } else if ((op & 0xF00E) == 0x000C) {                 /* mov.b/.w @(r0,Rm),Rn */
                tl_reg = n; tl_size = (op & 1) ? 2 : 1; tl_scale = 1;
            } else if ((op & 0xF00F) == 0x300C && n == m && n == tl_reg) {   /* add Rn,Rn */
                tl_scale = 2;
            }
            /* forget a literal when its register is written (the common writer forms) */
            if (h == 5 || h == 6 || h == 7 || h == 9 || h == 0xE ||
                (h == 4 && (op & 0xFF) != 0x0B && (op & 0xFF) != 0x2B && (op & 0xFF) != 0x22 &&
                 (op & 0xFF) != 0x12 && (op & 0xFF) != 0x02 && (op & 0xFF) != 0x13))
                P.mask &= (uint16_t)~(1u << n);
            if (call) P.mask &= 0xFF00;                           /* calls clobber r0-r7 */
            if (stop || call) {                                   /* the delay slot is ours too */
                uint32_t o2 = off + 1;
                if (o2 < 0x8000) { vis[o2 >> 3] |= (uint8_t)(1u << (o2 & 7)); if (o2 > maxoff) maxoff = o2; }
                if (a + 4 > hi) hi = a + 4;
                if (stop) break;
                a += 4;
                continue;
            }
            a += 2;
        }
    }
    memset(vis, 0, (maxoff >> 3) + 1);
    *code_end = hi;
    *pool_end = pool > hi ? pool : hi;
    return clean;
}

/* a register push (mov.l Rm,@-r15) or sts.l pr,@-r15 within the first 3 instructions, all of
 * them real instructions */
static int prologue(uint32_t a) {
    for (int k = 0; k < 3; k++) {
        uint32_t op;
        char txt[8];
        if (!rd16(a + 2u * (uint32_t)k, &op)) return 0;
        if ((op & 0xFF0F) == 0x2F06 || op == 0x4F22) return 1;
        if (op == 0x000B || op == 0xFFFF || op == 0x0000) return 0;
        sh4_decode((uint16_t)op, a, 0, 0, 0, txt, sizeof txt);
        if (txt[0] == '.') return 0;
    }
    return 0;
}

/* ---------- building ---------- */

static int find_u32(const vec_t *x, uint32_t v) {           /* x sorted, bit 0 = a mark */
    int lo_ = 0, hi_ = x->n - 1;
    while (lo_ <= hi_) {
        int mid = (lo_ + hi_) >> 1;
        uint32_t y = x->v[mid] & ~1u;
        if (y == v) return mid;
        if (y < v) lo_ = mid + 1; else hi_ = mid - 1;
    }
    return -1;
}

int an_funcs_build(src_t *s, an_progress_fn prog) {
    an_funcs_free();
    S_ = s; pg_base = 1; oom = 0;
    lo = s->entry;
    hi_end = s->base + s->size;
    if (s->is_rom && hi_end - lo > OS_CODE) hi_end = lo + OS_CODE;
    vec_t todo = {0}, gapc = {0};
    found.n = 0;
    alloc_funcs(hi_end - lo);
    int stopped = 0, sweeps = 0;

    vpush(&todo, lo);
    uint32_t table = names_table_base(), h;
    if (s->is_rom && table)                                  /* every syscall handler */
        for (uint32_t i = 0; i < 0x2000; i++)
            if (rd32(table + 4 * i, &h) && in_code(h)) vpush(&todo, h);

    for (;;) {
        /* 1. traverse from the pending starts, round by round, in address order */
        while (todo.n && !oom && !stopped) {
            vsortuniq(&todo);
            for (int i = 0; i < todo.n && !oom; i++) {
                uint32_t t = todo.v[i], ce, pe;
                if (an_func_at(t) >= 0) continue;
                explore(t, &ce, &pe, 1);
                add_func(t, ce, pe);
                if (prog && (nF & 63) == 0 && prog("functions", (uint32_t)nF, 0)) { stopped = 1; break; }
            }
            qsort(F, (size_t)nF, sizeof F[0], cmp_func);
            nSorted = nF;
            vec_t tmp = todo; todo = found; found = tmp; found.n = 0;
        }
        if (oom || stopped) break;

        /* 2. right after a function's pool and padding: a prologue starts another function;
         *    without one it is a candidate that a pointer can confirm (step 3) */
        gapc.n = 0;
        for (int i = 0; i < nF; i++) {
            uint32_t b = F[i].addr + F[i].pool, op;
            uint32_t next = i + 1 < nF ? F[i + 1].addr : hi_end;
            while (b < next && rd16(b, &op) && (op == 0x0009 || op == 0x0000)) b += 2;
            if (b >= next || an_func_at(b) >= 0) continue;
            if (prologue(b)) vpush(&todo, b); else vpush(&gapc, b);
        }
        if (todo.n) continue;
        if (sweeps++ >= 2) break;

        /* 3. aligned words outside code (literal pools, data) that point into code */
        int fi = 0;
        for (uint32_t a = (lo + 3) & ~3u; a + 4 <= hi_end && !oom; a += 4) {
            while (fi < nF && F[fi].addr + F[fi].size <= a) fi++;
            if (fi < nF && F[fi].addr <= a) { a = ((F[fi].addr + F[fi].size + 3) & ~3u) - 4; continue; }
            uint32_t v;
            if (!rd32(a, &v) || !in_code(v)) goto next_word;
            int g = find_u32(&gapc, v);
            if (g >= 0) gapc.v[g] |= 1;                          /* pointed to */
            else if (an_func_containing(v) < 0 && prologue(v)) vpush(&todo, v);
        next_word:
            if (prog && (a & 0xFFFF) == 0 && prog("pointers", a - lo, hi_end - lo)) { stopped = 1; break; }
        }
        if (stopped) break;
        for (int i = 0; i < gapc.n; i++) {
            uint32_t ce, pe;
            if ((gapc.v[i] & 1) && explore(gapc.v[i] & ~1u, &ce, &pe, 0)) vpush(&todo, gapc.v[i] & ~1u);
        }
        if (!todo.n) break;
    }
    vfree(&todo); vfree(&gapc); vfree(&found);
    qsort(F, (size_t)nF, sizeof F[0], cmp_func);
    nSorted = nF;
    g_ready = 1;
    g_partial = stopped || oom;
    return oom ? -1 : stopped;
}

/* ---------- cross-references ---------- */

static int cmp_xref(const void *a, const void *b) {
    uint32_t x = ((const an_xref_t *)a)->from, y = ((const an_xref_t *)b)->from;
    return x < y ? -1 : x > y;
}

int an_xrefs(src_t *s, uint32_t target, an_xref_t *out, int max, an_progress_fn prog) {
    if (!g_ready || S_ != s) { if (an_funcs_build(s, prog) > 0) return -1; }
    S_ = s; pg_base = 1;
    int cnt = 0;
    uint32_t pools[64];                 /* pool words already reported through their load */
    int npools = 0;
#define ADD(f, k) do { if (cnt < max) { out[cnt].from = (f); out[cnt].kind = (uint8_t)(k); } cnt++; } while (0)

    /* instructions: only inside detected code */
    for (int i = 0; i < nF; i++) {
        uint32_t a1 = F[i].addr + F[i].size, lv[16], lpc[16], op, t, v;
        uint16_t lmask = 0;
        for (uint32_t a = F[i].addr; a < a1 && rd16(a, &op); a += 2) {
            int n = (op >> 8) & 15, h = op >> 12, call = 0, end = 0;
            if (h == 0xD) {                                       /* mov.l @(disp,pc),Rn */
                uint32_t la = (a & ~3u) + 4 + (op & 0xFF) * 4;
                if (rd32(la, &v)) {
                    lv[n] = v; lpc[n] = a; lmask |= (uint16_t)(1u << n);
                    if (v == target) { ADD(a, AN_X_LOAD); if (npools < 64) pools[npools++] = la; }
                } else lmask &= (uint16_t)~(1u << n);
                continue;
            }
            if (h == 0xB || h == 0xA) {                           /* bsr / bra */
                t = a + 4 + (uint32_t)(s12(op & 0xFFF) * 2);
                if (t == target) ADD(a, h == 0xB ? AN_X_CALL : AN_X_JUMP);
                if (h == 0xB) call = 1; else end = 1;
            } else if ((op & 0xF900) == 0x8900) {                 /* bt bf bt/s bf/s */
                t = a + 4 + (uint32_t)(s8(op & 0xFF) * 2);
                if (t == target) ADD(a, AN_X_BRANCH);
            } else if ((op & 0xFF00) == 0xC700) {                 /* mova */
                if ((a & ~3u) + 4 + (op & 0xFF) * 4 == target) ADD(a, AN_X_MOVA);
            } else if ((op & 0xF0FF) == 0x400B || (op & 0xF0FF) == 0x402B) {   /* jsr / jmp @Rn */
                int k = (op & 0xF0FF) == 0x400B ? AN_X_CALL : AN_X_JUMP;
                if (((lmask >> n) & 1) && lv[n] == target) {
                    /* report the call instead of the load that fed it */
                    if (cnt > 0 && cnt <= max && out[cnt - 1].from == lpc[n] && out[cnt - 1].kind == AN_X_LOAD) {
                        out[cnt - 1].from = a; out[cnt - 1].kind = (uint8_t)k;
                    } else ADD(a, k);
                }
                if (k == AN_X_CALL) call = 1; else end = 1;
            } else if (op == 0x000B || op == 0x002B || (op & 0xF0FF) == 0x0023) end = 1;
            if (h == 5 || h == 6 || h == 7 || h == 9 || h == 0xE ||
                (h == 4 && (op & 0xFF) != 0x0B && (op & 0xFF) != 0x2B && (op & 0xFF) != 0x22 &&
                 (op & 0xFF) != 0x12 && (op & 0xFF) != 0x02 && (op & 0xFF) != 0x13))
                lmask &= (uint16_t)~(1u << n);
            if (call) lmask &= 0xFF00;
            if (end) lmask = 0;
        }
        if (prog && (i & 255) == 255 && prog("references", (uint32_t)i, (uint32_t)nF)) return -1;
    }
    /* data: aligned words outside code that hold the target */
    int fi = 0;
    for (uint32_t a = (lo + 3) & ~3u, v; a + 4 <= hi_end; a += 4) {
        while (fi < nF && F[fi].addr + F[fi].size <= a) fi++;
        if (fi < nF && F[fi].addr <= a) { a = ((F[fi].addr + F[fi].size + 3) & ~3u) - 4; continue; }
        if (rd32(a, &v) && v == target) {
            int seen = 0;
            for (int k = 0; k < npools; k++) if (pools[k] == a) { seen = 1; break; }
            if (!seen) ADD(a, AN_X_DATA);
        }
        if (prog && (a & 0xFFFF) == 0 && prog("references", a - lo, hi_end - lo)) return -1;
    }
#undef ADD
    qsort(out, (size_t)(cnt < max ? cnt : max), sizeof out[0], cmp_xref);
    return cnt;
}
