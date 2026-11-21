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

#define NHMAP_FONT_NAME TEXT("Terminal")
#define NHMAP_TTFONT_NAME TEXT("Consolas")
#define MAXWINDOWTEXT 255

#define CURSOR_BLINK_INTERVAL 1000 // milliseconds
#define CURSOR_HEIGHT 2 // pixels

/* NetHack Modern floating numbers */
#define CURSOR_TIMER_ID 0
#define FX_TIMER_ID 2
#define FX_FRAME_MS 30      /* ~33 frames per second while numbers float */
#define FX_FLOAT_MS 900     /* lifetime of one number */
#define FX_TEXT_MS 1400     /* lifetime of a status name */
#define FX_LEVEL_MS 2600    /* lifetime of "Level N!" */
#define FX_KILL_MS 1100     /* lifetime of "Killed" */
#define FX_STACK_MS 400     /* a new number on a square this young stacks */
#define FX_MAXFLOAT 64
#define FX_MAXANIM 32
#define FXM_LUNGE 1
#define FXM_SHAKE 2
#define FXM_DEATH 3
#define FX_DEATH_MS 360     /* shake ~35%, then fade out */
#define FX_LUNGE_MS 150     /* out ~40%, back ~60% */
#define FX_LUNGE_GAP 110    /* successive lunges in one turn queue up */
#define FX_SHAKE_MS 220

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

    /* NetHack Modern: floating damage/heal numbers (see fx.h) */
    struct fx_floater {
        int x, y;               /* map square */
        unsigned target;        /* m_id, 0 = hero */
        int type;               /* FXE_DAMAGE, FXE_HEAL or FXE_STATUS */
        int amount;
        char text[24];          /* status name; empty for numbers */
        DWORD life;             /* how long it floats, ms */
        int stack;              /* raised this many slots to avoid overlap */
        int xoff;               /* sideways jitter, percent of a tile */
        boolean low;            /* starts at the health bar, rises less */
        boolean big;            /* large hit: drawn bigger */
        COLORREF color;
        DWORD start;            /* GetTickCount() when (re)started */
    } floaters[FX_MAXFLOAT];
    int nfloaters;
    /* NetHack Modern Phase 4: sprites drawn off their square for a moment
       (melee lunge, hit shake); the map state itself never moves */
    struct fx_anim {
        int kind;               /* FXM_LUNGE or FXM_SHAKE */
        int x, y;               /* square whose creature moves */
        int dx, dy;             /* lunge direction */
        short tile;             /* creature tile when the event happened */
        boolean hit;            /* lunge connected */
        int strength;           /* shake: 1..100 */
        DWORD start, dur;       /* GetTickCount() window */
    } anims[FX_MAXANIM];
    int nanims;
    DWORD animSeq;              /* when the queued lunges finish */
    HDC fadeDC;                 /* scratch tile for fading sprites */
    HBITMAP fadeBmp, fadeBmpOld;
    int fadeW, fadeH;
    boolean fxTimerOn;
    DWORD shakeStart, shakeDur; /* screen shake (0 dur = none) */
    int shakeStrength;          /* 1..100 */
    HFONT hFxFont, hFxFontBig;  /* fonts for the numbers */
    HFONT hFxFontSmall;         /* font for status names */
    int fxFontTileH;            /* yFrontTile the fonts were made for */
    HDC compDC;                 /* composition buffer for onPaint */
    HBITMAP compBmp, compBmpOld;
    int compW, compH;

} NHMapWindow, *PNHMapWindow;

