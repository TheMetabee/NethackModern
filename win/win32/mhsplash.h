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

#endif /* MSWINSplashWindow_h */
