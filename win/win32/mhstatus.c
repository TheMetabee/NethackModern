/* $NHDT-Header: NetHack 5.0 mhstatus.c 2026/06/20_22:26:43 NetHack-5.0:1.47 $ */
/* Copyright (C) 2001 by Alex Kompel */
/* NetHack may be freely redistributed.  See license for details. */

#include <assert.h>
#include "winos.h"
#include "winMS.h"
#include "mhstatus.h"
#include "mhmsg.h"
#include "mhfont.h"
#include "mhtheme.h"

#ifndef STATUS_HILITES
#error STATUS_HILITES not defined
#endif

#define MAXWINDOWTEXT BUFSZ

#define STATUS_BLINK_INTERVAL 500 // milliseconds


extern COLORREF nhcolor_to_RGB(int c); /* from mhmap */
typedef struct back_buffer {
    HWND hWnd;
    HDC hdc;
    HBITMAP bm;
    HBITMAP orig_bm;
    int width;
    int height;
} back_buffer_t;

void back_buffer_free(back_buffer_t * back_buffer);
void back_buffer_allocate(back_buffer_t * back_buffer, int width, int height);
void back_buffer_size(back_buffer_t * back_buffer, int width, int height);
void back_buffer_init(back_buffer_t * back_buffer, HWND hWnd, int width, int height);

void back_buffer_free(back_buffer_t * back_buffer)
{
    if (back_buffer->bm != NULL) {
        SelectObject(back_buffer->hdc, back_buffer->orig_bm);
        DeleteObject(back_buffer->bm);
        back_buffer->bm = NULL;
    }
}

void back_buffer_allocate(back_buffer_t * back_buffer, int width, int height)
{
    HDC hdc = GetDC(back_buffer->hWnd);
    back_buffer->bm = CreateCompatibleBitmap(hdc, width, height);
    back_buffer->orig_bm =  SelectObject(back_buffer->hdc, back_buffer->bm);
    back_buffer->width = width;
    back_buffer->height = height;
    ReleaseDC(back_buffer->hWnd, hdc);
}

void back_buffer_size(back_buffer_t * back_buffer, int width, int height)
{
    if (back_buffer->bm == NULL || back_buffer->width != width
                                || back_buffer->height != height) {
        back_buffer_free(back_buffer);
        back_buffer_allocate(back_buffer, width, height);
    }
}

void back_buffer_init(back_buffer_t * back_buffer, HWND hWnd, int width, int height)
{
    back_buffer->hWnd = hWnd;
    back_buffer->hdc = CreateCompatibleDC(NULL);
    back_buffer->bm = NULL;

    back_buffer_size(back_buffer, width, height);
}


typedef struct mswin_nethack_status_window {
    int index;
    char window_text[NHSW_LINES][MAXWINDOWTEXT + 1];
    mswin_status_lines * status_lines;
    back_buffer_t back_buffer;
    boolean blink_state; /* true = invert blink text */
    boolean has_blink_fields; /* true if one or more has blink attribute */
} NHStatusWindow, *PNHStatusWindow;


static TCHAR szStatusWindowClass[] = TEXT("MSNHStatusWndClass");
LRESULT CALLBACK StatusWndProc(HWND, UINT, WPARAM, LPARAM);
static void register_status_window_class(void);
static LRESULT onWMPaint(HWND hWnd, WPARAM wParam, LPARAM lParam);

#define DEFAULT_COLOR_BG_STATUS COLOR_WINDOW
#define DEFAULT_COLOR_FG_STATUS COLOR_WINDOWTEXT

