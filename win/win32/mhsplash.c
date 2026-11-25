/* $NHDT-Header: NetHack 5.0 mhsplash.c 2026/06/20_22:26:43 NetHack-5.0:1.47 $ */
/* Copyright (C) 2001 by Alex Kompel */
/* NetHack may be freely redistributed.  See license for details. */

#include "win10.h"
#include "winMS.h"
#include "resource.h"
#include "mhsplash.h"
#include "mhmsg.h"
#include "mhfont.h"
#include "config.h"
#if !defined(VERSION_MAJOR)
#include "patchlevel.h"
#endif
#include "dlb.h"
#include "mhtheme.h"
#include "fxview.h"
#ifdef SND_LIB_MINIAUDIO
#include "../../sound/miniaudio/nhminiaudio.h"
#endif

#define LLEN 128

PNHWinApp GetNHApp(void);

INT_PTR CALLBACK NHSplashWndProc(HWND, UINT, WPARAM, LPARAM);

#define SPLASH_WIDTH_96DPI 440
#define SPLASH_HEIGHT_96DPI 322
#define SPLASH_OFFSET_X_96DPI 10
#define SPLASH_OFFSET_Y_96DPI 10
#define SPLASH_VERSION_X_96DPI 280
#define SPLASH_VERSION_Y_96DPI 0

typedef struct {
    int boarder_width;
    int boarder_height;
    int client_width;
    int client_height;
    int ok_control_width;
    int ok_control_height;
    int ok_control_offset_x;
    int ok_control_offset_y;
    int text_control_width;
    int text_control_height;
    int text_control_offset_x;
    int text_control_offset_y;
    int window_width;
    int window_height;
    int width;
    int height;
    int offset_x;
    int offset_y;
    int version_x;
    int version_y;
    HFONT hFont;
} SplashData;

static void
mswin_set_splash_data(HWND hWnd, SplashData * sd, double scale)
{
    RECT client_rect;
    RECT window_rect;
    RECT ok_control_rect;
    RECT text_control_rect;

    GetClientRect(hWnd, &client_rect);
    GetWindowRect(hWnd, &window_rect);
    GetWindowRect(GetDlgItem(hWnd, IDOK), &ok_control_rect);
    GetWindowRect(GetDlgItem(hWnd, IDC_EXTRAINFO), &text_control_rect);

    sd->boarder_width = (window_rect.right - window_rect.left) -
                                (client_rect.right - client_rect.left);
    sd->boarder_height = (window_rect.bottom - window_rect.top) -
        (client_rect.bottom - client_rect.top);

    sd->ok_control_width = ok_control_rect.right - ok_control_rect.left;
    sd->ok_control_height = ok_control_rect.bottom - ok_control_rect.top;

    sd->width = (int)(scale * SPLASH_WIDTH_96DPI);
    sd->height = (int)(scale * SPLASH_HEIGHT_96DPI);
    if (iflags.fx_theme) {
        /* NetHack Modern: the painted art (16:9) and a title band */
        sd->width = (int) (scale * 640);
        sd->height = (int) (scale * (360 + 86));
    }
    sd->offset_x = (int)(scale * SPLASH_OFFSET_X_96DPI);
    sd->offset_y = (int)(scale * SPLASH_OFFSET_Y_96DPI);
    sd->version_x = (int)(scale * SPLASH_VERSION_X_96DPI);
    sd->version_y = (int)(scale * SPLASH_VERSION_Y_96DPI);

    sd->client_width = sd->width + sd->offset_x * 2;
    sd->client_height = sd->height + sd->ok_control_height +
        sd->offset_y * 3;

    sd->window_width = sd->client_width + sd->boarder_width;
    sd->window_height = sd->client_height + sd->boarder_height;

    sd->ok_control_offset_x = (sd->client_width - sd->ok_control_width) / 2;
    sd->ok_control_offset_y = sd->client_height - sd->ok_control_height - sd->offset_y;

    sd->text_control_width = sd->client_width - sd->offset_x * 2;
    sd->text_control_height = text_control_rect.bottom - text_control_rect.top;

    sd->text_control_offset_x = sd->offset_x;
    sd->text_control_offset_y = sd->ok_control_offset_y - sd->offset_y -
                               sd->text_control_height;

    if (sd->hFont != NULL)
        DeleteObject(sd->hFont);

    sd->hFont = mswin_create_splashfont(hWnd);

    MoveWindow(hWnd, window_rect.left, window_rect.top,
        sd->window_width, sd->window_height, TRUE);

    MoveWindow(GetDlgItem(hWnd, IDOK),
        sd->ok_control_offset_x, sd->ok_control_offset_y,
        sd->ok_control_width, sd->ok_control_height, TRUE);

    MoveWindow(GetDlgItem(hWnd, IDC_EXTRAINFO),
        sd->text_control_offset_x, sd->text_control_offset_y,
        sd->text_control_width, sd->text_control_height, TRUE);

}

