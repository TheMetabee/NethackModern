/* $NHDT-Header: NetHack 5.0 mhmap.c 2026/07/06_15:00:50 NetHack-5.0:1.118 $ */
/* Copyright (C) 2001 by Alex Kompel      */
/* NetHack may be freely redistributed.  See license for details. */

#include "win10.h"
#include "winMS.h"
#include "winos.h"

#include "mhfont.h"
#include "mhinput.h"
#include "mhmap.h"
#include "mhmsg.h"
#include "resource.h"

#include "color.h"
#if !defined(PATCHLEVEL_H)
#include "patchlevel.h"
#endif
#include "mhtheme.h"
#include "mhmsgwnd.h"
#include "mhend.h"
#include "fxview.h"

#define NHMAP_FONT_NAME TEXT("Terminal")
#define NHMAP_TTFONT_NAME TEXT("Consolas")
#define MAXWINDOWTEXT 255

#define CURSOR_BLINK_INTERVAL 1000 // milliseconds
#define CURSOR_HEIGHT 2 // pixels

/* NetHack Modern floating numbers */
#define CURSOR_TIMER_ID 0
#define FX_TIMER_ID 2
#define FX_AURA_TIMER_ID 3
/* NetHack Modern: map zoom steps (times the normal tile size) */
static const double fx_zooms[] = { 0.75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0 };
#define FX_NZOOM ((int) (sizeof fx_zooms / sizeof fx_zooms[0]))
#define FX_AURA_MS 50       /* lasting effects (sleeping z's) redraw rate */
/* the statuses that get a lasting effect */
#define FX_AURA_MASK (FXA_ASLEEP | FXA_CONFUSED | FXA_STUNNED | FXA_SLOW \
                      | FXA_LEVITATE | FXA_REFLECT | FXA_PROTECT          \
                      | FXA_STONING | FXA_SLIMED | FXA_SICK | FXA_HALLU   \
                      | FXA_INVIS | FXA_CONFLICT)
#define FX_FRAME_MS 30      /* ~33 frames per second while numbers float */

#define TILEBMP_X(ntile) \
    ((ntile % GetNHApp()->mapTilesPerLine) * GetNHApp()->mapTile_X)
#define TILEBMP_Y(ntile) \
    ((ntile / GetNHApp()->mapTilesPerLine) * GetNHApp()->mapTile_Y)

/* map window data */
typedef struct mswin_nethack_map_window {
    HWND hWnd;                  /* window */

    glyph_info map[COLNO][ROWNO];
    glyph_info bkmap[COLNO][ROWNO];
    boolean locDirty[COLNO][ROWNO]; /* dirty flag for map location */
    boolean mapDirty;           /* one or more map locations are dirty */
    int mapMode;                /* current map mode */
    boolean bAsciiMode;         /* switch ASCII/tiled mode */
    boolean bFitToScreenMode;   /* switch Fit map to screen mode on/off */
    int xPos, yPos;             /* scroll position */
    int xPageSize, yPageSize;   /* scroll page size */
    int xMin, xMax, yMin, yMax; /* scroll range */
    int xCur, yCur;             /* position of the cursor */
    int xFrontTile, yFrontTile; /* size of tile in front buffer in pixels */
    int xBackTile, yBackTile;   /* size of tile in back buffer in pixels */
    POINT map_orig;             /* map origin point */

    HFONT hMapFont;             /* font for ASCII mode */
    HFONT hMapFontUnicode;      /* font for Unicode mode */
    boolean bUnicodeFont;       /* font supports unicode page 437 */

    int tileWidth;              /* width of tile in pixels at 96 dpi */
    int tileHeight;             /* height of tile in pixels at 96 dpi */
    double backScale;           /* scaling from source to back buffer */
    double frontScale;          /* scaling from back to front */
    double monitorScale;        /* from 96dpi to monitor dpi*/

    boolean cursorOn;
    int yNoBlinkCursor;         /* non-blinking cursor height inback buffer
                                   in pixels */
    int yBlinkCursor;           /* blinking cursor height inback buffer
                                   in pixels */

    int backWidth;              /* back buffer width */
    int backHeight;             /* back buffer height */
    HBITMAP hBackBuffer;        /* back buffe bitmap */
    HDC backBufferDC;          /* back buffer drawing context */

    HDC tileDC;                /* tile drawing context */
    /* floating numbers and status names are shared: fxv.floaters[] */
    /* creatures drawn off their squares (lunges, shakes, slides) and the
       screen shake are shared too: fxv.anims[], fxv.shake_* */
    HDC fadeDC;                 /* scratch tile for fading sprites */
    HBITMAP fadeBmp, fadeBmpOld;
    int fadeW, fadeH;
    boolean fxTimerOn;
    /* particles for rays and explosions are shared: fxv.parts[] */
    boolean auraTimerOn;        /* lasting effects are being animated */
    boolean zoomSet;            /* the player chose zoomLevel (else auto) */
    int zoomLevel;              /* index into fx_zooms[] */
    int zoomAuto;               /* what auto picked last layout */
    HDC partDC;                 /* black layer the particles are drawn on */
    HBITMAP partBmp, partBmpOld;
    int partW, partH;
    HFONT hFxFont, hFxFontBig;  /* fonts for the numbers */
    HFONT hFxFontSmall;         /* font for status names */
    int fxFontTileH;            /* yFrontTile the fonts were made for */
    HDC compDC;                 /* composition buffer for onPaint */
    HBITMAP compBmp, compBmpOld;
    int compW, compH;

} NHMapWindow, *PNHMapWindow;

static TCHAR szNHMapWindowClass[] = TEXT("MSNethackMapWndClass");

/* NetHack Modern: with the theme the map follows the hero by itself and
   Ctrl+arrows still scroll it, so its grey scroll bars stay hidden */
static void
fx_set_scroll(HWND hWnd, int bar, SCROLLINFO *si, BOOL redraw)
{
    if (iflags.fx_theme) {
        SetScrollInfo(hWnd, bar, si, FALSE);
        ShowScrollBar(hWnd, bar, FALSE);
    } else {
        SetScrollInfo(hWnd, bar, si, redraw);
    }
}
LRESULT CALLBACK MapWndProc(HWND, UINT, WPARAM, LPARAM);
static void register_map_window_class(void);
static void onMSNHCommand(HWND hWnd, WPARAM wParam, LPARAM lParam);
static void onMSNH_VScroll(HWND hWnd, WPARAM wParam, LPARAM lParam);
static void onMSNH_HScroll(HWND hWnd, WPARAM wParam, LPARAM lParam);
static void onPaint(HWND hWnd);
static void onCreate(HWND hWnd, WPARAM wParam, LPARAM lParam);
static void nhcoord2display(PNHMapWindow data, int x, int y, LPRECT lpOut);
static void paint(PNHMapWindow data, int i, int j);
static void dirtyAll(PNHMapWindow data);
static void dirty(PNHMapWindow data, int i, int j);
static void setGlyph(PNHMapWindow data, int i, int j,
                     const glyph_info *fg, const glyph_info *bg);
static void clearAll(PNHMapWindow data);
static void mswin_fx_event(const struct fx_event *ev);
static void fx_tick(PNHMapWindow data);
COLORREF nhcolor_to_RGB(int);
static void fx_paint_particles(PNHMapWindow, HDC);
static void fx_paint_messages(PNHMapWindow, HDC);
static void fx_paint_hero_frame(PNHMapWindow, HDC);
static boolean fx_paint_lighting(PNHMapWindow, HDC);
static int fx_paint_auras(PNHMapWindow, HDC);
static void fx_part_layer(PNHMapWindow, HDC);
static void fx_make_fonts(PNHMapWindow);
static void fx_aura_timer(PNHMapWindow, boolean);
static void fx_paint_overlay(PNHMapWindow data, HDC hdc);
static void fx_paint_translucent(PNHMapWindow, HDC);
static short fx_floor_tile(PNHMapWindow, int, int);
static void fx_paint_sprites(PNHMapWindow, HDC);
static void fx_paint_healthbar(PNHMapWindow data, int i, int j, RECT *rect);
static int fx_spun_x = -1, fx_spun_y = -1; /* a missile was drawn turned */

#if (VERSION_MAJOR < 4) && (VERSION_MINOR < 4) && (PATCHLEVEL < 2)
static void nhglyph2charcolor(short glyph, uchar *ch, int *color);
#endif
extern boolean win32_cursorblink;       /* from sys\windows\windsys.c */

HWND
mswin_init_map_window(void)
{
    static int run_once = 0;
    HWND hWnd;
    RECT rt;

    if (!run_once) {
        register_map_window_class();
        run_once = 1;
    }

    /* get window position */
    if (GetNHApp()->bAutoLayout) {
        SetRect(&rt, 0, 0, 0, 0);
    } else {
        mswin_get_window_placement(NHW_MAP, &rt);
    }

    /* create map window object */
    hWnd = CreateWindow(
        szNHMapWindowClass, /* registered class name */
        NULL,               /* window name */
        WS_CHILD | WS_HSCROLL | WS_VSCROLL | WS_CLIPSIBLINGS
            | WS_SIZEBOX,     /* window style */
        rt.left,              /* horizontal position of window */
        rt.top,               /* vertical position of window */
        rt.right - rt.left,   /* window width */
        rt.bottom - rt.top,   /* window height */
        GetNHApp()->hMainWnd, /* handle to parent or owner window */
        NULL,                 /* menu handle or child identifier */
        GetNHApp()->hApp,     /* handle to application instance */
        NULL);                /* window-creation data */
    if (!hWnd) {
        panic("Cannot create map window");
    }

    /* Set window caption */
    SetWindowText(hWnd, "Map");

    mswin_apply_window_style(hWnd);

    /* set cursor blink timer */
    SetTimer(hWnd, CURSOR_TIMER_ID, CURSOR_BLINK_INTERVAL, NULL);

    /* receive presentation events from the core */
    fx_set_handler(mswin_fx_event);

    return hWnd;
}

void
mswin_map_layout(HWND hWnd, LPSIZE map_size)
{
    /* check arguments */
    if (!IsWindow(hWnd) || !map_size || map_size->cx <= 0
        || map_size->cy <= 0)
        return;

    PNHMapWindow data = (PNHMapWindow) GetWindowLongPtr(hWnd, GWLP_USERDATA);

    /* calculate window size */
    RECT client_rt;
    GetClientRect(hWnd, &client_rt);

    SIZE wnd_size;
    wnd_size.cx = client_rt.right - client_rt.left;
    wnd_size.cy = client_rt.bottom - client_rt.top;

    // calculate back buffer scale
    data->monitorScale = win10_monitor_scale(hWnd);

    boolean bText = data->bAsciiMode ||
                    (u.uz.dlevel != 0 && Is_rogue_level(&u.uz));

    if (bText && !data->bFitToScreenMode)
        data->backScale = data->monitorScale;
    else
        data->backScale = 1.0;

    /* set back buffer tile size */
    if (bText && data->bFitToScreenMode) {
        data->xBackTile = wnd_size.cx / COLNO;
        data->yBackTile = wnd_size.cy / ROWNO;
        data->yBackTile = max(data->yBackTile, 12);
    } else {
        data->xBackTile = (int)(data->tileWidth * data->backScale);
        data->yBackTile = (int)(data->tileHeight * data->backScale);
    }

    if (bText) {
        LOGFONT lgfnt;

        ZeroMemory(&lgfnt, sizeof(lgfnt));
        if (data->bFitToScreenMode) {
            lgfnt.lfHeight = -data->yBackTile;     // height of font
            lgfnt.lfWidth = 0;                     // average character width
        } else {
            lgfnt.lfHeight = -data->yBackTile;     // height of font
            lgfnt.lfWidth = -data->xBackTile;      // average character width
        }
        lgfnt.lfEscapement = 0;                    // angle of escapement
        lgfnt.lfOrientation = 0;                   // base-line orientation angle
        lgfnt.lfWeight = FW_NORMAL;                // font weight
        lgfnt.lfItalic = FALSE;                    // italic attribute option
        lgfnt.lfUnderline = FALSE;                 // underline attribute option
        lgfnt.lfStrikeOut = FALSE;                 // strikeout attribute option
        lgfnt.lfCharSet = mswin_charset();         // character set identifier
        lgfnt.lfOutPrecision = OUT_DEFAULT_PRECIS; // output precision
        lgfnt.lfClipPrecision = CLIP_DEFAULT_PRECIS; // clipping precision
        if (data->bFitToScreenMode) {
            lgfnt.lfQuality = ANTIALIASED_QUALITY; // output quality
        } else {
            lgfnt.lfQuality = NONANTIALIASED_QUALITY; // output quality
        }
        if (iflags.wc_font_map && *iflags.wc_font_map) {
            lgfnt.lfPitchAndFamily = DEFAULT_PITCH; // pitch and family
            NH_A2W(iflags.wc_font_map, lgfnt.lfFaceName, LF_FACESIZE);
        } else {
            if (!data->bFitToScreenMode) {
                lgfnt.lfPitchAndFamily = FIXED_PITCH; // pitch and family
                NH_A2W(NHMAP_FONT_NAME, lgfnt.lfFaceName, LF_FACESIZE);
            } else {
                lgfnt.lfPitchAndFamily = DEFAULT_PITCH; // pitch and family
                NH_A2W(NHMAP_TTFONT_NAME, lgfnt.lfFaceName, LF_FACESIZE);
            }
        }

        TEXTMETRIC textMetrics;
        HFONT font = NULL;

        while (1) {

            if (font != NULL)
                DeleteObject(font);

            font = CreateFontIndirect(&lgfnt);

            SelectObject(data->backBufferDC, font);

            GetTextMetrics(data->backBufferDC, &textMetrics);

            if (!data->bFitToScreenMode)
                break;

            if ((textMetrics.tmHeight > data->yBackTile ||
                 textMetrics.tmAveCharWidth > data->xBackTile) &&
                lgfnt.lfHeight < -MIN_FONT_HEIGHT) {
                lgfnt.lfHeight++;
                continue;
            }

            break;
        }

        if (data->hMapFont)
            DeleteObject(data->hMapFont);

        data->hMapFont = font;

        data->bUnicodeFont = winos_font_support_cp437(data->hMapFont);

        // Same as above, but with ANSI_CHARSET for IBM and Unicode modes
        lgfnt.lfCharSet = ANSI_CHARSET;
        if (data->hMapFontUnicode)
            DeleteObject(data->hMapFontUnicode);
        data->hMapFontUnicode = CreateFontIndirect(&lgfnt);

        // set tile size to match font metrics

        data->xBackTile = textMetrics.tmAveCharWidth;
        data->yBackTile = textMetrics.tmHeight;

    }

    int backWidth = COLNO * data->xBackTile;
    int backHeight = ROWNO * data->yBackTile;

    /* create back buffer */

    if (data->backWidth != backWidth || data->backHeight != backHeight) {

        HDC frontBufferDC = GetDC(hWnd);
        HBITMAP hBackBuffer = CreateCompatibleBitmap(frontBufferDC, backWidth, backHeight);
        ReleaseDC(hWnd, frontBufferDC);

        if (data->hBackBuffer != NULL) {
            SelectBitmap(data->backBufferDC, hBackBuffer);
            DeleteObject(data->hBackBuffer);
        }

        data->backWidth = backWidth;
        data->backHeight = backHeight;

        SelectBitmap(data->backBufferDC, hBackBuffer);
        data->hBackBuffer = hBackBuffer;
    }

    /* calculate front buffer tile size */

    if (wnd_size.cx > 0 && wnd_size.cy > 0 && !bText && data->bFitToScreenMode) {
        double windowAspectRatio =
            (double) wnd_size.cx / (double) wnd_size.cy;

        double backAspectRatio =
            (double) data->backWidth / (double) data->backHeight;

        if (windowAspectRatio > backAspectRatio)
            data->frontScale = (double) wnd_size.cy / (double) data->backHeight;
        else
            data->frontScale = (double) wnd_size.cx / (double) data->backWidth;

    } else {

        if (bText) {
            data->frontScale = 1.0;
        } else {
            data->frontScale = data->monitorScale;
        }
        if (!bText && iflags.fx_theme && wnd_size.cy > 0) {
            /* NetHack Modern: zoom. Automatic shows the whole level; the
               player can zoom in (the map then follows the hero) with
               Ctrl+=/- and Ctrl+wheel, Ctrl+0 goes back to automatic */
            int z;
            /* the steps are sizes on screen, whatever size the tiles are
               drawn at: bigger tiles are shrunk to fit, and look their
               best zoomed in */
            double tref = (double) TILE_X / max(1, data->xBackTile);

            /* automatic: the largest step that shows the whole level */
            data->zoomAuto = 0;
            for (z = FX_NZOOM - 1; z >= 0; z--)
                if (data->yBackTile * data->monitorScale * fx_zooms[z] * tref
                        * ROWNO <= wnd_size.cy
                    && data->xBackTile * data->monitorScale * fx_zooms[z]
                           * tref * COLNO <= wnd_size.cx) {
                    data->zoomAuto = z;
                    break;
                }
            z = data->zoomSet ? data->zoomLevel : data->zoomAuto;
            data->frontScale = data->monitorScale * fx_zooms[z] * tref;
        }

    }

    /* TODO: Should we round instead of clamping? */
    data->xFrontTile = (int) ((double) data->xBackTile * data->frontScale);
    data->yFrontTile = (int) ((double) data->yBackTile * data->frontScale);

    /* ensure tile is at least one pixel in size */
    if (data->xFrontTile < 1) data->xFrontTile = 1;
    if (data->yFrontTile < 1) data->yFrontTile = 1;

    /* ensure front tile is non-zero in size */
    data->xFrontTile = max(data->xFrontTile, 1);
    data->yFrontTile = max(data->yFrontTile, 1);

    /* calculate ASCII cursor height */
    data->yBlinkCursor = (int) ((double) CURSOR_HEIGHT * data->backScale);
    data->yNoBlinkCursor = data->yBackTile;

    /* set map origin point */
    data->map_orig.x =
        max(0, client_rt.left + (wnd_size.cx - data->xFrontTile * COLNO) / 2);
    data->map_orig.y =
        max(0, client_rt.top + (wnd_size.cy - data->yFrontTile * ROWNO) / 2);

    data->map_orig.x -= data->map_orig.x % data->xFrontTile;
    data->map_orig.y -= data->map_orig.y % data->yFrontTile;

    // Set horizontal scroll

    data->xPageSize = min(COLNO, wnd_size.cx / data->xFrontTile);

    GetNHApp()->bNoHScroll = (data->xPageSize == COLNO);

    data->xMin = 0;
    data->xMax = COLNO - data->xPageSize;
    data->xPos = max(0, min(data->xMax, u.ux - (data->xPageSize / 2)));

    SCROLLINFO si;

    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = data->xMin;
    si.nMax = data->xMax;
    si.nPage = 1;
    si.nPos = data->xPos;
    fx_set_scroll(hWnd, SB_HORZ, &si, TRUE);

    data->yPageSize = min(ROWNO, wnd_size.cy / data->yFrontTile);

    GetNHApp()->bNoVScroll = (data->yPageSize == ROWNO);

    data->yMin = 0;
    data->yMax = ROWNO - data->yPageSize;
    data->yPos = max(0, min(data->yMax, u.uy - (data->yPageSize / 2)));

    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = data->yMin;
    si.nMax = data->yMax;
    si.nPage = 1;
    si.nPos = data->yPos;
    fx_set_scroll(hWnd, SB_VERT, &si, TRUE);

    mswin_cliparound(data->xCur, data->yCur);

    // redraw all map locations
    dirtyAll(data);

    // invalidate entire map window
    InvalidateRect(hWnd, NULL, TRUE);
}