static TCHAR szNHMapWindowClass[] = TEXT("MSNethackMapWndClass");
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
static void fx_paint_overlay(PNHMapWindow data, HDC hdc);
static boolean fx_shake_offset(PNHMapWindow, int *, int *);
static void fx_paint_sprites(PNHMapWindow, HDC);
static void fx_paint_healthbar(PNHMapWindow data, int i, int j, RECT *rect);

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
    SetScrollInfo(hWnd, SB_HORZ, &si, TRUE);

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
    SetScrollInfo(hWnd, SB_VERT, &si, TRUE);

    mswin_cliparound(data->xCur, data->yCur);

    // redraw all map locations
    dirtyAll(data);

    // invalidate entire map window
    InvalidateRect(hWnd, NULL, TRUE);
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
            /* rely on tileidx provided by NetHack core */
            ntile = data->map[i][j].gm.tileidx;
            t_x = TILEBMP_X(ntile);
            t_y = TILEBMP_Y(ntile);

            /* Don't use all black GLYPH_UNEXPLORED tile as a background */
            if (layer > 0 && bkglyph != GLYPH_UNEXPLORED) {
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
    data->nfloaters = 0; /* new level or full redraw: drop numbers */
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
    boolean shaking = fx_shake_offset(data, &shx, &shy);

    if (data->nfloaters > 0 || data->nanims > 0 || shaking) {
        /* compose map + floating numbers off screen, then copy, so the
           numbers don't flicker */
        RECT client;
        HDC hdc;

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
        FillRect(hdc, &client, (HBRUSH) GetStockObject(BLACK_BRUSH));
        /* a screen shake moves everything drawn this frame */
        data->map_orig.x += shx, data->map_orig.y += shy;
        StretchBlt(hdc,
            data->map_orig.x - (data->xPos * data->xFrontTile),
            data->map_orig.y - (data->yPos * data->yFrontTile), frontWidth,
            frontHeight, data->backBufferDC, 0, 0, data->backWidth,
            data->backHeight, SRCCOPY);
        fx_paint_sprites(data, hdc);
        fx_paint_overlay(data, hdc);
        data->map_orig.x -= shx, data->map_orig.y -= shy;
        BitBlt(hFrontBufferDC, ps.rcPaint.left, ps.rcPaint.top,
               ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, hdc, ps.rcPaint.left,
               ps.rcPaint.top, SRCCOPY);
    } else {
        StretchBlt(hFrontBufferDC,
            data->map_orig.x - (data->xPos * data->xFrontTile),
            data->map_orig.y - (data->yPos * data->yFrontTile), frontWidth, frontHeight,
                    data->backBufferDC, 0, 0, data->backWidth, data->backHeight, SRCCOPY);
    }

    EndPaint(hWnd, &ps);
}

/* ------------------------------------------------------------------ */
/* NetHack Modern: damage numbers and health bars (Phase 2)            */

static COLORREF
fx_kind_color(const struct fx_event *ev)
{
    if (ev->type == FXE_HEAL)
        return RGB(90, 255, 90);
    switch (ev->adtyp) {
    case AD_FIRE:
        return RGB(255, 150, 40);
    case AD_COLD:
        return RGB(120, 200, 255);
    case AD_ELEC:
        return RGB(255, 240, 80);
    case AD_DRST:
    case AD_DRDX:
    case AD_DRCO:
        return RGB(140, 230, 60);
    case AD_ACID:
        return RGB(200, 255, 60);
    case AD_MAGM:
        return RGB(230, 120, 255);
    case AD_SLEE:
        return RGB(170, 140, 255);
    case AD_DISN:
        return RGB(190, 190, 190);
    case AD_DRLI:
        return RGB(190, 80, 210);
    }
    /* physical or unknown: red when it's you getting hurt */
    return (ev->flags & FXF_HERO_TARGET) ? RGB(255, 70, 70)
                                         : RGB(255, 255, 255);
}

static struct fx_floater *fx_add_floater(PNHMapWindow, const struct fx_event *,
                                         int, const char *, COLORREF);
static void fx_status_floaters(PNHMapWindow, const struct fx_event *);
static void fx_motion_event(PNHMapWindow, const struct fx_event *);
static void fx_paint_sprites(PNHMapWindow, HDC);
static void fx_start_timer(PNHMapWindow);
static void fx_shake_event(PNHMapWindow, const struct fx_event *);
static void fx_blend_tile(PNHMapWindow, HDC, int, int, int, int, short, int);

