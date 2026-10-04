/* ui — colours and text drawing shared by the browser (main.c) and the debugger (debug.c). */
#ifndef UI_H
#define UI_H
#include <gint/display.h>
#include "../monitor/theme.h"

/* The colours of the current theme (S<->D switches; ../monitor/theme.h has both palettes). */
extern const uint16_t *ui_pal;
#define C_BG      ((int)ui_pal[P_BG])
#define C_TITLE   ((int)ui_pal[P_TITLE])
#define C_ADDR    ((int)ui_pal[P_ADDR])
#define C_HEXW    ((int)ui_pal[P_HEXW])
#define C_TEXT    ((int)ui_pal[P_TEXT])
#define C_BR      ((int)ui_pal[P_BR])
#define C_CALL    ((int)ui_pal[P_CALL])
#define C_RET     ((int)ui_pal[P_RET])
#define C_LIT     ((int)ui_pal[P_LIT])
#define C_NAME    ((int)ui_pal[P_NAME])
#define C_SLOT    ((int)ui_pal[P_SLOT])
#define C_CUR     ((int)ui_pal[P_CUR])
#define C_DATA    ((int)ui_pal[P_DATA])
#define C_STATUS  ((int)ui_pal[P_STATUS])
#define C_FKEY    ((int)ui_pal[P_FKEY])
#define C_KEY     ((int)ui_pal[P_KEY])
#define C_DIM     ((int)ui_pal[P_DIM])
#define C_ASCII   ((int)ui_pal[P_ASCII])
#define C_SEP     ((int)ui_pal[P_SEP])

/* The large anti-aliased font's cell (src/font_aa.h, AA_CW x AA_CH). */
#define UI_CW 7
#define UI_CH 11

/* Anti-aliased text at pixel (x, y), at most maxch characters, blended over what is drawn
 * (main.c). */
void aa_text(int x, int y, int color, const char *s, int maxch);

/* Switch between the dark and the light theme and remember the choice; returns 1 for light
 * (main.c). */
int ui_theme_toggle(void);

#endif