/* NetHack Modern: the tiles changed (another tileset picked) */
void
mswin_map_tiles_changed(HWND hWnd)
{
    PNHMapWindow data;
    SIZE sz;

    if (!hWnd || !IsWindow(hWnd))
        return;
    data = (PNHMapWindow) GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (!data)
        return;
    if (!data->bAsciiMode) {
        data->tileWidth = GetNHApp()->mapTile_X;
        data->tileHeight = GetNHApp()->mapTile_Y;
    }
    sz.cx = data->tileWidth * COLNO;
    sz.cy = data->tileHeight * ROWNO;
    mswin_map_layout(hWnd, &sz);
    InvalidateRect(hWnd, NULL, FALSE);
}

/* set map mode */
int
mswin_map_mode(HWND hWnd, int mode)
{
    PNHMapWindow data;
    int oldMode;
    SIZE mapSize;

    data = (PNHMapWindow) GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (mode == data->mapMode)
        return mode;

    oldMode = data->mapMode;
    data->mapMode = mode;

    switch (data->mapMode) {
    case MAP_MODE_ASCII4x6:
        data->bAsciiMode = TRUE;
        data->bFitToScreenMode = FALSE;
        data->tileWidth = 4;
        data->tileHeight = 6;
        break;

    case MAP_MODE_ASCII6x8:
        data->bAsciiMode = TRUE;
        data->bFitToScreenMode = FALSE;
        data->tileWidth = 6;
        data->tileHeight = 8;
        break;

    case MAP_MODE_ASCII8x8:
        data->bAsciiMode = TRUE;
        data->bFitToScreenMode = FALSE;
        data->tileWidth = 8;
        data->tileHeight = 8;
        break;

    case MAP_MODE_ASCII16x8:
        data->bAsciiMode = TRUE;
        data->bFitToScreenMode = FALSE;
        data->tileWidth = 16;
        data->tileHeight = 8;
        break;

    case MAP_MODE_ASCII7x12:
        data->bAsciiMode = TRUE;
        data->bFitToScreenMode = FALSE;
        data->tileWidth = 7;
        data->tileHeight = 12;
        break;

    case MAP_MODE_ASCII8x12:
        data->bAsciiMode = TRUE;
        data->bFitToScreenMode = FALSE;
        data->tileWidth = 8;
        data->tileHeight = 12;
        break;

    case MAP_MODE_ASCII16x12:
        data->bAsciiMode = TRUE;
        data->bFitToScreenMode = FALSE;
        data->tileWidth = 16;
        data->tileHeight = 12;
        break;

    case MAP_MODE_ASCII12x16:
        data->bAsciiMode = TRUE;
        data->bFitToScreenMode = FALSE;
        data->tileWidth = 12;
        data->tileHeight = 16;
        break;

    case MAP_MODE_ASCII10x18:
        data->bAsciiMode = TRUE;
        data->bFitToScreenMode = FALSE;
        data->tileWidth = 10;
        data->tileHeight = 18;
        break;

    case MAP_MODE_ASCII_FIT_TO_SCREEN:
        data->bAsciiMode = TRUE;
        data->bFitToScreenMode = TRUE;
        data->tileWidth = 12;
        data->tileHeight = 16;
        break;

    case MAP_MODE_TILES_FIT_TO_SCREEN:
        data->bAsciiMode = FALSE;
        data->bFitToScreenMode = TRUE;
        data->tileWidth = GetNHApp()->mapTile_X;
        data->tileHeight = GetNHApp()->mapTile_Y;
        break;

    case MAP_MODE_TILES:
    default:
        data->bAsciiMode = FALSE;
        data->bFitToScreenMode = FALSE;
        data->tileWidth = GetNHApp()->mapTile_X;
        data->tileHeight = GetNHApp()->mapTile_Y;
        break;
    }

    mapSize.cx = data->tileWidth * COLNO;
    mapSize.cy = data->tileHeight * ROWNO;

    mswin_map_layout(hWnd, &mapSize);

    mswin_update_inventory(0); /* for perm_invent to hide/show tiles */

    return oldMode;
}

void mswin_map_update(HWND hWnd)
{
    PNHMapWindow data = (PNHMapWindow)GetWindowLongPtr(hWnd, GWLP_USERDATA);

    if (data->mapDirty)
    {
        /* update back buffer */
        HBITMAP savedBitmap = SelectObject(data->tileDC, GetNHApp()->bmpMapTiles);

        for (int i = 0; i < COLNO; i++)
            for (int j = 0; j < ROWNO; j++)
                if (data->locDirty[i][j])
                {
                    paint(data, i, j);
                    RECT rect;
                    nhcoord2display(data, i, j, &rect);
                    InvalidateRect(data->hWnd, &rect, FALSE);
                }

        SelectObject(data->tileDC, savedBitmap);
        data->mapDirty = FALSE;
    }

}

/* register window class for map window */
void
register_map_window_class(void)
{
    WNDCLASS wcex;
    ZeroMemory(&wcex, sizeof(wcex));

    /* window class */
    wcex.style = CS_NOCLOSE | CS_DBLCLKS;
    wcex.lpfnWndProc = (WNDPROC) MapWndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = GetNHApp()->hApp;
    wcex.hIcon = NULL;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground =
        CreateSolidBrush(RGB(0, 0, 0)); /* set backgroup here */
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = szNHMapWindowClass;

    if (!RegisterClass(&wcex)) {
        panic("cannot register Map window class");
    }
}

/* map window procedure */
LRESULT CALLBACK
MapWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    PNHMapWindow data;

    data = (PNHMapWindow) GetWindowLongPtr(hWnd, GWLP_USERDATA);
    switch (message) {
    case WM_CREATE:
        onCreate(hWnd, wParam, lParam);
        break;

    case WM_MSNH_COMMAND:
        onMSNHCommand(hWnd, wParam, lParam);
        break;

    case WM_PAINT:
        onPaint(hWnd);
        break;

    case WM_SETFOCUS:
        /* transfer focus back to the main window */
        SetFocus(GetNHApp()->hMainWnd);
        break;

    case WM_HSCROLL:
        onMSNH_HScroll(hWnd, wParam, lParam);
        break;

    case WM_VSCROLL:
        onMSNH_VScroll(hWnd, wParam, lParam);
        break;

    case WM_SIZE: {
        RECT rt;
        SIZE size;

        if (data->bFitToScreenMode) {
            size.cx = LOWORD(lParam);
            size.cy = HIWORD(lParam);
        } else {
            /* mapping factor is unchanged we just need to adjust scroll bars
             */
            size.cx = data->xFrontTile * COLNO;
            size.cy = data->yFrontTile * ROWNO;
        }
        mswin_map_layout(hWnd, &size);

        /* update window placement */
        GetWindowRect(hWnd, &rt);
        ScreenToClient(GetNHApp()->hMainWnd, (LPPOINT) &rt);
        ScreenToClient(GetNHApp()->hMainWnd, ((LPPOINT) &rt) + 1);
        mswin_update_window_placement(NHW_MAP, &rt);
    } break;

    case WM_MOVE: {
        RECT rt;
        GetWindowRect(hWnd, &rt);
        ScreenToClient(GetNHApp()->hMainWnd, (LPPOINT) &rt);
        ScreenToClient(GetNHApp()->hMainWnd, ((LPPOINT) &rt) + 1);
        mswin_update_window_placement(NHW_MAP, &rt);
    }
        return FALSE;

    case WM_LBUTTONDOWN:
        NHEVENT_MS(CLICK_1,
                   max(0, min(COLNO, data->xPos
                                         + (LOWORD(lParam) - data->map_orig.x)
                                               / data->xFrontTile)),
                   max(0, min(ROWNO, data->yPos
                                         + (HIWORD(lParam) - data->map_orig.y)
                                               / data->yFrontTile)));
        return 0;

    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
        NHEVENT_MS(CLICK_2,
                   max(0, min(COLNO, data->xPos
                                         + (LOWORD(lParam) - data->map_orig.x)
                                               / data->xFrontTile)),
                   max(0, min(ROWNO, data->yPos
                                         + (HIWORD(lParam) - data->map_orig.y)
                                               / data->yFrontTile)));
        return 0;

    case WM_DESTROY:
        fx_set_handler((fx_handler_fn) 0);
        if (data->fxTimerOn)
            KillTimer(hWnd, FX_TIMER_ID);
            KillTimer(hWnd, FX_AURA_TIMER_ID);
        if (data->hFxFont)
            DeleteObject(data->hFxFont);
        if (data->hFxFontBig)
            DeleteObject(data->hFxFontBig);
        if (data->hFxFontSmall)
            DeleteObject(data->hFxFontSmall);
        if (data->fadeDC) {
            SelectObject(data->fadeDC, data->fadeBmpOld);
            DeleteObject(data->fadeBmp);
            DeleteDC(data->fadeDC);
        }
        if (data->partDC) {
            SelectObject(data->partDC, data->partBmpOld);
            DeleteObject(data->partBmp);
            DeleteDC(data->partDC);
        }
        if (data->compDC) {
            SelectObject(data->compDC, data->compBmpOld);
            DeleteObject(data->compBmp);
            DeleteDC(data->compDC);
        }
        if (data->hMapFont)
            DeleteObject(data->hMapFont);
        if (data->hMapFontUnicode)
            DeleteObject(data->hMapFontUnicode);
        if (data->hBackBuffer)
            DeleteBitmap(data->hBackBuffer);
        if (data->backBufferDC)
            DeleteDC(data->backBufferDC);
        free(data);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR) 0);
        windowdata[NHW_MAP].address = 0;
        break;

    case WM_TIMER:
        if (wParam == FX_TIMER_ID) {
            fx_tick(data);
            break;
        }
        if (wParam == FX_AURA_TIMER_ID) {
            InvalidateRect(hWnd, NULL, FALSE); /* paint stops it when idle */
            break;
        }
        data->cursorOn = !data->cursorOn;
        dirty(data, data->xCur, data->yCur);
        break;

    case WM_DPICHANGED: {
        RECT rt;
        GetWindowRect(hWnd, &rt);
        ScreenToClient(GetNHApp()->hMainWnd, (LPPOINT)&rt);
        ScreenToClient(GetNHApp()->hMainWnd, ((LPPOINT)&rt) + 1);
        mswin_update_window_placement(NHW_MAP, &rt);
    } break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

