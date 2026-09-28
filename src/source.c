#include "source.h"
#include <gint/gint.h>
#include <gint/bfile.h>
#include <stdlib.h>
#include <string.h>

#define G3A_HEADER   0x7000
#define ADDIN_CODE   0x00300000u   /* the OS maps the code image here */
#define ROM_BASE     0x80000000u
#define ROM_SIZE     0x01000000u   /* whole 16 MB NOR flash (OS + storage) */

/* ---------- OS-world helpers (BFile only works after a world switch) ---------- */

static const uint16_t PATTERN[] = u"\\\\fls0\\*.g3a";
static const uint16_t PREFIX[]  = u"\\\\fls0\\";

static void u16_to_ascii(const uint16_t *s, char *out, int cap) {
    int i = 0;
    while (s[i] && i < cap - 1) { out[i] = (s[i] < 0x80) ? (char)s[i] : '?'; i++; }
    out[i] = 0;
}

struct scan_args { src_entry_t *out; int max; int count; };

static void scan_worker(struct scan_args *a) {
    int sh = 0;
    uint16_t found[64];
    struct BFile_FileInfo fi;
    a->count = 0;
    int rc = BFile_FindFirst(PATTERN, &sh, found, &fi);
    if (rc < 0) { a->count = (rc == BFile_EntryNotFound) ? 0 : rc; return; }
    while (rc == 0 && a->count < a->max) {
        /* Fugue returns the bare file name; strip any directory part anyway */
        const uint16_t *nm = found;
        for (const uint16_t *p = found; *p; p++) if (*p == '\\') nm = p + 1;
        src_entry_t *e = &a->out[a->count];
        u16_to_ascii(nm, e->name, sizeof e->name);
        int n = 0;
        while (PREFIX[n]) { e->path[n] = PREFIX[n]; n++; }
        for (int i = 0; nm[i] && n < 63; i++) e->path[n++] = nm[i];
        e->path[n] = 0;
        e->size = fi.file_size;
        a->count++;
        rc = BFile_FindNext(sh, found, &fi);
    }
    BFile_FindClose(sh);
}

int src_scan_g3a(src_entry_t *out, int max) {
    struct scan_args a = { out, max, 0 };
    gint_world_switch(GINT_CALL(scan_worker, (void *)&a));
    return a.count;
}

struct read_args { const uint16_t *path; uint32_t off; void *buf; int n; int rc; int size; int fd; };

static void read_worker(struct read_args *r) {
    int fd = BFile_Open(r->path, BFile_ReadOnly);
    r->fd = fd;
    if (fd < 0) { r->rc = fd; return; }
    r->size = BFile_Size(fd);
    if (r->n > 0) {
        r->rc = BFile_Seek(fd, (int)r->off);
        if (r->rc >= 0) r->rc = BFile_Read(fd, r->buf, r->n, -1);
    } else r->rc = 0;
    BFile_Close(fd);
}

static int os_read(src_t *s, const uint16_t *path, uint32_t off, void *buf, int n, int *size) {
    struct read_args r = { path, off, buf, n, 0, 0, 0 };
    gint_world_switch(GINT_CALL(read_worker, (void *)&r));
    if (size) *size = r.size;
    if (s) { s->last_rc = r.rc; s->last_fd = r.fd; }
    return r.rc;
}

/* ---------- source ---------- */

static void cache_reset(src_t *s) {
    for (int i = 0; i < SRC_MAX_PAGES; i++) { s->tag[i] = -1; s->stamp[i] = 0; }
    s->clock = 0;
}

int src_open_file(src_t *s, const src_entry_t *e) {
    memset(s, 0, sizeof *s);
    strncpy(s->name, e->name, sizeof s->name - 1);
    memcpy(s->path, e->path, sizeof s->path);
    int size = 0;
    int rc = os_read(s, e->path, 0, 0, 0, &size);
    if (rc < 0) { s->last_err = rc; return rc; }
    if (size <= 0) size = (int)e->size;
    s->size = (uint32_t)size;
    s->base = ADDIN_CODE - G3A_HEADER;       /* header at base, code at 0x00300000 */
    s->entry = ADDIN_CODE;
    uint32_t want = (s->size + SRC_PAGE_SIZE - 1) >> SRC_PAGE_SHIFT;
    s->npages = want > SRC_MAX_PAGES ? SRC_MAX_PAGES : (int)want;
    if (s->npages < 1) s->npages = 1;
    while (s->npages > 1 && !(s->cache = malloc((size_t)s->npages * SRC_PAGE_SIZE))) s->npages /= 2;
    if (!s->cache) s->cache = malloc(SRC_PAGE_SIZE);
    if (!s->cache) { s->last_err = -99; return -99; }
    cache_reset(s);
    return 0;
}

void src_open_rom(src_t *s) {
    memset(s, 0, sizeof *s);
    s->is_rom = 1;
    strcpy(s->name, "OS ROM");
    s->rom = (const uint8_t *)ROM_BASE;
    s->base = ROM_BASE;
    s->size = ROM_SIZE;
    s->entry = ROM_BASE;
}

void src_close(src_t *s) {
    if (s->cache) free(s->cache);
    s->cache = 0;
}

/* Returns a pointer to the page holding [off] (file offset), loading it if needed. */
static const uint8_t *page_for(src_t *s, uint32_t off) {
    int32_t pg = (int32_t)(off >> SRC_PAGE_SHIFT);
    int victim = 0;
    for (int i = 0; i < s->npages; i++) {
        if (s->tag[i] == pg) { s->stamp[i] = ++s->clock; return s->cache + (uint32_t)i * SRC_PAGE_SIZE; }
        if (s->tag[i] == -1) { victim = i; break; }
        if (s->stamp[i] < s->stamp[victim]) victim = i;
    }
    uint8_t *dst = s->cache + (uint32_t)victim * SRC_PAGE_SIZE;
    uint32_t pos = (uint32_t)pg << SRC_PAGE_SHIFT;
    uint32_t n = s->size - pos;
    if (n > SRC_PAGE_SIZE) n = SRC_PAGE_SIZE;
    memset(dst, 0, SRC_PAGE_SIZE);
    int rc = os_read(s, s->path, pos, dst, (int)n, 0);
    if (rc < 0) { s->last_err = rc; return 0; }
    s->tag[victim] = pg; s->stamp[victim] = ++s->clock;
    return dst;
}

int src_bytes(src_t *s, uint32_t addr, uint8_t *buf, int n) {
    if (!src_contains(s, addr)) return 0;
    uint32_t off = addr - s->base;
    if (off + (uint32_t)n > s->size) n = (int)(s->size - off);
    if (s->is_rom) { memcpy(buf, s->rom + off, (size_t)n); return n; }
    int done = 0;
    while (done < n) {
        const uint8_t *pg = page_for(s, off + (uint32_t)done);
        if (!pg) break;
        uint32_t in = (off + (uint32_t)done) & (SRC_PAGE_SIZE - 1);
        int chunk = (int)(SRC_PAGE_SIZE - in);
        if (chunk > n - done) chunk = n - done;
        memcpy(buf + done, pg + in, (size_t)chunk);
        done += chunk;
    }
    return done;
}

int src_read(src_t *s, uint32_t addr, int size, uint32_t *out) {
    uint8_t b[4];
    if (src_bytes(s, addr, b, size) != size) return 0;
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v = (v << 8) | b[i];
    *out = v;
    return 1;
}
