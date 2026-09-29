/* analysis — whole-source passes: function detection and cross-references.
 *
 * Functions are found by recursive traversal from the entry points (the add-in's entry, or the
 * OS reset vector + every handler in its syscall table): follow fall-through, bt/bf/bra and gcc
 * jump tables (mova + mov.w @(r0,Rn) + braf) until rts / jmp, looking only at real code; bsr
 * targets and jsr/jmp @Rn targets whose Rn was loaded from a literal pool become functions.
 * Then two rules for code nothing calls directly: right after a function's literal pool and
 * padding, a prologue (a register push or sts.l pr within the first 3 instructions) starts a
 * function; and an address held in a literal pool or an aligned data word starts one if it has
 * that prologue, or if it sits right after a function and explores cleanly (every path returns,
 * no erased or zero words). Host prototype + scores: tools/proto_funcs.py. */
#ifndef ANALYSIS_H
#define ANALYSIS_H
#include <stdint.h>
#include "source.h"

typedef struct {
    uint32_t addr;   /* first instruction */
    uint16_t size;   /* code bytes (end of the last reachable instruction), capped at 0xffff */
    uint16_t pool;   /* bytes up to the end of its literal pool / jump tables (>= size) */
} an_func_t;

/* Progress callback: [what] is the phase, [done]/[total] its progress (total 0 = unknown).
 * Return non-zero to stop; the functions found so far are kept. */
typedef int (*an_progress_fn)(const char *what, uint32_t done, uint32_t total);

/* Find the functions of [s]. 0 = done, 1 = stopped by the user, -1 = out of memory (the table
 * holds what fitted). Replaces any previous result. */
int  an_funcs_build(src_t *s, an_progress_fn prog);
void an_funcs_free(void);
int  an_funcs_ready(void);                 /* a result exists (complete or not) */
int  an_funcs_partial(void);               /* stopped early or out of memory */
int  an_funcs_count(void);
const an_func_t *an_func(int i);
int  an_func_at(uint32_t addr);            /* index of the function starting at addr, or -1 */
int  an_func_containing(uint32_t addr);    /* index of the function whose code holds addr, or -1 */
int  an_func_before(uint32_t addr);        /* index of the last function starting at or before addr */

/* Cross-references to [target]: every instruction or data word in [s] that refers to it. */
enum an_xkind {
    AN_X_CALL,    /* bsr, or jsr @Rn with Rn loaded with the target */
    AN_X_JUMP,    /* bra, or jmp @Rn (a tail call) */
    AN_X_BRANCH,  /* bt bf bt/s bf/s */
    AN_X_LOAD,    /* mov.l @(disp,pc),Rn loading the target (a pointer used otherwise) */
    AN_X_MOVA,    /* mova pointing at it */
    AN_X_DATA,    /* an aligned data word outside code holding it */
};
typedef struct { uint32_t from; uint8_t kind; } an_xref_t;

/* Fills out[0..max) sorted by address; returns the count (> max = truncated, only max filled),
 * or -1 if stopped. Needs the functions (builds them first if missing). */
int an_xrefs(src_t *s, uint32_t target, an_xref_t *out, int max, an_progress_fn prog);

#endif