HWND
mswin_init_status_window(void)
{
    static int run_once = 0;
    HWND ret;
    NHStatusWindow *data;
    RECT rt;
    int width, height;

    if (!run_once) {
        register_status_window_class();
        run_once = 1;
    }

    /* get window position */
    if (GetNHApp()->bAutoLayout) {
        SetRect(&rt, 0, 0, 0, 0);
    } else {
        mswin_get_window_placement(NHW_STATUS, &rt);
    }

    /* create status window object */
    width = rt.right - rt.left;
    height = rt.bottom - rt.top;
    ret = CreateWindow(szStatusWindowClass, NULL,
                       WS_CHILD | WS_CLIPSIBLINGS | WS_SIZEBOX,
                       rt.left, /* horizontal position of window */
                       rt.top,  /* vertical position of window */
                       width,   /* window width */
                       height,  /* window height */
                       GetNHApp()->hMainWnd, NULL, GetNHApp()->hApp, NULL);
    if (!ret)
        panic("Cannot create status window");

    /* Set window caption */
    SetWindowText(ret, "Status");

    /* create window data */
    data = (PNHStatusWindow) malloc(sizeof(NHStatusWindow));
    if (!data)
        panic("out of memory");

    windowdata[NHW_STATUS].address = (genericptr_t) data;
    ZeroMemory(data, sizeof(NHStatusWindow));
    SetWindowLongPtr(ret, GWLP_USERDATA, (LONG_PTR) data);

    back_buffer_init(&data->back_buffer, ret, width, height);

    mswin_apply_window_style(ret);

    if (status_bg_brush == NULL) {
        status_bg_color = (COLORREF)GetSysColor(DEFAULT_COLOR_BG_STATUS);
        status_bg_brush = CreateSolidBrush(status_bg_color);
    }

    if (status_fg_brush == NULL) {
        status_fg_color = (COLORREF)GetSysColor(DEFAULT_COLOR_FG_STATUS);
        status_fg_brush = CreateSolidBrush(status_fg_color);
    }

    /* set cursor blink timer */
    SetTimer(ret, 0, STATUS_BLINK_INTERVAL, NULL);

    return ret;
}

void
register_status_window_class(void)
{
    WNDCLASS wcex;
    ZeroMemory(&wcex, sizeof(wcex));

    wcex.style = CS_NOCLOSE;
    wcex.lpfnWndProc = (WNDPROC) StatusWndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = GetNHApp()->hApp;
    wcex.hIcon = NULL;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = NULL;
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = szStatusWindowClass;

    RegisterClass(&wcex);
}

LRESULT CALLBACK
StatusWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    PNHStatusWindow data;

    data = (PNHStatusWindow) GetWindowLongPtr(hWnd, GWLP_USERDATA);
    switch (message) {
    case WM_MSNH_COMMAND: {
        switch (wParam) {
        case MSNH_MSG_PUTSTR: {
            PMSNHMsgPutstr msg_data = (PMSNHMsgPutstr) lParam;
            strncpy(data->window_text[data->index], msg_data->text,
                    MAXWINDOWTEXT);
            data->index = (data->index + 1) % NHSW_LINES;
            InvalidateRect(hWnd, NULL, TRUE);
        } break;

        case MSNH_MSG_CLEAR_WINDOW: {
            data->index = 0;
            ZeroMemory(data->window_text, sizeof(data->window_text));
            InvalidateRect(hWnd, NULL, TRUE);
        } break;

        case MSNH_MSG_GETTEXT: {
            PMSNHMsgGetText msg_data = (PMSNHMsgGetText) lParam;
            msg_data->buffer[0] = '\0';
            size_t space_remaining = msg_data->max_size;

            for (int line = 0; line < NHSW_LINES; line++) {
                mswin_status_line *status_line = &data->status_lines->lines[line];
                for (int i = 0; i < status_line->status_strings.count; i++) {
                    mswin_status_string * status_string = status_line->status_strings.status_strings[i];
                    if (status_string->str != NULL) {
                        if (status_string->space_in_front) {
                            strncat(msg_data->buffer, " ", space_remaining);
                            space_remaining = msg_data->max_size - strlen(msg_data->buffer);
                        }
                        strncat(msg_data->buffer, status_string->str, space_remaining);
                        space_remaining = msg_data->max_size - strlen(msg_data->buffer);
                    }
                }
                strncat(msg_data->buffer, "\r\n", space_remaining);
                space_remaining = msg_data->max_size - strlen(msg_data->buffer);
            }
        } break;

        case MSNH_MSG_UPDATE_STATUS: {
            PMSNHMsgUpdateStatus msg_data = (PMSNHMsgUpdateStatus) lParam;
            data->status_lines = msg_data->status_lines;
            InvalidateRect(hWnd, NULL, TRUE);
        } break;

                case MSNH_MSG_RANDOM_INPUT:
                    nhassert(0); // unexpected
                    break;

        } /* end switch( wParam ) { */
    } break;

    case WM_PAINT:
        return onWMPaint(hWnd, wParam, lParam);

    case WM_SIZE: {
        RECT rt;
        GetWindowRect(hWnd, &rt);
        ScreenToClient(GetNHApp()->hMainWnd, (LPPOINT) &rt);
        ScreenToClient(GetNHApp()->hMainWnd, ((LPPOINT) &rt) + 1);
        mswin_update_window_placement(NHW_STATUS, &rt);
    }
        return FALSE;

    case WM_MOVE: {
        RECT rt;
        GetWindowRect(hWnd, &rt);
        ScreenToClient(GetNHApp()->hMainWnd, (LPPOINT) &rt);
        ScreenToClient(GetNHApp()->hMainWnd, ((LPPOINT) &rt) + 1);
        mswin_update_window_placement(NHW_STATUS, &rt);
    }
        return FALSE;

    case WM_DESTROY:
        free(data);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR) 0);
        windowdata[NHW_STATUS].address = 0;
        break;

    case WM_SETFOCUS:
        SetFocus(GetNHApp()->hMainWnd);
        break;

    case WM_TIMER:
        data->blink_state = !data->blink_state;
        if (data->has_blink_fields)
            InvalidateRect(hWnd, NULL, TRUE);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* NetHack Modern (Phase 4c): the character panel                      */