/* on WM_COMMAND */
void
onMSNHCommand(HWND hWnd, WPARAM wParam, LPARAM lParam)
{
    PNHMapWindow data;

    data = (PNHMapWindow) GetWindowLongPtr(hWnd, GWLP_USERDATA);
    switch (wParam) {
    case MSNH_MSG_PRINT_GLYPH: {
        PMSNHMsgPrintGlyph msg_data = (PMSNHMsgPrintGlyph) lParam;
        setGlyph(data, msg_data->x, msg_data->y,
            &msg_data->glyphinfo, &msg_data->bkglyphinfo);
        if (msg_data->x == fx_spun_x && msg_data->y == fx_spun_y) {
            /* NetHack Modern: a missile was drawn turned here */
            fx_spun_x = fx_spun_y = -1;
            data->locDirty[msg_data->x][msg_data->y] = TRUE;
            data->mapDirty = TRUE;
        }
        if (iflags.fx_auras
            && ((msg_data->glyphinfo.fxauras & FX_AURA_MASK)
                || msg_data->glyphinfo.glyph == cmap_to_glyph(S_poisoncloud)
                || msg_data->glyphinfo.glyph == cmap_to_glyph(S_cloud)))
            fx_aura_timer(data, TRUE); /* lasting effects, drifting gas */
        if (msg_data->glyphinfo.glyph == cmap_to_glyph(S_digbeam))
            fxv_dig_particles(msg_data->x, msg_data->y);
        if (glyph_is_cmap_zap(msg_data->glyphinfo.glyph))
            fxv_ray_particles(msg_data->x, msg_data->y,
                             (msg_data->glyphinfo.glyph - GLYPH_ZAP_OFF) >> 2,
                             (msg_data->glyphinfo.glyph - GLYPH_ZAP_OFF) & 3);
    } break;

    case MSNH_MSG_CLIPAROUND: {
        PMSNHMsgClipAround msg_data = (PMSNHMsgClipAround) lParam;
        int x, y;
        BOOL scroll_x, scroll_y;
        int mcam = iflags.wc_scroll_margin;

        /* calculate if you should clip around */
        scroll_x =
            !GetNHApp()->bNoHScroll
            && (msg_data->x < (data->xPos + mcam)
                || msg_data->x > (data->xPos + data->xPageSize - mcam));
        scroll_y =
            !GetNHApp()->bNoVScroll
            && (msg_data->y < (data->yPos + mcam)
                || msg_data->y > (data->yPos + data->yPageSize - mcam));

        mcam += iflags.wc_scroll_amount - 1;
        /* get page size and center horizontally on x-position */
        if (scroll_x) {
            if (data->xPageSize <= 2 * mcam) {
                x = max(0, min(COLNO, msg_data->x - data->xPageSize / 2));
            } else if (msg_data->x < data->xPos + data->xPageSize / 2) {
                x = max(0, min(COLNO, msg_data->x - mcam));
            } else {
                x = max(0, min(COLNO, msg_data->x - data->xPageSize + mcam));
            }
            SendMessage(hWnd, WM_HSCROLL, (WPARAM) MAKELONG(SB_THUMBTRACK, x),
                        (LPARAM) NULL);
        }

        /* get page size and center vertically on y-position */
        if (scroll_y) {
            if (data->yPageSize <= 2 * mcam) {
                y = max(0, min(ROWNO, msg_data->y - data->yPageSize / 2));
            } else if (msg_data->y < data->yPos + data->yPageSize / 2) {
                y = max(0, min(ROWNO, msg_data->y - mcam));
            } else {
                y = max(0, min(ROWNO, msg_data->y - data->yPageSize + mcam));
            }
            SendMessage(hWnd, WM_VSCROLL, (WPARAM) MAKELONG(SB_THUMBTRACK, y),
                        (LPARAM) NULL);
        }
    } break;

    case MSNH_MSG_CLEAR_WINDOW:
        clearAll(data);
        break;

    case MSNH_MSG_CURSOR: {
        PMSNHMsgCursor msg_data = (PMSNHMsgCursor) lParam;

        if (data->xCur != msg_data->x || data->yCur != msg_data->y) {

            dirty(data, data->xCur, data->yCur);
            dirty(data, msg_data->x, msg_data->y);

            data->xCur = msg_data->x;
            data->yCur = msg_data->y;
        }

    } break;

    case MSNH_MSG_GETTEXT: {
        PMSNHMsgGetText msg_data = (PMSNHMsgGetText) lParam;
        size_t index;
        int col, row;
#if 0
        int color;
        unsigned special = 0U;
#endif
        int mgch;

        index = 0;
        for (row = 0; row < ROWNO; row++) {
            for (col = 0; col < COLNO; col++) {
                if (index >= msg_data->max_size)
                    break;
                if (data->map[col][row].glyph == NO_GLYPH)
                    mgch = ' ';
                msg_data->buffer[index] = data->map[col][row].ttychar;
                index++;
            }
            if (index >= msg_data->max_size - 1)
                break;
            msg_data->buffer[index++] = '\r';
            msg_data->buffer[index++] = '\n';
        }
        nhUse(mgch);
    } break;

#ifdef ENHANCED_SYMBOLS
    case MSNH_MSG_GETWIDETEXT: {
        PMSNHMsgGetWideText msg_data = (PMSNHMsgGetWideText) lParam;
        size_t index;
        int col, row;

        index = 0;
        for (row = 0; row < ROWNO; row++) {
            for (col = 0; col < COLNO; col++) {
                glyph_info *glyphinfo;
                uint32 ch;
                if (index >= msg_data->max_size)
                    break;
                glyphinfo = &data->map[col][row];
                if (glyphinfo->gm.u && glyphinfo->gm.u->utf8str) {
                    ch = glyphinfo->gm.u->utf32ch;
                } else {
                    ch = glyphinfo->ttychar;
                }
                winos_ascii_to_wide(msg_data->buffer + index, ch);
                index += wcslen(msg_data->buffer + index);
            }
            if (index >= msg_data->max_size - 1)
                break;
            msg_data->buffer[index++] = '\r';
            msg_data->buffer[index++] = '\n';
        }
    } break;
#endif

    case MSNH_MSG_RANDOM_INPUT:
        nhassert(0); // unexpected
        break;

    } /* end switch(wParam) */
}

/* what the shared effects (fxview.h) need from this window: the clock,
   and a repaint timer while something is moving */
static void fx_start_timer(PNHMapWindow);

static fxv_ms
mswin_fxv_now(void)
{
    return (fxv_ms) GetTickCount();
}

static void
mswin_fxv_start_timer(void)
{
    PNHMapWindow data = (PNHMapWindow) windowdata[NHW_MAP].address;

    if (data)
        fx_start_timer(data);
}

static const glyph_info *
mswin_fxv_glyph_at(int x, int y)
{
    PNHMapWindow data = (PNHMapWindow) windowdata[NHW_MAP].address;

    return data ? &data->map[x][y] : &nul_glyphinfo;
}

static boolean
mswin_fxv_tiles_shown(void)
{
    PNHMapWindow data = (PNHMapWindow) windowdata[NHW_MAP].address;

    return data && !data->bAsciiMode && !Is_rogue_level(&u.uz);
}

/* on WM_CREATE */
void
onCreate(HWND hWnd, WPARAM wParam, LPARAM lParam)
{
    PNHMapWindow data;

    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    /* set window data */
    data = (PNHMapWindow) malloc(sizeof(NHMapWindow));
    if (!data)
        panic("out of memory");

    ZeroMemory(data, sizeof(NHMapWindow));

    data->hWnd = hWnd;

    /* the shared effects (win/share/fxview.c) start empty too */
    {
        static const struct fxv_hooks hooks = {
            mswin_fxv_now, mswin_fxv_start_timer, mswin_fxv_glyph_at,
            mswin_fxv_tiles_shown, nhcolor_to_RGB
        };

        (void) memset(&fxv, 0, sizeof fxv);
        fxv_init(&hooks);
    }

    data->bAsciiMode = FALSE;
    data->cursorOn = TRUE;

    data->xFrontTile = GetNHApp()->mapTile_X;
    data->yFrontTile = GetNHApp()->mapTile_Y;
    data->tileWidth = GetNHApp()->mapTile_X;
    data->tileHeight = GetNHApp()->mapTile_Y;

    HDC hDC = GetDC(hWnd);
    data->backBufferDC = CreateCompatibleDC(hDC);
    data->tileDC = CreateCompatibleDC(hDC);
    ReleaseDC(hWnd, hDC);

    SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR) data);
    windowdata[NHW_MAP].address = (genericptr_t) data;

    clearAll(data);

}

/* NetHack Modern: a thrown or fired object in flight is turned to point
   the way it's going.  Which way the tile's own picture points is
   measured from the picture: the long axis of what's drawn, with the tip
   taken as its upper end (weapon art points up) or, when it lies flat,
   its right end.  Things that aren't long and thin (rocks, potions,
   boulders) are left as they are. */
#define FX_PI 3.14159265358979

static boolean
fx_paint_spun(PNHMapWindow data, int i, int j, int tile, const RECT *rect,
              int hdx, int hdy, boolean overbk)
{
    static HDC srcDC, dstDC;
    static HBITMAP srcBmp, dstBmp;
    static DWORD *src, *dst;
    static int sw, sh, dw, dh;
    int tw = GetNHApp()->mapTile_X, th = GetNHApp()->mapTile_Y;
    int w = data->xBackTile, h = data->yBackTile;
    boolean builtin = (GetNHApp()->bmpMapTiles == GetNHApp()->bmpTiles);
    int x, y, n = 0, margin;
    double sx = 0, sy = 0, cxx = 0, cyy = 0, cxy = 0, cx, cy, a, l1, l2,
           tr, det, ux, uy, rad = 0, rot, scale, cr, sr;
    static unsigned char *mask;
    static int masksz;
    BLENDFUNCTION bf;
    int fx_spin_floor;
    glyph_info floorgi;

    if (tw < 4 || th < 4 || w < 4 || h < 4)
        return FALSE;
    /* a bare room floor: what tile sets that draw everything on a floor
       drew this one on */
    map_glyphinfo(i, j, cmap_to_glyph(S_room), 0, &floorgi);
    fx_spin_floor = floorgi.gm.tileidx;
    /* the tile (and a bare floor below it) as plain pixels */
    if (!srcDC || sw != tw || sh != th * 2) {
        BITMAPINFO bi;

        if (!srcDC)
            srcDC = CreateCompatibleDC(data->backBufferDC);
        if (srcBmp)
            DeleteObject(srcBmp);
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = tw;
        bi.bmiHeader.biHeight = -(th * 2);
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        srcBmp = CreateDIBSection(srcDC, &bi, DIB_RGB_COLORS,
                                  (void **) &src, NULL, 0);
        if (!srcBmp)
            return FALSE;
        SelectObject(srcDC, srcBmp);
        sw = tw, sh = th * 2;
    }
    if (masksz < tw * th) {
        free(mask);
        mask = (unsigned char *) malloc(tw * th);
        masksz = mask ? tw * th : 0;
        if (!mask)
            return FALSE;
    }
    BitBlt(srcDC, 0, 0, tw, th, data->tileDC, TILEBMP_X(tile),
           TILEBMP_Y(tile), SRCCOPY);
    if (!builtin && fx_spin_floor >= 0)
        BitBlt(srcDC, 0, th, tw, th, data->tileDC, TILEBMP_X(fx_spin_floor),
               TILEBMP_Y(fx_spin_floor), SRCCOPY);
    GdiFlush();

    /* what's drawn: not the see-through colour, and (for tile sets that
       draw everything on a floor) not the floor; their frame is left out */
    margin = builtin ? 0 : tw / 12;
    for (y = 0; y < th; y++)
        for (x = 0; x < tw; x++) {
            DWORD p = src[y * tw + x];
            int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            boolean on = !(r == 71 && g == 108 && b == 108);

            if (on && !builtin) {
                if (x < margin || y < margin || x >= tw - margin
                    || y >= th - margin) {
                    on = FALSE;
                } else if (fx_spin_floor >= 0) {
                    DWORD q = src[(th + y) * tw + x];
                    int d = max(abs(r - (int) ((q >> 16) & 255)),
                                max(abs(g - (int) ((q >> 8) & 255)),
                                    abs(b - (int) (q & 255))));

                    on = (d > 28);
                }
            }
            mask[y * tw + x] = (unsigned char) on;
            if (on) {
                n++, sx += x, sy += y;
                cxx += (double) x * x, cyy += (double) y * y,
                cxy += (double) x * y;
            }
        }
    if (n < 6)
        return FALSE;
    cx = sx / n, cy = sy / n;
    cxx = cxx / n - cx * cx, cyy = cyy / n - cy * cy, cxy = cxy / n - cx * cy;
    tr = cxx + cyy, det = cxx * cyy - cxy * cxy;
    l1 = tr / 2 + sqrt(max(0.0, tr * tr / 4 - det));
    l2 = tr / 2 - sqrt(max(0.0, tr * tr / 4 - det));
    if (l1 < 3.0 * max(l2, 0.01))
        return FALSE; /* not long and thin */
    a = 0.5 * atan2(2 * cxy, cxx - cyy); /* the long axis (y down) */
    ux = cos(a), uy = sin(a);
    {
        /* which end is the tip: the steel one (blade, point, arrowhead:
           bright and colourless) where that's clear, else the narrower
           one (feathers, flights and hilts are wide), else the upper one
           (weapon art points up), or the right one if it lies flat.
           (ux, uy) points to the 'h' end; 'l' is the other. */
        double tmax = 0, sh = 0, sl = 0, wh = 0, wl = 0;
        int nh = 0, nl = 0;
        boolean flip;

        for (y = 0; y < th; y++)
            for (x = 0; x < tw; x++)
                if (mask[y * tw + x])
                    tmax = max(tmax, fabs((x - cx) * ux + (y - cy) * uy));
        for (y = 0; y < th; y++)
            for (x = 0; x < tw; x++)
                if (mask[y * tw + x]) {
                    double t = (x - cx) * ux + (y - cy) * uy,
                           s = fabs(-(x - cx) * uy + (y - cy) * ux);
                    DWORD p = src[y * tw + x];
                    int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                    int hi = max(r, max(g, b)), lo = min(r, min(g, b));
                    double steel = (hi / 255.0)
                                   * (1.0 - (double) (hi - lo) / max(hi, 1));

                    if (t > tmax / 2)
                        sh += steel, wh += s, nh++;
                    else if (t < -tmax / 2)
                        sl += steel, wl += s, nl++;
                }
        if (nh)
            sh /= nh, wh /= nh;
        if (nl)
            sl /= nl, wl /= nl;
        if (fabs(sh - sl) > 0.08)
            flip = (sl > sh);
        else if (fabs(wh - wl) > 0.15 * max(wh, wl))
            flip = (wl < wh);
        else
            flip = (fabs(uy) > 0.26) ? (uy > 0) : (ux < 0);
        if (flip)
            a += FX_PI;
    }
    /* now 'a' is the way the picture points; turn it to the heading */
    rot = atan2((double) hdy, (double) hdx) - a;
    rot = fmod(rot + 3 * FX_PI, 2 * FX_PI) - FX_PI;
    if (fabs(rot) < 0.05)
        return FALSE; /* already points that way */

    /* shrink it if turning it would take it past the square's edge */
    for (y = 0; y < th; y++)
        for (x = 0; x < tw; x++)
            if (mask[y * tw + x])
                rad = max(rad, sqrt((x + 0.5 - cx) * (x + 0.5 - cx)
                                    + (y + 0.5 - cy) * (y + 0.5 - cy)));
    scale = min(1.0, (min(tw, th) * 0.5) / max(rad, 1.0));

    if (!dstDC || dw != w || dh != h) {
        BITMAPINFO bi;

        if (!dstDC)
            dstDC = CreateCompatibleDC(data->backBufferDC);
        if (dstBmp)
            DeleteObject(dstBmp);
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dstBmp = CreateDIBSection(dstDC, &bi, DIB_RGB_COLORS,
                                  (void **) &dst, NULL, 0);
        if (!dstBmp)
            return FALSE;
        SelectObject(dstDC, dstBmp);
        dw = w, dh = h;
    }
    /* each pixel of the square, back through the turn to the picture
       (smoothly between its pixels), premultiplied for AlphaBlend */
    cr = cos(-rot), sr = sin(-rot);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            double qx = ((x + 0.5) * tw / w - tw * 0.5) / scale,
                   qy = ((y + 0.5) * th / h - th * 0.5) / scale;
            double px = cx + 0.5 + qx * cr - qy * sr - 0.5,
                   py = cy + 0.5 + qx * sr + qy * cr - 0.5;
            int x0 = (int) floor(px), y0 = (int) floor(py), k;
            double fx = px - x0, fy = py - y0, acc[4] = { 0, 0, 0, 0 };

            for (k = 0; k < 4; k++) {
                int xx = x0 + (k & 1), yy = y0 + (k >> 1);
                double wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                DWORD p;

                if (xx < 0 || yy < 0 || xx >= tw || yy >= th
                    || !mask[yy * tw + xx])
                    continue;
                p = src[yy * tw + xx];
                acc[0] += wgt * ((p >> 16) & 255);
                acc[1] += wgt * ((p >> 8) & 255);
                acc[2] += wgt * (p & 255);
                acc[3] += wgt;
            }
            dst[y * w + x] = ((DWORD) (acc[3] * 255 + 0.5) << 24)
                             | ((DWORD) (acc[0] + 0.5) << 16)
                             | ((DWORD) (acc[1] + 0.5) << 8)
                             | (DWORD) (acc[2] + 0.5);
        }
    GdiFlush();
    if (!overbk) {
        /* nothing under it was sent (it's in flight): the floor there */
        int bk = fx_floor_tile(data, i, j);

        if (bk >= 0)
            StretchBlt(data->backBufferDC, rect->left, rect->top, w, h,
                       data->tileDC, TILEBMP_X(bk), TILEBMP_Y(bk), tw, th,
                       SRCCOPY);
        else
            FillRect(data->backBufferDC, rect,
                     (HBRUSH) GetStockObject(BLACK_BRUSH));
    }
    fx_spun_x = i, fx_spun_y = j;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    return GdiAlphaBlend(data->backBufferDC, rect->left, rect->top, w, h,
                         dstDC, 0, 0, w, h, bf);
}

