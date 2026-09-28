// dasm — on-device SH-4A disassembler / binary browser for the fx-CG50 (gint).
//
// Opens any .g3a from storage memory (\\fls0) or the OS ROM at 0x80000000 and shows a
// colour listing: address · hex · mnemonic · operands, PC-relative literals resolved,
// branch targets followable with EXE / back with EXIT, syscall handlers named from the
// running OS's own syscall table, plus hex / strings / header panes.
//
// Keys (listing):  UP/DOWN line · LEFT/RIGHT page · SHIFT+LEFT/RIGHT ±0x1000 · +/- shift by 1 byte
//                  EXE follow branch / literal · EXIT back (history) · F1 open · F2 goto (hex: digits,
//                  F1-F6 = A-F) · F3 hex view · F4 strings from here · F5 header · F6 OS ROM · MENU quit
#include <gint/display.h>
#include <gint/keyboard.h>
#include <gint/gint.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sh4dec.h"
#include "source.h"
#include "names.h"

extern font_t font_dasm;

#define CW      6           /* character advance of the 5x7 font */
#define COLS    64
#define ROW_H   8
#define LIST_Y  10
#define ROWS    24          /* 10 + 24*8 = 202; status bar at 207 */
#define STAT_Y  207

#define C_BG     C_RGB(1, 2, 3)
#define C_TITLE  C_RGB(4, 10, 18)
#define C_ADDR   C_RGB(14, 30, 16)
#define C_HEXW   C_RGB(10, 22, 12)
#define C_TEXT   C_RGB(28, 56, 28)
#define C_BR     C_RGB(31, 58, 10)
#define C_CALL   C_RGB(31, 40, 6)
#define C_RET    C_RGB(31, 24, 26)
#define C_LIT    C_RGB(10, 52, 14)
#define C_NAME   C_RGB(8, 54, 31)
#define C_SLOT   C_RGB(16, 38, 31)
#define C_CUR    C_RGB(3, 9, 16)
#define C_DATA   C_RGB(18, 30, 18)
#define C_STATUS C_RGB(3, 6, 10)
#define C_KEY    C_RGB(31, 60, 31)
#define C_DIM    C_RGB(12, 24, 12)
#define C_ASCII  C_RGB(24, 48, 20)

typedef enum { V_PICKER, V_LIST, V_HEX, V_STRINGS, V_HEADER } view_t;

static src_t S;
static int have_src;
static view_t view = V_PICKER;
static uint32_t top;                 /* first row address (listing / hex) */
static int cur;                      /* cursor row */
static struct { uint32_t top; int cur; } hist[32];
static int nhist;
static char msg[48];

static src_entry_t files[64];
static int nfiles = -1, fsel, ftop;

#define MAXSTR 512
static struct { uint32_t addr; uint16_t len; } strs[MAXSTR];
static int nstrs, ssel, stop_;
static uint32_t strs_from;

/* ------------------------------------------------------------------ drawing */

static void text(int col, int y, int color, const char *s) { dtext(col * CW, y, color, s); }

static void textn(int col, int y, int color, const char *s, int maxcols) {
    if (maxcols <= 0) return;
    dtext_opt(col * CW, y, color, C_NONE, DTEXT_LEFT, DTEXT_TOP, s, maxcols);
}

static void title_bar(const char *left, const char *right) {
    drect(0, 0, DWIDTH - 1, 8, C_TITLE);
    textn(0, 1, C_KEY, left, 40);
    if (right) { int n = (int)strlen(right); if (n > 63) n = 63; textn(COLS - n, 1, C_TEXT, right, n); }
}

static void status_bar(const char *labels[6]) {
    drect(0, STAT_Y - 1, DWIDTH - 1, DHEIGHT - 1, C_STATUS);
    for (int i = 0; i < 6; i++) {
        if (!labels[i]) continue;
        int x0 = i * 64;
        drect(x0 + 1, STAT_Y, x0 + 62, DHEIGHT - 1, C_RGB(6, 14, 24));
        int n = (int)strlen(labels[i]);
        dtext(x0 + 32 - n * CW / 2, STAT_Y + 1, C_KEY, labels[i]);
    }
    if (msg[0]) { drect(0, STAT_Y - 1, DWIDTH - 1, DHEIGHT - 1, C_STATUS); textn(0, STAT_Y + 1, C_BR, msg, 63); }
}