/* called by the core (fx.c) */
static void
mswin_fx_event(const struct fx_event *ev)
{
    PNHMapWindow data = (PNHMapWindow) windowdata[NHW_MAP].address;
    struct fx_floater *f;
    DWORD now = GetTickCount();
    int i;

    if (!data)
        return;
    fx_motion_event(data, ev); /* lunges and shakes */
    fx_shake_event(data, ev);  /* the whole view */
    if (ev->type == FXE_STATUS) {
        if (iflags.fx_status_text)
            fx_status_floaters(data, ev);
        return;
    }
    if (ev->type == FXE_EXP) {
        /* "+20 XP" in light blue over the hero */
        if (iflags.fx_exp_text) {
            char buf[32];

            Snprintf(buf, sizeof buf, "+%d XP", ev->amount);
            f = fx_add_floater(data, ev, 0, buf, RGB(120, 200, 255));
            f->xoff = 0;
        }
        return;
    }
    if (ev->type == FXE_LEVEL) {
        /* "Level 5!" in big gold letters that linger above the hero */
        if (iflags.fx_status_text) {
            char buf[32];
            boolean up = (ev->flags & FXF_HIT) != 0;

            Snprintf(buf, sizeof buf, up ? "Level %d!" : "Level %d",
                     ev->amount);
            f = fx_add_floater(data, ev, 0, buf,
                               up ? RGB(255, 205, 60) : RGB(170, 120, 210));
            f->big = TRUE;
            f->life = FX_LEVEL_MS;
            f->xoff = 0; /* centred over the hero */
        }
        return;
    }
    if (!iflags.fx_damage_numbers)
        return;
    if (ev->type == FXE_DEATH) {
        /* "Killed" where the health bar was (pets: a grey "Died") */
        boolean pet = (ev->flags & FXF_TARGET_PET) != 0;

        f = fx_add_floater(data, ev, 0, pet ? "Died" : "Killed",
                           pet ? RGB(170, 170, 170) : RGB(235, 60, 50));
        f->life = FX_KILL_MS;
        f->low = TRUE;
        f->xoff = 0;
        f->stack = 0;
        return;
    }
    if (ev->type == FXE_MISS) {
        /* "Miss" floats up off whoever dodged */
        (void) fx_add_floater(data, ev, 0, "Miss", RGB(190, 190, 190));
        return;
    }
    if (ev->type != FXE_DAMAGE && ev->type != FXE_HEAL)
        return;
    if (ev->amount <= 0)
        return;
    /* small heals are almost always regeneration; showing every one
       would be noise */
    if (ev->type == FXE_HEAL && ev->amount < max(2, ev->hpmax / 10))
        return;

    /* while running/travelling/occupied, keep one running total per
       creature instead of a pile of numbers */
    if (ev->flags & FXF_BULK) {
        for (i = 0; i < data->nfloaters; i++) {
            f = &data->floaters[i];
            if (f->target == ev->target_id && f->type == ev->type) {
                f->amount += ev->amount;
                f->x = ev->x, f->y = ev->y;
                f->start = now;
                f->color = fx_kind_color(ev);
                return;
            }
        }
    }

    f = fx_add_floater(data, ev, ev->amount, (const char *) 0,
                       fx_kind_color(ev));
    f->big = (ev->type == FXE_DAMAGE
              && (ev->hp <= 0 || ev->amount * 4 >= max(ev->hpmax, 1)));
}

