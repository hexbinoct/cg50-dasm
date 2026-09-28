/* names — symbolic names for addresses: OS syscall handlers (read from the
 * running OS's own syscall table, so it fits whatever OS version we run on) and
 * syscall-number trampolines used by add-ins (mov.l #num,r0 ; jmp @trampoline). */
#ifndef NAMES_H
#define NAMES_H
#include <stdint.h>
#include "source.h"
#include "sh4dec.h"

#define SYSCALL_TRAMPOLINE 0x80020070u

void        names_init(void);                       /* reads the OS syscall table */
uint32_t    names_table_base(void);                 /* 0 if not found */
int         names_count(void);
const char *names_for_handler(uint32_t addr);       /* exact handler address -> name or NULL */
const char *names_for_syscall(uint32_t num);        /* syscall number -> name or NULL */

/* Resolve where an indirect jump/call at [pc] (jsr/jmp @rN, braf/bsrf) goes by
 * scanning back for the mov.l @(disp,pc),rN that loaded rN. Returns 1 and sets
 * *target; *name is the symbolic name or NULL (a syscall trampoline is resolved
 * to the syscall's name when the number load is found too). */
int names_resolve_indirect(src_t *s, uint32_t pc, const sh4_insn_t *in,
                           uint32_t *target, const char **name);

#endif