extern const char *const hu_stat[]; /* eat.c */
extern const char *const enc_stat[]; /* botl.c */

/* the text the core gave a status field, or "" */
static const char *
th_field(PNHStatusWindow data, int idx)
{
    int line, i;

    for (line = 0; line < NHSW_LINES; line++) {
        mswin_status_fields *f = &data->status_lines->lines[line].status_fields;

        for (i = 0; i < f->count; i++)
            if (f->status_fields[i]->field_index == idx
                && f->status_fields[i]->enabled)
                return f->status_fields[i]->string;
    }
    return "";
}

static void
th_text(HDC hdc, int x, int y, const char *s, COLORREF c)
{
    WCHAR w[BUFSZ];

    SetTextColor(hdc, c);
    winos_ascii_to_wide_str((const unsigned char *) s, w, SIZE(w));
    TextOutW(hdc, x, y, w, (int) wcslen(w));
}

static int
th_width(HDC hdc, const char *s)
{
    SIZE sz;

    GetTextExtentPoint32A(hdc, s, (int) strlen(s), &sz);
    return sz.cx;
}

static void
th_round(HDC hdc, int l, int t, int r, int b, int rad, COLORREF fill)
{
    HBRUSH br = CreateSolidBrush(fill);
    HGDIOBJ ob = SelectObject(hdc, br),
            op = SelectObject(hdc, GetStockObject(NULL_PEN));

    RoundRect(hdc, l, t, r + 1, b + 1, rad, rad);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(br);
}

/* a labelled bar: "HP 29 / 32" over a fill of 'pct' percent */
static void
th_bar(HDC hdc, int x, int y, int w, int h, const char *label, int cur,
       int max, COLORREF fill)
{
    char buf[64];
    int pct = (max > 0) ? (cur * 100) / max : 0;
    int fw;

    pct = (pct < 0) ? 0 : (pct > 100) ? 100 : pct;
    th_round(hdc, x, y, x + w, y + h, h / 2, TH_BG_HEADER);
    fw = (w * pct) / 100;
    if (fw > 0)
        th_round(hdc, x, y, x + max(fw, h / 2), y + h, h / 2, fill);
    Sprintf(buf, "%s  %d / %d", label, cur, max);
    SetBkMode(hdc, TRANSPARENT);
    {
        TEXTMETRIC tm;

        GetTextMetrics(hdc, &tm);
        th_text(hdc, x + (w - th_width(hdc, buf)) / 2,
                y + (h - tm.tmHeight) / 2, buf, RGB(250, 250, 252));
    }
}