void
mswin_display_splash_window(BOOL show_ver)
{
    MSG msg;
    strbuf_t strbuf;

    strbuf_init(&strbuf);

    HWND hWnd = CreateDialog(GetNHApp()->hApp, MAKEINTRESOURCE(IDD_SPLASH),
                        GetNHApp()->hMainWnd, NHSplashWndProc);
    if (!hWnd)
        panic("Cannot create Splash window");

    MonitorInfo monitorInfo;
    win10_monitor_info(hWnd, &monitorInfo);

    SplashData splashData;
    memset(&splashData, 0, sizeof(splashData));
    mswin_set_splash_data(hWnd, &splashData, monitorInfo.scale);
 
    SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR) &splashData);
    windowdata[NHW_SPLASH].address = (genericptr_t) &splashData;
    windowdata[NHW_SPLASH].isstatic = 1;
    
    GetNHApp()->hPopupWnd = hWnd;

    /* NetHack Modern: centre it over the game window (or the screen),
       by the size it really ended up */
    {
        RECT wr, mr;
        int w, h, left, top;

        GetWindowRect(hWnd, &wr);
        w = wr.right - wr.left, h = wr.bottom - wr.top;
        if (GetNHApp()->hMainWnd && IsWindowVisible(GetNHApp()->hMainWnd)
            && !IsIconic(GetNHApp()->hMainWnd))
            GetWindowRect(GetNHApp()->hMainWnd, &mr);
        else {
            MONITORINFO mi;

            mi.cbSize = sizeof mi;
            if (GetMonitorInfo(MonitorFromWindow(hWnd,
                                                 MONITOR_DEFAULTTONEAREST),
                               &mi))
                mr = mi.rcWork;
            else
                SetRect(&mr, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                        GetSystemMetrics(SM_CYSCREEN));
        }
        left = mr.left + ((mr.right - mr.left) - w) / 2;
        top = mr.top + ((mr.bottom - mr.top) - h) / 2;
        SetWindowPos(hWnd, NULL, left, top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    /* Fill the text control */
    strbuf_reserve(&strbuf, BUFSIZ);
    Sprintf(strbuf.str, "%s\n%s\n%s\n%s\n\n", COPYRIGHT_BANNER_A,
            COPYRIGHT_BANNER_B, nomakedefs.copyright_banner_c, COPYRIGHT_BANNER_D);

    if (show_ver) {
        /* Show complete version information */
        dlb *f;
        char verbuf[BUFSZ];
        /* int verstrsize = 0; */

        Sprintf(verbuf, "NetHack Modern v%s\n", MODERN_VERSION_STRING);
        strbuf_append(&strbuf, verbuf); /* NetHack Modern */
        getversionstring(verbuf, sizeof verbuf);
        strbuf_append(&strbuf, verbuf);
        strbuf_append(&strbuf, "\n\n");
            
        /* Add compile options */
        f = dlb_fopen(OPTIONS_USED, RDTMODE);
        if (f) {
            char line[LLEN + 1];

            while (dlb_fgets(line, LLEN, f))
                strbuf_append(&strbuf, line);
            (void) dlb_fclose(f);
        }
    } else {
        /* Show news, if any */
        if (iflags.news) {
            FILE *nf;

            iflags.news = 0; /* prevent newgame() from re-displaying news */
            /* BUG: this relies on current working directory */
            nf = fopen(NEWS, "r");
            if (nf != NULL) {
                char line[LLEN + 1];

                while (fgets(line, LLEN, nf))
                    strbuf_append(&strbuf, line);
                (void) fclose(nf);
            } else {
                strbuf_append(&strbuf, "No news.");
            }
        }
    }

    strbuf_nl_to_crlf(&strbuf);
    SetWindowText(GetDlgItem(hWnd, IDC_EXTRAINFO), strbuf.str);
    strbuf_empty(&strbuf);
    ShowWindow(hWnd, SW_SHOW);

    while (IsWindow(hWnd) && GetMessage(&msg, NULL, 0, 0) != 0) {
        if (!IsDialogMessage(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    GetNHApp()->hPopupWnd = NULL;
    DeleteObject(splashData.hFont);
}

/* NetHack Modern: the themed splash.  A gold serif title over the
   game's own red dragon tile, blown up as pixel art; the dialog, its
   text box and button in the dark theme. */
extern glyph_map glyphmap[MAX_GLYPH];

static void
th_splash_dark_titlebar(HWND hWnd)
{
    typedef HRESULT(WINAPI * dwmset_fn)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    dwmset_fn fn;
    BOOL on = TRUE;

    if (!dwm)
        return;
    fn = (dwmset_fn) (void *) GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (fn && FAILED(fn(hWnd, 20, &on, sizeof on)))
        (void) fn(hWnd, 19, &on, sizeof on);
}

static void
th_splash_paint(HWND hWnd, HDC hdc, SplashData *sd)
{
    RECT area, r;
    HFONT title, sub, old;
    char buf[BUFSZ];
    int th, tile, tx, ty, ds;
    HDC tdc;
    HGDIOBJ ob;

    static HBITMAP art;
    static boolean art_tried;
    int arth = 0;

    UNREFERENCED_PARAMETER(hWnd);
    SetRect(&area, sd->offset_x, sd->offset_y, sd->offset_x + sd->width,
            sd->offset_y + sd->height);
    if (!art_tried) {
        art_tried = TRUE;
        art = (HBITMAP) LoadImage(GetNHApp()->hApp,
                                  MAKEINTRESOURCE(IDB_SPLASH_ART),
                                  IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    }
    if (art) {
        /* the painting across the top, smoothly scaled */
        BITMAP bm;
        HDC adc = CreateCompatibleDC(hdc);
        HGDIOBJ oa = SelectObject(adc, art);
        POINT org;

        GetObject(art, sizeof bm, &bm);
        arth = sd->width * bm.bmHeight / max(1, bm.bmWidth);
        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, &org);
        StretchBlt(hdc, area.left, area.top, sd->width, arth, adc, 0, 0,
                   bm.bmWidth, bm.bmHeight, SRCCOPY);
        SelectObject(adc, oa);
        DeleteDC(adc);
        area.top += arth;
    }
    th = art ? (area.bottom - area.top) * 9 / 20 : sd->height / 5;
    title = CreateFontA(-th, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        VARIABLE_PITCH | FF_ROMAN, "Palatino Linotype");
    sub = CreateFontA(-(th / 3), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                      CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS,
                      TH_FONT_UI);
    SetBkMode(hdc, TRANSPARENT);

    /* the title, with a soft dark shadow */
    old = SelectObject(hdc, title);
    r = area;
    r.top += art ? (area.bottom - area.top) / 12 : sd->height / 20;
    r.bottom = r.top + th + th / 4;
    OffsetRect(&r, 2, 3);
    SetTextColor(hdc, RGB(0, 0, 0));
    DrawTextA(hdc, "NetHack", -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    OffsetRect(&r, -2, -3);
    SetTextColor(hdc, TH_ACCENT);
    DrawTextA(hdc, "NetHack", -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

    /* "MODERN  5.0.1" under it */
    SelectObject(hdc, sub);
    r.top = r.bottom;
    r.bottom = r.top + th / 2;
    Sprintf(buf, "M O D E R N     %d.%d.%d", VERSION_MAJOR, VERSION_MINOR,
            PATCHLEVEL);
    SetTextColor(hdc, TH_TEXT_DIM);
    DrawTextA(hdc, buf, -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

    /* without the painting: the red dragon, as big crisp pixel art */
    if (art)
        goto done;
    tile = glyphmap[monnum_to_glyph(PM_RED_DRAGON, MALE)].tileidx;
    ds = min(sd->height - (r.bottom - area.top) - sd->height / 12,
             sd->width / 2);
    ds -= ds % GetNHApp()->mapTile_Y; /* whole multiples stay crisp */
    if (ds > 0 && GetNHApp()->bmpMapTiles) {
        tx = (tile % GetNHApp()->mapTilesPerLine) * GetNHApp()->mapTile_X;
        ty = (tile / GetNHApp()->mapTilesPerLine) * GetNHApp()->mapTile_Y;
        tdc = CreateCompatibleDC(hdc);
        ob = SelectObject(tdc, GetNHApp()->bmpMapTiles);
        SetStretchBltMode(hdc, COLORONCOLOR);
        (*GetNHApp()->lpfnTransparentBlt)(
            hdc, area.left + (sd->width - ds) / 2,
            r.bottom + (area.bottom - r.bottom - ds) / 2, ds, ds, tdc, tx, ty,
            GetNHApp()->mapTile_X, GetNHApp()->mapTile_Y, TILE_BK_COLOR);
        SelectObject(tdc, ob);
        DeleteDC(tdc);
    }
 done:
    SelectObject(hdc, old);
    DeleteObject(title);
    DeleteObject(sub);
}

INT_PTR CALLBACK
NHSplashWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    static HBRUSH th_panel, th_box;
    UNREFERENCED_PARAMETER(lParam);

    switch (message) {
    case WM_INITDIALOG: {
        HDC hdc = GetDC(hWnd);
        cached_font * font = mswin_get_font(NHW_TEXT, ATR_NONE, hdc, FALSE);
        /* set text control font */
        SendMessage(hWnd, WM_SETFONT, (WPARAM)font->hFont,  0);
        ReleaseDC(hWnd, hdc);

        SetFocus(GetDlgItem(hWnd, IDOK));
        if (iflags.fx_theme) {
            /* NetHack Modern: dark, with a flat button */
            HWND ok = GetDlgItem(hWnd, IDOK);

            th_splash_dark_titlebar(hWnd);
            SetWindowTextA(hWnd, "Welcome to NetHack Modern");
            SetWindowLongPtr(ok, GWL_STYLE,
                             (GetWindowLongPtr(ok, GWL_STYLE) & ~BS_TYPEMASK)
                                 | BS_OWNERDRAW);
            if (!th_panel)
                th_panel = CreateSolidBrush(TH_BG_PANEL);
            if (!th_box)
                th_box = CreateSolidBrush(TH_BG_APP);
        }
    } break;

    case WM_CTLCOLORDLG:
        if (iflags.fx_theme && th_panel)
            return (INT_PTR) th_panel;
        break;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if (iflags.fx_theme && th_box) {
            SetTextColor((HDC) wParam, TH_TEXT_DIM);
            SetBkColor((HDC) wParam, TH_BG_APP);
            return (INT_PTR) th_box;
        }
        break;

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT) lParam;

        if (iflags.fx_theme && dis->CtlID == IDOK) {
            boolean down = (dis->itemState & ODS_SELECTED) != 0;
            HBRUSH bg = CreateSolidBrush(TH_BG_PANEL),
                   fill = CreateSolidBrush(down ? TH_SEPARATOR : TH_BG_HEADER),
                   edge = CreateSolidBrush(TH_ACCENT);
            HGDIOBJ ob, op;
            RECT r = dis->rcItem;

            FillRect(dis->hDC, &r, bg);
            ob = SelectObject(dis->hDC, fill);
            op = SelectObject(dis->hDC, GetStockObject(NULL_PEN));
            RoundRect(dis->hDC, r.left, r.top, r.right + 1, r.bottom + 1,
                      FXV_CORNER_GDI, FXV_CORNER_GDI);
            SelectObject(dis->hDC, op);
            SelectObject(dis->hDC, ob);
            if (dis->itemState & ODS_FOCUS) { /* a gold line underneath */
                RECT u = r;

                u.left += 6, u.right -= 6, u.top = u.bottom - 2;
                FillRect(dis->hDC, &u, edge);
            }
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, TH_TEXT);
            DrawTextA(dis->hDC, "Continue", -1, &r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DeleteObject(bg);
            DeleteObject(fill);
            DeleteObject(edge);
            return TRUE;
        }
        break;
    }

    case WM_PAINT: {
        char VersionString[BUFSZ];
        RECT rt;
        HDC hdcBitmap;
        HANDLE OldBitmap;
        HANDLE OldFont;
        PAINTSTRUCT ps;

        SplashData *splashData = (SplashData *) GetWindowLongPtr(hWnd, GWLP_USERDATA);

        HDC hdc = BeginPaint(hWnd, &ps);
        if (iflags.fx_theme) { /* NetHack Modern */
            th_splash_paint(hWnd, hdc, splashData);
            EndPaint(hWnd, &ps);
            break;
        }
        /* Show splash graphic */

        hdcBitmap = CreateCompatibleDC(hdc);
        SetBkMode(hdc, OPAQUE);
        OldBitmap = SelectObject(hdcBitmap, GetNHApp()->bmpSplash);
        (*GetNHApp()->lpfnTransparentBlt)(hdc,
            splashData->offset_x, splashData->offset_y,
            splashData->width, splashData->height, hdcBitmap,
            0, 0, SPLASH_WIDTH_96DPI, SPLASH_HEIGHT_96DPI,
            TILE_BK_COLOR);

        SelectObject(hdcBitmap, OldBitmap);
        DeleteDC(hdcBitmap);

        SetBkMode(hdc, TRANSPARENT);
        /* Print version number */

        SetTextColor(hdc, RGB(0, 0, 0));
        rt.right = rt.left = splashData->offset_x + splashData->version_x;
        rt.bottom = rt.top = splashData->offset_y + splashData->version_y;
        Sprintf(VersionString, "%d.%d.%d", VERSION_MAJOR, VERSION_MINOR,
                PATCHLEVEL);
        OldFont = SelectObject(hdc, splashData->hFont);
        DrawText(hdc, VersionString, strlen(VersionString), &rt,
                 DT_LEFT | DT_NOPREFIX | DT_CALCRECT);
        DrawText(hdc, VersionString, strlen(VersionString), &rt,
                 DT_LEFT | DT_NOPREFIX);
        EndPaint(hWnd, &ps);
        nhUse(OldFont);
    } break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDCANCEL:
        case IDOK:
            mswin_window_mark_dead(mswin_winid_from_handle(hWnd));
            if (GetNHApp()->hMainWnd == hWnd)
                GetNHApp()->hMainWnd = NULL;
            DestroyWindow(hWnd);
            SetFocus(GetNHApp()->hMainWnd);
            return TRUE;
        }
        break;

    case WM_DPICHANGED: {
        SplashData *splashData = (SplashData *)GetWindowLongPtr(hWnd, GWLP_USERDATA);

        MonitorInfo monitorInfo;
        win10_monitor_info(hWnd, &monitorInfo);

        mswin_set_splash_data(hWnd, splashData, monitorInfo.scale);

        InvalidateRect(hWnd, NULL, TRUE);
    } break;

    }
    return FALSE;
}

/* NetHack Modern: the title screen.  It fills the game window before
   play begins, in place of the old splash popup: the painting, the
   title, both version numbers, and Play / About / Exit.  Every game
   ends back here (see mswin_relaunch in NetHackW.c). */

#define MS_PLAY 0
#define MS_ABOUT 1
#define MS_EXIT 2
#define MS_NBTN 3

static const char *const ms_label[MS_NBTN] = {
    "Play NetHack Modern", "About", "Exit"
};
static HWND ms_hwnd;
static int ms_result = -1; /* the button chosen; -1 while waiting */
static int ms_focus = MS_PLAY, ms_hot = -1, ms_down = -1;
static RECT ms_btn[MS_NBTN];
#define MS_SOUND MS_NBTN /* the speaker icon, beside the buttons */
static RECT ms_snd;
/* a newer version: the badge in the top left corner and its buttons
   (fxview.c); they're MS_UPD + FXV_UPD_* for focus and the mouse */
#define MS_UPD (MS_SOUND + 1)
#define MS_UPD_TIMER 1 /* looks at the check every half second until it
                          has an answer */
static boolean ms_upd;
static RECT ms_upanel, ms_ubtn[FXV_UPD_NBTN];
static HBITMAP ms_bg;         /* the painting, cropped, scaled, shaded */
static int ms_bg_w, ms_bg_h;  /* ... for this size */

static HFONT
ms_font(int px, int weight, BOOL serif)
{
    return CreateFontA(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       VARIABLE_PITCH | (serif ? FF_ROMAN : FF_SWISS),
                       serif ? "Palatino Linotype" : TH_FONT_UI);
}

/* how big to draw things: 1.0 for a 720-pixel-high window */
static double
ms_scale(int w, int h)
{
    return fxv_scr_scale(w, h); /* shared with SDL (fxview.c) */
}

/* the painting fills the window (cropped to keep its shape), darkened
   towards the bottom so the title and buttons read over it */
static void
ms_make_bg(HDC hdc, int w, int h)
{
    static HBITMAP art;
    static boolean art_tried;
    HDC bdc;
    HGDIOBJ ob;
    RECT all;
    HBRUSH b;

    if (ms_bg && ms_bg_w == w && ms_bg_h == h)
        return;
    if (ms_bg)
        DeleteObject(ms_bg);
    ms_bg = CreateCompatibleBitmap(hdc, max(1, w), max(1, h));
    ms_bg_w = w, ms_bg_h = h;
    bdc = CreateCompatibleDC(hdc);
    ob = SelectObject(bdc, ms_bg);
    SetRect(&all, 0, 0, w, h);
    b = CreateSolidBrush(TH_BG_APP);
    FillRect(bdc, &all, b);
    DeleteObject(b);

    if (!art_tried) {
        art_tried = TRUE;
        art = (HBITMAP) LoadImage(GetNHApp()->hApp,
                                  MAKEINTRESOURCE(IDB_SPLASH_ART),
                                  IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    }
    if (art) {
        BITMAP bm;
        HDC adc = CreateCompatibleDC(hdc);
        HGDIOBJ oa = SelectObject(adc, art);
        POINT org;
        double sc;
        int dw, dh;

        GetObject(art, sizeof bm, &bm);
        sc = max((double) w / bm.bmWidth, (double) h / bm.bmHeight);
        dw = (int) (bm.bmWidth * sc + 0.5);
        dh = (int) (bm.bmHeight * sc + 0.5);
        SetStretchBltMode(bdc, HALFTONE);
        SetBrushOrgEx(bdc, 0, 0, &org);
        /* crop evenly from the sides; from the bottom more than the top,
           so the rider and the moon stay in view */
        StretchBlt(bdc, (w - dw) / 2, (h - dh) / 3, dw, dh, adc, 0, 0,
                   bm.bmWidth, bm.bmHeight, SRCCOPY);
        SelectObject(adc, oa);
        DeleteDC(adc);
    }

    /* the shade: a one-pixel-wide column of black, transparent at the
       top and nearly opaque at the bottom, stretched across */
    if (h > 0) {
        BITMAPINFO bi;
        DWORD *px = NULL;
        HBITMAP sh;

        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = 1;
        bi.bmiHeader.biHeight = -h; /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        sh = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void **) &px, NULL,
                              0);
        if (sh && px) {
            HDC sdc = CreateCompatibleDC(hdc);
            HGDIOBJ os = SelectObject(sdc, sh);
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            int y;

            for (y = 0; y < h; y++) /* the curve is shared (fxview.c) */
                px[y] = ((DWORD) fxv_title_shade(y, h)) << 24; /* black,
                                                                  premult. */
            GdiFlush();
            (void) GdiAlphaBlend(bdc, 0, 0, w, h, sdc, 0, 0, 1, h, bf);
            SelectObject(sdc, os);
            DeleteDC(sdc);
        }
        if (sh)
            DeleteObject(sh);
    }
    SelectObject(bdc, ob);
    DeleteDC(bdc);
}

/* where the buttons go: Play wide and gold, About and Exit beneath */
static void
ms_layout(int w, int h)
{
    int r[FXV_TITLE_NBTN][4], i; /* shared with SDL (fxview.c) */

    fxv_title_buttons(w, h, r);
    for (i = 0; i < MS_NBTN; i++)
        SetRect(&ms_btn[i], r[i][0], r[i][1], r[i][2], r[i][3]);
}

static void
ms_text(HDC hdc, const char *s, RECT *r, UINT how, COLORREF c, int shadow)
{
    if (shadow) {
        OffsetRect(r, shadow, shadow);
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextA(hdc, s, -1, r, how | DT_NOPREFIX);
        OffsetRect(r, -shadow, -shadow);
    }
    SetTextColor(hdc, c);
    DrawTextA(hdc, s, -1, r, how | DT_NOPREFIX);
}

/* a rounded button in the title screen's style: 'primary' is the wide
   gold one, the others dark; used by the character screens too */
void
mswin_ms_button(HDC hdc, const RECT *rp, const char *label, boolean primary,
                boolean hot, boolean focus, boolean down, boolean enabled,
                double s)
{
    RECT r = *rp;
    COLORREF fill, ink, edge;
    HBRUSH fb;
    HPEN pen;
    HGDIOBJ ob, op;
    int rad = FXV_CORNER_GDI; /* the panels' small corners */

    if (!enabled)
        hot = down = FALSE;
    if (primary && enabled) {
        fill = down ? RGB(196, 152, 70) : hot ? RGB(244, 206, 124)
                                              : TH_ACCENT;
        ink = TH_BG_APP;
        edge = focus ? RGB(255, 236, 190) : fill;
    } else if (primary) {
        fill = RGB(92, 80, 58);
        ink = RGB(40, 36, 30);
        edge = focus ? TH_ACCENT : fill;
    } else {
        fill = down ? TH_SEPARATOR : hot ? TH_BG_HEADER : TH_BG_PANEL;
        ink = !enabled ? TH_SEPARATOR : hot || focus ? TH_TEXT : TH_TEXT_DIM;
        edge = focus ? TH_ACCENT : TH_SEPARATOR;
    }
    fb = CreateSolidBrush(fill);
    pen = CreatePen(PS_SOLID, focus ? max(2, (int) (2 * s)) : 1, edge);
    ob = SelectObject(hdc, fb);
    op = SelectObject(hdc, pen);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(pen);
    DeleteObject(fb);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ink);
    DrawTextA(hdc, label, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void
ms_button(HDC hdc, int i, double s)
{
    mswin_ms_button(hdc, &ms_btn[i], ms_label[i], i == MS_PLAY, ms_hot == i,
                    ms_focus == i, ms_down == i && ms_hot == i, TRUE, s);
}

/* Tab and the arrows go round the buttons: Play, About, Exit, then the
   badge's, if it's there */
static int
ms_next(int f, int dir)
{
    int order[MS_NBTN + FXV_UPD_NBTN], n = 0, i;

    for (i = 0; i < MS_NBTN; i++)
        order[n++] = i;
    if (ms_upd)
        for (i = 0; i < FXV_UPD_NBTN; i++)
            order[n++] = MS_UPD + i;
    for (i = 0; i < n; i++)
        if (order[i] == f)
            return order[(i + dir + n) % n];
    return MS_PLAY;
}

/* the newer-version badge, if there's a newer version: a panel in the
   top left corner, its words, and its buttons in a row */
static void
ms_update_badge(HDC hdc, int w, int h, double s, HFONT words, HFONT btn)
{
    char text[BUFSZ];
    int panel[4], b[FXV_UPD_NBTN][4], i;
    HBRUSH fill;
    HPEN edge;
    HGDIOBJ ob, op, of;
    RECT r;

    ms_upd = fxv_update_badge(text, sizeof text);
    if (!ms_upd) {
        if (ms_focus >= MS_UPD) /* (it was skipped) */
            ms_focus = MS_PLAY;
        return;
    }
    fxv_update_layout(w, h, panel, b);
    SetRect(&ms_upanel, panel[0], panel[1], panel[2], panel[3]);
    for (i = 0; i < FXV_UPD_NBTN; i++)
        SetRect(&ms_ubtn[i], b[i][0], b[i][1], b[i][2], b[i][3]);

    fill = CreateSolidBrush(TH_BG_APP);
    edge = CreatePen(PS_SOLID, 1, TH_SEPARATOR);
    ob = SelectObject(hdc, fill);
    op = SelectObject(hdc, edge);
    RoundRect(hdc, panel[0], panel[1], panel[2], panel[3], FXV_CORNER_GDI,
              FXV_CORNER_GDI);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(edge);
    DeleteObject(fill);

    of = SelectObject(hdc, words);
    SetRect(&r, panel[0], panel[1] + (int) (10 * s), panel[2],
            b[0][1] - (int) (4 * s));
    ms_text(hdc, text, &r, DT_CENTER | DT_TOP | DT_SINGLELINE, TH_ACCENT, 1);
    SelectObject(hdc, btn);
    for (i = 0; i < FXV_UPD_NBTN; i++)
        mswin_ms_button(hdc, &ms_ubtn[i], fxv_upd_label[i], FALSE,
                        ms_hot == MS_UPD + i, ms_focus == MS_UPD + i,
                        ms_down == MS_UPD + i && ms_hot == MS_UPD + i, TRUE,
                        s);
    SelectObject(hdc, of);
}

/* the shared pieces of the full-window screens (see mhcharsel.c) */
HFONT
mswin_ms_font(int px, int weight, BOOL serif)
{
    return ms_font(px, weight, serif);
}

void
mswin_ms_text(HDC hdc, const char *s, RECT *r, UINT how, COLORREF c,
              int shadow)
{
    ms_text(hdc, s, r, how, c, shadow);
}

/* copy the painting, sized to w x h, into 'dst', then darken all of it
   by 'dim' (0..255) so panels and text read over it */
void
mswin_ms_backdrop(HDC hdc, HDC dst, int w, int h, int dim)
{
    HDC bdc;
    HGDIOBJ obb;

    ms_make_bg(hdc, w, h);
    bdc = CreateCompatibleDC(hdc);
    obb = SelectObject(bdc, ms_bg);
    BitBlt(dst, 0, 0, w, h, bdc, 0, 0, SRCCOPY);
    SelectObject(bdc, obb);
    if (dim > 0) {
        HBITMAP one = CreateCompatibleBitmap(hdc, 1, 1);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE) min(dim, 255), 0 };
        RECT px;

        obb = SelectObject(bdc, one);
        SetRect(&px, 0, 0, 1, 1);
        FillRect(bdc, &px, (HBRUSH) GetStockObject(BLACK_BRUSH));
        (void) GdiAlphaBlend(dst, 0, 0, w, h, bdc, 0, 0, 1, 1, bf);
        SelectObject(bdc, obb);
        DeleteObject(one);
    }
    DeleteDC(bdc);
}

void
mswin_ms_backdrop_free(void)
{
    if (ms_bg)
        DeleteObject(ms_bg), ms_bg = NULL;
}

/* NetHack Modern: the menu music.  It starts with the title screen,
   carries on through the name and character screens, and fades out as
   the game begins: when the hero is chosen, when a restored game is
   drawn, or failing those at the first wait for a command (see
   mswproc.c).  The speaker icon on each of those
   screens (or Ctrl+M) turns it off and on; that's kept in the
   fx_menu_music option, in nethackmodern.rc. */

static boolean mm_on;   /* playing now */
static boolean mm_over; /* the game has begun: no more music */
static char mm_path[MAX_PATH + 48];

#define MM_FILE "music_title.mp3"

/* the track: <exedir>\sounds\, or the source copy when running out of
   the build tree (binary\<cfg>\<platform>), as for the other sounds */
static boolean
mm_find(void)
{
    static const char *const tries[] = {
        "sounds\\", "..\\..\\..\\sound\\modern\\"
    };
    char exe[MAX_PATH], *p;
    int i;

    if (!GetModuleFileNameA(NULL, exe, sizeof exe))
        return FALSE;
    if ((p = strrchr(exe, '\\')) != 0)
        p[1] = '\0';
    for (i = 0; i < SIZE(tries); i++) {
        Snprintf(mm_path, sizeof mm_path, "%s%s%s", exe, tries[i], MM_FILE);
        if (GetFileAttributesA(mm_path) != INVALID_FILE_ATTRIBUTES)
            return TRUE;
    }
    mm_path[0] = '\0';
    return FALSE;
}

/* make the music match what's wanted */
static void
mm_sync(void)
{
#ifdef SND_LIB_MINIAUDIO
    boolean want = !mm_over && iflags.fx_menu_music && iflags.sounds;

    if (want == mm_on)
        return;
    if (want) {
        static boolean first = TRUE;

        if (!mm_path[0] && !mm_find())
            return;
        if (!nhma_init())
            return; /* no audio device */
        nhma_set_master_volume(0.8f);
        nhma_music(mm_path, 0.55f, first ? 2500 : 900, 1);
        first = FALSE;
        mm_on = TRUE;
    } else {
        nhma_music((const char *) 0, 0.0f, mm_over ? 2500 : 700, 0);
        mm_on = FALSE;
    }
#endif
}

void
mswin_menu_music_start(void)
{
    mm_sync();
}

/* the game has begun: fade the music out for good */
void
mswin_menu_music_end(void)
{
    if (mm_over)
        return;
    mm_over = TRUE;
    mm_sync();
}

void
mswin_menu_music_toggle(void)
{
    iflags.fx_menu_music = !iflags.fx_menu_music;
    cfg_remember_option("fx_menu_music", iflags.fx_menu_music
                                             ? "fx_menu_music"
                                             : "!fx_menu_music");
    mm_sync();
}

/* where the speaker icon goes: the top right corner */
void
mswin_ms_sound_rect(int w, double s, RECT *r)
{
    int q[4];

    fxv_title_sound_rect(w, s, q); /* shared with SDL (fxview.c) */
    SetRect(r, q[0], q[1], q[2], q[3]);
}

/* the speaker: with sound waves while the music is on, crossed out when
   it's off; pointed at, it says what it does */
void
mswin_ms_sound_icon(HDC hdc, const RECT *r, boolean hot, double s)
{
    boolean on = iflags.fx_menu_music;
    int cx = (r->left + r->right) / 2 - (int) (3 * s),
        cy = (r->top + r->bottom) / 2;
    double u = s * 1.05;
    COLORREF ink = hot ? TH_ACCENT : on ? TH_TEXT : TH_TEXT_DIM;
    HBRUSH bg = CreateSolidBrush(hot ? TH_BG_HEADER : TH_BG_PANEL),
           ib = CreateSolidBrush(ink);
    HPEN edge = CreatePen(PS_SOLID, 1, hot ? TH_ACCENT : TH_SEPARATOR),
         line = CreatePen(PS_SOLID, max(2, (int) (2.2 * u)), ink);
    HGDIOBJ ob = SelectObject(hdc, bg), op = SelectObject(hdc, edge);
    HGDIOBJ obr = GetCurrentObject(hdc, OBJ_BRUSH);
    POINT sp[6];
#define UX(v) (cx + (int) ((v) * u))
#define UY(v) (cy + (int) ((v) * u))

    nhUse(obr);
    Ellipse(hdc, r->left, r->top, r->right, r->bottom);

    /* the speaker itself */
    sp[0].x = UX(-10), sp[0].y = UY(-4);
    sp[1].x = UX(-4), sp[1].y = UY(-4);
    sp[2].x = UX(3), sp[2].y = UY(-10);
    sp[3].x = UX(3), sp[3].y = UY(10);
    sp[4].x = UX(-4), sp[4].y = UY(4);
    sp[5].x = UX(-10), sp[5].y = UY(4);
    SelectObject(hdc, ib);
    SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, sp, 6);

    SelectObject(hdc, line);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    if (on) {
        int k;

        for (k = 1; k <= 2; k++) {
            int rr = (int) ((k == 1 ? 6 : 11) * u), ax = UX(4);

            Arc(hdc, ax - rr, cy - rr, ax + rr, cy + rr, ax + rr, cy + rr,
                ax + rr, cy - rr);
        }
    } else {
        MoveToEx(hdc, UX(8), UY(-5), NULL);
        LineTo(hdc, UX(17), UY(5));
        MoveToEx(hdc, UX(17), UY(-5), NULL);
        LineTo(hdc, UX(8), UY(5));
    }
#undef UX
#undef UY
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(bg);
    DeleteObject(ib);
    DeleteObject(edge);
    DeleteObject(line);

    if (hot) {
        HFONT f = ms_font(max(10, (int) (14 * s)), FW_SEMIBOLD, FALSE);
        HGDIOBJ of = SelectObject(hdc, f);
        RECT t;

        SetRect(&t, r->left - (int) (260 * s), r->top,
                r->left - (int) (10 * s), r->bottom);
        SetBkMode(hdc, TRANSPARENT);
        ms_text(hdc, on ? "Music on  (Ctrl+M)" : "Music off  (Ctrl+M)", &t,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE, TH_TEXT, 1);
        SelectObject(hdc, of);
        DeleteObject(f);
    }
}

