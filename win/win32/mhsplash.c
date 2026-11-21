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

    int left = monitorInfo.left + (monitorInfo.width - splashData.window_width) / 2;
    int top = monitorInfo.top + (monitorInfo.height - splashData.window_height) / 2;
    MoveWindow(hWnd, left, top, splashData.window_width, splashData.window_height, TRUE);

    /* Fill the text control */
    strbuf_reserve(&strbuf, BUFSIZ);
    Sprintf(strbuf.str, "%s\n%s\n%s\n%s\n\n", COPYRIGHT_BANNER_A,
            COPYRIGHT_BANNER_B, nomakedefs.copyright_banner_c, COPYRIGHT_BANNER_D);

    if (show_ver) {
        /* Show complete version information */
        dlb *f;
        char verbuf[BUFSZ];
        /* int verstrsize = 0; */

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
            RoundRect(dis->hDC, r.left, r.top, r.right + 1, r.bottom + 1, 10,
                      10);
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