/* a vertical rule between sections */
static void
th_rule(HDC hdc, int x, int t, int b)
{
    RECT r;
    HBRUSH br = CreateSolidBrush(TH_SEPARATOR);

    SetRect(&r, x, t, x + 1, b);
    FillRect(hdc, &r, br);
    DeleteObject(br);
}

/* "Weak" with the trailing blanks the core pads it with removed */
static void
th_trim(char *dst, const char *src, size_t n)
{
    size_t k;

    (void) strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
    for (k = strlen(dst); k > 0 && dst[k - 1] == ' '; k--)
        dst[k - 1] = '\0';
    while (*dst == ' ')
        memmove(dst, dst + 1, strlen(dst));
}

/* the equipment doll: what's worn and wielded, as tiles in two rows of
   six slots starting at x0; returns its width */
static int
th_paint_doll(HDC hdc, int x0, int y1, int rowh)
{
    struct obj *slot[12];
    int cell = rowh - 3, gap = 4, i;
    HDC tdc;
    HGDIOBJ oldBmp;

    slot[0] = uarmh;   /* helmet */
    slot[1] = uamul;   /* amulet */
    slot[2] = uarmc;   /* cloak */
    slot[3] = uarm;    /* body armour */
    slot[4] = uarmu;   /* shirt */
    slot[5] = uarms;   /* shield */
    slot[6] = uwep;    /* weapon */
    slot[7] = uswapwep; /* off-hand or alternate weapon */
    slot[8] = uarmg;   /* gloves */
    slot[9] = uleft;   /* left ring */
    slot[10] = uright; /* right ring */
    slot[11] = uarmf;  /* boots */

    tdc = CreateCompatibleDC(hdc);
    oldBmp = SelectObject(tdc, GetNHApp()->bmpMapTiles);
    for (i = 0; i < 12; i++) {
        int cx = x0 + (i % 6) * (cell + gap);
        int cy = y1 + (i / 6) * rowh;
        struct obj *o = slot[i];
        COLORREF edge = TH_SEPARATOR;

        if (o && o->bknown)
            edge = o->cursed ? RGB(210, 64, 60)
                   : o->blessed ? RGB(92, 190, 92) : TH_SEPARATOR;
        /* the slot: a rounded box, its edge telling blessed/cursed */
        th_round(hdc, cx, cy, cx + cell, cy + cell, 6, edge);
        th_round(hdc, cx + 1, cy + 1, cx + cell - 1, cy + cell - 1, 5,
                 o ? TH_BG_HEADER : TH_BG_PANEL);
        if (o) {
            glyph_info gi;
            int t, tx, ty, pad = max(1, cell / 10);

            map_glyphinfo(0, 0, obj_to_glyph(o, rn2_on_display_rng), 0,
                          &gi);
            t = gi.gm.tileidx;
            tx = (t % GetNHApp()->mapTilesPerLine) * GetNHApp()->mapTile_X;
            ty = (t / GetNHApp()->mapTilesPerLine) * GetNHApp()->mapTile_Y;
            (*GetNHApp()->lpfnTransparentBlt)(
                hdc, cx + pad, cy + pad, cell - 2 * pad, cell - 2 * pad, tdc,
                tx, ty, GetNHApp()->mapTile_X, GetNHApp()->mapTile_Y,
                TILE_BK_COLOR);
        }
    }
    SelectObject(tdc, oldBmp);
    DeleteDC(tdc);
    return 6 * cell + 5 * gap;
}