static void
ms_paint(HWND hWnd, HDC hdc)
{
    RECT cr, r;
    int w, h, th;
    double s;
    HDC mdc, bdc;
    HBITMAP mbm;
    HGDIOBJ omb, obb, of;
    HFONT title, sub, btn, small, small_b;
    char buf[BUFSZ];

    GetClientRect(hWnd, &cr);
    w = cr.right, h = cr.bottom;
    if (w <= 0 || h <= 0)
        return;
    s = ms_scale(w, h);
    ms_make_bg(hdc, w, h);
    ms_layout(w, h);

    /* draw off screen, then copy, so nothing flickers */
    mdc = CreateCompatibleDC(hdc);
    mbm = CreateCompatibleBitmap(hdc, w, h);
    omb = SelectObject(mdc, mbm);
    bdc = CreateCompatibleDC(hdc);
    obb = SelectObject(bdc, ms_bg);
    BitBlt(mdc, 0, 0, w, h, bdc, 0, 0, SRCCOPY);
    SelectObject(bdc, obb);
    DeleteDC(bdc);
    SetBkMode(mdc, TRANSPARENT);

    th = (int) (96 * s);
    title = ms_font(th, FW_BOLD, TRUE);
    sub = ms_font(max(10, th / 4), FW_SEMIBOLD, FALSE);
    btn = ms_font(max(11, (int) (19 * s)), FW_SEMIBOLD, FALSE);
    small = ms_font(max(10, (int) (14 * s)), FW_NORMAL, FALSE);
    small_b = ms_font(max(10, (int) (15 * s)), FW_SEMIBOLD, FALSE);

    /* the title, just above the buttons */
    of = SelectObject(mdc, title);
    r.left = 0, r.right = w;
    r.bottom = ms_btn[MS_PLAY].top - (int) (22 * s) - th / 4 - th / 6;
    r.top = r.bottom - th - th / 4;
    ms_text(mdc, "NetHack", &r, DT_CENTER | DT_BOTTOM | DT_SINGLELINE,
            TH_ACCENT, max(2, (int) (3 * s)));
    SelectObject(mdc, sub);
    r.top = r.bottom - th / 10;
    r.bottom = r.top + th / 4 + th / 6;
    ms_text(mdc, "M  O  D  E  R  N", &r, DT_CENTER | DT_TOP | DT_SINGLELINE,
            TH_TEXT, max(1, (int) (2 * s)));

    /* the buttons */
    SelectObject(mdc, btn);
    {
        int i;

        for (i = 0; i < MS_NBTN; i++)
            ms_button(mdc, i, s);
    }
    mswin_ms_sound_rect(w, s, &ms_snd);
    mswin_ms_sound_icon(mdc, &ms_snd, ms_hot == MS_SOUND, s);

    /* a newer version: the badge, top left (once the check, on its own
       thread, has an answer; until then this just finds none) */
    ms_update_badge(mdc, w, h, s, small_b, btn);

    /* the versions, bottom left */
    r.left = (int) (24 * s), r.right = w / 2;
    r.bottom = h - (int) (18 * s);
    r.top = r.bottom - (int) (20 * s);
    SelectObject(mdc, small);
    Sprintf(buf, "NetHack %d.%d.%d", VERSION_MAJOR, VERSION_MINOR,
            PATCHLEVEL);
    ms_text(mdc, buf, &r, DT_LEFT | DT_BOTTOM | DT_SINGLELINE, TH_TEXT_DIM,
            1);
    OffsetRect(&r, 0, -(int) (21 * s));
    SelectObject(mdc, small_b);
    Sprintf(buf, "NetHack Modern v%s", MODERN_VERSION_STRING);
    ms_text(mdc, buf, &r, DT_LEFT | DT_BOTTOM | DT_SINGLELINE, TH_ACCENT, 1);

    /* the copyright, bottom right */
    SelectObject(mdc, small);
    r.left = w / 2, r.right = w - (int) (24 * s);
    r.bottom = h - (int) (18 * s);
    r.top = r.bottom - (int) (20 * s);
    ms_text(mdc, COPYRIGHT_BANNER_D, &r, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE,
            TH_TEXT_DIM, 1);
    OffsetRect(&r, 0, -(int) (21 * s));
    ms_text(mdc, COPYRIGHT_BANNER_A, &r, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE,
            TH_TEXT_DIM, 1);

    SelectObject(mdc, of);
    DeleteObject(title);
    DeleteObject(sub);
    DeleteObject(btn);
    DeleteObject(small);
    DeleteObject(small_b);

    BitBlt(hdc, 0, 0, w, h, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, omb);
    DeleteObject(mbm);
    DeleteDC(mdc);
}

