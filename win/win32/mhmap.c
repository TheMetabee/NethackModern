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
                      | FXA_INVIS)
#define FX_FRAME_MS 30      /* ~33 frames per second while numbers float */
#define FX_FLOAT_MS 900     /* lifetime of one number */
#define FX_TEXT_MS 1400     /* lifetime of a status name */
#define FX_LEVEL_MS 2600    /* lifetime of "Level N!" */
#define FX_KILL_MS 1100     /* lifetime of "Killed" */
#define FX_STACK_MS 400     /* a new number on a square this young stacks */
#define FX_MAXFLOAT 64
#define FX_MAXANIM 96
#define FXM_LUNGE 1
#define FXM_SHAKE 2
#define FXM_DEATH 3
#define FXM_SLIDE 4         /* hurtled: glide in from the previous square */
#define FXM_GHOST 5         /* speed afterimage left behind by a step */
#define FX_GHOST_MS 460
#define FX_INVIS_ALPHA 110  /* how solid an invisible-but-seen creature is */
#define FX_SLIDE_MS 70      /* one hurtled step */
#define FX_STEP_MS 110      /* an ordinary step */
#define FX_DEATH_MS 360     /* shake ~35%, then fade out */
#define FX_LUNGE_MS 150     /* out ~40%, back ~60% */
#define FX_LUNGE_GAP 110    /* successive lunges in one turn queue up */
#define FX_SHAKE_MS 220
#define FX_MAXPART 600      /* particles alive at once */

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
    /* NetHack Modern: particles for rays and explosions, in map squares
       (so scrolling carries them along) */
    struct fx_part {
        float x, y;             /* position, squares (centre = +0.5) */
        float vx, vy;           /* squares per second */
        float grav;             /* added to vy per second (< 0 rises) */
        float drag;             /* fraction of speed kept per second */
        float size;             /* radius, fraction of a tile */
        COLORREF c0, c1;        /* colour when born, when it dies */
        DWORD start, life;
    } parts[FX_MAXPART];
    int nparts;
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
static void fx_ray_particles(PNHMapWindow, int, int, int, int);
static void fx_burst_particles(PNHMapWindow, int, int, int, int);
static void fx_impact_particles(PNHMapWindow, int, int, boolean);
static void fx_wand_particles(PNHMapWindow, const struct fx_event *);
static void fx_dig_particles(PNHMapWindow, int, int);
static void fx_teleport_particles(PNHMapWindow, const struct fx_event *);
static void fx_summon_particles(PNHMapWindow, const struct fx_event *);
static void fx_quaff_particles(PNHMapWindow, const struct fx_event *);
static void fx_detect_particles(PNHMapWindow, const struct fx_event *);
static void fx_scroll_particles(PNHMapWindow, const struct fx_event *);
COLORREF nhcolor_to_RGB(int);
static void fx_paint_particles(PNHMapWindow, HDC);
static void fx_paint_messages(PNHMapWindow, HDC);
static void fx_paint_hero_frame(PNHMapWindow, HDC);
static void fx_paint_lighting(PNHMapWindow, HDC);
static int fx_paint_auras(PNHMapWindow, HDC);
static void fx_part_layer(PNHMapWindow, HDC);
static void fx_make_fonts(PNHMapWindow);
static void fx_aura_timer(PNHMapWindow, boolean);
static void fx_paint_overlay(PNHMapWindow data, HDC hdc);
static boolean fx_shake_offset(PNHMapWindow, int *, int *);
static void fx_paint_translucent(PNHMapWindow, HDC);
static short fx_floor_tile(PNHMapWindow, int, int);
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
        if (!bText && iflags.fx_theme && wnd_size.cy > 0) {
            /* NetHack Modern: zoom. Automatic shows the whole level; the
               player can zoom in (the map then follows the hero) with
               Ctrl+=/- and Ctrl+wheel, Ctrl+0 goes back to automatic */
            int z;

            /* automatic: the largest step that shows the whole level */
            data->zoomAuto = 0;
            for (z = FX_NZOOM - 1; z >= 0; z--)
                if (data->yBackTile * data->monitorScale * fx_zooms[z]
                        * ROWNO <= wnd_size.cy
                    && data->xBackTile * data->monitorScale * fx_zooms[z]
                           * COLNO <= wnd_size.cx) {
                    data->zoomAuto = z;
                    break;
                }
            z = data->zoomSet ? data->zoomLevel : data->zoomAuto;
            data->frontScale = data->monitorScale * fx_zooms[z];
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
        if (iflags.fx_auras
            && ((msg_data->glyphinfo.fxauras & FX_AURA_MASK)
                || msg_data->glyphinfo.glyph == cmap_to_glyph(S_poisoncloud)
                || msg_data->glyphinfo.glyph == cmap_to_glyph(S_cloud)))
            fx_aura_timer(data, TRUE); /* lasting effects, drifting gas */
        if (msg_data->glyphinfo.glyph == cmap_to_glyph(S_digbeam))
            fx_dig_particles(data, msg_data->x, msg_data->y);
        if (glyph_is_cmap_zap(msg_data->glyphinfo.glyph))
            fx_ray_particles(data, msg_data->x, msg_data->y,
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

    /* always compose off screen, then copy: switching between this and a
       direct stretch whenever an animation starts or stops made the whole
       map twitch by a pixel (the two stretches round differently) */
    (void) shaking;
    {
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
        fx_paint_lighting(data, hdc);
        fx_paint_hero_frame(data, hdc);
        if (!fx_paint_auras(data, hdc))
            fx_aura_timer(data, FALSE);
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
    if (ev->type == FXE_MOVE && (ev->flags & FXF_HERO_TARGET)
        && iflags.fx_theme && IsWindow(mswin_hwnd_from_winid(WIN_STATUS)))
        /* the character panel's action bar depends on where we stand */
        InvalidateRect(mswin_hwnd_from_winid(WIN_STATUS), NULL, FALSE);
    fx_shake_event(data, ev);  /* the whole view */
    if (ev->type == FXE_EXPLODE && !(ev->flags & (FXF_UNSEEN | FXF_BULK)))
        fx_burst_particles(data, ev->x, ev->y, ev->adtyp, ev->amount);
    if (ev->type == FXE_WAND && !(ev->flags & FXF_BULK))
        fx_wand_particles(data, ev);
    if (ev->type == FXE_TELEPORT && !(ev->flags & FXF_BULK))
        fx_teleport_particles(data, ev);
    if (ev->type == FXE_SUMMON)
        fx_summon_particles(data, ev);
    if (ev->type == FXE_QUAFF && !(ev->flags & FXF_UNSEEN))
        fx_quaff_particles(data, ev);
    if (ev->type == FXE_READ && ev->adtyp == 1 && !(ev->flags & FXF_BULK))
        fx_scroll_particles(data, ev);
    if ((ev->type == FXE_DETECT || ev->type == FXE_UNIHORN)
        && !(ev->flags & FXF_UNSEEN))
        fx_detect_particles(data, ev);
    if (ev->type == FXE_IMPACT && !(ev->flags & FXF_BULK))
        fx_impact_particles(data, ev->x, ev->y,
                            (ev->flags & FXF_RESISTED) != 0);
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
        /* recoil away from the attacker that lunged in */
        if (i >= 0)
            a->dx = data->anims[i].dx, a->dy = data->anims[i].dy;
        else
            a->dx = a->dy = 0;
        a->tile = data->map[ev->x][ev->y].gm.tileidx;
        a->hit = TRUE;
        a->strength = max(15, min(100, pct * 3));
        a->start = start;
        a->dur = FX_SHAKE_MS;
        fx_start_timer(data);
    } else if (ev->type == FXE_KNOCKBACK || ev->type == FXE_MOVE) {
        boolean step = (ev->type == FXE_MOVE);
        int lim = step ? 2 : 1;

        dx = ev->x - ev->sx, dy = ev->y - ev->sy;
        if (!isok(ev->x, ev->y) || dx < -lim || dx > lim || dy < -lim
            || dy > lim)
            return;
        /* (a step is reported before the map is redrawn, so whether
           something is drawn there is checked when painting) */
        if (!step && !glyph_is_monster(data->map[ev->x][ev->y].glyph))
            return;
        /* the previous step's slide is over: this one takes over */
        for (i = 0; i < data->nanims; i++)
            if (data->anims[i].kind == FXM_SLIDE
                && ((data->anims[i].x == ev->sx && data->anims[i].y == ev->sy)
                    || (data->anims[i].x == ev->x
                        && data->anims[i].y == ev->y)))
                data->anims[i].dur = 0;
        /* shakes and recoils of the knocked creature are left behind */
        for (i = 0; i < data->nanims; i++)
            if (data->anims[i].kind == FXM_SHAKE
                && data->anims[i].x == ev->sx && data->anims[i].y == ev->sy)
                data->anims[i].dur = 0;
        a = fx_new_anim(data);
        a->kind = FXM_SLIDE;
        a->x = ev->x, a->y = ev->y;
        a->dx = dx, a->dy = dy;
        a->tile = data->map[ev->x][ev->y].gm.tileidx; /* refreshed in paint */
        a->hit = !step;
        a->strength = 0;
        a->start = now;
        a->dur = step ? (max(abs(dx), abs(dy)) > 1 ? FX_STEP_MS + 40
                                                   : FX_STEP_MS)
                      : FX_SLIDE_MS;
        if (step && iflags.fx_afterimage) {
            int k;
            short tile = a->tile;

            for (k = 1; k <= 1; k++) { /* one afterimage */
                a = fx_new_anim(data);
                a->kind = FXM_GHOST;
                a->x = ev->x, a->y = ev->y;
                a->dx = dx, a->dy = dy;
                a->tile = tile;
                a->hit = FALSE;
                a->strength = k; /* 1 = nearest the creature */
                a->start = now;
                a->dur = FX_GHOST_MS;
            }
        }
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

/* the core is pausing (nh_delay_output, explosion and missile frames):
   keep animations moving for 'ms' instead of freezing the window */
void
mswin_map_fx_pump(int ms)
{
    PNHMapWindow data = (PNHMapWindow) windowdata[NHW_MAP].address;
    DWORD start = GetTickCount();

    if (!data || (!data->nanims && !data->nfloaters && !data->shakeDur
                  && !data->nparts)) {
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

/* private generator: the game's RNG is never touched */
static float
fx_prand(void)
{
    static unsigned long seed = 0;

    if (!seed)
        seed = (unsigned long) GetTickCount() | 1UL;
    seed = seed * 1103515245UL + 12345UL;
    return (float) ((seed >> 8) & 0xFFFF) / 65535.0f;
}

static float
fx_prange(float lo, float hi)
{
    return lo + (hi - lo) * fx_prand();
}

/* the look of one element's particles */
struct fx_pstyle {
    COLORREF c0, c1, c2;   /* two birth colours to pick between, death */
    float speed;           /* squares/second */
    float grav;            /* < 0 rises (embers), > 0 falls (frost, acid) */
    float drag;
    float size;
    int life_lo, life_hi;  /* ms */
};

static const struct fx_pstyle *
fx_part_style(int kind)
{
    static const struct fx_pstyle
        fire = { RGB(255, 200, 70), RGB(255, 120, 20), RGB(90, 10, 0),
                 1.3f, -1.6f, 0.25f, 0.075f, 380, 800 },
        cold = { RGB(235, 250, 255), RGB(130, 200, 255), RGB(0, 30, 80),
                 1.0f, 0.7f, 0.3f, 0.06f, 420, 850 },
        elec = { RGB(255, 255, 210), RGB(255, 235, 60), RGB(60, 50, 0),
                 3.2f, 0.0f, 0.05f, 0.045f, 160, 340 },
        sleep = { RGB(210, 190, 255), RGB(150, 120, 255), RGB(30, 10, 70),
                  0.6f, -0.3f, 0.4f, 0.065f, 500, 900 },
        death = { RGB(150, 60, 170), RGB(90, 90, 90), RGB(10, 0, 15),
                  0.9f, 0.0f, 0.3f, 0.07f, 400, 800 },
        poison = { RGB(160, 230, 70), RGB(90, 170, 40), RGB(10, 35, 0),
                   0.5f, -0.25f, 0.45f, 0.11f, 600, 1100 },
        acid = { RGB(220, 255, 70), RGB(170, 230, 30), RGB(30, 45, 0),
                 0.9f, 1.8f, 0.4f, 0.06f, 400, 800 },
        magic = { RGB(255, 170, 255), RGB(190, 110, 255), RGB(40, 0, 60),
                  1.4f, 0.0f, 0.2f, 0.055f, 300, 650 };

    switch (kind) {
    case AD_FIRE:
        return &fire;
    case AD_COLD:
        return &cold;
    case AD_ELEC:
        return &elec;
    case AD_SLEE:
        return &sleep;
    case AD_DISN:
    case AD_DRLI:
        return &death;
    case AD_DRST:
        return &poison;
    case AD_ACID:
        return &acid;
    default:
        return &magic;
    }
}

static struct fx_part *
fx_new_part(PNHMapWindow data)
{
    if (data->nparts >= FX_MAXPART) {
        /* full: recycle the oldest */
        memmove(&data->parts[0], &data->parts[1],
                (FX_MAXPART - 1) * sizeof data->parts[0]);
        data->nparts--;
    }
    return &data->parts[data->nparts++];
}

static void
fx_spawn(PNHMapWindow data, const struct fx_pstyle *st, float x, float y,
         float speed, float sizemul, float lifemul)
{
    struct fx_part *p = fx_new_part(data);
    float ang = fx_prange(0.0f, 6.2831853f), v = speed * fx_prange(0.25f, 1.0f);

    p->x = x, p->y = y;
    p->vx = v * (float) cos(ang);
    p->vy = v * (float) sin(ang);
    p->grav = st->grav;
    p->drag = st->drag;
    p->size = st->size * sizemul * fx_prange(0.7f, 1.3f);
    p->c0 = (fx_prand() < 0.5f) ? st->c0 : st->c1;
    p->c1 = st->c2;
    p->start = GetTickCount();
    p->life = (DWORD) (lifemul * fx_prange((float) st->life_lo,
                                           (float) st->life_hi));
}

/* zap glyph type -> damage kind (ZT_* order in zap.c) */
static int
fx_zap_kind(int zt)
{
    static const int kinds[NUM_ZAP] = { AD_MAGM, AD_FIRE, AD_COLD, AD_SLEE,
                                        AD_DISN, AD_ELEC, AD_DRST, AD_ACID };

    return (zt >= 0 && zt < NUM_ZAP) ? kinds[zt] : AD_MAGM;
}

/* a ray (wand, spell or breath) is being drawn through <x,y>; 'dir' is
   the beam's shape: 0 |, 1 -, 2 \, 3 / */
static void
fx_ray_particles(PNHMapWindow data, int x, int y, int zt, int dir)
{
    /* unit vector across the beam, for each shape */
    static const float across[4][2] = { { 1.0f, 0.0f }, { 0.0f, 1.0f },
                                        { 0.7071f, -0.7071f },
                                        { 0.7071f, 0.7071f } };
    const struct fx_pstyle *st;
    int i, n;

    if (!iflags.fx_particles || !isok(x, y))
        return;
    st = fx_part_style(fx_zap_kind(zt));
    n = (st == fx_part_style(AD_ELEC)) ? 8 : 6;
    for (i = 0; i < n; i++) {
        float along = fx_prange(-0.5f, 0.5f);
        float side = (fx_prand() < 0.5f) ? -1.0f : 1.0f;
        float sp = fx_prange(1.1f, 2.4f) * (0.6f + 0.4f * st->speed);
        struct fx_part *p;

        /* born on the beam's line, anywhere along this square */
        fx_spawn(data, st,
                 x + 0.5f + along * across[dir & 3][1],
                 y + 0.5f - along * across[dir & 3][0], 0.0f, 1.0f, 1.2f);
        p = &data->parts[data->nparts - 1];
        /* thrown out to one side, with a little scatter */
        p->vx = side * across[dir & 3][0] * sp + fx_prange(-0.3f, 0.3f);
        p->vy = side * across[dir & 3][1] * sp + fx_prange(-0.3f, 0.3f);
    }
    fx_start_timer(data);
}

/* an explosion centred on <x,y>: a burst over its 3x3 area, more for
   bigger blasts */
static void
fx_burst_particles(PNHMapWindow data, int x, int y, int kind, int dam)
{
    const struct fx_pstyle *st;
    int i, n;

    if (!iflags.fx_particles || !isok(x, y))
        return;
    st = fx_part_style(kind);
    n = 70 + min(dam, 60) * 2;
    for (i = 0; i < n; i++) {
        /* most from the middle flying outward, some from the edges */
        float r = fx_prange(0.0f, 0.6f), a = fx_prange(0.0f, 6.2831853f);

        fx_spawn(data, st, x + 0.5f + r * (float) cos(a),
                 y + 0.5f + r * (float) sin(a), st->speed * 1.9f, 1.15f,
                 1.1f);
    }
    fx_start_timer(data);
}

/* a force strike (wand of striking, force bolt): a quick puff of pale
   specks and a ring snapping outward; smaller and greyer when it bounced
   off ("Boing!") */
static void
fx_impact_particles(PNHMapWindow data, int x, int y, boolean resisted)
{
    static const struct fx_pstyle
        force = { RGB(245, 250, 255), RGB(170, 210, 255), RGB(10, 25, 60),
                  5.5f, 0.0f, 0.02f, 0.075f, 260, 480 },
        bounce = { RGB(210, 215, 225), RGB(150, 165, 190), RGB(15, 18, 25),
                   3.2f, 0.0f, 0.02f, 0.06f, 200, 340 };
    const struct fx_pstyle *st = resisted ? &bounce : &force;
    int i, nspeck = resisted ? 18 : 48, nring = resisted ? 24 : 40;
    float ringv = resisted ? 4.0f : 7.0f; /* reaches ~0.7 / ~1.3 squares */

    if (!iflags.fx_particles || !isok(x, y))
        return;
    /* a bright flash at the point of impact */
    for (i = 0; i < (resisted ? 2 : 4); i++) {
        struct fx_part *p;

        fx_spawn(data, st, x + 0.5f, y + 0.5f, 0.0f, 1.0f, 1.0f);
        p = &data->parts[data->nparts - 1];
        p->vx = p->vy = 0.0f;
        p->size = resisted ? 0.2f : 0.32f;
        p->c0 = RGB(255, 255, 255);
        p->life = resisted ? 110 : 150;
    }
    /* scattered specks */
    for (i = 0; i < nspeck; i++)
        fx_spawn(data, st, x + 0.5f + fx_prange(-0.15f, 0.15f),
                 y + 0.5f + fx_prange(-0.15f, 0.15f), st->speed, 1.0f, 1.0f);
    /* the ring: evenly spaced, all at the same speed, stopping short */
    for (i = 0; i < nring; i++) {
        struct fx_part *p;
        float a = (6.2831853f * i) / nring + fx_prange(-0.05f, 0.05f);

        fx_spawn(data, st, x + 0.5f, y + 0.5f, 0.0f, 0.8f, 1.0f);
        p = &data->parts[data->nparts - 1];
        p->vx = ringv * (float) cos(a);
        p->vy = ringv * (float) sin(a);
        p->drag = 0.004f; /* snaps out, then hangs */
        p->c0 = st->c0;
        p->size = st->size * 1.1f;
        p->life = resisted ? 300 : 420;
    }
    fx_start_timer(data);
}

/* ---- wand effects ----------------------------------------------- */

/* a bright colour of random hue (polymorph, wishing) */
static COLORREF
fx_rainbow(void)
{
    float h = fx_prange(0.0f, 6.0f), f = h - (float) (int) h;
    int q = (int) (255 * (1.0f - f)), t = (int) (255 * f);

    switch ((int) h) {
    case 0:
        return RGB(255, t, 60);
    case 1:
        return RGB(q, 255, 60);
    case 2:
        return RGB(60, 255, t);
    case 3:
        return RGB(60, q, 255);
    case 4:
        return RGB(t, 60, 255);
    default:
        return RGB(255, 60, q);
    }
}

/* the look of each wand; 0 is an unidentified wand (plain) */
static const struct fx_pstyle *
fx_wand_style(int look, boolean *rainbow)
{
    static const struct fx_pstyle
        plain = { RGB(225, 228, 238), RGB(175, 185, 205), RGB(20, 22, 30),
                  0.8f, 0.0f, 0.2f, 0.05f, 250, 450 },
        striking = { RGB(245, 250, 255), RGB(170, 210, 255), RGB(10, 25, 60),
                     1.0f, 0.0f, 0.2f, 0.05f, 250, 450 },
        locking = { RGB(215, 185, 120), RGB(160, 160, 178), RGB(30, 25, 15),
                    1.0f, 0.8f, 0.25f, 0.055f, 300, 550 },
        opening = { RGB(255, 225, 130), RGB(255, 190, 70), RGB(60, 35, 0),
                    1.0f, -0.8f, 0.25f, 0.055f, 300, 600 },
        invis = { RGB(240, 248, 255), RGB(190, 212, 238), RGB(0, 0, 0),
                  0.7f, 0.0f, 0.1f, 0.045f, 200, 400 },
        probing = { RGB(140, 255, 240), RGB(70, 200, 255), RGB(0, 30, 40),
                    1.6f, 0.0f, 0.15f, 0.045f, 220, 420 },
        slow = { RGB(110, 140, 255), RGB(70, 90, 210), RGB(5, 10, 40),
                 0.5f, 0.4f, 0.4f, 0.06f, 600, 1000 },
        speed = { RGB(230, 255, 130), RGB(255, 255, 185), RGB(40, 50, 0),
                  2.8f, 0.0f, 0.05f, 0.045f, 180, 340 },
        undead = { RGB(255, 250, 225), RGB(255, 230, 160), RGB(60, 50, 20),
                   0.9f, -1.2f, 0.3f, 0.06f, 400, 750 },
        cancel = { RGB(185, 105, 235), RGB(115, 75, 155), RGB(0, 0, 0),
                   0.9f, 0.0f, 0.25f, 0.06f, 350, 650 },
        poly = { RGB(255, 120, 120), RGB(120, 255, 120), RGB(20, 20, 20),
                 1.2f, 0.0f, 0.2f, 0.055f, 350, 650 },
        tele = { RGB(255, 120, 235), RGB(180, 100, 255), RGB(30, 0, 50),
                 1.4f, 0.0f, 0.2f, 0.055f, 300, 600 },
        dig = { RGB(195, 165, 125), RGB(145, 125, 100), RGB(30, 25, 20),
                1.0f, 1.2f, 0.3f, 0.06f, 350, 700 },
        light = { RGB(255, 248, 210), RGB(255, 222, 145), RGB(70, 50, 10),
                  0.4f, -0.4f, 0.3f, 0.06f, 600, 1100 },
        secret = { RGB(160, 225, 255), RGB(95, 175, 255), RGB(0, 20, 50),
                   0.5f, 0.0f, 0.3f, 0.055f, 500, 900 },
        create = { RGB(175, 145, 195), RGB(115, 105, 135), RGB(15, 10, 20),
                   1.2f, -0.2f, 0.3f, 0.12f, 600, 1000 },
        enlight = { RGB(255, 235, 150), RGB(255, 255, 215), RGB(60, 40, 0),
                    0.4f, -1.6f, 0.5f, 0.055f, 700, 1300 },
        wish = { RGB(255, 225, 120), RGB(255, 255, 255), RGB(40, 30, 0),
                 1.0f, 3.0f, 0.5f, 0.06f, 700, 1300 },
        stasis = { RGB(205, 238, 255), RGB(155, 195, 225), RGB(20, 30, 40),
                   0.15f, 0.0f, 0.5f, 0.055f, 900, 1500 };

    *rainbow = FALSE;
    switch (look) {
    case WAN_STRIKING:
        return &striking;
    case WAN_LOCKING:
        return &locking;
    case WAN_OPENING:
        return &opening;
    case WAN_MAKE_INVISIBLE:
        return &invis;
    case WAN_PROBING:
        return &probing;
    case WAN_SLOW_MONSTER:
        return &slow;
    case WAN_SPEED_MONSTER:
        return &speed;
    case WAN_UNDEAD_TURNING:
        return &undead;
    case WAN_CANCELLATION:
        return &cancel;
    case WAN_POLYMORPH:
        *rainbow = TRUE;
        return &poly;
    case WAN_TELEPORTATION:
        return &tele;
    case WAN_DIGGING:
        return &dig;
    case WAN_LIGHT:
        return &light;
    case WAN_SECRET_DOOR_DETECTION:
        return &secret;
    case WAN_CREATE_MONSTER:
        return &create;
    case WAN_ENLIGHTENMENT:
        return &enlight;
    case WAN_WISHING:
        *rainbow = TRUE;
        return &wish;
    case WAN_STASIS:
        return &stasis;
    default:
        return &plain;
    }
}

/* spawn one particle of style 'st', 'delay' ms from now */
static struct fx_part *
fx_spawn_at(PNHMapWindow data, const struct fx_pstyle *st, boolean rainbow,
            float x, float y, float speed, float sizemul, DWORD delay)
{
    struct fx_part *p;

    fx_spawn(data, st, x, y, speed, sizemul, 1.0f);
    p = &data->parts[data->nparts - 1];
    if (rainbow)
        p->c0 = fx_rainbow();
    p->start += delay;
    return p;
}

/* a ring of 'n' particles centred on square <x,y>, reaching 'radius'
   squares; 'drag' sets how sharply it stops (small = snaps out) */
static void
fx_ring(PNHMapWindow data, const struct fx_pstyle *st, boolean rainbow,
        int x, int y, int n, float radius, float drag, DWORD life,
        DWORD delay)
{
    int i;
    float v = radius * (float) -log(drag);

    for (i = 0; i < n; i++) {
        float a = (6.2831853f * i) / n + fx_prange(-0.04f, 0.04f);
        struct fx_part *p = fx_spawn_at(data, st, rainbow, x + 0.5f,
                                        y + 0.5f, 0.0f, 1.0f, delay);

        p->vx = v * (float) cos(a);
        p->vy = v * (float) sin(a);
        p->drag = drag;
        p->grav = 0.0f;
        p->life = life;
    }
}

/* the digging beam: rock dust kicked up along its path */
static void
fx_dig_particles(PNHMapWindow data, int x, int y)
{
    boolean rb;
    const struct fx_pstyle *st = fx_wand_style(WAN_DIGGING, &rb);
    int i;

    if (!iflags.fx_particles || !isok(x, y))
        return;
    for (i = 0; i < 7; i++)
        (void) fx_spawn_at(data, st, FALSE, x + fx_prange(0.2f, 0.8f),
                           y + fx_prange(0.2f, 0.8f), st->speed, 1.0f, 0);
    fx_start_timer(data);
}

static void
fx_wand_particles(PNHMapWindow data, const struct fx_event *ev)
{
    boolean rb;
    const struct fx_pstyle *st;
    int i, x = ev->x, y = ev->y, look = ev->otyp;

    if (!iflags.fx_particles || !isok(x, y))
        return;
    if (Hallucination && look)
        look = WAN_POLYMORPH; /* everything's a rainbow */
    st = fx_wand_style(look, &rb);

    switch (ev->adtyp) {
    case FXW_BEAM: {
        /* a short streak on each square, timed so the beam travels */
        DWORD delay = (DWORD) (ev->amount * 22);

        for (i = 0; i < (look ? 5 : 3); i++)
            (void) fx_spawn_at(data, st, rb, x + fx_prange(0.25f, 0.75f),
                               y + fx_prange(0.25f, 0.75f),
                               st->speed * 0.35f, 0.9f, delay);
        break;
    }
    case FXW_HIT:
        /* a burst where it strikes, and a small ring once identified */
        for (i = 0; i < (look ? 28 : 14); i++)
            (void) fx_spawn_at(data, st, rb, x + 0.5f, y + 0.5f,
                               st->speed * 1.4f + 0.6f, 1.0f, 0);
        if (look)
            fx_ring(data, st, rb, x, y, 18, 0.8f, 0.01f, 380, 0);
        break;
    case FXW_SELF:
        switch (look) {
        case WAN_LIGHT:
            /* a wave of light rolling out, then warm motes hanging */
            fx_ring(data, st, FALSE, x, y, 90, 5.0f, 0.02f, 750, 0);
            for (i = 0; i < 70; i++) {
                float r = fx_prange(0.3f, 4.5f), a = fx_prange(0, 6.283f);

                (void) fx_spawn_at(data, st, FALSE,
                                   x + 0.5f + r * (float) cos(a),
                                   y + 0.5f + r * (float) sin(a) * 0.8f,
                                   st->speed, 1.0f, (DWORD) (r * 70));
            }
            break;
        case WAN_SECRET_DOOR_DETECTION:
            /* two searching pulses */
            fx_ring(data, st, FALSE, x, y, 90, 7.0f, 0.03f, 850, 0);
            fx_ring(data, st, FALSE, x, y, 70, 5.0f, 0.03f, 750, 180);
            break;
        case WAN_CREATE_MONSTER:
            /* dark smoke swirling out from you */
            for (i = 0; i < 55; i++)
                (void) fx_spawn_at(data, st, FALSE,
                                   x + 0.5f + fx_prange(-0.3f, 0.3f),
                                   y + 0.5f + fx_prange(-0.3f, 0.3f),
                                   st->speed * 1.6f, 1.0f,
                                   (DWORD) fx_prange(0, 160));
            break;
        case WAN_ENLIGHTENMENT:
            /* golden motes rising all around you */
            for (i = 0; i < 70; i++)
                (void) fx_spawn_at(data, st, FALSE,
                                   x + 0.5f + fx_prange(-1.2f, 1.2f),
                                   y + 0.6f + fx_prange(-0.4f, 0.8f),
                                   st->speed, 1.0f,
                                   (DWORD) fx_prange(0, 350));
            fx_ring(data, st, FALSE, x, y, 40, 1.6f, 0.02f, 600, 0);
            break;
        case WAN_WISHING: {
            /* a fountain of coloured sparks and a golden ring */
            for (i = 0; i < 110; i++) {
                struct fx_part *p = fx_spawn_at(
                    data, st, TRUE, x + 0.5f + fx_prange(-0.15f, 0.15f),
                    y + 0.5f, 0.0f, 1.0f, (DWORD) fx_prange(0, 300));

                p->vx = fx_prange(-1.4f, 1.4f);
                p->vy = fx_prange(-5.5f, -3.0f);
            }
            fx_ring(data, st, FALSE, x, y, 50, 2.0f, 0.02f, 700, 0);
            break;
        }
        case WAN_DIGGING:
            break; /* its dust is drawn along the beam */
        case WAN_STASIS:
            /* time stills: a slow ring, and motes that barely move */
            fx_ring(data, st, FALSE, x, y, 60, 3.0f, 0.15f, 1300, 0);
            for (i = 0; i < 50; i++) {
                float r = fx_prange(0.2f, 2.6f), a = fx_prange(0, 6.283f);

                (void) fx_spawn_at(data, st, FALSE,
                                   x + 0.5f + r * (float) cos(a),
                                   y + 0.5f + r * (float) sin(a),
                                   st->speed, 1.0f, 0);
            }
            break;
        default:
            /* unidentified: a plain shimmer */
            fx_ring(data, st, FALSE, x, y, 36, 1.5f, 0.02f, 500, 0);
            break;
        }
        break;
    }
    fx_start_timer(data);
}

/* teleporting: sparks rush in and the creature is gone; at the other
   end a flash and a burst as it appears */
static void
fx_teleport_particles(PNHMapWindow data, const struct fx_event *ev)
{
    static const struct fx_pstyle
        tele = { RGB(255, 140, 240), RGB(190, 120, 255), RGB(40, 0, 60),
                 1.6f, 0.0f, 0.2f, 0.06f, 300, 500 };
    int i;

    if (!iflags.fx_particles)
        return;
    if ((ev->amount & 1) && isok(ev->sx, ev->sy)) {
        /* departure: a ring collapsing inward, and a column rising */
        for (i = 0; i < 36; i++) {
            float a = (6.2831853f * i) / 36, r = fx_prange(0.75f, 1.0f);
            struct fx_part *p = fx_spawn_at(
                data, &tele, FALSE, ev->sx + 0.5f + r * (float) cos(a),
                ev->sy + 0.5f + r * (float) sin(a), 0.0f, 1.0f, 0);

            p->drag = 0.01f; /* ends just at the middle */
            p->vx = -r * 4.6f * (float) cos(a);
            p->vy = -r * 4.6f * (float) sin(a);
            p->life = 380;
        }
        for (i = 0; i < 18; i++) {
            struct fx_part *p = fx_spawn_at(
                data, &tele, FALSE, ev->sx + 0.5f + fx_prange(-0.2f, 0.2f),
                ev->sy + fx_prange(0.4f, 0.9f), 0.0f, 0.8f,
                (DWORD) fx_prange(80, 260));

            p->vx = 0.0f;
            p->vy = fx_prange(-2.6f, -1.4f);
            p->life = 420;
        }
    }
    if ((ev->amount & 2) && isok(ev->x, ev->y)) {
        /* arrival, a moment later: a flash, a burst and a ring */
        DWORD at = (ev->amount & 1) ? 260 : 0;

        for (i = 0; i < 3; i++) {
            struct fx_part *p = fx_spawn_at(data, &tele, FALSE,
                                            ev->x + 0.5f, ev->y + 0.5f, 0.0f,
                                            1.0f, at);

            p->vx = p->vy = 0.0f;
            p->size = 0.3f;
            p->c0 = RGB(255, 235, 255);
            p->life = 160;
        }
        for (i = 0; i < 30; i++)
            (void) fx_spawn_at(data, &tele, FALSE, ev->x + 0.5f, ev->y + 0.5f,
                               tele.speed * 1.6f, 1.0f, at);
        fx_ring(data, &tele, FALSE, ev->x, ev->y, 26, 0.9f, 0.01f, 400, at);
    }
    fx_start_timer(data);
}

/* a summoned monster appearing: a spiral drawing in to its square, a
   flash and a ring; each of a group a little after the one before */
static void
fx_summon_particles(PNHMapWindow data, const struct fx_event *ev)
{
    static const struct fx_pstyle styles[4] = {
        /* arcane: violet */
        { RGB(235, 170, 255), RGB(160, 110, 255), RGB(30, 0, 60),
          1.4f, 0.0f, 0.22f, 0.06f, 350, 600 },
        /* demonic: fire and embers */
        { RGB(255, 190, 80), RGB(240, 70, 30), RGB(50, 0, 0),
          1.4f, -0.5f, 0.22f, 0.07f, 400, 700 },
        /* holy: gold-white */
        { RGB(255, 250, 215), RGB(255, 215, 110), RGB(60, 40, 0),
          1.3f, -0.3f, 0.2f, 0.06f, 400, 700 },
        /* swarm: dusty green-brown */
        { RGB(200, 220, 120), RGB(150, 130, 70), RGB(20, 25, 0),
          1.8f, 0.4f, 0.25f, 0.05f, 250, 450 },
    };
    const struct fx_pstyle *st;
    DWORD at;
    float cx, cy;
    int i, k = ev->adtyp;

    if (!iflags.fx_particles || !isok(ev->x, ev->y))
        return;
    k = (k < 0 || k > 3) ? 0 : k;
    st = &styles[k];
    at = (DWORD) min(ev->amount, 12) * 120; /* one after another */
    cx = ev->x + 0.5f, cy = ev->y + 0.5f;
    if (k != FXS_SWARM) {
        /* a spiral of motes drawing in to the middle */
        for (i = 0; i < 28; i++) {
            float a = (6.2831853f * i) / 14, r = 0.55f + 0.5f * (i / 28.0f);
            struct fx_part *p = fx_spawn_at(data, st, FALSE,
                                            cx + r * (float) cos(a),
                                            cy + r * (float) sin(a), 0.0f,
                                            0.9f, at + (DWORD) (i * 9));

            /* inward, and around */
            p->vx = (float) (-cos(a) * 2.2f - sin(a) * 2.6f) * r;
            p->vy = (float) (-sin(a) * 2.2f + cos(a) * 2.6f) * r;
            p->drag = 0.04f;
            p->life = 330;
        }
    }
    /* the moment it's there: a flash, a burst and a ring */
    at += (k == FXS_SWARM) ? 0 : 280;
    for (i = 0; i < 3; i++) {
        struct fx_part *p = fx_spawn_at(data, st, FALSE, cx, cy, 0.0f, 1.0f,
                                        at);

        p->vx = p->vy = 0.0f;
        p->size = 0.32f;
        p->c0 = (k == FXS_DEMONIC) ? RGB(255, 230, 160) : RGB(255, 250, 255);
        p->life = 170;
    }
    for (i = 0; i < 24; i++)
        (void) fx_spawn_at(data, st, FALSE, cx, cy, st->speed * 1.5f, 1.0f,
                           at);
    fx_ring(data, st, FALSE, ev->x, ev->y, 22, 0.8f, 0.01f, 380, at);
    if (k == FXS_DEMONIC) {
        /* a curl of smoke rising after */
        static const struct fx_pstyle smoke = {
            RGB(90, 80, 80), RGB(60, 55, 55), RGB(10, 10, 10),
            0.3f, -1.2f, 0.35f, 0.03f, 700, 1100 };

        for (i = 0; i < 10; i++)
            (void) fx_spawn_at(data, &smoke, FALSE,
                               cx + fx_prange(-0.3f, 0.3f),
                               cy + fx_prange(-0.1f, 0.3f), 0.3f, 1.0f,
                               at + 60);
    }
    fx_start_timer(data);
}

/* drinking a potion: first a swirl of its own colour rising around the
   hero (what anyone could see); then, if its type is known, its effect */
static void
fx_quaff_particles(PNHMapWindow data, const struct fx_event *ev)
{
    static const struct fx_pstyle
        swirl = { RGB(255, 255, 255), RGB(255, 255, 255), RGB(10, 10, 10),
                  0.0f, -0.6f, 0.4f, 0.06f, 500, 850 },
        heal = { RGB(200, 255, 200), RGB(110, 240, 120), RGB(0, 40, 0),
                 0.5f, -1.3f, 0.45f, 0.06f, 600, 1100 },
        energy = { RGB(190, 215, 255), RGB(110, 150, 255), RGB(0, 10, 50),
                   0.5f, -1.3f, 0.45f, 0.06f, 600, 1100 },
        gold = { RGB(255, 235, 150), RGB(255, 200, 80), RGB(50, 30, 0),
                 0.4f, -1.8f, 0.45f, 0.06f, 700, 1300 },
        acid = { RGB(220, 255, 70), RGB(170, 230, 30), RGB(30, 45, 0),
                 1.4f, -0.4f, 0.3f, 0.05f, 300, 600 },
        sick = { RGB(170, 210, 60), RGB(120, 160, 40), RGB(15, 25, 0),
                 0.5f, -0.6f, 0.4f, 0.08f, 600, 1000 },
        booze = { RGB(255, 200, 110), RGB(230, 160, 60), RGB(40, 20, 0),
                  0.4f, -0.9f, 0.4f, 0.06f, 600, 1000 },
        holy = { RGB(255, 252, 230), RGB(255, 235, 170), RGB(60, 50, 20),
                 0.5f, -1.4f, 0.4f, 0.06f, 600, 1100 },
        unholy = { RGB(200, 40, 40), RGB(120, 20, 40), RGB(20, 0, 5),
                   0.5f, -0.9f, 0.4f, 0.06f, 600, 1000 },
        detect = { RGB(220, 170, 255), RGB(160, 110, 255), RGB(25, 0, 50),
                   0.5f, 0.0f, 0.3f, 0.055f, 500, 900 };
    const struct fx_pstyle *st = 0;
    boolean rb = FALSE;
    int i, x = ev->x, y = ev->y, n = 50;

    if (!iflags.fx_particles || !isok(x, y))
        return;

    if (ev->adtyp == 0) {
        /* a spiral of the potion's colour rising round the hero */
        COLORREF c = nhcolor_to_RGB(ev->amount);

        if (GetRValue(c) + GetGValue(c) + GetBValue(c) < 200)
            c = RGB(110, 110, 135); /* inky or black: still visible */
        for (i = 0; i < 26; i++) {
            float a = (6.2831853f * i) / 13.0f;
            struct fx_part *p = fx_spawn_at(
                data, &swirl, Hallucination, x + 0.5f + 0.42f * (float) cos(a),
                y + 0.85f, 0.0f, 1.0f, (DWORD) (i * 22));

            if (!Hallucination)
                p->c0 = c;
            p->vx = -0.9f * (float) sin(a);
            p->vy = -1.1f;
        }
        fx_start_timer(data);
        return;
    }

    switch (ev->otyp) {
    case POT_HEALING:
        st = &heal, n = 35;
        break;
    case POT_EXTRA_HEALING:
        st = &heal, n = 55;
        break;
    case POT_FULL_HEALING:
        st = &heal, n = 85;
        break;
    case POT_GAIN_ENERGY:
        st = &energy;
        break;
    case POT_GAIN_LEVEL:
    case POT_GAIN_ABILITY:
    case POT_RESTORE_ABILITY:
        st = &gold, n = (ev->otyp == POT_GAIN_LEVEL) ? 80 : 50;
        break;
    case POT_ACID:
        st = &acid, n = 40;
        break;
    case POT_SICKNESS:
        st = &sick, n = 30;
        break;
    case POT_BOOZE:
        st = &booze, n = 30;
        break;
    case POT_POLYMORPH:
        st = &booze, rb = TRUE, n = 60;
        break;
    case POT_MONSTER_DETECTION:
    case POT_OBJECT_DETECTION:
        fx_ring(data, &detect, FALSE, x, y, 80, 6.0f, 0.03f, 800, 0);
        fx_start_timer(data);
        return;
    case POT_ENLIGHTENMENT:
        st = &gold, n = 60;
        break;
    case POT_WATER:
        if (ev->mndx > 0)
            st = &holy;
        else if (ev->mndx < 0)
            st = &unholy;
        break;
    default:
        break;
    }
    if (!st)
        return;
    /* motes rising from all around the hero, and a soft ring */
    for (i = 0; i < n; i++)
        (void) fx_spawn_at(data, st, rb, x + 0.5f + fx_prange(-0.9f, 0.9f),
                           y + 0.7f + fx_prange(-0.3f, 0.5f), st->speed,
                           1.0f, (DWORD) fx_prange(0, 300));
    fx_ring(data, st, rb, x, y, 32, 1.3f, 0.02f, 550, 0);
    fx_start_timer(data);
}

/* detection: each thing found flashes briefly where it appears, in a
   ripple spreading out from the hero; a unicorn horn that cured
   something sparkles clean around the hero */
static void
fx_detect_particles(PNHMapWindow data, const struct fx_event *ev)
{
    static const struct fx_pstyle
        mon = { RGB(255, 140, 170), RGB(220, 90, 255), RGB(40, 0, 30),
                1.4f, -0.4f, 0.2f, 0.055f, 350, 650 },
        obj = { RGB(255, 235, 140), RGB(140, 230, 255), RGB(30, 30, 10),
                1.4f, -0.4f, 0.2f, 0.055f, 350, 650 },
        cure = { RGB(235, 250, 255), RGB(150, 230, 255), RGB(0, 30, 40),
                 0.6f, -1.6f, 0.4f, 0.06f, 500, 950 };
    int i, x = ev->x, y = ev->y;

    if (!iflags.fx_particles || !isok(x, y))
        return;
    if (ev->type == FXE_UNIHORN) {
        if (ev->amount <= 0)
            return;
        for (i = 0; i < 45; i++)
            (void) fx_spawn_at(data, &cure, FALSE,
                               x + 0.5f + fx_prange(-0.7f, 0.7f),
                               y + 0.6f + fx_prange(-0.3f, 0.5f), cure.speed,
                               1.0f, (DWORD) fx_prange(0, 250));
        fx_ring(data, &cure, FALSE, x, y, 30, 1.2f, 0.02f, 500, 0);
        fx_start_timer(data);
        return;
    }
    if (ev->flags & FXF_HERO_TARGET)
        return; /* the summary is for the sound */
    {
        const struct fx_pstyle *st = (ev->adtyp == FXD_MONSTER) ? &mon : &obj;
        DWORD at = (DWORD) (ev->amount * 45); /* the ripple spreading out */

        for (i = 0; i < 3; i++) {
            struct fx_part *p = fx_spawn_at(data, st, FALSE, x + 0.5f,
                                            y + 0.5f, 0.0f, 1.0f, at);

            p->vx = p->vy = 0.0f;
            p->size = 0.22f;
            p->c0 = RGB(255, 255, 255);
            p->life = 160;
        }
        for (i = 0; i < 12; i++)
            (void) fx_spawn_at(data, st, FALSE, x + 0.5f, y + 0.5f,
                               st->speed, 1.0f, at);
        fx_ring(data, st, FALSE, x, y, 14, 0.7f, 0.01f, 420, at);
    }
    fx_start_timer(data);
}

/* a scroll whose type is known has taken effect: its look around the hero
   (teleportation, fire and the detection scrolls have their own) */
static void
fx_scroll_particles(PNHMapWindow data, const struct fx_event *ev)
{
    static const struct fx_pstyle
        wpn = { RGB(200, 230, 255), RGB(110, 170, 255), RGB(0, 15, 50),
                0.4f, -2.0f, 0.4f, 0.055f, 600, 1000 },
        armor = { RGB(255, 235, 150), RGB(255, 200, 80), RGB(50, 30, 0),
                  0.6f, -0.9f, 0.4f, 0.06f, 600, 1100 },
        holy = { RGB(255, 252, 230), RGB(255, 235, 170), RGB(60, 50, 20),
                 0.5f, -1.4f, 0.4f, 0.06f, 700, 1200 },
        ident = { RGB(170, 255, 250), RGB(90, 210, 255), RGB(0, 30, 40),
                  1.4f, -0.3f, 0.25f, 0.05f, 400, 700 },
        mapping = { RGB(200, 240, 255), RGB(120, 200, 255), RGB(0, 20, 40),
                    0.0f, 0.0f, 0.1f, 0.055f, 900, 900 },
        earth = { RGB(195, 165, 125), RGB(145, 125, 100), RGB(30, 25, 20),
                  2.2f, 1.4f, 0.2f, 0.07f, 450, 850 },
        taming = { RGB(255, 170, 210), RGB(255, 120, 180), RGB(60, 0, 25),
                   0.5f, -1.0f, 0.4f, 0.065f, 700, 1200 },
        genocide = { RGB(210, 30, 40), RGB(120, 0, 30), RGB(15, 0, 5),
                     0.0f, 0.0f, 0.1f, 0.07f, 1000, 1000 },
        charging = { RGB(255, 255, 200), RGB(140, 200, 255), RGB(20, 20, 50),
                     3.0f, 0.0f, 0.05f, 0.045f, 160, 360 },
        scare = { RGB(170, 90, 230), RGB(90, 40, 150), RGB(10, 0, 20),
                  0.0f, 0.0f, 0.1f, 0.07f, 800, 800 },
        shards = { RGB(200, 200, 205), RGB(140, 140, 150), RGB(20, 20, 25),
                   1.6f, 2.4f, 0.3f, 0.05f, 450, 800 },
        fog = { RGB(150, 150, 165), RGB(110, 110, 125), RGB(10, 10, 15),
                0.4f, 0.0f, 0.4f, 0.12f, 900, 1500 },
        confuse = { RGB(255, 110, 120), RGB(200, 90, 230), RGB(40, 0, 30),
                    0.4f, -0.6f, 0.4f, 0.055f, 600, 1000 },
        punish = { RGB(160, 160, 170), RGB(110, 110, 120), RGB(15, 15, 20),
                   1.4f, 0.8f, 0.2f, 0.06f, 400, 700 };
    boolean rb;
    int i, x = ev->x, y = ev->y;

    if (!iflags.fx_particles || !isok(x, y))
        return;
    switch (ev->otyp) {
    case SCR_ENCHANT_WEAPON:
        /* blue sparks climbing as the weapon glows */
        for (i = 0; i < 45; i++)
            (void) fx_spawn_at(data, &wpn, FALSE,
                               x + 0.5f + fx_prange(-0.35f, 0.35f),
                               y + 0.8f, wpn.speed, 1.0f,
                               (DWORD) fx_prange(0, 350));
        fx_ring(data, &wpn, FALSE, x, y, 24, 0.9f, 0.02f, 450, 0);
        break;
    case SCR_ENCHANT_ARMOR:
        /* a golden shimmer all over */
        for (i = 0; i < 60; i++)
            (void) fx_spawn_at(data, &armor, FALSE,
                               x + 0.5f + fx_prange(-0.55f, 0.55f),
                               y + 0.5f + fx_prange(-0.5f, 0.5f),
                               armor.speed, 1.0f, (DWORD) fx_prange(0, 300));
        fx_ring(data, &armor, FALSE, x, y, 32, 1.1f, 0.02f, 550, 0);
        break;
    case SCR_REMOVE_CURSE:
        for (i = 0; i < 60; i++)
            (void) fx_spawn_at(data, &holy, FALSE,
                               x + 0.5f + fx_prange(-1.0f, 1.0f),
                               y + 0.7f + fx_prange(-0.3f, 0.5f), holy.speed,
                               1.0f, (DWORD) fx_prange(0, 400));
        fx_ring(data, &holy, FALSE, x, y, 40, 1.6f, 0.02f, 650, 0);
        break;
    case SCR_IDENTIFY:
        for (i = 0; i < 30; i++)
            (void) fx_spawn_at(data, &ident, FALSE, x + 0.5f, y + 0.4f,
                               ident.speed, 1.0f, (DWORD) fx_prange(0, 150));
        fx_ring(data, &ident, FALSE, x, y, 24, 0.9f, 0.01f, 400, 0);
        break;
    case SCR_MAGIC_MAPPING:
        /* the level drawing itself out from you */
        fx_ring(data, &mapping, FALSE, x, y, 120, 12.0f, 0.15f, 1300, 0);
        fx_ring(data, &mapping, FALSE, x, y, 90, 8.0f, 0.15f, 1100, 200);
        break;
    case SCR_LIGHT:
        /* the same wave of light as the wand */
        {
            struct fx_event le = *ev;

            le.type = FXE_WAND;
            le.adtyp = FXW_SELF;
            le.otyp = WAN_LIGHT;
            fx_wand_particles(data, &le);
        }
        return;
    case SCR_CREATE_MONSTER:
    case SCR_STINKING_CLOUD:
        {
            struct fx_event le = *ev;

            le.type = FXE_WAND;
            le.adtyp = FXW_SELF;
            le.otyp = WAN_CREATE_MONSTER; /* a puff of smoke */
            fx_wand_particles(data, &le);
        }
        return;
    case SCR_EARTH:
        /* dust thrown up all around as the boulders land */
        for (i = 0; i < 70; i++) {
            float r = fx_prange(0.6f, 1.6f), a = fx_prange(0, 6.283f);

            (void) fx_spawn_at(data, &earth, FALSE,
                               x + 0.5f + r * (float) cos(a),
                               y + 0.5f + r * (float) sin(a), earth.speed,
                               1.0f, (DWORD) fx_prange(0, 200));
        }
        break;
    case SCR_TAMING:
        for (i = 0; i < 40; i++)
            (void) fx_spawn_at(data, &taming, FALSE,
                               x + 0.5f + fx_prange(-1.2f, 1.2f),
                               y + 0.6f + fx_prange(-0.6f, 0.6f),
                               taming.speed, 1.0f, (DWORD) fx_prange(0, 400));
        fx_ring(data, &taming, FALSE, x, y, 40, 1.8f, 0.03f, 700, 0);
        break;
    case SCR_GENOCIDE:
        /* a dark red pulse rolling out, twice */
        fx_ring(data, &genocide, FALSE, x, y, 90, 7.0f, 0.1f, 1200, 0);
        fx_ring(data, &genocide, FALSE, x, y, 70, 4.5f, 0.1f, 1000, 250);
        break;
    case SCR_CHARGING:
        for (i = 0; i < 50; i++)
            (void) fx_spawn_at(data, &charging, FALSE,
                               x + 0.5f + fx_prange(-0.3f, 0.3f),
                               y + 0.5f + fx_prange(-0.3f, 0.3f),
                               charging.speed, 0.9f,
                               (DWORD) fx_prange(0, 400));
        break;
    case SCR_SCARE_MONSTER:
        fx_ring(data, &scare, FALSE, x, y, 80, 5.0f, 0.08f, 1000, 0);
        break;
    case SCR_DESTROY_ARMOR:
        for (i = 0; i < 40; i++)
            (void) fx_spawn_at(data, &shards, FALSE,
                               x + 0.5f + fx_prange(-0.4f, 0.4f),
                               y + 0.4f + fx_prange(-0.3f, 0.3f),
                               shards.speed, 1.0f, (DWORD) fx_prange(0, 150));
        break;
    case SCR_AMNESIA:
        for (i = 0; i < 40; i++)
            (void) fx_spawn_at(data, &fog, FALSE,
                               x + 0.5f + fx_prange(-1.0f, 1.0f),
                               y + 0.5f + fx_prange(-0.8f, 0.8f), fog.speed,
                               1.0f, (DWORD) fx_prange(0, 500));
        break;
    case SCR_CONFUSE_MONSTER:
        /* the hands glowing */
        for (i = 0; i < 30; i++)
            (void) fx_spawn_at(data, &confuse, FALSE,
                               x + 0.5f + ((i & 1) ? 0.35f : -0.35f)
                                   + fx_prange(-0.1f, 0.1f),
                               y + 0.65f, confuse.speed, 1.0f,
                               (DWORD) fx_prange(0, 400));
        break;
    case SCR_PUNISHMENT:
        for (i = 0; i < 26; i++)
            (void) fx_spawn_at(data, &punish, FALSE, x + 0.5f, y + 0.95f,
                               punish.speed, 1.0f, (DWORD) fx_prange(0, 100));
        break;
    default:
        return;
    }
    (void) rb;
    fx_start_timer(data);
}

static COLORREF
fx_lerp_color(COLORREF a, COLORREF b, float t)
{
    return RGB((int) (GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int) (GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int) (GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

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

    if (!data->nparts)
        return;
    ox = data->map_orig.x - data->xPos * data->xFrontTile;
    oy = data->map_orig.y - data->yPos * data->yFrontTile;
    tw = data->xFrontTile, th = data->yFrontTile;
    fx_part_layer(data, hdc);
    /* where they are this frame */
    for (i = 0; i < data->nparts; i++) {
        struct fx_part *p = &data->parts[i];
        float s = (float) (long) (now - p->start) / 1000.0f;
        float keep = (float) pow(p->drag, s), dist;
        double px, py, rad;

        if (s < 0)
            s = 0;
        /* integrate v*drag^t: distance = v*(drag^t - 1)/ln(drag) */
        dist = (p->drag > 0.0f && p->drag < 1.0f)
                   ? (float) ((keep - 1.0f) / log(p->drag)) : s;
        px = ox + (p->x + p->vx * dist) * tw;
        py = oy + (p->y + p->vy * dist + 0.5f * p->grav * s * s) * th;
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
    for (i = 0; i < data->nparts; i++) {
        struct fx_part *p = &data->parts[i];
        float s = (float) (long) (now - p->start) / 1000.0f;
        float k = (float) (long) (now - p->start) / (float) p->life;
        float keep = (float) pow(p->drag, s), dist;
        double px, py, rad;
        COLORREF c;
        int ir;

        if (k < 0 || k >= 1)
            continue;
        dist = (p->drag > 0.0f && p->drag < 1.0f)
                   ? (float) ((keep - 1.0f) / log(p->drag)) : s;
        px = ox + (p->x + p->vx * dist) * tw;
        py = oy + (p->y + p->vy * dist + 0.5f * p->grav * s * s) * th;
        rad = p->size * tw * (1.0 - 0.55 * k);
        c = fx_lerp_color(p->c0, p->c1, k);
        /* faint glow around it (added on as light) */
        ir = (int) (rad * 2.2 + 0.5);
        SetDCBrushColor(data->partDC, fx_lerp_color(RGB(0, 0, 0), c, 0.35f));
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
    for (i = 0; i < data->nparts; i++) {
        struct fx_part *p = &data->parts[i];
        float s = (float) (long) (now - p->start) / 1000.0f;
        float k = (float) (long) (now - p->start) / (float) p->life;
        float keep = (float) pow(p->drag, s), dist;
        double px, py, rad;
        int ir;

        if (k < 0 || k >= 1)
            continue;
        dist = (p->drag > 0.0f && p->drag < 1.0f)
                   ? (float) ((keep - 1.0f) / log(p->drag)) : s;
        px = ox + (p->x + p->vx * dist) * tw;
        py = oy + (p->y + p->vy * dist + 0.5f * p->grav * s * s) * th;
        rad = p->size * tw * (1.0 - 0.55 * k);
        ir = max(1, (int) (rad + 0.5));
        SetDCBrushColor(hdc, fx_lerp_color(p->c0, p->c1, k));
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
                        c = fx_lerp_color(RGB(255, 80, 80), RGB(80, 160, 255),
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
static void
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
    BLENDFUNCTION bf;

    if (!iflags.fx_lighting || data->bAsciiMode || Is_rogue_level(&u.uz)
        || u.uswallow || !program_state.in_moveloop)
        return;
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
            return;
        SelectObject(lightDC, lightBmp);
    }
    fx_light_map(bright, warm);
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
}

/* where the hero is drawn while sliding onto its square: the offset in
   pixels from the square (0, 0 when standing still) */
static void
fx_hero_slide_offset(PNHMapWindow data, int *offx, int *offy)
{
    DWORD now = GetTickCount();
    int i;

    *offx = *offy = 0;
    for (i = 0; i < data->nanims; i++) {
        struct fx_anim *a = &data->anims[i];
        long age = (long) (now - a->start);
        double e;

        if (a->kind != FXM_SLIDE || a->x != u.ux || a->y != u.uy
            || age < 0 || age >= (long) a->dur)
            continue;
        /* the same easing fx_paint_sprites uses */
        e = 1.0 - (double) age / a->dur;
        e = a->hit ? e * e : e * e * (3 - 2 * e);
        *offx = (int) (-a->dx * data->xFrontTile * e);
        *offy = (int) (-a->dy * data->yFrontTile * e);
        return;
    }
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
        if (a->kind == FXM_GHOST) {
            unsigned long au = data->map[a->x][a->y].fxauras;
            int nghost = (au & (FXA_VERYFAST | FXA_FAST)) ? 1 : 0;

            /* the hero's intrinsic speed is too common to trail every
               step: only extra speed (boots, potion, spell) shows */
            if (a->x == u.ux && a->y == u.uy && !(au & FXA_VERYFAST))
                nghost = 0;
            double back, lag;
            int alpha;

            if (!iflags.fx_afterimage)
                continue; /* turned off since the step */
            if (a->strength > nghost
                || !glyph_is_monster(data->map[a->x][a->y].glyph))
                continue;
            /* where the creature is on its slide right now (the slide
               runs FX_STEP_MS); a single afterimage hangs back along the
               way it came (further when very fast), then slowly catches
               up as it fades */
            lag = (au & FXA_VERYFAST) ? 0.95 : 0.75;
            back = 1.0 - min(1.0, (double) age / FX_STEP_MS);
            back = back * back * (3 - 2 * back);
            back = min(1.0, back + lag * (1.0 - p) * (1.0 - p));
            alpha = (int) (125 * (1.0 - p));
            cx = ox + a->x * tw - (int) (a->dx * tw * back);
            cy = oy + a->y * th - (int) (a->dy * th * back);
            fx_blend_tile(data, hdc, cx, cy, tw, th,
                          data->map[a->x][a->y].gm.tileidx, alpha);
            continue;
        }
        if (a->kind == FXM_SLIDE) {
            /* whatever is drawn there now (the step was reported before
               the map caught up); nothing to slide if it's gone */
            if (!glyph_is_monster(data->map[a->x][a->y].glyph))
                continue;
            a->tile = data->map[a->x][a->y].gm.tileidx;
        }
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
        } else if (a->kind == FXM_SLIDE) {
            /* from the square it left, easing in (steps a bit softer) */
            e = 1.0 - p;
            e = a->hit ? e * e : e * e * (3 - 2 * e);
            offx = (int) (-a->dx * tw * e);
            offy = (int) (-a->dy * th * e);
        } else { /* FXM_SHAKE: decaying side-to-side jitter */
            double amp = tw * (0.04 + 0.10 * a->strength / 100.0);
            /* plus a push away from the blow, springing back */
            double push = tw * (0.06 + 0.10 * a->strength / 100.0)
                          * (1.0 - p) * (1.0 - p);

            offx = (int) (amp * sin(p * 2 * 3.14159265358979 * 3.0) * (1.0 - p));
            offy = (int) (amp * 0.3 * sin(p * 2 * 3.14159265358979 * 5.0) * (1.0 - p));
            offx += (int) (a->dx * push);
            offy += (int) (a->dy * push);
        }
        cx = ox + a->x * tw, cy = oy + a->y * th;
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
                          FX_INVIS_ALPHA);
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
    int x, y, i, ox, oy, tw, th;

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
            boolean busy = FALSE;

            if (!(data->map[x][y].fxauras & FXA_INVIS)
                || !glyph_is_monster(data->map[x][y].glyph))
                continue;
            for (i = 0; i < data->nanims; i++) {
                struct fx_anim *a = &data->anims[i];
                long age = (long) (now - a->start);

                if (a->x == x && a->y == y && a->kind != FXM_GHOST
                    && age >= 0 && age < (long) a->dur) {
                    busy = TRUE;
                    break;
                }
            }
            if (busy)
                continue;
            cx = ox + x * tw, cy = oy + y * th;
            if ((bk = fx_floor_tile(data, x, y)) >= 0)
                StretchBlt(hdc, cx, cy, tw, th, data->tileDC, TILEBMP_X(bk),
                           TILEBMP_Y(bk), GetNHApp()->mapTile_X,
                           GetNHApp()->mapTile_Y, SRCCOPY);
            fx_blend_tile(data, hdc, cx, cy, tw, th,
                          data->map[x][y].gm.tileidx, FX_INVIS_ALPHA);
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
    for (i = j = 0; i < data->nparts; i++)
        if ((long) (now - data->parts[i].start) < (long) data->parts[i].life)
            data->parts[j++] = data->parts[i];
    data->nparts = j;
    if (!data->nfloaters && !data->nanims && !data->shakeDur && !data->nparts
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
