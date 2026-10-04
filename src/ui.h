/* ui — colours and text drawing shared by the browser (main.c) and the debugger (debug.c). */
#ifndef UI_H
#define UI_H
#include <gint/display.h>

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
#define C_SEP    C_RGB(6, 14, 22)

/* The large anti-aliased font's cell (src/font_aa.h, AA_CW x AA_CH). */
#define UI_CW 7
#define UI_CH 11

/* Anti-aliased text at pixel (x, y), at most maxch characters, blended over what is drawn
 * (main.c). */
void aa_text(int x, int y, int color, const char *s, int maxch);

#endif