static int
ms_hit(int x, int y)
{
    POINT pt;
    int i;

    pt.x = x, pt.y = y;
    for (i = 0; i < MS_NBTN; i++)
        if (PtInRect(&ms_btn[i], pt))
            return i;
    if (PtInRect(&ms_snd, pt))
        return MS_SOUND;
    if (ms_upd)
        for (i = 0; i < FXV_UPD_NBTN; i++)
            if (PtInRect(&ms_ubtn[i], pt))
                return MS_UPD + i;
    return -1;
}

static void
ms_choose(HWND hWnd, int i)
{
    if (i >= MS_UPD && i < MS_UPD + FXV_UPD_NBTN) {
        /* What's new, the download page, or skip it */
        (void) fxv_update_choose(i - MS_UPD);
        GetNHApp()->hPopupWnd = hWnd;
        SetFocus(hWnd);
        InvalidateRect(hWnd, NULL, FALSE);
        return;
    }
    if (i == MS_SOUND) {
        mswin_menu_music_toggle();
        InvalidateRect(hWnd, NULL, FALSE);
        return;
    }
    if (i == MS_ABOUT) {
        /* the full version and build options, as before */
        mswin_display_splash_window(TRUE);
        GetNHApp()->hPopupWnd = hWnd;
        SetFocus(hWnd);
        return;
    }
    ms_result = i;
}