/* start a floater over ev's square: a number, or text if not null */
static struct fx_floater *
fx_add_floater(PNHMapWindow data, const struct fx_event *ev, int amount,
               const char *text, COLORREF color)
{
    struct fx_floater *f;
    DWORD now = GetTickCount();
    int i, stack = 0;

    /* raise above floaters still young on the same square */
    for (i = 0; i < data->nfloaters; i++) {
        f = &data->floaters[i];
        if (f->x == ev->x && f->y == ev->y && now - f->start < FX_STACK_MS
            && f->stack >= stack)
            stack = f->stack + 1;
    }

    if (data->nfloaters >= FX_MAXFLOAT) {
        /* drop the oldest */
        memmove(&data->floaters[0], &data->floaters[1],
                (FX_MAXFLOAT - 1) * sizeof data->floaters[0]);
        data->nfloaters--;
    }
    f = &data->floaters[data->nfloaters++];
    f->x = ev->x, f->y = ev->y;
    f->target = ev->target_id;
    f->type = ev->type;
    f->amount = amount;
    f->text[0] = '\0';
    if (text)
        Strcpy(f->text, text);
    f->life = text ? FX_TEXT_MS : FX_FLOAT_MS;
    f->low = FALSE;
    f->stack = min(stack, 4);
    {
        /* jitter sideways so repeated numbers don't sit exactly on top of
           each other; a private generator, never the game's RNG */
        static unsigned long seed = 0;

        if (!seed)
            seed = (unsigned long) now | 1UL;
        seed = seed * 1103515245UL + 12345UL;
        f->xoff = (int) ((seed >> 16) % 61UL) - 30; /* -30..+30 */
    }
    f->big = FALSE;
    f->color = color;
    f->start = now;

    if (!data->fxTimerOn) {
        SetTimer(data->hWnd, FX_TIMER_ID, FX_FRAME_MS, NULL);
        data->fxTimerOn = TRUE;
    }
    InvalidateRect(data->hWnd, NULL, FALSE);
    return f;
}

/* status names, ToME style: "Confused" floats up when it lands */
#define FXS_GOOD RGB(255, 215, 90)
#define FXS_BAD RGB(255, 95, 85)
#define FXS_MIND RGB(205, 130, 255)
#define FXS_BODY RGB(160, 225, 80)
#define FXS_LOST RGB(175, 175, 175)
static const struct fx_status_name {
    unsigned long bit;
    const char *name;
    COLORREF color;
} fx_status_names[] = {
    { FXA_VERYFAST, "Very fast", FXS_GOOD },
    { FXA_FAST, "Fast", FXS_GOOD },
    { FXA_SLOW, "Slowed", FXS_BAD },
    { FXA_INVIS, "Invisible", FXS_GOOD },
    { FXA_LEVITATE, "Levitating", FXS_GOOD },
    { FXA_FLYING, "Flying", FXS_GOOD },
    { FXA_REFLECT, "Reflecting", FXS_GOOD },
    { FXA_PROTECT, "Protected", FXS_GOOD },
    { FXA_CONFUSED, "Confused", FXS_MIND },
    { FXA_STUNNED, "Stunned", FXS_MIND },
    { FXA_HALLU, "Hallucinating", FXS_MIND },
    { FXA_STONING, "Stoning!", FXS_BAD },
    { FXA_SLIMED, "Slimed!", FXS_BODY },
    { FXA_SICK, "Sick", FXS_BODY },
    { FXA_BLIND, "Blinded", FXS_BAD },
    { FXA_FLEEING, "Fleeing", FXS_BAD },
    { FXA_CANCELLED, "Cancelled", FXS_BAD },
    { FXA_HELPLESS, "Paralyzed", FXS_BAD },
    { FXA_ASLEEP, "Asleep", FXS_MIND },
};

