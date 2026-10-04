/* theme — the two colour themes of DASM and the resident monitor (S<->D switches), and where
 * the choice is kept.
 *
 * The dark theme reads well in the emulator but the real calculator's LCD loses contrast on
 * dark backgrounds, hence a light one. The choice is a word at the top of the free DRAM
 * (MON_THEME, above the monitor's memory): it survives leaving DASM and powering off, and both
 * DASM and the monitor read and write it through P2 (uncached), so neither has to flush. */
#ifndef THEME_H
#define THEME_H
#include <stdint.h>

/* As gint's C_RGB: r, g, b are 0-31 each. The dark theme's green values above 31 spill into the
 * red bits; they are kept bit for bit, as DASM has always looked. */
#define TH_RGB(r, g, b) ((uint16_t)(((r) << 11) | ((g) << 6) | (b)))

enum {
    P_BG, P_TITLE, P_STATUS, P_FKEY, P_KEY, P_TEXT, P_DIM, P_SEP, P_CUR, P_PCBAR,
    P_ADDR, P_HEXW, P_BR, P_CALL, P_RET, P_LIT, P_NAME, P_SLOT, P_DATA, P_ASCII,
    P_N
};

static const uint16_t theme_pal[2][P_N] = {
    {   /* dark */
        [P_BG] = TH_RGB(1, 2, 3),       [P_TITLE] = TH_RGB(4, 10, 18),  [P_STATUS] = TH_RGB(3, 6, 10),
        [P_FKEY] = TH_RGB(6, 14, 24),   [P_KEY] = TH_RGB(31, 60, 31),   [P_TEXT] = TH_RGB(28, 56, 28),
        [P_DIM] = TH_RGB(12, 24, 12),   [P_SEP] = TH_RGB(6, 14, 22),    [P_CUR] = TH_RGB(3, 9, 16),
        [P_PCBAR] = TH_RGB(8, 14, 2),   [P_ADDR] = TH_RGB(14, 30, 16),  [P_HEXW] = TH_RGB(10, 22, 12),
        [P_BR] = TH_RGB(31, 58, 10),    [P_CALL] = TH_RGB(31, 40, 6),   [P_RET] = TH_RGB(31, 24, 26),
        [P_LIT] = TH_RGB(10, 52, 14),   [P_NAME] = TH_RGB(8, 54, 31),   [P_SLOT] = TH_RGB(16, 38, 31),
        [P_DATA] = TH_RGB(18, 30, 18),  [P_ASCII] = TH_RGB(24, 48, 20),
    },
    {   /* light: dark text on white, bars in pale blue */
        [P_BG] = TH_RGB(31, 31, 31),    [P_TITLE] = TH_RGB(22, 24, 31), [P_STATUS] = TH_RGB(26, 27, 30),
        [P_FKEY] = TH_RGB(19, 21, 29),  [P_KEY] = TH_RGB(0, 0, 0),      [P_TEXT] = TH_RGB(1, 1, 1),
        [P_DIM] = TH_RGB(12, 12, 12),   [P_SEP] = TH_RGB(20, 21, 26),   [P_CUR] = TH_RGB(22, 26, 31),
        [P_PCBAR] = TH_RGB(31, 30, 16), [P_ADDR] = TH_RGB(0, 13, 6),    [P_HEXW] = TH_RGB(10, 10, 12),
        [P_BR] = TH_RGB(18, 13, 0),     [P_CALL] = TH_RGB(26, 8, 0),    [P_RET] = TH_RGB(24, 0, 12),
        [P_LIT] = TH_RGB(0, 17, 2),     [P_NAME] = TH_RGB(0, 11, 26),   [P_SLOT] = TH_RGB(8, 10, 22),
        [P_DATA] = TH_RGB(11, 11, 11),  [P_ASCII] = TH_RGB(10, 13, 4),
    },
};

#define MON_THEME   0xAC7FFFF8u     /* P2: the theme word (the last 8 bytes of the free DRAM) */
#define THEME_MAGIC 0x54484D00u     /* "THM" + 0 dark / 1 light */

static inline int theme_get(void) {    /* 0 dark (also when never set), 1 light */
    uint32_t v = *(volatile uint32_t *)MON_THEME;
    return (v & ~1u) == THEME_MAGIC ? (int)(v & 1) : 0;
}

static inline void theme_put(int light) {
    *(volatile uint32_t *)MON_THEME = THEME_MAGIC | (light ? 1u : 0u);
}

#endif