static void
th_paint_status(PNHStatusWindow data, HDC hdc, int width, int height)
{
    static const char *const statnm[6] = { "St", "Dx", "Co", "In", "Wi", "Ch" };
    cached_font *fn = mswin_get_font(NHW_STATUS, ATR_NONE, hdc, FALSE);
    cached_font *fb = mswin_get_font(NHW_STATUS, ATR_BOLD, hdc, FALSE);
    TEXTMETRIC tm;
    int pad, rowh, y1, y2, x, i, w, colw;
    char buf[BUFSZ], tmp[BUFSZ];
    int dollx;
    int hp = Upolyd ? u.mh : u.uhp, hpmax = Upolyd ? u.mhmax : u.uhpmax;
    int hpct = hpmax > 0 ? (hp * 100) / hpmax : 0;
    COLORREF hpc = (hpct > 66) ? RGB(92, 190, 92)
                   : (hpct > 33) ? RGB(214, 170, 52) : RGB(210, 64, 60);

    SelectObject(hdc, fn->hFont);
    GetTextMetrics(hdc, &tm);
    rowh = tm.tmHeight + tm.tmHeight / 4;
    pad = max(6, (height - 2 * rowh) / 2);
    y1 = pad, y2 = pad + rowh;
    SetBkMode(hdc, TRANSPARENT);
    x = 12;

    /* identity: title, then alignment and where */
    SelectObject(hdc, fb->hFont);
    th_trim(buf, th_field(data, BL_TITLE), sizeof buf);
    th_text(hdc, x, y1, buf, TH_ACCENT);
    w = th_width(hdc, buf);
    SelectObject(hdc, fn->hFont);
    th_trim(tmp, th_field(data, BL_LEVELDESC), sizeof tmp);
    for (i = 0; tmp[i]; i++)
        if (tmp[i] == ':')
            tmp[i] = ' ';
    Sprintf(buf, "%s   -   %s",
            (u.ualign.type == A_CHAOTIC) ? "Chaotic"
            : (u.ualign.type == A_NEUTRAL) ? "Neutral" : "Lawful",
            tmp);
    th_text(hdc, x, y2, buf, TH_TEXT_DIM);
    w = max(w, th_width(hdc, buf));
    x += w + 18;
    th_rule(hdc, x - 9, y1, y2 + rowh - 2);

    /* HP and Pw bars */
    {
        int bw = max(170, tm.tmAveCharWidth * 22), bh = rowh - 6;

        th_bar(hdc, x, y1 + 2, bw, bh, "HP", hp, hpmax, hpc);
        th_bar(hdc, x, y2 + 2, bw, bh, "Pw", u.uen, u.uenmax,
               RGB(70, 120, 220));
        x += bw + 18;
        th_rule(hdc, x - 9, y1, y2 + rowh - 2);
    }

    /* the six stats, two rows of three */
    colw = tm.tmAveCharWidth * 7;
    for (i = 0; i < 6; i++) {
        int sx = x + (i % 3) * colw, sy = (i < 3) ? y1 : y2;
        int v = ACURR(i);

        th_text(hdc, sx, sy, statnm[i], TH_TEXT_DIM);
        if (i == A_STR && v > 18) {
            if (v > STR18(100))
                Sprintf(buf, "%d", v - 100);
            else if (v < STR18(100))
                Sprintf(buf, "18/%02d", v - 18);
            else
                Strcpy(buf, "18/**");
        } else {
            Sprintf(buf, "%d", v);
        }
        th_text(hdc, sx + th_width(hdc, "Wi "), sy, buf, TH_TEXT);
    }
    x += 3 * colw + 10;
    th_rule(hdc, x - 9, y1, y2 + rowh - 2);

    /* armour class and level; gold and the turn */
    Sprintf(buf, "AC %d", u.uac);
    th_text(hdc, x, y1, buf, TH_TEXT);
    w = th_width(hdc, buf);
    if (Upolyd)
        Sprintf(buf, "HD %d", mons[u.umonnum].mlevel);
    else if (flags.showexp)
        Sprintf(buf, "Lvl %d  (%ld xp)", u.ulevel, u.uexp);
    else
        Sprintf(buf, "Lvl %d", u.ulevel);
    th_text(hdc, x + w + 16, y1, buf, TH_TEXT);
    w += 16 + th_width(hdc, buf);
    Sprintf(buf, "$ %ld", money_cnt(gi.invent));
    th_text(hdc, x, y2, buf, RGB(242, 210, 96));
    i = th_width(hdc, buf);
    if (flags.time) {
        Sprintf(buf, "Turn %ld", svm.moves);
        th_text(hdc, x + max(i, th_width(hdc, "AC 10")) + 16, y2, buf,
                TH_TEXT_DIM);
        i = max(i, th_width(hdc, "AC 10")) + 16 + th_width(hdc, buf);
    }
    x += max(w, i) + 18;
    th_rule(hdc, x - 9, y1, y2 + rowh - 2);

    /* what's worn and wielded */
    dollx = th_paint_doll(hdc, x, y1, rowh);
    x += dollx + 18;
    th_rule(hdc, x - 9, y1, y2 + rowh - 2);

    /* conditions as chips, wrapping onto the second row */
    {
        struct {
            const char *name;
            COLORREF c;
        } chip[24];
        int nchip = 0, cx = x, cy = y1, cap = near_capacity();

        if (u.uhs != NOT_HUNGRY) {
            static char hbuf[20];

            th_trim(hbuf, hu_stat[u.uhs], sizeof hbuf);
            chip[nchip].name = hbuf;
            chip[nchip++].c = (u.uhs == SATIATED) ? RGB(214, 190, 80)
                              : (u.uhs == HUNGRY) ? RGB(242, 152, 62)
                                                  : RGB(235, 84, 76);
        }
        if (cap > UNENCUMBERED) {
            chip[nchip].name = enc_stat[cap];
            chip[nchip++].c = (cap == SLT_ENCUMBER) ? RGB(214, 190, 80)
                              : (cap == MOD_ENCUMBER) ? RGB(242, 152, 62)
                                                      : RGB(235, 84, 76);
        }
#define CHIP(cond, nm, col) \
    if ((cond) && nchip < SIZE(chip)) {   \
        chip[nchip].name = (nm);          \
        chip[nchip++].c = (col);          \
    }
        CHIP(Stoned, "Stoning", RGB(235, 84, 76))
        CHIP(Slimed, "Slimed", RGB(235, 84, 76))
        CHIP(Strangled, "Strangled", RGB(235, 84, 76))
        CHIP(Sick && (u.usick_type & SICK_VOMITABLE), "Food poisoned",
             RGB(235, 84, 76))
        CHIP(Sick && (u.usick_type & SICK_NONVOMITABLE), "Ill",
             RGB(235, 84, 76))
        CHIP(Stunned, "Stunned", RGB(242, 152, 62))
        CHIP(Confusion, "Confused", RGB(200, 120, 235))
        CHIP(Hallucination, "Hallucinating", RGB(242, 134, 242))
        CHIP(Blind, "Blind", RGB(160, 166, 180))
        CHIP(Deaf, "Deaf", RGB(160, 166, 180))
        CHIP(u.ustuck && !u.uswallow, "Held", RGB(242, 152, 62))
        CHIP(u.utrap, "Trapped", RGB(242, 152, 62))
        CHIP(Wounded_legs, "Wounded legs", RGB(242, 152, 62))
        CHIP(Levitation, "Levitating", RGB(116, 172, 255))
        CHIP(Flying && !Levitation, "Flying", RGB(116, 172, 255))
        CHIP(u.usteed, "Riding", RGB(116, 172, 255))
#undef CHIP
        for (i = 0; i < nchip; i++) {
            int cw = th_width(hdc, chip[i].name) + 16;

            if (cx + cw > width - 8 && cy == y1)
                cx = x, cy = y2;
            th_round(hdc, cx, cy + 1, cx + cw, cy + rowh - 3, rowh / 2,
                     TH_BG_HEADER);
            th_text(hdc, cx + 8, cy + (rowh - 3 - tm.tmHeight) / 2 + 1,
                    chip[i].name, chip[i].c);
            cx += cw + 6;
        }
    }
}