static void
paintTile(PNHMapWindow data, int i, int j, RECT * rect)
{
    short ntile;
    int t_x, t_y;
    int glyph, bkglyph;
    int layer;
#ifdef USE_PILEMARK
//    int color;
//    unsigned special = 0U;
//    int mgch;
#endif
    layer = 0;
    glyph = data->map[i][j].glyph;
    bkglyph = data->bkmap[i][j].glyph;

    if (glyph == NO_GLYPH && bkglyph == NO_GLYPH) {
        HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(data->backBufferDC, rect, blackBrush);
        DeleteObject(blackBrush);
    }


    if (glyph != NO_GLYPH) {
        if (bkglyph != NO_GLYPH) {
            ntile = data->bkmap[i][j].gm.tileidx;
            t_x = TILEBMP_X(ntile);
            t_y = TILEBMP_Y(ntile);

            StretchBlt(data->backBufferDC, rect->left, rect->top,
                       data->xBackTile, data->yBackTile, data->tileDC, t_x,
                       t_y, GetNHApp()->mapTile_X, GetNHApp()->mapTile_Y,
                       SRCCOPY);
            layer++;
        }
        if (glyph != bkglyph) {
            int fdx, fdy;

            /* rely on tileidx provided by NetHack core */
            ntile = data->map[i][j].gm.tileidx;
            t_x = TILEBMP_X(ntile);
            t_y = TILEBMP_Y(ntile);

            if (iflags.fx_theme && iflags.fx_turn_missiles
                && glyph_is_object(glyph)
                && fx_flight_at(i, j, &fdx, &fdy)
                && fx_paint_spun(data, i, j, ntile, rect, fdx, fdy,
                                 layer > 0 && bkglyph != GLYPH_UNEXPLORED)) {
                ; /* NetHack Modern: a missile, pointing where it's going */
            /* Don't use all black GLYPH_UNEXPLORED tile as a background */
            } else if (layer > 0 && bkglyph != GLYPH_UNEXPLORED) {
                (*GetNHApp()->lpfnTransparentBlt)(
                    data->backBufferDC, rect->left, rect->top,
                    data->xBackTile, data->yBackTile, data->tileDC, t_x,
                    t_y, GetNHApp()->mapTile_X,
                    GetNHApp()->mapTile_Y, TILE_BK_COLOR);
            } else {
                StretchBlt(data->backBufferDC, rect->left, rect->top,
                            data->xBackTile, data->yBackTile, data->tileDC,
                            t_x, t_y, GetNHApp()->mapTile_X,
                            GetNHApp()->mapTile_Y, SRCCOPY);
            }
            layer++;
        }

#ifdef USE_PILEMARK
        if ((data->map[i][j].gm.glyphflags & MG_PET) != 0
#else
        if (glyph_is_pet(glyph)
#endif
            && iflags.wc_hilite_pet) {
            /* apply pet mark transparently over
                pet image */
            HDC hdcPetMark;
            HBITMAP bmPetMarkOld;

            /* this is DC for petmark bitmap */
            hdcPetMark = CreateCompatibleDC(data->backBufferDC);
            bmPetMarkOld =
                SelectObject(hdcPetMark, GetNHApp()->bmpPetMark);

            (*GetNHApp()->lpfnTransparentBlt)(
                data->backBufferDC, rect->left, rect->top,
                data->xBackTile, data->yBackTile, hdcPetMark, 0, 0,
                TILE_X, TILE_Y, TILE_BK_COLOR);
            SelectObject(hdcPetMark, bmPetMarkOld);
            DeleteDC(hdcPetMark);
        }
#ifdef USE_PILEMARK
        if ((data->map[i][j].gm.glyphflags & MG_OBJPILE) != 0
            && iflags.hilite_pile) {
            /* apply pilemark transparently over other image */
            HDC hdcPileMark;
            HBITMAP bmPileMarkOld;

            /* this is DC for pilemark bitmap */
            hdcPileMark = CreateCompatibleDC(data->backBufferDC);
            bmPileMarkOld = SelectObject(hdcPileMark,
                                            GetNHApp()->bmpPileMark);

            (*GetNHApp()->lpfnTransparentBlt)(
                data->backBufferDC, rect->left, rect->top,
                data->xBackTile, data->yBackTile, hdcPileMark, 0, 0,
                TILE_X, TILE_Y, TILE_BK_COLOR);
            SelectObject(hdcPileMark, bmPileMarkOld);
            DeleteDC(hdcPileMark);
        }
#endif
        fx_paint_healthbar(data, i, j, rect);
    }       /* glyph != NO_GLYPH */

    if (i == data->xCur && j == data->yCur &&
        (data->cursorOn || !win32_cursorblink))
        DrawFocusRect(data->backBufferDC, rect);
}


static void
paintGlyph(PNHMapWindow data, int i, int j, RECT * rect)
{
    if (data->map[i][j].glyph >= 0) {

        glyph_info *glyphinfo;
        uint32 ch;
        WCHAR wch[3];
        int color;
        uint32 rgbcolor;
//        unsigned special;
//        int mgch;
        HBRUSH back_brush;
        COLORREF OldFg;

        SetBkMode(data->backBufferDC, TRANSPARENT);

        HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(data->backBufferDC, rect, blackBrush);
        DeleteObject(blackBrush);

        glyphinfo = &data->map[i][j];
        ch = glyphinfo->ttychar;
        color = (int) glyphinfo->gm.sym.color;
        rgbcolor = nhcolor_to_RGB(color);
#ifdef ENHANCED_SYMBOLS
        if (SYMHANDLING(H_UTF8)
            && glyphinfo->gm.u
            && glyphinfo->gm.u->utf8str) {
            ch = glyphinfo->gm.u->utf32ch;
        }
#endif
        if (glyphinfo->gm.customcolor != 0
            && (mswin_procs.wincap2 & WC2_EXTRACOLORS) != 0) {
            if ((glyphinfo->gm.customcolor & NH_BASIC_COLOR) != 0) {
                color = (int) COLORVAL(glyphinfo->gm.customcolor);
                rgbcolor = nhcolor_to_RGB(color);
            }
        }
        if ((((data->map[i][j].gm.glyphflags & MG_PET) && iflags.hilite_pet)
             || ((data->map[i][j].gm.glyphflags & MG_OBJPILE) && iflags.hilite_pile))
            || ((data->map[i][j].gm.glyphflags & (MG_DETECT | MG_BW_LAVA
                                                  | MG_BW_ICE | MG_BW_SINK
                                                  | MG_BW_ENGR)) != 0)) {
            back_brush =
                CreateSolidBrush(nhcolor_to_RGB(CLR_GRAY));
            FillRect(data->backBufferDC, rect, back_brush);
            DeleteObject(back_brush);
            switch (color) {
            case CLR_GRAY:
            case CLR_WHITE:
                OldFg = SetTextColor(
                    data->backBufferDC, nhcolor_to_RGB(CLR_BLACK));
                break;
            default:
                OldFg =
                    SetTextColor(data->backBufferDC, rgbcolor);
            }
        } else {
            OldFg = SetTextColor(data->backBufferDC, rgbcolor);
        }
        if (data->bUnicodeFont || SYMHANDLING(H_UTF8)) {
            winos_ascii_to_wide(wch, ch);
            if (wch[0] == 0x2591 || wch[0] == 0x2592) {
                int intensity = 80;
                HBRUSH brush = CreateSolidBrush(RGB(intensity, intensity, intensity));
                FillRect(data->backBufferDC, rect, brush);
                DeleteObject(brush);
                intensity = (wch[0] == 0x2591 ? 1 : 2);
                brush = CreateSolidBrush(RGB(
                            GetRValue(rgbcolor)*intensity/2,
                            GetGValue(rgbcolor)*intensity/2,
                            GetBValue(rgbcolor)*intensity/2));
                RECT smallRect = {0};
                smallRect.left = rect->left + 1;
                smallRect.top = rect->top + 1;
                smallRect.right = rect->right - 1;
                smallRect.bottom = rect->bottom - 1;
                FillRect(data->backBufferDC, &smallRect, brush);
                DeleteObject(brush);
            } else {
                SelectObject(data->backBufferDC, data->hMapFontUnicode);
                DrawTextW(data->backBufferDC, wch, -1, rect,
                    DT_CENTER | DT_VCENTER | DT_NOPREFIX
                    | DT_SINGLELINE);
            }
        } else {
            char ch8 = (char) ch;
            SelectObject(data->backBufferDC, data->hMapFont);
            DrawTextA(data->backBufferDC, &ch8, 1, rect,
                        DT_CENTER | DT_VCENTER | DT_NOPREFIX
                            | DT_SINGLELINE);
        }

        SetTextColor(data->backBufferDC, OldFg);
        fx_paint_healthbar(data, i, j, rect);
    }

    if (i == data->xCur && j == data->yCur &&
        (data->cursorOn || !win32_cursorblink)) {
        int yCursor = (win32_cursorblink ? data->yBlinkCursor :
                                           data->yNoBlinkCursor);
        PatBlt(data->backBufferDC,
                rect->left, rect->bottom - yCursor,
                rect->right - rect->left,
                yCursor,
                DSTINVERT);
    }
}

static void setGlyph(PNHMapWindow data, int i, int j,
                     const glyph_info *fg, const glyph_info *bg)
{
    if ((data->map[i][j].glyph != fg->glyph)
            || (data->bkmap[i][j].glyph != bg->glyph)
        || data->map[i][j].ttychar != fg->ttychar
        || data->map[i][j].gm.sym.color != fg->gm.sym.color
        || data->map[i][j].gm.customcolor != fg->gm.customcolor
        || data->map[i][j].gm.glyphflags != fg->gm.glyphflags
        || data->map[i][j].gm.tileidx != fg->gm.tileidx
        || data->map[i][j].fxauras != fg->fxauras
        || data->map[i][j].fxhealth != fg->fxhealth) {
        data->map[i][j] = *fg;
        data->bkmap[i][j] = *bg;
        data->locDirty[i][j] = TRUE;
        data->mapDirty = TRUE;
    }
}

static void clearAll(PNHMapWindow data)
{
    for (int x = 0; x < COLNO; x++)
        for (int y = 0; y < ROWNO; y++) {
            data->map[x][y] = nul_glyphinfo;
            data->bkmap[x][y] = nul_glyphinfo;
            data->locDirty[x][y] = TRUE;
        }
    data->mapDirty = TRUE;
    fxv.nfloaters = 0; /* new level or full redraw: drop numbers */
}

static void dirtyAll(PNHMapWindow data)
{
    for (int i = 0; i < COLNO; i++)
        for (int j = 0; j < ROWNO; j++)
            data->locDirty[i][j] = TRUE;
    data->mapDirty = TRUE;
}

static void dirty(PNHMapWindow data, int x, int y)
{
    data->locDirty[x][y] = TRUE;
    data->mapDirty = TRUE;
}

static void
paint(PNHMapWindow data, int i, int j)
{
    RECT rect;

    rect.left = i * data->xBackTile;
    rect.top = j * data->yBackTile;
    rect.right = rect.left + data->xBackTile;
    rect.bottom = rect.top + data->yBackTile;

    if (data->bAsciiMode || Is_rogue_level(&u.uz)) {
        paintGlyph(data, i, j, &rect);
    } else {
        paintTile(data, i, j, &rect);
    }

    data->locDirty[i][j] = FALSE;
}


/* on WM_PAINT */
void
onPaint(HWND hWnd)
{
    PNHMapWindow data = (PNHMapWindow) GetWindowLongPtr(hWnd, GWLP_USERDATA);

    PAINTSTRUCT ps;
    HDC hFrontBufferDC = BeginPaint(hWnd, &ps);

    /* stretch back buffer onto front buffer window */
    int frontWidth = COLNO * data->xFrontTile;
    int frontHeight = ROWNO * data->yFrontTile;

    int shx = 0, shy = 0;
    boolean shaking;

    {
        float fdx, fdy;

        shaking = fxv_shake_at(GetTickCount(), &fdx, &fdy);
        shx = (int) (fdx * data->xFrontTile);
        shy = (int) (fdy * data->xFrontTile);
    }

    /* always compose off screen, then copy: switching between this and a
       direct stretch whenever an animation starts or stops made the whole
       map twitch by a pixel (the two stretches round differently) */
    (void) shaking;
    {
        /* compose map + floating numbers off screen, then copy, so the
           numbers don't flicker */
        RECT client;
        HDC hdc;
        boolean flicker;

        GetClientRect(hWnd, &client);
        if (!data->compDC || data->compW < client.right
            || data->compH < client.bottom) {
            if (data->compDC) {
                SelectObject(data->compDC, data->compBmpOld);
                DeleteObject(data->compBmp);
                DeleteDC(data->compDC);
            }
            data->compDC = CreateCompatibleDC(hFrontBufferDC);
            data->compW = max(client.right, 1);
            data->compH = max(client.bottom, 1);
            data->compBmp = CreateCompatibleBitmap(hFrontBufferDC,
                                                   data->compW, data->compH);
            data->compBmpOld = SelectObject(data->compDC, data->compBmp);
        }
        hdc = data->compDC;
        {
            /* the backdrop round the map */
            static HBRUSH backdrop;

            if (!backdrop)
                backdrop = CreateSolidBrush(iflags.fx_theme ? TH_BG_APP
                                                            : RGB(0, 0, 0));
            FillRect(hdc, &client, backdrop);
        }
        /* a screen shake moves everything drawn this frame */
        data->map_orig.x += shx, data->map_orig.y += shy;
        /* shrinking big tiles: average, don't drop pixels */
        if (data->xFrontTile < data->xBackTile) {
            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, NULL);
        } else
            SetStretchBltMode(hdc, COLORONCOLOR);
        StretchBlt(hdc,
            data->map_orig.x - (data->xPos * data->xFrontTile),
            data->map_orig.y - (data->yPos * data->yFrontTile), frontWidth,
            frontHeight, data->backBufferDC, 0, 0, data->backWidth,
            data->backHeight, SRCCOPY);
        if (iflags.fx_theme) {
            /* column 0 is never part of the level: backdrop, not black */
            static HBRUSH col0;
            RECT r;

            if (!col0)
                col0 = CreateSolidBrush(TH_BG_APP);
            r.left = data->map_orig.x - data->xPos * data->xFrontTile;
            r.top = data->map_orig.y - data->yPos * data->yFrontTile;
            r.right = r.left + data->xFrontTile;
            r.bottom = r.top + frontHeight;
            FillRect(hdc, &r, col0);
        }
        fx_paint_translucent(data, hdc);
        fx_paint_sprites(data, hdc);
        flicker = fx_paint_lighting(data, hdc);
        fx_paint_hero_frame(data, hdc);
        if (!fx_paint_auras(data, hdc) && !flicker)
            fx_aura_timer(data, FALSE);
        else if (flicker)
            fx_aura_timer(data, TRUE); /* lamp flames keep moving */
        fx_paint_particles(data, hdc);
        fx_paint_overlay(data, hdc);
        data->map_orig.x -= shx, data->map_orig.y -= shy;
        fx_paint_messages(data, hdc); /* steady, even while shaking */
        BitBlt(hFrontBufferDC, ps.rcPaint.left, ps.rcPaint.top,
               ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, hdc, ps.rcPaint.left,
               ps.rcPaint.top, SRCCOPY);
    }

    EndPaint(hWnd, &ps);
}