/* ------------------------------------------------------------------ helpers */

static int rd(void *ctx, uint32_t addr, int size, uint32_t *out) {
    return src_read((src_t *)ctx, addr, size, out);
}

static uint32_t clamp_addr(uint32_t a) {
    if (a < S.base) a = S.base;
    if (a > S.base + S.size - 2) a = S.base + S.size - 2;
    return a;
}

static void push_hist(void) {
    if (nhist < 32) { hist[nhist].top = top; hist[nhist].cur = cur; nhist++; }
    else { memmove(hist, hist + 1, sizeof hist[0] * 31); hist[31].top = top; hist[31].cur = cur; }
}

static void goto_addr(uint32_t a, int remember) {
    if (!src_contains(&S, a)) { snprintf(msg, sizeof msg, "0x%08lx is outside %s", (unsigned long)a, S.name); return; }
    if (remember) push_hist();
    top = clamp_addr(a & ~1u); cur = 0;
    if (view == V_HEX) top &= ~15u;
}

/* ------------------------------------------------------------------ listing */

static int kind_color(const sh4_insn_t *in, const char *txt) {
    if (txt[0] == '.') return C_DATA;
    switch (in->kind) {
    case SH4_K_BRANCH: return C_BR;
    case SH4_K_CALL: case SH4_K_CALL_REG: return C_CALL;
    case SH4_K_JUMP_REG: return C_BR;
    case SH4_K_RET: return C_RET;
    default: return C_TEXT;
    }
}

/* Build the comment for an instruction; returns colour, "" if none. */
static int comment_for(uint32_t pc, const sh4_insn_t *in, char *out, int cap) {
    out[0] = 0;
    const char *nm;
    uint32_t tgt;
    switch (in->kind) {
    case SH4_K_LITERAL:
        if (!in->lit_ok) { snprintf(out, cap, "=?"); return C_DIM; }
        if (in->lit_size == 4) {
            nm = names_for_handler(in->lit_val);
            if (nm) { snprintf(out, cap, "=0x%08lx %s", (unsigned long)in->lit_val, nm); return C_NAME; }
            snprintf(out, cap, "=0x%08lx", (unsigned long)in->lit_val);
            return C_LIT;
        }
        snprintf(out, cap, "=0x%04lx (%ld)", (unsigned long)in->lit_val, (long)(int16_t)in->lit_val);
        return C_LIT;
    case SH4_K_MOVA:
        snprintf(out, cap, "&0x%08lx", (unsigned long)in->lit_addr); return C_LIT;
    case SH4_K_JUMP_REG: case SH4_K_CALL_REG:
        if (names_resolve_indirect(&S, pc, in, &tgt, &nm)) {
            if (nm) snprintf(out, cap, "-> %s", nm);
            else snprintf(out, cap, "-> 0x%08lx", (unsigned long)tgt);
            return nm ? C_NAME : C_LIT;
        }
        return C_LIT;
    case SH4_K_BRANCH: case SH4_K_CALL:
        nm = names_for_handler(in->target);
        if (nm) { snprintf(out, cap, "%s", nm); return C_NAME; }
        return C_LIT;
    default: return C_LIT;
    }
}