static LRESULT
onWMPaint(HWND hWnd, WPARAM wParam UNUSED, LPARAM lParam UNUSED)
{
    SIZE sz;
    WCHAR wbuf[BUFSZ];
    RECT rt;
    PAINTSTRUCT ps;
    PNHStatusWindow data;
    int width, height;
    RECT clear_rect;
    HDC front_buffer_hdc;

    data = (PNHStatusWindow) GetWindowLongPtr(hWnd, GWLP_USERDATA);

    front_buffer_hdc = BeginPaint(hWnd, &ps);
    GetClientRect(hWnd, &rt);

    width = rt.right - rt.left;
    height = rt.bottom - rt.top;

    back_buffer_size(&data->back_buffer, width, height);

    HDC hdc = data->back_buffer.hdc;

    SetBkColor(hdc, status_bg_color);
    SetTextColor(hdc, status_fg_color);

    clear_rect.left = 0;
    clear_rect.top = 0;
    clear_rect.right = width;
    clear_rect.bottom = height;

    FillRect(hdc, &clear_rect, status_bg_brush);

    if (iflags.fx_theme && data->status_lines != NULL) {
        /* NetHack Modern: the character panel instead of two text lines */
        th_paint_status(data, hdc, width, height);
    } else if (data->status_lines != NULL) {
        
        data->has_blink_fields = FALSE;

        for (int line = 0; line < NHSW_LINES; line++) {
            LONG left = rt.left;
            LONG cy = 0;
            int vlen;

            mswin_status_line * status_line = &data->status_lines->lines[line];

            for (int i = 0; i < status_line->status_strings.count; i++) {
                mswin_status_string * status_string = status_line->status_strings.status_strings[i];
                int clr, atr;
                int fntatr = ATR_NONE;
                COLORREF nFg, nBg;

                if (status_string->str == NULL || status_string->str[0] == '\0')
                    continue;

                clr = status_string->color;
                atr = status_string->attribute;

                const char *str = status_string->str;

                vlen = strlen(str);

                if (atr & HL_BOLD)
                    fntatr = ATR_BOLD;
                else if (atr & HL_INVERSE)
                    fntatr = ATR_INVERSE;
                else if (atr & HL_ULINE)
                    fntatr = ATR_ULINE;
                else if (atr & HL_BLINK) {
                    data->has_blink_fields = TRUE;
                    if (data->blink_state) {
                        fntatr = ATR_INVERSE;
                        atr |= HL_INVERSE;
                    }
                }
                else if (atr & HL_DIM)
                    fntatr = ATR_DIM;

                cached_font * fnt = mswin_get_font(NHW_STATUS, fntatr, hdc, FALSE);

                BOOL useUnicode = fnt->supportsUnicode;

                winos_ascii_to_wide_str((const unsigned char *) str, wbuf, SIZE(wbuf));

                nFg = (clr == NO_COLOR ? status_fg_color
                    : ((clr >= 0 && clr < CLR_MAX) ? nhcolor_to_RGB(clr)
                        : status_fg_color));

                if (atr & HL_DIM) {
                    /* make a dim representation - this can produce color shift */
                    float redReduction = 0.5f;
                    float greenReduction = 0.5f;
                    float blueReduction = 0.25f;
                    uchar red = (uchar)(GetRValue(nFg) * (1.0f - redReduction));
                    uchar green = (uchar)(GetGValue(nFg) * (1.0f - greenReduction));
                    uchar blue = (uchar)(GetBValue(nFg) * (1.0f - blueReduction));
                    nFg = RGB(red, green, blue);
                }

                nBg = status_bg_color;

                if (status_string->space_in_front)
                    rt.left += fnt->width;

                sz.cy = -1;

                if (status_string->draw_bar && iflags.wc2_hitpointbar) {
                    /* NOTE: we current don't support bar attributes */

                    /* when we are drawing bar we need to look at the hp status
                     * field to get the correct percentage and color */
                    COLORREF bar_color = nhcolor_to_RGB(status_string->bar_color);
                    HBRUSH back_brush = CreateSolidBrush(bar_color);
                    RECT barrect;

                    /* prepare for drawing */
                    SelectObject(hdc, fnt->hFont);
                    SetBkMode(hdc, OPAQUE);
                    SetBkColor(hdc, status_bg_color);
                    SetTextColor(hdc, status_fg_color);

                    if (useUnicode) {
                        /* get bounding rectangle */
                        GetTextExtentPoint32W(hdc, wbuf, vlen, &sz);

                        /* first draw title normally */
                        DrawTextW(hdc, wbuf, vlen, &rt, DT_LEFT);
                    }
                    else {
                        /* get bounding rectangle */
                        GetTextExtentPoint32A(hdc, str, vlen, &sz);

                        /* first draw title normally */
                        DrawTextA(hdc, str, vlen, &rt, DT_LEFT);
                    }
                    int bar_percent = status_string->bar_percent;
                    if (bar_percent > 0) {
                        /* calc bar length */
                        barrect.left = rt.left;
                        barrect.top = rt.top;
                        barrect.bottom = sz.cy;
                        if (bar_percent > 0)
                            barrect.right = (int)((bar_percent * sz.cx) / 100);
                        /* else
                            barrect.right = sz.cx; */

                            /* then draw hpbar on top of title */
                        FillRect(hdc, &barrect, back_brush);
                        SetBkMode(hdc, TRANSPARENT);
                        SetTextColor(hdc, nBg);

                        if (useUnicode)
                            DrawTextW(hdc, wbuf, vlen, &barrect, DT_LEFT);
                        else
                            DrawTextA(hdc, str, vlen, &barrect, DT_LEFT);
                    }
                    DeleteObject(back_brush);
                }
                else {
                    if (atr & HL_INVERSE) {
                        COLORREF tmp = nFg;
                        nFg = nBg;
                        nBg = tmp;
                    }

                    /* prepare for drawing */
                    SelectObject(hdc, fnt->hFont);
                    SetBkMode(hdc, OPAQUE);
                    SetBkColor(hdc, nBg);
                    SetTextColor(hdc, nFg);

                    if (useUnicode) {
                        /* get bounding rectangle */
                        GetTextExtentPoint32W(hdc, wbuf, vlen, &sz);

                        /* draw */
                        DrawTextW(hdc, wbuf, vlen, &rt, DT_LEFT);
                    }
                    else {
                        /* get bounding rectangle */
                        GetTextExtentPoint32A(hdc, str, vlen, &sz);

                        /* draw */
                        DrawTextA(hdc, str, vlen, &rt, DT_LEFT);
                    }
                }
                assert(sz.cy >= 0);

                rt.left += sz.cx;
                cy = max(cy, sz.cy);
            }

            rt.left = left;
            rt.top += cy;
        }
    }

    BitBlt(front_buffer_hdc, 0, 0, width, height, hdc, 0, 0, SRCCOPY);

    EndPaint(hWnd, &ps);

    return 0;
}

void
mswin_status_window_size(HWND hWnd, LPSIZE sz)
{
    RECT rt;

    GetClientRect(hWnd, &rt);

    PNHStatusWindow data = (PNHStatusWindow) GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (data) {
        HDC hdc = GetDC(hWnd);
        cached_font * font = mswin_get_font(NHW_STATUS, ATR_NONE, hdc, FALSE);
        HGDIOBJ saveFont = SelectObject(hdc, font->hFont);

        SIZE text_sz;
        GetTextExtentPoint32(hdc, _T("W"), 1, &text_sz);

        TEXTMETRIC tm;
        GetTextMetrics(hdc, &tm);

        rt.bottom = rt.top + text_sz.cy * NHSW_LINES;
        if (iflags.fx_theme) /* the character panel: two roomy rows */
            rt.bottom = rt.top + 2 * (tm.tmHeight + tm.tmHeight / 4) + 16;

        SelectObject(hdc, saveFont);
        ReleaseDC(hWnd, hdc);
    }
    AdjustWindowRect(&rt, GetWindowLong(hWnd, GWL_STYLE), FALSE);
    sz->cx = rt.right - rt.left;
    sz->cy = rt.bottom - rt.top;
}