/* ------------------------------------------------------------------ */
/* NetHack Modern: damage numbers and health bars (Phase 2)            */

static void fx_paint_sprites(PNHMapWindow, HDC);
static void fx_start_timer(PNHMapWindow);
static void fx_blend_tile(PNHMapWindow, HDC, int, int, int, int, short, int);

/* called by the core (fx.c) */
static void
mswin_fx_event(const struct fx_event *ev)
{
    PNHMapWindow data = (PNHMapWindow) windowdata[NHW_MAP].address;
    DWORD now = GetTickCount();

    if (ev->type == FXE_UI && ev->text && !strcmp(ev->text, "death"))
        mswin_end_snapshot(); /* the death screen's last words */
    if (!data)
        return;
    fxv_motion_event(ev); /* lunges and shakes */
    if (ev->type == FXE_MOVE && (ev->flags & FXF_HERO_TARGET)
        && iflags.fx_theme && IsWindow(mswin_hwnd_from_winid(WIN_STATUS)))
        /* the character panel's action bar depends on where we stand */
        InvalidateRect(mswin_hwnd_from_winid(WIN_STATUS), NULL, FALSE);
    fxv_shake_event(ev);  /* the whole view */
    if (ev->type == FXE_EXPLODE && !(ev->flags & (FXF_UNSEEN | FXF_BULK)))
        fxv_burst_particles(ev->x, ev->y, ev->adtyp, ev->amount);
    if (ev->type == FXE_WAND && !(ev->flags & FXF_BULK))
        fxv_wand_particles(ev);
    if (ev->type == FXE_TELEPORT && !(ev->flags & FXF_BULK))
        fxv_teleport_particles(ev);
    if (ev->type == FXE_SUMMON)
        fxv_summon_particles(ev);
    if (ev->type == FXE_ENGRAVE)
        fxv_engrave_particles(ev);
    if (ev->type == FXE_QUAFF && !(ev->flags & FXF_UNSEEN))
        fxv_quaff_particles(ev);
    if (ev->type == FXE_READ && ev->adtyp == 1 && !(ev->flags & FXF_BULK))
        fxv_scroll_particles(ev);
    if (ev->type == FXE_CAST && ev->otyp == SPE_CHAIN_LIGHTNING)
        fxv_chain_until = (ev->adtyp == 0) ? now + 4000 : 0;
    if (ev->type == FXE_CAST && !(ev->flags & (FXF_UNSEEN | FXF_BULK)))
        fxv_cast_particles(ev);
    if (ev->type == FXE_DAMAGE && ev->adtyp == AD_ELEC && now < fxv_chain_until
        && iflags.fx_particles && isok(ev->x, ev->y)) {
        /* a creature caught in the chain: a burst of sparks on it */
        fxv_chain_sparks(ev->x + 0.5f, ev->y + 0.5f, 40, 0);
        fxv_chain_sparks(ev->x + 0.5f, ev->y + 0.5f, 18, 150);
        fxv_chain_sparks(ev->x + 0.5f, ev->y + 0.5f, 10, 320);
        fx_start_timer(data);
    }
    if ((ev->type == FXE_DETECT || ev->type == FXE_UNIHORN)
        && !(ev->flags & FXF_UNSEEN))
        fxv_detect_particles(ev);
    if (ev->type == FXE_IMPACT && !(ev->flags & FXF_BULK))
        fxv_impact_particles(ev->x, ev->y,
                            (ev->flags & FXF_RESISTED) != 0);
    fxv_floater_event(ev); /* numbers, status names, Killed, Level N */
}

static void
fx_start_timer(PNHMapWindow data)
{
    if (!data->fxTimerOn) {
        SetTimer(data->hWnd, FX_TIMER_ID, FX_FRAME_MS, NULL);
        data->fxTimerOn = TRUE;
    }
    InvalidateRect(data->hWnd, NULL, FALSE);
}

/* draw tile 'tile' at <x,y> on hdc, blended at 'alpha' (0..255) over
   whatever is already there (used to fade dying creatures out) */
static void
fx_blend_tile(PNHMapWindow data, HDC hdc, int x, int y, int w, int h,
              short tile, int alpha)
{
    BLENDFUNCTION bf;

    if (alpha <= 0)
        return;
    if (!data->fadeDC || data->fadeW != w || data->fadeH != h) {
        if (data->fadeDC) {
            SelectObject(data->fadeDC, data->fadeBmpOld);
            DeleteObject(data->fadeBmp);
            DeleteDC(data->fadeDC);
        }
        data->fadeDC = CreateCompatibleDC(hdc);
        data->fadeBmp = CreateCompatibleBitmap(hdc, w, h);
        data->fadeBmpOld = SelectObject(data->fadeDC, data->fadeBmp);
        data->fadeW = w, data->fadeH = h;
    }
    /* scratch = what's on the map now, with the sprite on top */
    BitBlt(data->fadeDC, 0, 0, w, h, hdc, x, y, SRCCOPY);
    (*GetNHApp()->lpfnTransparentBlt)(
        data->fadeDC, 0, 0, w, h, data->tileDC, TILEBMP_X(tile),
        TILEBMP_Y(tile), GetNHApp()->mapTile_X, GetNHApp()->mapTile_Y,
        TILE_BK_COLOR);
    /* ...then lay the scratch back down, only partly opaque */
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = (BYTE) min(alpha, 255);
    bf.AlphaFormat = 0;
    (void) GdiAlphaBlend(hdc, x, y, w, h, data->fadeDC, 0, 0, w, h, bf);
}

/* the core is pausing (nh_delay_output, explosion and missile frames):
   keep animations moving for 'ms' instead of freezing the window */
void
mswin_map_fx_pump(int ms)
{
    PNHMapWindow data = (PNHMapWindow) windowdata[NHW_MAP].address;
    DWORD start = GetTickCount();

    if (!data || (!fxv.nanims && !fxv.nfloaters && !fxv.shake_dur
                  && !fxv.nparts)) {
        Sleep(ms);
        return;
    }
    for (;;) {
        long left = ms - (long) (GetTickCount() - start);

        if (left <= 0)
            break;
        fx_tick(data);
        UpdateWindow(data->hWnd);
        Sleep(min(left, 10L));
    }
}

/* ---- particles ---------------------------------------------------- */

/* what spawns them and how they move is shared with the other front ends
   (win/share/fxview.c); only drawing them is here */

/* particles are drawn as soft dots on a black layer, which is then OR-ed
   onto the map: light adds, so they glow over whatever is underneath */
static void
fx_paint_particles(PNHMapWindow data, HDC hdc)
{
    DWORD now = GetTickCount();
    int i, ox, oy, tw, th;
    int l = 1 << 30, t = 1 << 30, r = -(1 << 30), b = -(1 << 30);
    HGDIOBJ oldBrush, oldPen;
    RECT box;

    if (!fxv.nparts)
        return;
    ox = data->map_orig.x - data->xPos * data->xFrontTile;
    oy = data->map_orig.y - data->yPos * data->yFrontTile;
    tw = data->xFrontTile, th = data->yFrontTile;
    fx_part_layer(data, hdc);
    /* where they are this frame */
    for (i = 0; i < fxv.nparts; i++) {
        struct fxv_part *p = &fxv.parts[i];
        float sx, sy, k;
        double px, py, rad;

        fxv_part_at(p, now, &sx, &sy, &k);
        px = ox + sx * tw;
        py = oy + sy * th;
        rad = p->size * tw;
        l = min(l, (int) (px - rad * 2.2) - 1);
        t = min(t, (int) (py - rad * 2.2) - 1);
        r = max(r, (int) (px + rad * 2.2) + 2);
        b = max(b, (int) (py + rad * 2.2) + 2);
    }
    SetRect(&box, max(l, 0), max(t, 0), min(r, data->partW),
            min(b, data->partH));
    if (box.right <= box.left || box.bottom <= box.top)
        return;
    FillRect(data->partDC, &box, (HBRUSH) GetStockObject(BLACK_BRUSH));
    oldBrush = SelectObject(data->partDC, GetStockObject(DC_BRUSH));
    oldPen = SelectObject(data->partDC, GetStockObject(NULL_PEN));
    for (i = 0; i < fxv.nparts; i++) {
        struct fxv_part *p = &fxv.parts[i];
        float sx, sy, k;
        double px, py, rad;
        COLORREF c;
        int ir;

        fxv_part_at(p, now, &sx, &sy, &k);
        if (k < 0 || k >= 1)
            continue;
        px = ox + sx * tw;
        py = oy + sy * th;
        rad = p->size * tw * (1.0 - 0.55 * k);
        c = fxv_lerp_color(p->c0, p->c1, k);
        /* faint glow around it (added on as light) */
        ir = (int) (rad * 2.2 + 0.5);
        SetDCBrushColor(data->partDC, fxv_lerp_color(RGB(0, 0, 0), c, 0.35f));
        Ellipse(data->partDC, (int) px - ir, (int) py - ir, (int) px + ir + 1,
                (int) py + ir + 1);
    }
    SelectObject(data->partDC, oldPen);
    SelectObject(data->partDC, oldBrush);
    BitBlt(hdc, box.left, box.top, box.right - box.left,
           box.bottom - box.top, data->partDC, box.left, box.top, SRCPAINT);

    /* ...and a solid core on top, so it shows even over a bright ray */
    oldBrush = SelectObject(hdc, GetStockObject(DC_BRUSH));
    oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    for (i = 0; i < fxv.nparts; i++) {
        struct fxv_part *p = &fxv.parts[i];
        float sx, sy, k;
        double px, py, rad;
        int ir;

        fxv_part_at(p, now, &sx, &sy, &k);
        if (k < 0 || k >= 1)
            continue;
        px = ox + sx * tw;
        py = oy + sy * th;
        rad = p->size * tw * (1.0 - 0.55 * k);
        ir = max(1, (int) (rad + 0.5));
        SetDCBrushColor(hdc, fxv_lerp_color(p->c0, p->c1, k));
        Ellipse(hdc, (int) px - ir, (int) py - ir, (int) px + ir + 1,
                (int) py + ir + 1);
    }
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
}

/* ---- lasting effects (Phase 4b) ----------------------------------- */

static void
fx_aura_timer(PNHMapWindow data, boolean on)
{
    if (on && !data->auraTimerOn) {
        SetTimer(data->hWnd, FX_AURA_TIMER_ID, FX_AURA_MS, NULL);
        data->auraTimerOn = TRUE;
        InvalidateRect(data->hWnd, NULL, FALSE);
    } else if (!on && data->auraTimerOn) {
        KillTimer(data->hWnd, FX_AURA_TIMER_ID);
        data->auraTimerOn = FALSE;
    }
}

/* make sure the black glow layer exists at the composition size */
static void
fx_part_layer(PNHMapWindow data, HDC hdc)
{
    if (!data->partDC || data->partW != data->compW
        || data->partH != data->compH) {
        if (data->partDC) {
            SelectObject(data->partDC, data->partBmpOld);
            DeleteObject(data->partBmp);
            DeleteDC(data->partDC);
        }
        data->partDC = CreateCompatibleDC(hdc);
        data->partW = data->compW, data->partH = data->compH;
        data->partBmp = CreateCompatibleBitmap(hdc, data->partW, data->partH);
        data->partBmpOld = SelectObject(data->partDC, data->partBmp);
    }
}

/* a soft dot on the glow layer */
static void
fx_dot(HDC dc, int x, int y, int r, COLORREF c)
{
    SetDCBrushColor(dc, c);
    Ellipse(dc, x - r, y - r, x + r + 1, y + r + 1);
}

static COLORREF
fx_dim(COLORREF c, double k)
{
    if (k < 0)
        k = 0;
    if (k > 1)
        k = 1;
    return RGB((int) (GetRValue(c) * k), (int) (GetGValue(c) * k),
               (int) (GetBValue(c) * k));
}

/* a phase 0..1 that loops every 'period' ms, offset per creature so a
   room of them doesn't move in step */
static double
fx_phase(DWORD now, int x, int y, int salt, DWORD period)
{
    DWORD beat = (DWORD) (x * 389 + y * 157 + salt * 613);

    return (double) ((now + beat) % period) / period;
}

/* lasting status effects on creatures, for as long as the core says so
   (the square's fxauras): drawn as light on the black glow layer and
   added onto the map; returns how many squares had one */
