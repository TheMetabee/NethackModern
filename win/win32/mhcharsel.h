/* NetHack Modern: the name and character screens (mhcharsel.c) */
/* NetHack may be freely redistributed.  See license for details. */

#ifndef MSWINCharSel_h
#define MSWINCharSel_h

#include "winMS.h"
#include "config.h"
#include "global.h"

/* ask for the hero's name in the game window; FALSE if the player
   chose Back */
boolean mswin_name_screen(char *name, int size);

/* choose role, race, gender and alignment (flags.init*, which must hold
   a valid combination on entry); 'randomize' re-rolls them for the
   Random button.  1 to play, 0 if the player chose Back, -1 if the
   screen couldn't be shown */
int mswin_char_screen(void (*randomize)(void *), void *arg);

/* keep whichever of them is showing filling the game window */
void mswin_char_screen_place(HWND main);

#endif /* MSWINCharSel_h */
