/* source — the bytes being browsed: a .g3a in storage memory (read through a
 * page cache with BFile, in the OS world) or the OS ROM mapped at 0x80000000. */
#ifndef SOURCE_H
#define SOURCE_H
#include <stdint.h>

#define SRC_PAGE_SHIFT 12
#define SRC_PAGE_SIZE  (1u << SRC_PAGE_SHIFT)
#define SRC_MAX_PAGES  64          /* 256 KB cache */

typedef struct {
    int      is_rom;
    char     name[32];             /* display name */
    uint16_t path[64];             /* FONTCHARACTER path for BFile (files) */
    uint32_t size;                 /* bytes in the source */
    uint32_t base;                 /* address of byte 0 */
    uint32_t entry;                /* where the listing opens */
    const uint8_t *rom;            /* is_rom: direct pointer */
    /* page cache (files) */
    uint8_t *cache;                /* npages * SRC_PAGE_SIZE */
    int32_t  tag[SRC_MAX_PAGES];   /* page number held by slot, -1 = empty */
    uint32_t stamp[SRC_MAX_PAGES]; /* LRU */
    uint32_t clock;
    int      npages;
    int      last_err;             /* last BFile error */
    int      last_rc;              /* last BFile_Read return value (debug) */
    int      last_fd;              /* last BFile_Open return value (debug) */
} src_t;

/* Directory listing of \\fls0\*.g3a */
typedef struct {
    char     name[32];
    uint16_t path[64];
    uint32_t size;
} src_entry_t;

int  src_scan_g3a(src_entry_t *out, int max);          /* returns count (>=0) or -err */
int  src_open_file(src_t *s, const src_entry_t *e);    /* 0 ok, <0 BFile error */
void src_open_rom(src_t *s);
void src_close(src_t *s);

/* Read [size] bytes (1,2,4) big-endian at [addr]; 1 if inside the source. */
int  src_read(src_t *s, uint32_t addr, int size, uint32_t *out);
/* Copy up to [n] bytes at [addr] into buf; returns bytes copied (0 outside). */
int  src_bytes(src_t *s, uint32_t addr, uint8_t *buf, int n);
static inline int src_contains(const src_t *s, uint32_t addr) {
    return addr >= s->base && addr - s->base < s->size;
}

#endif