static LRESULT CALLBACK
ms_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        ms_paint(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int hot = ms_hit(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

        if (hot != ms_hot) {
            TRACKMOUSEEVENT tme;

            ms_hot = hot;
            if (hot >= 0 && hot != MS_SOUND)
                ms_focus = hot;
            tme.cbSize = sizeof tme;
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hWnd;
            tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (ms_hot >= 0) {
            ms_hot = -1;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, ms_hot >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        SetFocus(hWnd);
        ms_down = ms_hit(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (ms_down >= 0)
            SetCapture(hWnd);
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int was = ms_down,
            at = ms_hit(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

        ms_down = -1;
        if (GetCapture() == hWnd)
            ReleaseCapture();
        InvalidateRect(hWnd, NULL, FALSE);
        if (was >= 0 && was == at)
            ms_choose(hWnd, at);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        switch (wParam) {
        case VK_LEFT:
        case VK_UP:
            ms_focus = ms_next(ms_focus, -1);
            break;
        case VK_TAB:
            ms_focus = ms_next(ms_focus,
                               (GetKeyState(VK_SHIFT) < 0) ? -1 : 1);
            break;
        case VK_RIGHT:
        case VK_DOWN:
            ms_focus = ms_next(ms_focus, 1);
            break;
        case VK_RETURN:
        case VK_SPACE:
            ms_choose(hWnd, ms_focus);
            return 0;
        case 'M':
            if (GetKeyState(VK_CONTROL) < 0)
                ms_choose(hWnd, MS_SOUND);
            return 0;
        default:
            return 0;
        }
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    case WM_TIMER:
        /* the update check (on its own thread) has an answer: show the
           badge if there's news, and stop looking */
        if (wParam == MS_UPD_TIMER && fxu_status() != FXU_BUSY) {
            KillTimer(hWnd, MS_UPD_TIMER);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    case WM_CHAR: {
        int c = tolower((int) wParam);

        if (fxu_badge() && c && strchr(fxv_upd_key, c)) {
            ms_choose(hWnd, MS_UPD + (int) (strchr(fxv_upd_key, c)
                                            - fxv_upd_key));
            return 0;
        }
        switch (c) {
        case 'p':
            ms_choose(hWnd, MS_PLAY);
            break;
        case 'a':
            ms_choose(hWnd, MS_ABOUT);
            break;
        case 'q':
        case 'x':
            ms_choose(hWnd, MS_EXIT);
            break;
        case 'm':
            ms_choose(hWnd, MS_SOUND);
            break;
        }
        return 0;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

/* keep the title screen filling the game window as it's resized */
void
mswin_menu_screen_place(HWND main)
{
    RECT rc;

    if (!ms_hwnd || !IsWindow(ms_hwnd))
        return;
    GetClientRect(main, &rc);
    SetWindowPos(ms_hwnd, HWND_TOP, 0, 0, rc.right, rc.bottom, 0);
}

/* show the title screen and wait; returns only when Play is chosen
   (Exit, or closing the window, ends the program) */
void
mswin_display_menu_screen(void)
{
    static boolean registered;
    HWND main = GetNHApp()->hMainWnd, bar;
    MSG msg;

    if (!registered) {
        WNDCLASS wc;

        ZeroMemory(&wc, sizeof wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ms_proc;
        wc.hInstance = GetNHApp()->hApp;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = TEXT("MSNHMenuScreen");
        RegisterClass(&wc);
        registered = TRUE;
    }
    /* the topic bar stays hidden until the game starts */
    bar = FindWindowEx(main, NULL, TEXT("MSNHTopBar"), NULL);
    if (bar)
        ShowWindow(bar, SW_HIDE);

    ms_result = -1;
    ms_focus = MS_PLAY, ms_hot = ms_down = -1;
    ms_hwnd = CreateWindow(TEXT("MSNHMenuScreen"), NULL,
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 1,
                           1, main, NULL, GetNHApp()->hApp, NULL);
    if (!ms_hwnd) {
        if (bar)
            ShowWindow(bar, SW_SHOW);
        mswin_play_again = TRUE;
        return; /* no title screen; just play */
    }
    mswin_menu_screen_place(main);
    mswin_menu_music_start();
    /* is there a newer version? It's asked in the background; the timer
       looks for the answer */
    fxv_update_title();
    if (fxu_status() == FXU_BUSY)
        SetTimer(ms_hwnd, MS_UPD_TIMER, 500, NULL);
    GetNHApp()->hPopupWnd = ms_hwnd; /* the main window sends focus here */
    SetForegroundWindow(main);
    SetFocus(ms_hwnd);

    while (ms_result < 0 && GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GetNHApp()->hPopupWnd = NULL;
    DestroyWindow(ms_hwnd);
    ms_hwnd = NULL;
    if (ms_bg)
        DeleteObject(ms_bg), ms_bg = NULL;
    if (bar)
        ShowWindow(bar, SW_SHOW);
    SetFocus(main);

    if (ms_result != MS_PLAY)
        bail((char *) 0); /* Exit: nothing started, nothing to save */
    mswin_play_again = TRUE; /* from now on, a finished game comes back */
}