static int
fx_paint_auras(PNHMapWindow data, HDC hdc)
{
#define FX_Z_PERIOD 2100   /* ms for one z to rise and fade */
#define FX_Z_COUNT 3       /* z's in the air at once */
#define TAU 6.2831853
    DWORD now = GetTickCount();
    int x, y, n = 0, ox, oy, tw, th, i;
    HFONT oldFont;
    int oldBk;
    COLORREF oldColor;
    HGDIOBJ oldBrush, oldPen;

    if (!iflags.fx_auras)
        return 0;
    ox = data->map_orig.x - data->xPos * data->xFrontTile;
    oy = data->map_orig.y - data->yPos * data->yFrontTile;
    tw = data->xFrontTile, th = data->yFrontTile;
    fx_make_fonts(data);
    fx_part_layer(data, hdc);
    oldFont = SelectObject(data->partDC, data->hFxFontSmall);
    oldBk = SetBkMode(data->partDC, TRANSPARENT);
    oldColor = GetTextColor(data->partDC);
    oldBrush = SelectObject(data->partDC, GetStockObject(DC_BRUSH));
    oldPen = SelectObject(data->partDC, GetStockObject(NULL_PEN));

    for (x = 1; x < COLNO; x++)
        for (y = 0; y < ROWNO; y++) {
            unsigned long au = data->map[x][y].fxauras & FX_AURA_MASK;
            int glyph = data->map[x][y].glyph;
            /* gas clouds (stinking cloud, breath, a hezrou's trail) and
               plain clouds drift where they're drawn */
            int gas = (glyph == cmap_to_glyph(S_poisoncloud)) ? 1
                      : (glyph == cmap_to_glyph(S_cloud)) ? 2 : 0;
            RECT box;
            int bx, by, cx, cy, hx, hy, r1 = max(2, tw / 13);

            if (!gas && (!au || !glyph_is_monster(glyph)))
                continue;
            if (gas)
                au = 0UL;
            n++;
            bx = ox + x * tw, by = oy + y * th;
            cx = bx + tw / 2, cy = by + th / 2;   /* middle of the tile */
            hx = cx, hy = by + th / 8;           /* about the head */
            /* effects live in the tile and a margin around it */
            SetRect(&box, bx - tw / 2, by - th, bx + tw + tw / 2,
                    by + th + th / 2);
            if (box.right < 0 || box.bottom < 0 || box.left > data->partW
                || box.top > data->partH)
                continue;
            FillRect(data->partDC, &box, (HBRUSH) GetStockObject(BLACK_BRUSH));

            if (gas) {
                /* billowing gas: big soft puffs rolling round the square
                   and spilling past its edges, each swelling and thinning
                   on its own beat, so a cloud seems to churn */
                COLORREF gc = (gas == 1) ? RGB(130, 215, 60)
                                         : RGB(160, 165, 175);
                COLORREF hc = (gas == 1) ? RGB(200, 255, 120)
                                         : RGB(215, 220, 228);

                for (i = 0; i < 5; i++) {
                    double t = fmod(fx_phase(now, x, y, 29 + i * 5, 2300)
                                    + i / 5.0, 1.0);
                    double k = sin(t * 3.14159);
                    /* each puff circles the middle, slowly, its own way */
                    double a = (x * 1.7 + y * 2.3 + i * 1.26)
                               + (now % 9000) / 9000.0 * 6.2831853
                                     * ((i & 1) ? 1 : -1);
                    int px = cx + (int) (tw * 0.30 * cos(a));
                    int py = cy + (int) (th * (0.26 * sin(a) - 0.12 * t));
                    int rr = (int) (tw * (0.22 + 0.2 * t));

                    fx_dot(data->partDC, px, py, rr, fx_dim(gc, k * 0.38));
                    fx_dot(data->partDC, px, py, (rr * 2) / 3,
                           fx_dim(gc, k * 0.55));
                    fx_dot(data->partDC, px - rr / 5, py - rr / 5,
                           max(1, rr / 3), fx_dim(hc, k * 0.6));
                }
            }
            if (au & FXA_ASLEEP) {
                /* z's rising, drifting, growing and fading */
                for (i = 0; i < FX_Z_COUNT; i++) {
                    double t = fx_phase(now, x, y,
                                        i * 7, FX_Z_PERIOD);
                    double fade;
                    int zx, zy, c;
                    const char *z;

                    t = fmod(t + (double) i / FX_Z_COUNT, 1.0);
                    fade = (t < 0.15) ? t / 0.15 : 1.0 - (t - 0.15) / 0.85;
                    zx = bx + (int) (tw * (0.55 + 0.35 * t
                                           + 0.06 * sin(t * TAU * 1.5)));
                    zy = by + (int) (th * (0.30 - 0.95 * t));
                    c = (int) (255 * fade * fade);
                    z = (t < 0.45) ? "z" : "Z";
                    SetTextColor(data->partDC,
                                 RGB((c * 190) / 255, (c * 215) / 255, c));
                    TextOutA(data->partDC, zx, zy - th / 3, z, 1);
                }
            }
            if (au & (FXA_STUNNED | FXA_CONFUSED | FXA_HALLU)) {
                /* things circling the head: stars when stunned, swirling
                   motes when confused, every colour when hallucinating */
                boolean stun = (au & FXA_STUNNED) != 0;
                boolean hallu = (au & FXA_HALLU) != 0;
                int k = stun ? 3 : 4;
                double spin = fx_phase(now, x, y, 3, stun ? 1100 : 1700);

                for (i = 0; i < k; i++) {
                    double a = (spin + (double) i / k) * TAU;
                    double depth = 0.55 + 0.45 * sin(a); /* front brighter */
                    int px = hx + (int) (tw * 0.38 * cos(a));
                    int py = hy - th / 8 + (int) (th * 0.13 * sin(a));
                    COLORREF c;

                    if (hallu)
                        c = fxv_lerp_color(RGB(255, 80, 80), RGB(80, 160, 255),
                                          (float) fmod(spin * 3 + i * 0.3, 1.0));
                    else if (stun)
                        c = RGB(255, 230, 90);
                    else
                        c = RGB(220, 140, 255);
                    if (stun) {
                        /* a glowing four-point star */
                        int sr = max(4, tw / 6), j;

                        fx_dot(data->partDC, px, py, sr,
                               fx_dim(c, depth * 0.25));
                        for (j = 1; j <= sr; j++) {
                            int ar = max(1, (sr - j) / 3);

                            fx_dot(data->partDC, px - j, py, ar,
                                   fx_dim(c, depth));
                            fx_dot(data->partDC, px + j, py, ar,
                                   fx_dim(c, depth));
                            fx_dot(data->partDC, px, py - j, ar,
                                   fx_dim(c, depth));
                            fx_dot(data->partDC, px, py + j, ar,
                                   fx_dim(c, depth));
                        }
                        fx_dot(data->partDC, px, py, max(2, sr / 3),
                               fx_dim(RGB(255, 255, 230), depth));
                    } else {
                        fx_dot(data->partDC, px, py, r1 * 2 + 1,
                               fx_dim(c, depth * 0.3));
                        fx_dot(data->partDC, px, py, r1, fx_dim(c, depth));
                    }
                }
            }
            if (au & FXA_SLOW) {
                /* heavy blue motes sinking slowly at the feet */
                /* (drops sinking down the body, each with a soft glow,
                   like something weighing it down) */
                for (i = 0; i < 4; i++) {
                    double t = fmod(fx_phase(now, x, y, 11, 2600)
                                    + i / 4.0, 1.0);
                    double k = sin(t * 3.14159);
                    int px = bx + (int) (tw * (0.18 + 0.21 * i));
                    int py = by + (int) (th * (0.15 + 0.85 * t));
                    int rr = max(3, tw / 8);

                    fx_dot(data->partDC, px, py, rr * 2,
                           fx_dim(RGB(70, 110, 255), k * 0.3));
                    fx_dot(data->partDC, px, py, rr,
                           fx_dim(RGB(110, 150, 255), k));
                }
            }
            if (au & FXA_LEVITATE) {
                /* soft air stirring under the feet */
                for (i = 0; i < 4; i++) {
                    double t = fmod(fx_phase(now, x, y, 5, 1500)
                                    + i / 4.0, 1.0);
                    int side = (i & 1) ? 1 : -1;
                    int px = cx + side * (int) (tw * (0.08 + 0.32 * t));
                    int py = by + th - th / 10 + (int) (th * 0.15 * t);

                    fx_dot(data->partDC, px, py, r1,
                           fx_dim(RGB(190, 225, 255), (1.0 - t) * 0.7));
                }
            }
            if (au & FXA_REFLECT) {
                /* every few seconds a glint sweeps across */
                double t = fx_phase(now, x, y, 2, 2600);

                if (t < 0.14) {
                    double g = t / 0.14;
                    int gx = bx + (int) (tw * (0.1 + 0.8 * g));
                    int gy = by + (int) (th * (0.85 - 0.7 * g));

                    for (i = -2; i <= 2; i++)
                        fx_dot(data->partDC, gx + i * tw / 14,
                               gy - i * th / 14, max(1, r1 - abs(i) / 2),
                               fx_dim(RGB(255, 255, 255),
                                      (1.0 - abs(i) * 0.3)
                                          * sin(g * 3.14159)));
                }
            }
            if ((au & FXA_CONFLICT) && x == u.ux && y == u.uy) {
                /* the hero radiating Conflict: dark red rings pulsing
                   outward, one after another */
                for (i = 0; i < 2; i++) {
                    double t = fmod(fx_phase(now, x, y, 21, 1800)
                                    + i / 2.0, 1.0);
                    double r = 0.3 + 0.55 * t, k = sin(t * 3.14159) * 0.75;
                    int j;

                    for (j = 0; j < 20; j++) {
                        double a = j / 20.0 * TAU;

                        fx_dot(data->partDC, cx + (int) (tw * r * cos(a)),
                               cy + (int) (th * r * sin(a)), r1,
                               fx_dim(RGB(200, 30, 40), k));
                    }
                }
            } else if (au & FXA_CONFLICT) {
                /* a creature in its grip: a throbbing red glow, and red
                   embers flickering up off it like rage */
                double beat = fx_phase(now, x, y, 23, 700);
                double k = 0.25 + 0.2 * sin(beat * TAU);

                fx_dot(data->partDC, cx, cy + th / 6, tw / 2,
                       fx_dim(RGB(170, 20, 25), k));
                for (i = 0; i < 5; i++) {
                    double t = fmod(fx_phase(now, x, y, 27 + i, 1100)
                                    + i / 5.0, 1.0);
                    int px = bx + (int) (tw * (0.15 + 0.17 * i
                                               + 0.05 * sin(t * TAU * 2)));
                    int py = by + (int) (th * (0.85 - 0.9 * t));

                    fx_dot(data->partDC, px, py, r1,
                           fx_dim(RGB(255, 70, 50), sin(t * 3.14159)));
                }
            }
            if (au & FXA_PROTECT) {
                /* a slow golden ring of light around it, breathing */
                double t = fx_phase(now, x, y, 9, 3000);
                double br = 0.35 + 0.25 * sin(t * TAU);

                for (i = 0; i < 16; i++) {
                    double a = (i / 16.0 + t * 0.25) * TAU;

                    fx_dot(data->partDC,
                           cx + (int) (tw * 0.52 * cos(a)),
                           cy + (int) (th * 0.52 * sin(a)), r1,
                           fx_dim(RGB(255, 210, 110), br));
                }
            }
            if (au & FXA_STONING) {
                /* grey grit creeping inward */
                for (i = 0; i < 5; i++) {
                    double t = fmod(fx_phase(now, x, y, 13, 1900)
                                    + i / 5.0, 1.0);
                    double a = (i * 0.21 + 0.1) * TAU;
                    double r = 0.6 * (1.0 - t);

                    fx_dot(data->partDC, cx + (int) (tw * r * cos(a)),
                           cy + (int) (th * r * sin(a)), r1,
                           fx_dim(RGB(200, 200, 190), t * 0.8));
                }
            }
            if (au & FXA_SLIMED) {
                /* green slime dripping off */
                for (i = 0; i < 3; i++) {
                    double t = fmod(fx_phase(now, x, y, 17, 1600)
                                    + i / 3.0, 1.0);
                    int px = bx + (int) (tw * (0.22 + 0.28 * i));
                    int py = by + (int) (th * (0.45 + 0.6 * t * t));

                    fx_dot(data->partDC, px, py, r1 + (t > 0.6),
                           fx_dim(RGB(120, 230, 60), 1.0 - t * 0.7));
                }
            }
            if (au & FXA_SICK) {
                /* sickly bubbles rising and popping */
                for (i = 0; i < 3; i++) {
                    double t = fmod(fx_phase(now, x, y, 19, 2000)
                                    + i / 3.0, 1.0);
                    int px = bx + (int) (tw * (0.3 + 0.2 * i)
                                         + tw * 0.05 * sin(t * TAU * 2));
                    int py = by + (int) (th * (0.85 - 0.8 * t));

                    fx_dot(data->partDC, px, py, r1 + (int) (t * 1.5),
                           fx_dim(RGB(190, 220, 70), sin(t * 3.14159) * 0.8));
                }
            }
            if (au & FXA_INVIS) {
                /* seen though invisible (it is also drawn see-through):
                   just two faint specks drifting slowly down it */
                for (i = 0; i < 2; i++) {
                    double t = fmod(fx_phase(now, x, y, 23, 3400)
                                    + i / 2.0, 1.0);
                    int px = bx + (int) (tw * (0.32 + 0.36 * i));
                    int py = by + (int) (th * (0.2 + 0.7 * t));

                    fx_dot(data->partDC, px, py, max(1, r1 - 1),
                           fx_dim(RGB(150, 190, 230),
                                  sin(t * 3.14159) * 0.4));
                }
            }
            BitBlt(hdc, box.left, box.top, box.right - box.left,
                   box.bottom - box.top, data->partDC, box.left, box.top,
                   SRCPAINT);
        }
    SelectObject(data->partDC, oldPen);
    SelectObject(data->partDC, oldBrush);
    SetTextColor(data->partDC, oldColor);
    SetBkMode(data->partDC, oldBk);
    SelectObject(data->partDC, oldFont);
    return n;
#undef TAU
}

/* NetHack Modern: zoom the map; dir +1 in, -1 out, 0 back to auto */
void
mswin_map_zoom(int dir)
{
    HWND hWnd = mswin_hwnd_from_winid(WIN_MAP);
    PNHMapWindow data;
    SIZE sz;
    int z;

    if (!hWnd || !iflags.fx_theme)
        return;
    data = (PNHMapWindow) GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (!data || data->bAsciiMode || data->bFitToScreenMode)
        return;
    if (dir == 0) {
        data->zoomSet = FALSE;
    } else {
        z = data->zoomSet ? data->zoomLevel : data->zoomAuto;
        z += dir;
        if (z < 0)
            z = 0;
        if (z >= FX_NZOOM)
            z = FX_NZOOM - 1;
        data->zoomLevel = z;
        data->zoomSet = TRUE;
    }
    sz.cx = data->tileWidth * COLNO;
    sz.cy = data->tileHeight * ROWNO;
    mswin_map_layout(hWnd, &sz);
}

/* NetHack Modern: messages float in the map's lower left, newest at the
   bottom, each outlined so it reads over any tiles (and over a soft
   panel with fx_msg_backdrop); older turns fade and then drop away */
