/* NetHack Modern: the death screen and the score list (mhend.c) */
/* NetHack may be freely redistributed.  See license for details. */

#ifndef MSWINEnd_h
#define MSWINEnd_h

#include "winMS.h"
#include "config.h"
#include "global.h"

/* remember the messages of the hero's last turn; called as the hero
   dies, before the end-of-game questions add their own */
void mswin_end_snapshot(void);

/* the death screen, in place of the tombstone dialog: 'rip' is the
   tombstone's lines (name, gold, cause, year) and 'summary' the text
   below it ("Goodbye ..."), each line ending in CR LF.  FALSE if it
   couldn't be shown */
boolean mswin_death_screen(const char *rip, const char *summary);

/* the score list: the lines topten() writes are collected here, then
   shown as a table.  FALSE if it couldn't be shown */
void mswin_scores_add(int attr, const char *text);
void mswin_scores_clear(void);
boolean mswin_scores_screen(void);

/* keep whichever is showing filling the game window */
void mswin_end_screen_place(HWND main);

#endif /* MSWINEnd_h */