static void draw_listing(void) {
    dclear(C_BG);
    char t[64], txt[48], com[48], right[24];
    if (S.is_rom) snprintf(right, sizeof right, "%d/%d", nhist, names_count());
    else snprintf(right, sizeof right, "fd%d rd%d %d/%d", S.last_fd, S.last_rc, nhist, names_count());
    snprintf(t, sizeof t, "%s  0x%08lx", S.name, (unsigned long)(top + 2u * (uint32_t)cur));
    title_bar(t, right);

    int prev_delay = 0;
    uint32_t op;
    sh4_insn_t in;
    if (src_read(&S, top - 2, 2, &op)) { sh4_decode((uint16_t)op, top - 2, rd, &S, &in, txt, sizeof txt); prev_delay = in.delay; }
    for (int i = 0; i < ROWS; i++) {
        uint32_t a = top + 2u * (uint32_t)i;
        int y = LIST_Y + i * ROW_H;
        if (i == cur) drect(0, y - 1, DWIDTH - 1, y + ROW_H - 2, C_CUR);
        if (!src_read(&S, a, 2, &op)) { prev_delay = 0; continue; }
        sh4_decode((uint16_t)op, a, rd, &S, &in, txt, sizeof txt);
        snprintf(t, sizeof t, "%08lx", (unsigned long)a); text(0, y, C_ADDR, t);
        snprintf(t, sizeof t, "%04lx", (unsigned long)op); text(9, y, C_HEXW, t);
        int col = 14 + (prev_delay ? 1 : 0);
        int color = prev_delay ? C_SLOT : kind_color(&in, txt);
        int n = (int)strlen(txt);
        textn(col, y, color, txt, COLS - col);
        int ccol = comment_for(a, &in, com, sizeof com);
        if (com[0]) {
            int cc = col + n + 2; if (cc < 42) cc = 42;
            if (cc < COLS - 2) { text(cc, y, C_DIM, ";"); textn(cc + 1, y, ccol, com, COLS - cc - 1); }
        }
        prev_delay = in.delay;
    }
    static const char *lab[6] = { "OPEN", "GOTO", "HEX", "STR", "HDR", "ROM" };
    status_bar(lab);
}