static void
fx_paint_messages(PNHMapWindow data, HDC hdc)
{
#define FX_MSG_SHOW 6     /* lines at most */
#define FX_MSG_KEEP 6     /* turns an old line stays up */
    struct {
        WCHAR w[TBUFSZ];
        int len;
        COLORREF c;
        RECT r;
    } ln[FX_MSG_SHOW];
    char buf[TBUFSZ];
    RECT client, box, block;
    cached_font *fnt;
    HGDIOBJ oldFont;
    int n = 0, i, y, wmax, pad, ol, margin;
    UINT align = DT_LEFT;
    boolean scrolled = FALSE;

    if (!(iflags.fx_theme && iflags.fx_msg_overlay))
        return;
    GetClientRect(data->hWnd, &client);
    fnt = mswin_get_font(NHW_MESSAGE, ATR_BOLD, hdc, FALSE); /* semibold */
    oldFont = SelectObject(hdc, fnt->hFont);
    margin = max(10, fnt->height / 2);
    wmax = min((client.right - client.left) * 3 / 5,
               (client.right - client.left) - 2 * margin);
    ol = max(2, fnt->height / 10); /* outline thickness */

    /* newest first, up to FX_MSG_SHOW, recent turns only */
    for (i = 0; n < FX_MSG_SHOW; i++) {
        COLORREF c;
        long turn;
        boolean recent, sc;
        double k2;
        long age;

        if (!mswin_msg_overlay_line(i, buf, sizeof buf, &c, &turn, &recent,
                                    &sc))
            break;
        scrolled = sc;
        age = svm.moves - turn;
        if (!scrolled && !recent && age > FX_MSG_KEEP)
            break;
        if (!*buf)
            continue;
        k2 = recent || scrolled ? 1.0 : (age <= 1) ? 0.82
             : (age <= 3) ? 0.66 : 0.5;
        ln[n].c = RGB((int) (GetRValue(c) * k2), (int) (GetGValue(c) * k2),
                      (int) (GetBValue(c) * k2));
        winos_ascii_to_wide_str((const unsigned char *) buf, ln[n].w,
                                TBUFSZ);
        ln[n].w[TBUFSZ - 1] = 0;
        ln[n].len = (int) wcslen(ln[n].w);
        SetRect(&ln[n].r, 0, 0, wmax, 0);
        DrawTextW(hdc, ln[n].w, ln[n].len, &ln[n].r,
                  DT_NOPREFIX | DT_WORDBREAK | DT_CALCRECT);
        n++;
    }
    if (!n) {
        SelectObject(hdc, oldFont);
        return;
    }

    /* the corner (or edge middle) the player picked; the newest line is
       always the lowest, nearest the action below for the top ones */
    {
        int pos = iflags.fx_msg_pos, gap = max(2, fnt->height / 8), tot = 0;
        boolean top = (pos >= 0 && pos <= 2);
        int col = (pos < 0 || pos > 5) ? 0 : pos % 3; /* left, centre, right */

        for (i = 0; i < n; i++)
            tot += (ln[i].r.bottom - ln[i].r.top) + (i ? gap : 0);
        y = top ? client.top + margin + tot : client.bottom - margin;
        for (i = 0; i < n; i++) {
            int h = ln[i].r.bottom - ln[i].r.top, w = ln[i].r.right
                                                       - ln[i].r.left, x;

            y -= h;
            x = (col == 0) ? client.left + margin
                : (col == 1) ? client.left + (client.right - client.left - w) / 2
                             : client.right - margin - w;
            OffsetRect(&ln[i].r, x - ln[i].r.left, y - ln[i].r.top);
            y -= gap;
        }
        align = (col == 1) ? DT_CENTER : (col == 2) ? DT_RIGHT : DT_LEFT;
    }
    /* the block they take up together */
    block = ln[0].r;
    for (i = 1; i < n; i++)
        UnionRect(&block, &block, &ln[i].r);

    if (iflags.fx_msg_backdrop) {
        /* a soft dark panel behind them */
        static HDC dotDC;
        static HBITMAP dotBmp;
        BLENDFUNCTION bf;

        pad = max(6, fnt->height / 3);
        box = block;
        InflateRect(&box, pad, pad);
        if (!dotDC) {
            dotDC = CreateCompatibleDC(hdc);
            dotBmp = CreateCompatibleBitmap(hdc, 1, 1);
            SelectObject(dotDC, dotBmp);
            SetPixel(dotDC, 0, 0, TH_BG_PANEL);
        }
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 185;
        bf.AlphaFormat = 0;
        (void) GdiAlphaBlend(hdc, box.left, box.top, box.right - box.left,
                             box.bottom - box.top, dotDC, 0, 0, 1, 1, bf);
    }

    if (!iflags.fx_msg_backdrop) {
        /* where the lines lie over the map itself (zoomed in), a soft
           shade behind them, darkest under the text and fading out all
           round, so busy floors don't swallow it */
        RECT maprc, isect;

        maprc.left = data->map_orig.x - data->xPos * data->xFrontTile;
        maprc.top = data->map_orig.y - data->yPos * data->yFrontTile;
        maprc.right = maprc.left + COLNO * data->xFrontTile;
        maprc.bottom = maprc.top + ROWNO * data->yFrontTile;
        if (IntersectRect(&isect, &maprc, &block)) {
            static HDC shDC;
            static HBITMAP shBmp;
            BLENDFUNCTION bf;
            int step, sx, sy, fade = 3 * margin;
            RECT area = block;

            InflateRect(&area, fade, fade);
            IntersectRect(&area, &area, &client);
            if (!shDC) {
                shDC = CreateCompatibleDC(hdc);
                shBmp = CreateCompatibleBitmap(hdc, 1, 1);
                SelectObject(shDC, shBmp);
                SetPixel(shDC, 0, 0, RGB(4, 5, 8));
            }
            bf.BlendOp = AC_SRC_OVER;
            bf.BlendFlags = 0;
            bf.AlphaFormat = 0;
            step = max(6, fnt->height / 3);
            for (sy = area.top; sy < area.bottom; sy += step)
                for (sx = area.left; sx < area.right; sx += step) {
                    /* how far this cell is outside the text block */
                    int ox = (sx + step / 2 < block.left)
                                 ? block.left - (sx + step / 2)
                             : (sx + step / 2 > block.right)
                                 ? (sx + step / 2) - block.right : 0;
                    int oy = (sy + step / 2 < block.top)
                                 ? block.top - (sy + step / 2)
                             : (sy + step / 2 > block.bottom)
                                 ? (sy + step / 2) - block.bottom : 0;
                    double d = sqrt((double) ox * ox + (double) oy * oy);
                    double a = 1.0 - min(1.0, d / fade);

                    a = a * a * (3 - 2 * a);

                    if (a <= 0.02)
                        continue;
                    bf.SourceConstantAlpha = (BYTE) (232 * a);
                    (void) GdiAlphaBlend(hdc, sx, sy, step, step, shDC, 0, 0,
                                         1, 1, bf);
                }
        }
    }

    /* outline first (dark, all round), then the text */
    SetBkMode(hdc, TRANSPARENT);
    for (i = 0; i < n; i++) {
        int dx, dy;

        /* a soft shadow down and right, then a solid round outline */
        SetTextColor(hdc, RGB(0, 0, 0));
        for (dy = 1; dy <= ol + 1; dy++) {
            RECT r = ln[i].r;

            OffsetRect(&r, dy, dy + 1);
            DrawTextW(hdc, ln[i].w, ln[i].len, &r,
                      DT_NOPREFIX | DT_WORDBREAK | align);
        }
        SetTextColor(hdc, RGB(4, 5, 8));
        for (dy = -ol; dy <= ol; dy++)
            for (dx = -ol; dx <= ol; dx++) {
                RECT r = ln[i].r;

                if (!dx && !dy)
                    continue;
                if (dx * dx + dy * dy > ol * ol + 1)
                    continue; /* round, not square, corners */
                OffsetRect(&r, dx, dy);
                DrawTextW(hdc, ln[i].w, ln[i].len, &r,
                          DT_NOPREFIX | DT_WORDBREAK | align);
            }
        SetTextColor(hdc, ln[i].c);
        DrawTextW(hdc, ln[i].w, ln[i].len, &ln[i].r,
                  DT_NOPREFIX | DT_WORDBREAK | align);
    }
    if (scrolled) {
        /* looking back through history (Ctrl+P) */
        RECT r = ln[n - 1].r;

        OffsetRect(&r, 0, -(fnt->height + 2));
        SetTextColor(hdc, TH_ACCENT);
        DrawTextW(hdc, L"\x25B2 earlier messages", -1, &r,
                  DT_NOPREFIX | DT_SINGLELINE | align);
    }
    SelectObject(hdc, oldFont);
#undef FX_MSG_SHOW
#undef FX_MSG_KEEP
}

/* NetHack Modern: four small gold corner marks around the hero's square,
   with a dark edge so they show on any floor */
/* NetHack Modern: lighting.  A shade layer over the map, made from how
   lit each square looks (fx_light_map) and smoothed between squares so
   light fades off instead of stopping at tile edges.  It is built at a
   few pixels per square and stretched; rebuilt only when the light
   changes. */
