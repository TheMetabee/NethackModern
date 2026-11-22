/* $NHDT-Header: NetHack 5.0 mhsplash.h 2015/05/25_00:13:31 NetHack-5.0:1.8 $ */
/* Copyright (C) 2002 by Alex Kompel */
/* NetHack may be freely redistributed.  See license for details. */

#ifndef MSWINSplashWindow_h
#define MSWINSplashWindow_h

#include "winMS.h"
#include "config.h"
#include "global.h"

void mswin_display_splash_window(BOOL);
void mswin_display_menu_screen(void);   /* NetHack Modern */
void mswin_menu_screen_place(HWND main);

/* NetHack Modern: the title screen's look, shared with the name and
   character screens (mhcharsel.c) */
HFONT mswin_ms_font(int px, int weight, BOOL serif);
void mswin_ms_text(HDC hdc, const char *s, RECT *r, UINT how, COLORREF c,
                   int shadow);
void mswin_ms_button(HDC hdc, const RECT *r, const char *label,
                     boolean primary, boolean hot, boolean focus,
                     boolean down, boolean enabled, double s);
void mswin_ms_backdrop(HDC hdc, HDC dst, int w, int h, int dim);
void mswin_ms_backdrop_free(void);

/* NetHack Modern: the menu music and the speaker icon that toggles it */
void mswin_menu_music_start(void);
void mswin_menu_music_end(void);
void mswin_menu_music_toggle(void);
void mswin_ms_sound_rect(int w, double s, RECT *r);
void mswin_ms_sound_icon(HDC hdc, const RECT *r, boolean hot, double s);

#endif /* MSWINSplashWindow_h */