/* Follow the instruction under the cursor. */
static void follow(void) {
    uint32_t a = top + 2u * (uint32_t)cur, op, tgt;
    char txt[48]; sh4_insn_t in; const char *nm;
    if (!src_read(&S, a, 2, &op)) return;
    sh4_decode((uint16_t)op, a, rd, &S, &in, txt, sizeof txt);
    switch (in.kind) {
    case SH4_K_BRANCH: case SH4_K_CALL: goto_addr(in.target, 1); break;
    case SH4_K_JUMP_REG: case SH4_K_CALL_REG:
        if (names_resolve_indirect(&S, a, &in, &tgt, &nm)) {
            if (src_contains(&S, tgt)) goto_addr(tgt, 1);
            else snprintf(msg, sizeof msg, "-> 0x%08lx %s (outside %s)", (unsigned long)tgt, nm ? nm : "", S.name);
        } else snprintf(msg, sizeof msg, "target register not resolved");
        break;
    case SH4_K_LITERAL:
        if (in.lit_ok && in.lit_size == 4 && src_contains(&S, in.lit_val)) goto_addr(in.lit_val, 1);
        else if (in.lit_ok && in.lit_size == 4 && !S.is_rom && (in.lit_val >> 24) == 0x80)
            snprintf(msg, sizeof msg, "0x%08lx is in the OS ROM (F6)", (unsigned long)in.lit_val);
        else goto_addr(in.lit_addr, 1);
        break;
    case SH4_K_MOVA: goto_addr(in.lit_addr, 1); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ hex view */

static void draw_hex(void) {
    dclear(C_BG);
    char t[80], right[24];
    snprintf(right, sizeof right, "hex");
    snprintf(t, sizeof t, "%s  0x%08lx", S.name, (unsigned long)(top + 16u * (uint32_t)cur));
    title_bar(t, right);
    uint8_t b[16];
    for (int i = 0; i < ROWS; i++) {
        uint32_t a = top + 16u * (uint32_t)i;
        int y = LIST_Y + i * ROW_H;
        if (i == cur) drect(0, y - 1, DWIDTH - 1, y + ROW_H - 2, C_CUR);
        int n = src_bytes(&S, a, b, 16);
        if (n <= 0) continue;
        snprintf(t, sizeof t, "%08lx", (unsigned long)a); text(0, y, C_ADDR, t);
        char *p = t;
        for (int j = 0; j < 16; j++) {
            if (j < n) p += sprintf(p, "%02x", b[j]); else { *p++ = ' '; *p++ = ' '; }
            if ((j & 3) == 3) *p++ = ' ';
        }
        *p = 0;
        text(9, y, C_TEXT, t);
        for (int j = 0; j < 16; j++) t[j] = (j < n && b[j] >= 0x20 && b[j] < 0x7f) ? (char)b[j] : (j < n ? '.' : ' ');
        t[16] = 0;
        text(46, y, C_ASCII, t);
    }
    static const char *lab[6] = { "OPEN", "GOTO", "LIST", "STR", "HDR", "ROM" };
    status_bar(lab);
}

/* ------------------------------------------------------------------ strings */

static void scan_strings(uint32_t from) {
    nstrs = 0; ssel = 0; stop_ = 0; strs_from = from;
    uint8_t buf[1024];
    uint32_t a = from, run_start = 0; int run = 0;
    uint32_t end = S.base + S.size;
    while (a < end && nstrs < MAXSTR) {
        int n = src_bytes(&S, a, buf, (int)((end - a) < sizeof buf ? (end - a) : sizeof buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (c >= 0x20 && c < 0x7f) { if (!run) run_start = a + (uint32_t)i; run++; }
            else {
                if (run >= 6 && (c == 0 || c == '\n' || c == '\r')) {
                    strs[nstrs].addr = run_start; strs[nstrs].len = (uint16_t)(run > 255 ? 255 : run); nstrs++;
                    if (nstrs >= MAXSTR) break;
                }
                run = 0;
            }
        }
        a += (uint32_t)n;
    }
}

static void draw_strings(void) {
    dclear(C_BG);
    char t[80];
    snprintf(t, sizeof t, "%s  strings from 0x%08lx", S.name, (unsigned long)strs_from);
    snprintf(msg, sizeof msg, "%s", "");
    char right[24]; snprintf(right, sizeof right, "%d%s", nstrs, nstrs >= MAXSTR ? "+" : "");
    title_bar(t, right);
    if (ssel < stop_) stop_ = ssel;
    if (ssel >= stop_ + ROWS) stop_ = ssel - ROWS + 1;
    uint8_t b[52];
    for (int i = 0; i < ROWS && stop_ + i < nstrs; i++) {
        int k = stop_ + i, y = LIST_Y + i * ROW_H;
        if (k == ssel) drect(0, y - 1, DWIDTH - 1, y + ROW_H - 2, C_CUR);
        snprintf(t, sizeof t, "%08lx %3d", (unsigned long)strs[k].addr, strs[k].len); text(0, y, C_ADDR, t);
        int n = strs[k].len < 50 ? strs[k].len : 50;
        n = src_bytes(&S, strs[k].addr, b, n);
        for (int j = 0; j < n; j++) t[j] = (char)b[j];
        t[n] = 0;
        text(13, y, C_ASCII, t);
    }
    if (!nstrs) text(2, LIST_Y + 8, C_DIM, "no strings found");
    static const char *lab[6] = { "OPEN", "GOTO", "HEX", "LIST", "HDR", "ROM" };
    status_bar(lab);
}

/* ------------------------------------------------------------------ header */

static void hdr_str(uint32_t off, int len, char *out) {
    uint8_t b[64]; if (len > 63) len = 63;
    int n = src_bytes(&S, S.base + off, b, len);
    int i; for (i = 0; i < n && b[i]; i++) out[i] = (b[i] >= 0x20 && b[i] < 0x7f) ? (char)b[i] : '?';
    out[i] = 0;
}

static void draw_header(void) {
    dclear(C_BG);
    char t[80], s[64];
    snprintf(t, sizeof t, "%s  header", S.name);
    title_bar(t, 0);
    int y = LIST_Y + 2;
    if (S.is_rom) {
        text(1, y, C_TEXT, "OS ROM mapped at 0x80000000 (P1), 16 MB NOR flash"); y += ROW_H + 2;
        snprintf(t, sizeof t, "syscall table   0x%08lx", (unsigned long)names_table_base()); text(1, y, C_LIT, t); y += ROW_H;
        snprintf(t, sizeof t, "named handlers  %d (libfxcg numbering)", names_count()); text(1, y, C_LIT, t); y += ROW_H;
        uint32_t v;
        if (src_read(&S, 0x80000000, 4, &v)) { snprintf(t, sizeof t, "reset vector    0x%08lx", (unsigned long)v); text(1, y, C_LIT, t); y += ROW_H; }
        y += ROW_H;
        text(1, y, C_DIM, "Add-in mapping: file 0x7000+o <-> address 0x00300000+o"); y += ROW_H;
        text(1, y, C_DIM, "RAM 0x08100000-0x0817ffff, syscall trampoline 0x80020070");
    } else {
        uint32_t v;
        hdr_str(0x40, 16, s);  snprintf(t, sizeof t, "name       %s", s); text(1, y, C_TEXT, t); y += ROW_H;
        hdr_str(0x60, 11, s);  snprintf(t, sizeof t, "internal   %s", s); text(1, y, C_TEXT, t); y += ROW_H;
        hdr_str(0x130, 10, s); snprintf(t, sizeof t, "version    %s", s); text(1, y, C_TEXT, t); y += ROW_H;
        hdr_str(0x13c, 14, s); snprintf(t, sizeof t, "date       %s", s); text(1, y, C_TEXT, t); y += ROW_H;
        hdr_str(0xebc, 40, s); snprintf(t, sizeof t, "filename   %s", s); text(1, y, C_TEXT, t); y += ROW_H;
        snprintf(t, sizeof t, "file size  %lu bytes (code %lu)", (unsigned long)S.size, (unsigned long)(S.size > 0x7000 ? S.size - 0x7000 : 0));
        text(1, y, C_LIT, t); y += ROW_H;
        if (src_read(&S, S.base + 0x10, 4, &v)) { snprintf(t, sizeof t, "hdr size   %lu (inverted field)", (unsigned long)(~v)); text(1, y, C_LIT, t); y += ROW_H; }
        if (src_read(&S, S.base + 0x2e, 4, &v)) { snprintf(t, sizeof t, "code size  %lu (+4)", (unsigned long)v); text(1, y, C_LIT, t); y += ROW_H; }
        if (src_read(&S, S.base + 0x20, 4, &v)) { snprintf(t, sizeof t, "checksum2  0x%08lx", (unsigned long)v); text(1, y, C_LIT, t); y += ROW_H; }
        if (src_read(&S, S.base + 0x300000 - S.base + 0, 4, &v)) {}
        y += 2;
        text(1, y, C_DIM, "entry 0x00300000 = file offset 0x7000"); y += ROW_H;
        for (int j = 0; j < 8; j++) {
            hdr_str(0x6b + (uint32_t)j * 24, 24, s);
            if (s[0]) { snprintf(t, sizeof t, "label%d     %s", j, s); text(1, y, C_DIM, t); y += ROW_H; }
        }
        /* selected icon 92x64 RGB565 BE at 0x4000 */
        static uint8_t row[92 * 2];
        int x0 = DWIDTH - 92 - 4, y0 = LIST_Y + 2;
        drect(x0 - 1, y0 - 1, x0 + 92, y0 + 64, C_DIM);
        for (int j = 0; j < 64; j++) {
            if (src_bytes(&S, S.base + 0x4000 + (uint32_t)j * 184, row, 184) != 184) break;
            for (int i = 0; i < 92; i++) dpixel(x0 + i, y0 + j, (row[2 * i] << 8) | row[2 * i + 1]);
        }
    }
    static const char *lab[6] = { "OPEN", "GOTO", "HEX", "STR", "LIST", "ROM" };
    status_bar(lab);
}

/* ------------------------------------------------------------------ picker */

static void draw_picker(void) {
    dclear(C_BG);
    title_bar("DASM  SH-4A disassembler / binary browser", "fx-CG50");
    char t[80];
    if (nfiles < 0) { text(2, LIST_Y + 8, C_BR, "storage scan failed"); }
    else if (nfiles == 0) text(2, LIST_Y + 8, C_DIM, "no .g3a files in \\\\fls0");
    if (fsel < ftop) ftop = fsel;
    if (fsel >= ftop + ROWS - 2) ftop = fsel - (ROWS - 2) + 1;
    text(2, LIST_Y, C_DIM, "add-ins in storage memory:");
    for (int i = 0; i < ROWS - 2 && ftop + i < nfiles; i++) {
        int k = ftop + i, y = LIST_Y + (i + 1) * ROW_H;
        if (k == fsel) drect(0, y - 1, DWIDTH - 1, y + ROW_H - 2, C_CUR);
        snprintf(t, sizeof t, "%-28s %8lu", files[k].name, (unsigned long)files[k].size);
        text(2, y, k == fsel ? C_KEY : C_TEXT, t);
    }
    text(2, LIST_Y + (ROWS - 1) * ROW_H, C_DIM, "EXE open  F6 OS ROM  MENU quit");
    static const char *lab[6] = { 0, 0, 0, 0, 0, "ROM" };
    status_bar(lab);
}

static void open_file(int k) {
    if (have_src) src_close(&S);
    int rc = src_open_file(&S, &files[k]);
    if (rc < 0) { have_src = 0; snprintf(msg, sizeof msg, "open failed: BFile %d", rc); view = V_PICKER; return; }
    have_src = 1; nhist = 0; top = S.entry; cur = 0; nstrs = 0; view = V_LIST;
}

static void open_rom(void) {
    if (have_src) src_close(&S);
    src_open_rom(&S);
    have_src = 1; nhist = 0; top = S.entry; cur = 0; nstrs = 0; view = V_LIST;
}

/* ------------------------------------------------------------------ goto input */

static int goto_dialog(void) {
    char digits[9] = ""; int n = 0;
    for (;;) {
        char t[64];
        drect(0, STAT_Y - 1, DWIDTH - 1, DHEIGHT - 1, C_STATUS);
        snprintf(t, sizeof t, "Goto address: 0x%s_   (F1-F6 = A-F, DEL, EXE, EXIT)", digits);
        text(0, STAT_Y + 1, C_KEY, t);
        dupdate();
        key_event_t ev = getkey();
        int k = ev.key, d = -1;
        if (k >= KEY_F1 && k <= KEY_F6) d = 10 + (k - KEY_F1);
        else switch (k) {
            case KEY_0: d = 0; break; case KEY_1: d = 1; break; case KEY_2: d = 2; break;
            case KEY_3: d = 3; break; case KEY_4: d = 4; break; case KEY_5: d = 5; break;
            case KEY_6: d = 6; break; case KEY_7: d = 7; break; case KEY_8: d = 8; break;
            case KEY_9: d = 9; break;
            case KEY_DEL: if (n) digits[--n] = 0; break;
            case KEY_EXIT: return 0;
            case KEY_EXE: {
                if (!n) return 0;
                uint32_t a = (uint32_t)strtoul(digits, 0, 16);
                goto_addr(a, 1);
                return 1;
            }
            default: break;
        }
        if (d >= 0 && n < 8) { digits[n++] = "0123456789abcdef"[d]; digits[n] = 0; }
    }
}

/* ------------------------------------------------------------------ main loop */

static void redraw(void) {
    switch (view) {
    case V_PICKER: draw_picker(); break;
    case V_LIST: draw_listing(); break;
    case V_HEX: draw_hex(); break;
    case V_STRINGS: draw_strings(); break;
    case V_HEADER: draw_header(); break;
    }
    dupdate();
}

static void scroll_rows(int delta_rows, int step) {
    /* move the cursor by delta_rows; scroll when it leaves the window */
    int c = cur + delta_rows;
    if (c < 0) { top = clamp_addr(top + (uint32_t)((int32_t)c * step)); if (top == S.base) c = 0; else c = 0; }
    else if (c >= ROWS) { top = clamp_addr(top + (uint32_t)((c - ROWS + 1) * step)); c = ROWS - 1; }
    cur = c;
}

int main(void) {
    dfont(&font_dasm);
    names_init();
    nfiles = src_scan_g3a(files, 64);
    redraw();

    for (;;) {
        key_event_t ev = getkey();
        if (ev.type == KEYEV_OSMENU) { redraw(); continue; }
        int k = ev.key, shift = ev.shift;
        msg[0] = 0;

        if (view == V_PICKER) {
            if (k == KEY_UP && fsel > 0) fsel--;
            else if (k == KEY_DOWN && fsel < nfiles - 1) fsel++;
            else if (k == KEY_EXE && nfiles > 0) open_file(fsel);
            else if (k == KEY_F6) open_rom();
            redraw();
            continue;
        }

        /* common: F-keys */
        int handled = 1;
        switch (k) {
        case KEY_F1: view = V_PICKER; break;
        case KEY_F2: if (view == V_STRINGS || view == V_HEADER) view = V_LIST; goto_dialog(); break;
        case KEY_F3: if (view == V_HEX) view = V_LIST; else { view = V_HEX; top &= ~15u; cur = 0; } break;
        case KEY_F4:
            if (view == V_STRINGS) view = V_LIST;
            else { scan_strings(top + (uint32_t)cur * (view == V_HEX ? 16u : 2u)); view = V_STRINGS; }
            break;
        case KEY_F5: view = (view == V_HEADER) ? V_LIST : V_HEADER; break;
        case KEY_F6: if (!S.is_rom) open_rom(); else snprintf(msg, sizeof msg, "already browsing the OS ROM"); break;
        default: handled = 0;
        }
        if (handled) { redraw(); continue; }

        if (view == V_LIST) {
            switch (k) {
            case KEY_UP: scroll_rows(-1, 2); break;
            case KEY_DOWN: scroll_rows(1, 2); break;
            case KEY_LEFT: top = clamp_addr(top - (shift ? 0x1000u : ROWS * 2u)); break;
            case KEY_RIGHT: top = clamp_addr(top + (shift ? 0x1000u : ROWS * 2u)); break;
            case KEY_ADD: top = clamp_addr(top + 1); break;
            case KEY_SUB: top = clamp_addr(top - 1); break;
            case KEY_EXE: follow(); break;
            case KEY_EXIT:
                if (nhist) { nhist--; top = hist[nhist].top; cur = hist[nhist].cur; }
                else view = V_PICKER;
                break;
            default: break;
            }
        } else if (view == V_HEX) {
            switch (k) {
            case KEY_UP: scroll_rows(-1, 16); top &= ~15u; break;
            case KEY_DOWN: scroll_rows(1, 16); top &= ~15u; break;
            case KEY_LEFT: top = clamp_addr(top - (shift ? 0x1000u : ROWS * 16u)) & ~15u; break;
            case KEY_RIGHT: top = clamp_addr(top + (shift ? 0x1000u : ROWS * 16u)) & ~15u; break;
            case KEY_EXE: { uint32_t a = top + 16u * (uint32_t)cur; view = V_LIST; goto_addr(a, 1); break; }
            case KEY_EXIT: view = V_LIST; break;
            default: break;
            }
            if (top < S.base) top = S.base;
        } else if (view == V_STRINGS) {
            switch (k) {
            case KEY_UP: if (ssel > 0) ssel--; break;
            case KEY_DOWN: if (ssel < nstrs - 1) ssel++; break;
            case KEY_LEFT: ssel -= ROWS; if (ssel < 0) ssel = 0; break;
            case KEY_RIGHT: ssel += ROWS; if (ssel >= nstrs) ssel = nstrs ? nstrs - 1 : 0; break;
            case KEY_EXE: if (nstrs) { view = V_HEX; goto_addr(strs[ssel].addr, 1); } break;
            case KEY_EXIT: view = V_LIST; break;
            default: break;
            }
        } else if (view == V_HEADER) {
            if (k == KEY_EXIT || k == KEY_EXE) view = V_LIST;
        }
        redraw();
    }
    return 0;
}