static boolean
fx_paint_lighting(PNHMapWindow data, HDC hdc)
{
    static unsigned char bright[COLNO][ROWNO], warm[COLNO][ROWNO];
    static unsigned char pbright[COLNO][ROWNO], pwarm[COLNO][ROWNO];
    static unsigned char rock[COLNO][ROWNO], prock[COLNO][ROWNO];
    static HDC lightDC;
    static HBITMAP lightBmp;
    static DWORD *bits;
    static int built = 0;
    const int S = 8; /* layer pixels per square */
    int W = COLNO * S, H = ROWNO * S;
    int ox, oy, tw, th, x, y;
    boolean flicker;
    BLENDFUNCTION bf;

    if (!iflags.fx_lighting || data->bAsciiMode || Is_rogue_level(&u.uz)
        || u.uswallow || !program_state.in_moveloop)
        return FALSE;
    if (!lightDC) {
        BITMAPINFO bi;

        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H; /* top down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        lightDC = CreateCompatibleDC(hdc);
        lightBmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS,
                                    (void **) &bits, NULL, 0);
        if (!lightDC || !lightBmp || !bits)
            return FALSE;
        SelectObject(lightDC, lightBmp);
    }
    flicker = fx_light_map(bright, warm, GetTickCount() / 1000.0) != 0;
    /* squares never seen take their neighbours' shade, so the dark of
       the unknown doesn't make a halo round what you remember */
    for (x = 0; x < COLNO; x++)
        for (y = 0; y < ROWNO; y++) {
            int g = data->map[x][y].glyph;

            rock[x][y] = 0;
            if ((g == NO_GLYPH || g == GLYPH_UNEXPLORED)
                && !cansee(x, y))
                bright[x][y] = fx_shade_level(FALSE), warm[x][y] = 0;
            /* solid rock is only backdrop: keep it well down so the
               rooms and corridors stand out */
            else if (glyph_is_cmap(g) && glyph_to_cmap(g) == S_stone)
                bright[x][y] = fx_shade_level(TRUE), warm[x][y] = 0,
                rock[x][y] = 1;
        }
    if (!built || memcmp(bright, pbright, sizeof bright)
        || memcmp(warm, pwarm, sizeof warm)
        || memcmp(rock, prock, sizeof rock)) {
        int px, py;

        memcpy(pbright, bright, sizeof bright);
        memcpy(pwarm, warm, sizeof warm);
        memcpy(prock, rock, sizeof rock);
        built = 1;
        for (py = 0; py < H; py++) {
            double v = (py + 0.5) / S - 0.5;
            int y0 = (int) floor(v), y1 = y0 + 1;
            double fy = v - y0;

            fy = fy * fy * (3 - 2 * fy); /* tighter fade: less bleed */

            y0 = max(0, min(ROWNO - 1, y0));
            y1 = max(0, min(ROWNO - 1, y1));
            for (px = 0; px < W; px++) {
                double u0 = (px + 0.5) / S - 0.5;
                int x0 = (int) floor(u0), x1 = x0 + 1;
                double fx = u0 - x0, b, w, d, a;

                fx = fx * fx * (3 - 2 * fx);
                int A, R, G, B;

                int cx = max(1, min(COLNO - 1, px / S)),
                    cy = max(0, min(ROWNO - 1, py / S));
                double bs[4], ws[4];
                int sx[4], sy[4], k;

                x0 = max(1, min(COLNO - 1, x0));
                x1 = max(1, min(COLNO - 1, x1));
                if (rock[cx][cy]) {
                    /* rock is one flat shade: no light spills in */
                    b = bright[cx][cy], w = 0;
                } else {
                    /* blend only with squares that aren't rock, so
                       nothing bleeds out into it either */
                    sx[0] = x0, sy[0] = y0, sx[1] = x1, sy[1] = y0;
                    sx[2] = x0, sy[2] = y1, sx[3] = x1, sy[3] = y1;
                    for (k = 0; k < 4; k++) {
                        int qx = sx[k], qy = sy[k];

                        if (rock[qx][qy])
                            qx = cx, qy = cy;
                        bs[k] = bright[qx][qy], ws[k] = warm[qx][qy];
                    }
                    b = (bs[0] * (1 - fx) + bs[1] * fx) * (1 - fy)
                        + (bs[2] * (1 - fx) + bs[3] * fx) * fy;
                    w = (ws[0] * (1 - fx) + ws[1] * fx) * (1 - fy)
                        + (ws[2] * (1 - fx) + ws[3] * fx) * fy;
                }
                d = 1.0 - b / 255.0;          /* how much shade */
                w = (w / 255.0) * 0.26;       /* a warm lamp tint */
                a = d + w - d * w;
                /* premultiplied: black shade plus a lamp-orange tint */
                A = (int) (a * 255.0);
                R = (int) (255 * w * (1 - d));
                G = (int) (140 * w * (1 - d));
                B = (int) (40 * w * (1 - d));
                bits[py * W + px] = ((DWORD) A << 24) | ((DWORD) R << 16)
                                    | ((DWORD) G << 8) | (DWORD) B;
            }
        }
    }
    ox = data->map_orig.x - data->xPos * data->xFrontTile;
    oy = data->map_orig.y - data->yPos * data->yFrontTile;
    tw = data->xFrontTile, th = data->yFrontTile;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    /* column 0 is never part of the level: leave it alone */
    (void) GdiAlphaBlend(hdc, ox + tw, oy, (COLNO - 1) * tw, ROWNO * th,
                         lightDC, S, 0, W - S, H, bf);
    return flicker;
}

/* where the hero is drawn while sliding onto its square: the offset in
   pixels from the square (0, 0 when standing still) */
static void
fx_hero_slide_offset(PNHMapWindow data, int *offx, int *offy)
{
    float fx, fy;

    fxv_hero_slide(GetTickCount(), &fx, &fy);
    *offx = (int) (fx * data->xFrontTile);
    *offy = (int) (fy * data->yFrontTile);
}

static void
fx_paint_hero_frame(PNHMapWindow data, HDC hdc)
{
    static HBRUSH gold, dark;
    int ox, oy, tw, th, x, y, len, t, k;

    if (!iflags.fx_theme || !iflags.fx_hero_frame || u.uswallow
        || !isok(u.ux, u.uy))
        return;
    if (!gold)
        gold = CreateSolidBrush(TH_ACCENT);
    if (!dark)
        dark = CreateSolidBrush(RGB(10, 10, 14));
    ox = data->map_orig.x - data->xPos * data->xFrontTile;
    oy = data->map_orig.y - data->yPos * data->yFrontTile;
    tw = data->xFrontTile, th = data->yFrontTile;
    x = ox + u.ux * tw, y = oy + u.uy * th;
    {
        int sx, sy;

        fx_hero_slide_offset(data, &sx, &sy); /* move with the hero */
        x += sx, y += sy;
    }
    len = max(3, min(tw, th) / 4);  /* arm length */
    t = max(1, min(tw, th) / 16);   /* thickness */
    if (iflags.fx_hero_frame_light) {
        /* the light version: thin, short, half see-through, no edge */
        static HDC dotDC;
        static HBITMAP dotBmp;
        BLENDFUNCTION bf;
        RECT r[8];
        int j;

        len = max(3, min(tw, th) / 5);
        t = 1;
        if (!dotDC) {
            dotDC = CreateCompatibleDC(hdc);
            dotBmp = CreateCompatibleBitmap(hdc, 1, 1);
            SelectObject(dotDC, dotBmp);
            SetPixel(dotDC, 0, 0, RGB(255, 226, 150));
        }
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 150;
        bf.AlphaFormat = 0;
        SetRect(&r[0], x, y, x + len, y + t);
        SetRect(&r[1], x, y + t, x + t, y + len);
        SetRect(&r[2], x + tw - len, y, x + tw, y + t);
        SetRect(&r[3], x + tw - t, y + t, x + tw, y + len);
        SetRect(&r[4], x, y + th - t, x + len, y + th);
        SetRect(&r[5], x, y + th - len, x + t, y + th - t);
        SetRect(&r[6], x + tw - len, y + th - t, x + tw, y + th);
        SetRect(&r[7], x + tw - t, y + th - len, x + tw, y + th - t);
        for (j = 0; j < 8; j++)
            (void) GdiAlphaBlend(hdc, r[j].left, r[j].top,
                                 r[j].right - r[j].left,
                                 r[j].bottom - r[j].top, dotDC, 0, 0, 1, 1,
                                 bf);
        return;
    }
    for (k = 0; k < 2; k++) {
        /* first a dark outline one pixel wider, then the gold */
        HBRUSH br = k ? gold : dark;
        int g = k ? 0 : 1;
        RECT r[8];
        int j;

        /* top-left, top-right, bottom-left, bottom-right: across, down */
        SetRect(&r[0], x - g, y - g, x + len + g, y + t + g);
        SetRect(&r[1], x - g, y - g, x + t + g, y + len + g);
        SetRect(&r[2], x + tw - len - g, y - g, x + tw + g, y + t + g);
        SetRect(&r[3], x + tw - t - g, y - g, x + tw + g, y + len + g);
        SetRect(&r[4], x - g, y + th - t - g, x + len + g, y + th + g);
        SetRect(&r[5], x - g, y + th - len - g, x + t + g, y + th + g);
        SetRect(&r[6], x + tw - len - g, y + th - t - g, x + tw + g,
                y + th + g);
        SetRect(&r[7], x + tw - t - g, y + th - len - g, x + tw + g,
                y + th + g);
        for (j = 0; j < 8; j++)
            FillRect(hdc, &r[j], br);
    }
}

/* a keypress snaps every sprite back to its square (design rule 2) */
void
mswin_map_fx_snap(void)
{
    PNHMapWindow data = (PNHMapWindow) windowdata[NHW_MAP].address;

    if (fxv_snap() && data)
        InvalidateRect(data->hWnd, NULL, FALSE);
}

/* draw animating creatures off their squares onto the composed image */
static void
fx_paint_sprites(PNHMapWindow data, HDC hdc)
{
    DWORD now = GetTickCount();
    HBITMAP saved;
    int i, ox, oy, tw, th;

    if (!fxv.nanims)
        return;
    ox = data->map_orig.x - data->xPos * data->xFrontTile;
    oy = data->map_orig.y - data->yPos * data->yFrontTile;
    tw = data->xFrontTile, th = data->yFrontTile;
    saved = SelectObject(data->tileDC, GetNHApp()->bmpMapTiles);
    for (i = 0; i < fxv.nanims; i++) {
        struct fxv_anim *a = &fxv.anims[i];
        float fx, fy;
        int cx, cy, offx, offy, alpha;
        short bk;

        /* where it is this frame is shared (fxview.c); drawing it is
           here */
        if (!fxv_anim_pose(a, now, &fx, &fy, &alpha))
            continue;
        offx = (int) (fx * tw), offy = (int) (fy * th);
        cx = ox + a->x * tw, cy = oy + a->y * th;
        if (a->kind == FXM_GHOST || a->kind == FXM_DEATH) {
            /* an afterimage, or a dying creature fading out over what's
               left there */
            fx_blend_tile(data, hdc, cx + offx, cy + offy, tw, th, a->tile,
                          alpha);
            continue;
        }
        /* the floor under it, so it doesn't leave a copy behind */
        bk = fx_floor_tile(data, a->x, a->y);
        if (bk >= 0) {
            StretchBlt(hdc, cx, cy, tw, th, data->tileDC, TILEBMP_X(bk),
                       TILEBMP_Y(bk), GetNHApp()->mapTile_X,
                       GetNHApp()->mapTile_Y, SRCCOPY);
        } else {
            RECT r;

            SetRect(&r, cx, cy, cx + tw, cy + th);
            FillRect(hdc, &r, (HBRUSH) GetStockObject(BLACK_BRUSH));
        }
        if ((data->map[a->x][a->y].fxauras & FXA_INVIS) && iflags.fx_auras)
            fx_blend_tile(data, hdc, cx + offx, cy + offy, tw, th, a->tile,
                          FXV_INVIS_ALPHA);
        else
            (*GetNHApp()->lpfnTransparentBlt)(
                hdc, cx + offx, cy + offy, tw, th, data->tileDC,
                TILEBMP_X(a->tile), TILEBMP_Y(a->tile),
                GetNHApp()->mapTile_X, GetNHApp()->mapTile_Y, TILE_BK_COLOR);
    }
    SelectObject(data->tileDC, saved);
}

/* the floor tile under a creature on <x,y>; for plain lit room floor the
   core sends no background glyph (it reports GLYPH_UNEXPLORED and the
   creature tile is drawn opaque), so it's the room-floor tile */
static short
fx_floor_tile(PNHMapWindow data, int x, int y)
{
    if (data->bkmap[x][y].glyph != NO_GLYPH
        && data->bkmap[x][y].glyph != GLYPH_UNEXPLORED)
        return data->bkmap[x][y].gm.tileidx;
    {
        glyph_info floorgi;

        map_glyphinfo(x, y, cmap_to_glyph(S_room), 0, &floorgi);
        return floorgi.gm.tileidx;
    }
}

/* creatures that are invisible but seen anyway (see invisible, or the
   hero's own invisibility) are drawn see-through: the floor goes back
   under them and they're laid on top only partly opaque; creatures in
   the middle of an animation are handled by fx_paint_sprites() */
static void
fx_paint_translucent(PNHMapWindow data, HDC hdc)
{
    DWORD now = GetTickCount();
    HBITMAP saved;
    int x, y, ox, oy, tw, th;

    if (!iflags.fx_auras || data->bAsciiMode || Is_rogue_level(&u.uz))
        return;
    ox = data->map_orig.x - data->xPos * data->xFrontTile;
    oy = data->map_orig.y - data->yPos * data->yFrontTile;
    tw = data->xFrontTile, th = data->yFrontTile;
    saved = SelectObject(data->tileDC, GetNHApp()->bmpMapTiles);
    for (x = 1; x < COLNO; x++)
        for (y = 0; y < ROWNO; y++) {
            short bk;
            int cx, cy;
            if (!(data->map[x][y].fxauras & FXA_INVIS)
                || !glyph_is_monster(data->map[x][y].glyph))
                continue;
            if (fxv_square_busy(x, y, now))
                continue; /* drawn by fx_paint_sprites() */
            cx = ox + x * tw, cy = oy + y * th;
            if ((bk = fx_floor_tile(data, x, y)) >= 0)
                StretchBlt(hdc, cx, cy, tw, th, data->tileDC, TILEBMP_X(bk),
                           TILEBMP_Y(bk), GetNHApp()->mapTile_X,
                           GetNHApp()->mapTile_Y, SRCCOPY);
            fx_blend_tile(data, hdc, cx, cy, tw, th,
                          data->map[x][y].gm.tileidx, FXV_INVIS_ALPHA);
        }
    SelectObject(data->tileDC, saved);
}

/* animation frame: expire old numbers, stop the timer when idle */
static void
fx_tick(PNHMapWindow data)
{
    DWORD now = GetTickCount();

    if (!fxv_expire(now) && data->fxTimerOn) {
        KillTimer(data->hWnd, FX_TIMER_ID);
        data->fxTimerOn = FALSE;
    }
    /* one more repaint after the last number goes, to erase it */
    InvalidateRect(data->hWnd, NULL, FALSE);
}

static void
fx_make_fonts(PNHMapWindow data)
{
    LOGFONT lf;
    int h = data->yFrontTile;

    if (data->hFxFont && data->fxFontTileH == h)
        return;
    if (data->hFxFont)
        DeleteObject(data->hFxFont);
    if (data->hFxFontBig)
        DeleteObject(data->hFxFontBig);
    if (data->hFxFontSmall)
        DeleteObject(data->hFxFontSmall);
    ZeroMemory(&lf, sizeof lf);
    lf.lfWeight = FW_HEAVY;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    strcpy(lf.lfFaceName, "Segoe UI");
    lf.lfHeight = -max(12, (h * 3) / 5);
    data->hFxFont = CreateFontIndirect(&lf);
    lf.lfHeight = -max(16, (h * 17) / 20);
    data->hFxFontBig = CreateFontIndirect(&lf);
    lf.lfWeight = FW_BOLD;
    lf.lfHeight = -max(11, (h * 2) / 5);
    data->hFxFontSmall = CreateFontIndirect(&lf);
    data->fxFontTileH = h;
}

/* draw floating numbers onto the composed front-buffer image */
static void
fx_paint_overlay(PNHMapWindow data, HDC hdc)
{
    DWORD now = GetTickCount();
    int i, ox, oy, o, dx, dy;
    HFONT oldFont;
    int oldBk;

    if (!fxv.nfloaters)
        return;
    fx_make_fonts(data);
    oldFont = SelectObject(hdc, data->hFxFont);
    oldBk = SetBkMode(hdc, TRANSPARENT);
    ox = data->map_orig.x - data->xPos * data->xFrontTile;
    oy = data->map_orig.y - data->yPos * data->yFrontTile;
    o = max(1, data->yFrontTile / 24); /* outline thickness */

    for (i = 0; i < fxv.nfloaters; i++) {
        struct fxv_floater *f = &fxv.floaters[i];
        char buf[32];
        RECT r;
        int cx, cy, fade;
        float fcx, fcy, bright;
        COLORREF c = f->color;

        if (!fxv_floater_at(f, now, &fcx, &fcy, &bright))
            continue;
        cx = ox + (int) (fcx * data->xFrontTile);
        cy = oy + (int) (fcy * data->yFrontTile);
        /* fade by darkening near the end (the map is mostly dark, so
           this reads as fading out without alpha blending) */
        if (bright < 1.0f) {
            fade = (int) (255 * bright);
            c = RGB(GetRValue(c) * fade / 255, GetGValue(c) * fade / 255,
                    GetBValue(c) * fade / 255);
        }
        fxv_floater_text(f, buf, sizeof buf);
        SelectObject(hdc, f->big ? data->hFxFontBig
                          : f->text[0] ? data->hFxFontSmall : data->hFxFont);
        SetRect(&r, cx - data->xFrontTile * 2, cy - data->yFrontTile,
                cx + data->xFrontTile * 2, cy + data->yFrontTile);
        /* black outline */
        SetTextColor(hdc, RGB(0, 0, 0));
        for (dx = -o; dx <= o; dx += o)
            for (dy = -o; dy <= o; dy += o) {
                RECT rr = r;

                if (!dx && !dy)
                    continue;
                OffsetRect(&rr, dx, dy);
                DrawTextA(hdc, buf, -1, &rr,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE
                              | DT_NOPREFIX | DT_NOCLIP);
            }
        SetTextColor(hdc, c);
        DrawTextA(hdc, buf, -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX
                      | DT_NOCLIP);
    }
    SetBkMode(hdc, oldBk);
    SelectObject(hdc, oldFont);
}

/* thin bar along the bottom of a hurt creature's square (back buffer) */
static void
fx_paint_healthbar(PNHMapWindow data, int i, int j, RECT *rect)
{
    int pct = data->map[i][j].fxhealth;
    int h, w, fillw;
    RECT bar;
    HBRUSH br;
    COLORREF c;

    if (!iflags.fx_healthbars || pct <= 0 || pct >= 100)
        return;
    h = max(2, data->yBackTile / 10);
    w = (rect->right - rect->left) - 4;
    if (w < 4)
        return;
    SetRect(&bar, rect->left + 2, rect->bottom - h - 1, rect->right - 2,
            rect->bottom - 1);
    br = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(data->backBufferDC, &bar, br);
    DeleteObject(br);
    fillw = max(1, (w * pct) / 100);
    c = pct > 50 ? RGB(60, 220, 60) : pct > 25 ? RGB(240, 200, 40)
                                               : RGB(230, 50, 40);
    SetRect(&bar, rect->left + 2, rect->bottom - h - 1,
            rect->left + 2 + fillw, rect->bottom - 1);
    br = CreateSolidBrush(c);
    FillRect(data->backBufferDC, &bar, br);
    DeleteObject(br);
}

/* on WM_VSCROLL */
void
onMSNH_VScroll(HWND hWnd, WPARAM wParam, LPARAM lParam)
{
    PNHMapWindow data;
    SCROLLINFO si;
    int yNewPos;
    int yDelta;

    UNREFERENCED_PARAMETER(lParam);

    /* get window data */
    data = (PNHMapWindow) GetWindowLongPtr(hWnd, GWLP_USERDATA);

    switch (LOWORD(wParam)) {
    /* User clicked shaft left of the scroll box. */
    case SB_PAGEUP:
        yNewPos = data->yPos - data->yPageSize;
        break;

    /* User clicked shaft right of the scroll box. */
    case SB_PAGEDOWN:
        yNewPos = data->yPos + data->yPageSize;
        break;

    /* User clicked the left arrow. */
    case SB_LINEUP:
        yNewPos = data->yPos - 1;
        break;

    /* User clicked the right arrow. */
    case SB_LINEDOWN:
        yNewPos = data->yPos + 1;
        break;

    /* User dragged the scroll box. */
    case SB_THUMBTRACK:
        yNewPos = HIWORD(wParam);
        break;

    default:
        yNewPos = data->yPos;
    }

    yNewPos = max(0, min(data->yMax, yNewPos));
    if (yNewPos == data->yPos)
        return;

    yDelta = yNewPos - data->yPos;
    data->yPos = yNewPos;

    ScrollWindowEx(hWnd, 0, -data->yFrontTile * yDelta, (CONST RECT *) NULL,
                   (CONST RECT *) NULL, (HRGN) NULL, (LPRECT) NULL,
                   SW_INVALIDATE | SW_ERASE);

    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    si.nPos = data->yPos;
    fx_set_scroll(hWnd, SB_VERT, &si, TRUE);
}

/* on WM_HSCROLL */
void
onMSNH_HScroll(HWND hWnd, WPARAM wParam, LPARAM lParam)
{
    PNHMapWindow data;
    SCROLLINFO si;
    int xNewPos;
    int xDelta;

    UNREFERENCED_PARAMETER(lParam);

    /* get window data */
    data = (PNHMapWindow) GetWindowLongPtr(hWnd, GWLP_USERDATA);

    switch (LOWORD(wParam)) {
    /* User clicked shaft left of the scroll box. */
    case SB_PAGEUP:
        xNewPos = data->xPos - data->xPageSize;
        break;

    /* User clicked shaft right of the scroll box. */
    case SB_PAGEDOWN:
        xNewPos = data->xPos + data->xPageSize;
        break;

    /* User clicked the left arrow. */
    case SB_LINEUP:
        xNewPos = data->xPos - 1;
        break;

    /* User clicked the right arrow. */
    case SB_LINEDOWN:
        xNewPos = data->xPos + 1;
        break;

    /* User dragged the scroll box. */
    case SB_THUMBTRACK:
        xNewPos = HIWORD(wParam);
        break;

    default:
        xNewPos = data->xPos;
    }

    xNewPos = max(0, min(data->xMax, xNewPos));
    if (xNewPos == data->xPos)
        return;

    xDelta = xNewPos - data->xPos;
    data->xPos = xNewPos;

    ScrollWindowEx(hWnd, -data->xFrontTile * xDelta, 0, (CONST RECT *) NULL,
                   (CONST RECT *) NULL, (HRGN) NULL, (LPRECT) NULL,
                   SW_INVALIDATE | SW_ERASE);

    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    si.nPos = data->xPos;
    fx_set_scroll(hWnd, SB_HORZ, &si, TRUE);
}

/* map nethack map coordinates to the screen location */
void
nhcoord2display(PNHMapWindow data, int x, int y, LPRECT lpOut)
{
    lpOut->left = (x - data->xPos) * data->xFrontTile + data->map_orig.x;
    lpOut->top = (y - data->yPos) * data->yFrontTile + data->map_orig.y;
    lpOut->right = lpOut->left + data->xFrontTile;
    lpOut->bottom = lpOut->top + data->yFrontTile;
}

#if (VERSION_MAJOR < 4) && (VERSION_MINOR < 4) && (PATCHLEVEL < 2)
/* map glyph to character/color combination */
void
nhglyph2charcolor(short g, uchar *ch, int *color)
{
    int offset;

#define zap_color(n) *color = iflags.use_color ? zapcolors[n] : NO_COLOR
#define cmap_color(n) *color = iflags.use_color ? defsyms[n].color : NO_COLOR
#define obj_color(n) \
    *color = iflags.use_color ? objects[n].oc_color : NO_COLOR
#define mon_color(n) *color = iflags.use_color ? mons[n].mcolor : NO_COLOR
#define pet_color(n) *color = iflags.use_color ? mons[n].mcolor : NO_COLOR
#define warn_color(n) \
    *color = iflags.use_color ? def_warnsyms[n].color : NO_COLOR

    if ((offset = (g - GLYPH_WARNING_OFF)) >= 0) { /* a warning flash */
        *ch = showsyms[offset + SYM_OFF_W];
        warn_color(offset);
    } else if ((offset = (g - GLYPH_SWALLOW_OFF)) >= 0) { /* swallow */
        /* see swallow_to_glyph() in display.c */
        *ch = (uchar) showsyms[(S_sw_tl + (offset & 0x7)) + SYM_OFF_P];
        mon_color(offset >> 3);
    } else if ((offset = (g - GLYPH_ZAP_OFF)) >= 0) { /* zap beam */
        /* see zapdir_to_glyph() in display.c */
        *ch = showsyms[(S_vbeam + (offset & 0x3)) + SYM_OFF_P];
        zap_color((offset >> 2));
    } else if ((offset = (g - GLYPH_CMAP_OFF)) >= 0) { /* cmap */
        *ch = showsyms[offset + SYM_OFF_P];
        cmap_color(offset);
    } else if ((offset = (g - GLYPH_OBJ_OFF)) >= 0) { /* object */
        *ch = showsyms[(int) objects[offset].oc_class + SYM_OFF_O];
        obj_color(offset);
    } else if ((offset = (g - GLYPH_BODY_OFF)) >= 0) { /* a corpse */
        *ch = showsyms[(int) objects[CORPSE].oc_class + SYM_OFF_O];
        mon_color(offset);
    } else if ((offset = (g - GLYPH_PET_OFF)) >= 0) { /* a pet */
        *ch = showsyms[(int) mons[offset].mlet + SYM_OFF_M];
        pet_color(offset);
    } else { /* a monster */
        *ch = showsyms[(int) mons[g].mlet + SYM_OFF_M];
        mon_color(g);
    }
    // end of wintty code
}
#endif

/* map nethack color to RGB */
COLORREF
nhcolor_to_RGB(int c)
{
    if (c >= 0 && c < CLR_MAX && iflags.fx_theme && c < 16)
        return th_palette[c]; /* NetHack Modern: tuned for dark panels */
    if (c >= 0 && c < CLR_MAX)
        return GetNHApp()->regMapColors[c];
    return RGB(0x00, 0x00, 0x00);
}