static void
fx_status_floaters(PNHMapWindow data, const struct fx_event *ev)
{
    unsigned long gain = ev->st_gain, lose = ev->st_lose;
    char buf[32];
    int i;

    /* speeding up from fast to very fast isn't losing "Fast" */
    if ((gain & FXA_VERYFAST) && (lose & FXA_FAST))
        lose &= ~FXA_FAST;
    if ((lose & FXA_VERYFAST) && (gain & FXA_FAST))
        gain &= ~FXA_FAST; /* "-Very fast" says enough */
    if (ev->flags & FXF_BULK)
        lose = 0UL; /* while running, only news that matters */
    /* a pet "flees" for a few turns when the game makes it step out of
       the hero's way ("You stop.  Your dog is in the way!"); that isn't
       fear, so don't announce it */
    if (ev->flags & FXF_TARGET_PET) {
        gain &= ~FXA_FLEEING;
        lose &= ~FXA_FLEEING;
    }
    for (i = 0; i < SIZE(fx_status_names); i++) {
        const struct fx_status_name *s = &fx_status_names[i];

        if (gain & s->bit)
            (void) fx_add_floater(data, ev, 0, s->name, s->color);
        if (lose & s->bit) {
            Snprintf(buf, sizeof buf, "-%s", s->name);
            (void) fx_add_floater(data, ev, 0, buf, FXS_LOST);
        }
    }
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

static struct fx_anim *
fx_new_anim(PNHMapWindow data)
{
    if (data->nanims >= FX_MAXANIM) {
        memmove(&data->anims[0], &data->anims[1],
                (FX_MAXANIM - 1) * sizeof data->anims[0]);
        data->nanims--;
    }
    return &data->anims[data->nanims++];
}

/* melee attacks lunge the attacker toward its target; damage shakes the
   victim. Only in tiles mode, never while running, and only for a
   creature actually drawn on that square. */
static void
fx_motion_event(PNHMapWindow data, const struct fx_event *ev)
{
    DWORD now = GetTickCount();
    struct fx_anim *a;
    int dx, dy, i;

    if (!iflags.fx_motion || (ev->flags & FXF_BULK)
        || data->bAsciiMode || Is_rogue_level(&u.uz))
        return;

    if (ev->type == FXE_ATTACK && !(ev->flags & FXF_RANGED)) {
        dx = ev->x - ev->sx, dy = ev->y - ev->sy;
        if ((!dx && !dy) || dx < -1 || dx > 1 || dy < -1 || dy > 1)
            return; /* not adjacent */
        if (!isok(ev->sx, ev->sy)
            || !glyph_is_monster(data->map[ev->sx][ev->sy].glyph))
            return; /* attacker not visibly there */
        a = fx_new_anim(data);
        a->kind = FXM_LUNGE;
        a->x = ev->sx, a->y = ev->sy;
        a->dx = dx, a->dy = dy;
        a->tile = data->map[ev->sx][ev->sy].gm.tileidx;
        a->hit = (ev->flags & FXF_HIT) != 0;
        a->strength = 0;
        /* several attacks in one turn play one after another */
        a->start = ((long) (data->animSeq - now) > 0) ? data->animSeq : now;
        a->dur = FX_LUNGE_MS;
        data->animSeq = a->start + FX_LUNGE_GAP;
        fx_start_timer(data);
    } else if (ev->type == FXE_DAMAGE && ev->amount > 0) {
        DWORD start = now;
        int pct;

        if (!isok(ev->x, ev->y)
            || !glyph_is_monster(data->map[ev->x][ev->y].glyph))
            return;
        /* start at the peak of a lunge that just went for this square */
        for (i = data->nanims - 1; i >= 0; i--) {
            struct fx_anim *l = &data->anims[i];

            if (l->kind == FXM_LUNGE && l->x + l->dx == ev->x
                && l->y + l->dy == ev->y
                && (long) (l->start + l->dur - now) > 0) {
                start = l->start + (l->dur * 2) / 5;
                break;
            }
        }
        pct = (ev->amount * 100) / max(ev->hpmax, 1);
        a = fx_new_anim(data);
        a->kind = FXM_SHAKE;
        a->x = ev->x, a->y = ev->y;
        a->dx = a->dy = 0;
        a->tile = data->map[ev->x][ev->y].gm.tileidx;
        a->hit = TRUE;
        a->strength = max(15, min(100, pct * 3));
        a->start = start;
        a->dur = FX_SHAKE_MS;
        fx_start_timer(data);
    } else if (ev->type == FXE_DEATH) {
        DWORD start = now;

        if (!isok(ev->x, ev->y)
            || !glyph_is_monster(data->map[ev->x][ev->y].glyph))
            return; /* nothing of it drawn there to fade */
        /* follow the killing blow's shake on this square */
        for (i = data->nanims - 1; i >= 0; i--) {
            struct fx_anim *l = &data->anims[i];

            if (l->kind == FXM_SHAKE && l->x == ev->x && l->y == ev->y
                && (long) (l->start + l->dur - now) > 0) {
                start = l->start + l->dur / 2;
                l->dur = l->dur / 2; /* the death shake takes over */
                break;
            }
        }
        a = fx_new_anim(data);
        a->kind = FXM_DEATH;
        a->x = ev->x, a->y = ev->y;
        a->dx = a->dy = 0;
        a->tile = data->map[ev->x][ev->y].gm.tileidx;
        a->hit = TRUE;
        a->strength = 60;
        a->start = start;
        a->dur = FX_DEATH_MS;
        fx_start_timer(data);
    }
}

/* explosions, earthquakes and very big hits on the hero shake the whole
   map view (tiles and ASCII alike); never while running */
static void
fx_shake_event(PNHMapWindow data, const struct fx_event *ev)
{
    DWORD now = GetTickCount();
    int s = 0;

    if (!iflags.fx_screen_shake || (ev->flags & FXF_BULK))
        return;
    if (ev->type == FXE_SHAKE) {
        s = ev->amount;
    } else if (ev->type == FXE_EXPLODE && !(ev->flags & FXF_UNSEEN)) {
        s = 30 + ev->amount * 2; /* harder for bigger blasts */
    } else if (ev->type == FXE_DAMAGE && (ev->flags & FXF_HERO_TARGET)
               && ev->hpmax > 0 && ev->amount * 4 >= ev->hpmax) {
        /* lost a quarter or more of max HP in one go: a small jolt */
        s = 15 + min(25, (ev->amount * 100 / ev->hpmax - 25) / 2);
    }
    if (s <= 0)
        return;
    s = min(s, 100);
    /* don't cut a bigger shake that's still going short */
    if (data->shakeDur && (long) (data->shakeStart + data->shakeDur - now) > 0
        && data->shakeStrength >= s)
        return;
    data->shakeStart = now;
    data->shakeStrength = s;
    data->shakeDur = 280 + 4 * s;
    fx_start_timer(data);
}

/* where the view sits this frame; FALSE when not shaking */
static boolean
fx_shake_offset(PNHMapWindow data, int *dx, int *dy)
{
    long age;
    double p, amp, decay;

    *dx = *dy = 0;
    if (!data->shakeDur)
        return FALSE;
    age = (long) (GetTickCount() - data->shakeStart);
    if (age < 0 || age >= (long) data->shakeDur) {
        data->shakeDur = 0;
        return FALSE;
    }
    p = (double) age / data->shakeDur;
    decay = (1.0 - p) * (1.0 - p);
    amp = data->xFrontTile * (0.06 + 0.22 * data->shakeStrength / 100.0);
    /* two unrelated frequencies per axis so it rattles, not sways */
    *dx = (int) (amp * decay * (0.7 * sin(age * 0.105) + 0.3 * sin(age * 0.297)));
    *dy = (int) (amp * 0.8 * decay
                 * (0.6 * cos(age * 0.083) + 0.4 * sin(age * 0.241)));
    return TRUE;
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

/* a keypress snaps every sprite back to its square (design rule 2) */
void
mswin_map_fx_snap(void)
{
    PNHMapWindow data = (PNHMapWindow) windowdata[NHW_MAP].address;

    if (data && (data->nanims || data->shakeDur)) {
        data->nanims = 0;
        data->animSeq = 0;
        data->shakeDur = 0;
        InvalidateRect(data->hWnd, NULL, FALSE);
    }
}

/* draw animating creatures off their squares onto the composed image */
static void
fx_paint_sprites(PNHMapWindow data, HDC hdc)
{
    DWORD now = GetTickCount();
    HBITMAP saved;
    int i, ox, oy, tw, th;

    if (!data->nanims)
        return;
    ox = data->map_orig.x - data->xPos * data->xFrontTile;
    oy = data->map_orig.y - data->yPos * data->yFrontTile;
    tw = data->xFrontTile, th = data->yFrontTile;
    saved = SelectObject(data->tileDC, GetNHApp()->bmpMapTiles);
    for (i = 0; i < data->nanims; i++) {
        struct fx_anim *a = &data->anims[i];
        long age = (long) (now - a->start);
        double p, e;
        int cx, cy, offx = 0, offy = 0;
        short bk;

        if (age < 0 || age >= (long) a->dur)
            continue;
        p = (double) age / a->dur;
        if (a->kind == FXM_DEATH) {
            /* a small shake, then fade out over what's left there (its
               corpse, items or bare floor) */
            double fadefrom = 0.35;
            int alpha = (p < fadefrom) ? 255
                        : (int) (255 * (1.0 - (p - fadefrom) / (1 - fadefrom)));
            double amp = tw * 0.06 * (p < fadefrom ? 1.0 - p / fadefrom : 0.0);

            offx = (int) (amp * sin(p * 2 * 3.14159265358979 * 9.0));
            cx = ox + a->x * tw, cy = oy + a->y * th;
            fx_blend_tile(data, hdc, cx + offx, cy, tw, th, a->tile, alpha);
            continue;
        }
        if (a->kind == FXM_LUNGE) {
            /* quick push out, softer ease back */
            double reach = (a->hit ? 0.28 : 0.2) * tw;

            e = (p < 0.4) ? (p / 0.4) : (1.0 - (p - 0.4) / 0.6);
            e = e * e * (3 - 2 * e); /* smoothstep */
            offx = (int) (a->dx * reach * e);
            offy = (int) (a->dy * reach * e);
        } else { /* FXM_SHAKE: decaying side-to-side jitter */
            double amp = tw * (0.04 + 0.10 * a->strength / 100.0);

            offx = (int) (amp * sin(p * 2 * 3.14159265358979 * 3.0) * (1.0 - p));
            offy = (int) (amp * 0.3 * sin(p * 2 * 3.14159265358979 * 5.0) * (1.0 - p));
        }
        cx = ox + a->x * tw, cy = oy + a->y * th;
        /* the floor under it, so it doesn't leave a copy behind; for
           plain lit room floor the core sends no background glyph (it
           reports GLYPH_UNEXPLORED and the creature tile is drawn
           opaque), so put the room-floor tile back ourselves */
        bk = -1;
        if (data->bkmap[a->x][a->y].glyph != NO_GLYPH
            && data->bkmap[a->x][a->y].glyph != GLYPH_UNEXPLORED) {
            bk = data->bkmap[a->x][a->y].gm.tileidx;
        } else {
            glyph_info floorgi;

            map_glyphinfo(a->x, a->y, cmap_to_glyph(S_room), 0, &floorgi);
            bk = floorgi.gm.tileidx;
        }
        if (bk >= 0) {
            StretchBlt(hdc, cx, cy, tw, th, data->tileDC, TILEBMP_X(bk),
                       TILEBMP_Y(bk), GetNHApp()->mapTile_X,
                       GetNHApp()->mapTile_Y, SRCCOPY);
        } else {
            RECT r;

            SetRect(&r, cx, cy, cx + tw, cy + th);
            FillRect(hdc, &r, (HBRUSH) GetStockObject(BLACK_BRUSH));
        }
        (*GetNHApp()->lpfnTransparentBlt)(
            hdc, cx + offx, cy + offy, tw, th, data->tileDC,
            TILEBMP_X(a->tile), TILEBMP_Y(a->tile), GetNHApp()->mapTile_X,
            GetNHApp()->mapTile_Y, TILE_BK_COLOR);
    }
    SelectObject(data->tileDC, saved);
}

/* animation frame: expire old numbers, stop the timer when idle */
static void
fx_tick(PNHMapWindow data)
{
    DWORD now = GetTickCount();
    int i, j;

    for (i = j = 0; i < data->nfloaters; i++)
        if (now - data->floaters[i].start < data->floaters[i].life)
            data->floaters[j++] = data->floaters[i];
    data->nfloaters = j;
    for (i = j = 0; i < data->nanims; i++)
        if ((long) (now - data->anims[i].start) < (long) data->anims[i].dur)
            data->anims[j++] = data->anims[i];
    data->nanims = j;
    if (data->shakeDur
        && (long) (now - data->shakeStart) >= (long) data->shakeDur)
        data->shakeDur = 0;
    if (!data->nfloaters && !data->nanims && !data->shakeDur
        && data->fxTimerOn) {
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

    if (!data->nfloaters)
        return;
    fx_make_fonts(data);
    oldFont = SelectObject(hdc, data->hFxFont);
    oldBk = SetBkMode(hdc, TRANSPARENT);
    ox = data->map_orig.x - data->xPos * data->xFrontTile;
    oy = data->map_orig.y - data->yPos * data->yFrontTile;
    o = max(1, data->yFrontTile / 24); /* outline thickness */

    for (i = 0; i < data->nfloaters; i++) {
        struct fx_floater *f = &data->floaters[i];
        DWORD age = now - f->start;
        double t = (double) age / f->life; /* 0..1 */
        char buf[32];
        RECT r;
        int cx, cy, rise, fade;
        double fadefrom;
        COLORREF c = f->color;

        if (age >= f->life)
            continue;
        if (f->low) {
            /* "Killed": starts at the health bar, drifts up a little */
            rise = (int) (data->yFrontTile * 0.35 * (1.0 - (1.0 - t) * (1.0 - t)))
                   - (data->yFrontTile * 3) / 5;
            fadefrom = 0.6;
        } else if (f->life == FX_LEVEL_MS) {
            /* level messages rise in the first third, hang above the
               hero, then fade only over the last quarter */
            double tr = min(1.0, t / 0.33);

            rise = (int) (data->yFrontTile * 1.3 * (1.0 - (1.0 - tr) * (1.0 - tr)));
            fadefrom = 0.75;
        } else {
            /* rise fast, then ease out */
            rise = (int) (data->yFrontTile * 0.9 * (1.0 - (1.0 - t) * (1.0 - t)));
            fadefrom = 0.66;
        }
        cx = ox + f->x * data->xFrontTile + data->xFrontTile / 2
             + (f->xoff * data->xFrontTile) / 100;
        cy = oy + f->y * data->yFrontTile + data->yFrontTile / 3 - rise
             - f->stack * (data->yFrontTile * 9) / 20;
        /* fade by darkening near the end (the map is mostly dark, so
           this reads as fading out without alpha blending) */
        if (t > fadefrom) {
            fade = (int) (255 * (1.0 - (t - fadefrom) / (1.0 - fadefrom)));
            c = RGB(GetRValue(c) * fade / 255, GetGValue(c) * fade / 255,
                    GetBValue(c) * fade / 255);
        }
        if (f->text[0])
            Strcpy(buf, f->text);
        else
            Sprintf(buf, "%s%d", f->type == FXE_HEAL ? "+"
                                 : f->target == 0 ? "-" : "", f->amount);
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
    SetScrollInfo(hWnd, SB_VERT, &si, TRUE);
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
    SetScrollInfo(hWnd, SB_HORZ, &si, TRUE);
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
    if (c >= 0 && c < CLR_MAX)
        return GetNHApp()->regMapColors[c];
    return RGB(0x00, 0x00, 0x00);
}
