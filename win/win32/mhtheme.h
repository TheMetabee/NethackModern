/* NetHack Modern: the dark interface theme for NetHackW (Phase 4c).
 * One palette for every window, used when the fx_theme option is on.
 */
#ifndef MHTHEME_H
#define MHTHEME_H

#define TH_BG_APP     RGB(14, 16, 20)    /* behind everything; map backdrop */
#define TH_BG_PANEL   RGB(24, 27, 33)    /* message, inventory, status */
#define TH_BG_HEADER  RGB(33, 37, 46)    /* group headings in lists */
#define TH_SEPARATOR  RGB(52, 58, 70)    /* thin lines between things */
#define TH_TEXT       RGB(222, 226, 233) /* ordinary text */
#define TH_TEXT_DIM   RGB(138, 146, 160) /* letters, older messages */
#define TH_ACCENT     RGB(229, 186, 96)  /* headings, highlights */

#define TH_FONT_UI    "Segoe UI"         /* messages, menus, status */
#define TH_FONT_MONO  "Consolas"         /* text windows (aligned columns) */

/* the game's 16 colours, tuned to read well on the dark panels */
extern const COLORREF th_palette[16];

#endif /* MHTHEME_H */
