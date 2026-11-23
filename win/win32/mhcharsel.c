/* NetHack Modern: the name and character screens.
 * NetHack may be freely redistributed.  See license for details.
 *
 * These take the place of the "Who are you?" text box and the old
 * player selection dialog.  Like the title screen they fill the game
 * window, painted over the same art, and work with the mouse or the
 * keyboard alone.
 *
 * The character screen keeps the game's rules about which choices go
 * together: role first, then race, gender and alignment, each limited
 * by the ones before it.  Choices that don't fit are shown dimmed, and
 * pointing at one says why.  Changing an earlier choice moves a later
 * one that no longer fits to the first that does.
 */

#include "win10.h"
#include "winMS.h"
#include "hack.h"
#include "mhsplash.h"
#include "mhcharsel.h"
#include "mhdlg.h"
#include "mhtheme.h"
#include "fxview.h"

PNHWinApp GetNHApp(void);

#define CS_NAME 0 /* asking for a name */
#define CS_CHAR 1 /* choosing role, race, gender, alignment */

/* the groups of choices, then the other things that can have focus */
#define G_ROLE FXV_CS_ROLE
#define G_RACE FXV_CS_RACE
#define G_GEND FXV_CS_GEND
#define G_ALIGN FXV_CS_ALIGN
#define G_NGRP FXV_CS_NGRP
#define G_BTN 4 /* the row of buttons */
#define G_BOX 5 /* the name box */
#define G_SND 6 /* the speaker icon: the menu music on or off */

#define B_BACK FXV_CSB_BACK
#define B_RANDOM FXV_CSB_RANDOM
#define B_PLAY FXV_CSB_PLAY
#define B_N FXV_CSB_N

#define MAXROWS FXV_CS_MAXROWS
#define cs_n fxv_cs_n /* how many choices in each group (shared) */

static HWND cs_hwnd;
static int cs_mode;
static int cs_result = -1;      /* 1 go on, 0 back; -1 while waiting */
static int cs_fg = G_ROLE;      /* the focus: a group, G_BTN or G_BOX */
static int cs_fb = B_PLAY;      /* ... the button, when it's G_BTN */
static int cs_lastgrp = G_ROLE; /* where Up from the buttons goes */
static int cs_lastmid = G_RACE; /* where Right from the roles goes */
static int cs_hg = -1, cs_hi = -1; /* what's under the mouse */
static int cs_dg = -1, cs_di = -1; /* what the mouse is pressing */
static RECT cs_row[G_NGRP][MAXROWS], cs_panel[G_NGRP], cs_info;
static RECT cs_btn[B_N], cs_box, cs_snd;
static char cs_name[PL_NSIZ];
static boolean cs_caret = TRUE;
static void (*cs_rnd)(void *);
static void *cs_rnd_arg;

static const char *const cs_btn_label[2][B_N] = {
    { "Back", "", "Continue" },
    { "Back", "Random", "Play" },
};

/*
 * the rules and the words are shared with the SDL port (fxview.c)
 */

#define cs_value fxv_cs_value
#define cs_ok fxv_cs_ok
#define cs_fix fxv_cs_fix
#define cs_pick fxv_cs_pick
#define cs_role_name fxv_cs_role_name
#define cs_label fxv_cs_label
#define cs_why_not fxv_cs_why_not

/*
 * moving about
 */

static void
cs_focus_group(int g)
{
    cs_fg = g;
    cs_lastgrp = g;
    if (g != G_ROLE)
        cs_lastmid = g;
}

static boolean
cs_btn_used(int b)
{
    return cs_mode == CS_CHAR || b != B_RANDOM;
}

static void
cs_focus_btn(int b)
{
    cs_fg = G_BTN;
    cs_fb = b;
}

static int
cs_next_btn(int dir)
{
    int b = cs_fb;

    do
        b = (b + B_N + dir) % B_N;
    while (!cs_btn_used(b));
    return b;
}

#define cs_step fxv_cs_step

static boolean
cs_move(int g, int dir)
{
    int i = cs_step(g, cs_value(g), dir);

    return i >= 0 && cs_pick(g, i);
}

/* Tab and Shift+Tab: through the groups, then the buttons */
static void
cs_tab(int dir)
{
    static const int stops[2][7] = {
        { G_BOX, 10 + B_PLAY, 10 + B_BACK, -1 },
        { G_ROLE, G_RACE, G_GEND, G_ALIGN, 10 + B_PLAY, 10 + B_RANDOM,
          10 + B_BACK },
    };
    const int *st = stops[cs_mode];
    int n = (cs_mode == CS_NAME) ? 3 : 7, cur = 0, k;

    for (k = 0; k < n; k++)
        if ((cs_fg == G_BTN) ? st[k] == 10 + cs_fb : st[k] == cs_fg)
            cur = k;
    k = st[(cur + n + dir) % n];
    if (k >= 10)
        cs_focus_btn(k - 10);
    else if (k == G_BOX)
        cs_fg = G_BOX;
    else
        cs_focus_group(k);
}

/* the name, with spaces trimmed off both ends */
static void
cs_trimmed(char *out)
{
    const char *p = cs_name;
    size_t n;

    while (*p == ' ')
        p++;
    Strcpy(out, p);
    n = strlen(out);
    while (n > 0 && out[n - 1] == ' ')
        out[--n] = '\0';
}

static void
cs_activate(int b)
{
    if (b == B_BACK) {
        cs_result = 0;
    } else if (b == B_RANDOM) {
        if (cs_mode == CS_CHAR && cs_rnd) {
            (*cs_rnd)(cs_rnd_arg);
            cs_fix();
        }
    } else if (cs_mode == CS_NAME) {
        char t[PL_NSIZ];

        cs_trimmed(t);
        if (*t)
            cs_result = 1;
    } else {
        cs_result = 1;
    }
    InvalidateRect(cs_hwnd, NULL, FALSE);
}

/*
 * where things go: shared with the SDL port (fxview.c)
 */

static double
cs_scale(int w, int h)
{
    return fxv_cs_scale(w, h);
}

#define S(v) ((int) ((v) * s + 0.5))

static void
cs_rect(RECT *r, const int q[4])
{
    SetRect(r, q[0], q[1], q[2], q[3]);
}

static void
cs_layout(int w, int h, int *ptop)
{
    struct fxv_cs_layout L;
    int g, i;

    fxv_cs_layout(&L, w, h, cs_mode == CS_NAME);
    *ptop = L.top;
    for (g = 0; g < G_NGRP; g++) {
        cs_rect(&cs_panel[g], L.panel[g]);
        for (i = 0; i < cs_n[g] && i < MAXROWS; i++)
            cs_rect(&cs_row[g][i], L.row[g][i]);
    }
    cs_rect(&cs_info, L.info);
    cs_rect(&cs_box, L.box);
    for (i = 0; i < B_N; i++)
        cs_rect(&cs_btn[i], L.btn[i]);
}

/*
 * drawing
 */

static void
cs_round(HDC hdc, const RECT *r, COLORREF fill, COLORREF edge, int pw,
         int rad)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, pw, edge);
    HGDIOBJ ob = SelectObject(hdc, b), op = SelectObject(hdc, p);

    nhUse(rad); /* every box has the panels' small corners */
    RoundRect(hdc, r->left, r->top, r->right, r->bottom, FXV_CORNER_GDI,
              FXV_CORNER_GDI);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(p);
    DeleteObject(b);
}

/* a wrapped paragraph from r->top down; moves r->top past it */
static void
cs_para(HDC hdc, const char *t, RECT *r, COLORREF c)
{
    RECT m = *r;

    DrawTextA(hdc, t, -1, &m, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    m.right = r->right;
    SetTextColor(hdc, c);
    DrawTextA(hdc, t, -1, &m, DT_WORDBREAK | DT_NOPREFIX);
    r->top = m.bottom;
}

static void
cs_hints(HDC hdc, int w, int h, double s, const char *t)
{
    RECT r;

    SetRect(&r, 0, h - S(34), w, h - S(10));
    mswin_ms_text(hdc, t, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE,
                  TH_TEXT_DIM, 1);
}

static void
cs_paint_buttons(HDC hdc, double s)
{
    int b;

    for (b = 0; b < B_N; b++) {
        boolean ok = TRUE;

        if (!cs_btn_used(b))
            continue;
        if (b == B_PLAY && cs_mode == CS_NAME) {
            char t[PL_NSIZ];

            cs_trimmed(t);
            ok = (*t != '\0');
        }
        mswin_ms_button(hdc, &cs_btn[b], cs_btn_label[cs_mode][b],
                        b == B_PLAY, cs_hg == G_BTN && cs_hi == b,
                        cs_fg == G_BTN && cs_fb == b,
                        cs_dg == G_BTN && cs_di == b && cs_hi == b, ok, s);
    }
}

static void
cs_paint_name(HDC hdc, int w, int h, double s)
{
    HFONT big = mswin_ms_font(S(60), FW_BOLD, TRUE),
          sub = mswin_ms_font(max(10, S(17)), FW_NORMAL, FALSE),
          txt = mswin_ms_font(max(11, S(26)), FW_SEMIBOLD, FALSE),
          btn = mswin_ms_font(max(11, S(19)), FW_SEMIBOLD, FALSE),
          small = mswin_ms_font(max(10, S(14)), FW_NORMAL, FALSE);
    HGDIOBJ of;
    RECT r;
    int top;
    boolean focus = (cs_fg == G_BOX);

    cs_layout(w, h, &top);

    of = SelectObject(hdc, big);
    SetRect(&r, 0, top, w, top + S(76));
    mswin_ms_text(hdc, "Who are you?", &r,
                  DT_CENTER | DT_BOTTOM | DT_SINGLELINE, TH_ACCENT,
                  max(2, S(3)));

    /* the box */
    cs_round(hdc, &cs_box, TH_BG_PANEL, focus ? TH_ACCENT : TH_SEPARATOR,
             focus ? max(2, S(2)) : 1, S(12));
    SelectObject(hdc, txt);
    r = cs_box;
    r.left += S(18), r.right -= S(18);
    if (cs_name[0]) {
        SIZE sz;
        int n = (int) strlen(cs_name);

        SetTextColor(hdc, TH_TEXT);
        DrawTextA(hdc, cs_name, n, &r,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GetTextExtentPoint32A(hdc, cs_name, n, &sz);
        r.left += sz.cx + S(2);
    } else {
        SetTextColor(hdc, TH_SEPARATOR);
        DrawTextA(hdc, "Enter a name", -1, &r,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (focus && cs_caret) {
        RECT c;

        SetRect(&c, r.left, cs_box.top + S(14), r.left + max(2, S(2)),
                cs_box.bottom - S(14));
        {
            HBRUSH b = CreateSolidBrush(TH_ACCENT);

            FillRect(hdc, &c, b);
            DeleteObject(b);
        }
    }

    SelectObject(hdc, sub);
    SetRect(&r, 0, cs_box.bottom + S(10), w, cs_box.bottom + S(36));
    mswin_ms_text(hdc, "If this hero has a saved game, it will carry on "
                       "from there.",
                  &r, DT_CENTER | DT_TOP | DT_SINGLELINE, TH_TEXT_DIM, 1);

    SelectObject(hdc, btn);
    cs_paint_buttons(hdc, s);

    SelectObject(hdc, small);
    cs_hints(hdc, w, h, s,
             "Type a name        Enter: continue        Esc: back        "
             "Ctrl+M: music");

    SelectObject(hdc, of);
    DeleteObject(big);
    DeleteObject(sub);
    DeleteObject(txt);
    DeleteObject(btn);
    DeleteObject(small);
}

static void
cs_paint_group(HDC hdc, int g, double s, HFONT head, HFONT row, HFONT rowb)
{
    static const char *const title[G_NGRP] = {
        "Role", "Race", "Gender", "Alignment"
    };
    RECT *p = &cs_panel[g], r;
    boolean focus = (cs_fg == g);
    int i, sel = cs_value(g);
    char buf[BUFSZ];

    cs_round(hdc, p, TH_BG_PANEL, focus ? TH_ACCENT : TH_SEPARATOR,
             focus ? max(2, S(2)) : 1, S(14));
    SelectObject(hdc, head);
    SetRect(&r, p->left + S(16), p->top + S(6), p->right - S(12),
            p->top + S(36));
    SetTextColor(hdc, TH_ACCENT);
    DrawTextA(hdc, title[g], -1, &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    for (i = 0; i < cs_n[g] && i < MAXROWS; i++) {
        boolean ok = cs_ok(g, i), hot = (cs_hg == g && cs_hi == i);
        COLORREF ink;

        r = cs_row[g][i];
        if (i == sel) {
            cs_round(hdc, &r, TH_ACCENT,
                     focus ? RGB(255, 236, 190) : TH_ACCENT,
                     focus ? max(2, S(2)) : 1, S(10));
            ink = TH_BG_APP;
        } else if (hot && ok) {
            cs_round(hdc, &r, TH_BG_HEADER, TH_BG_HEADER, 1, S(10));
            ink = TH_TEXT;
        } else {
            ink = ok ? TH_TEXT : RGB(76, 82, 94);
        }
        SelectObject(hdc, i == sel ? rowb : row);
        r.left += S(12);
        SetTextColor(hdc, ink);
        DrawTextA(hdc, cs_label(g, i, buf), -1, &r,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (!ok && hot) {
            /* a small cross to say "not with these choices" */
            r.right -= S(10);
            SetTextColor(hdc, RGB(200, 96, 80));
            DrawTextA(hdc, "x", 1, &r,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
}

/* the right-hand panel: about the role pointed at, or the one chosen */
static void
cs_paint_info(HDC hdc, double s, HFONT name, HFONT head, HFONT body,
              HFONT note)
{
    RECT r = cs_info, line;
    int rl = (cs_hg == G_ROLE && cs_hi >= 0) ? cs_hi : flags.initrole;
    char buf[BUFSZ];

    cs_round(hdc, &r, TH_BG_PANEL, TH_SEPARATOR, 1, S(14));
    r.left += S(22), r.right -= S(22), r.top += S(16);

    SelectObject(hdc, name);
    line = r;
    line.bottom = line.top + S(42);
    SetTextColor(hdc, TH_ACCENT);
    DrawTextA(hdc, cs_role_name(rl), -1, &line,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    r.top = line.bottom;

    /* the words are shared with the SDL port (fxview.c) */
    SelectObject(hdc, body);
    fxv_cs_info(rl, FXV_CSI_STARTS, buf);
    cs_para(hdc, buf, &r, TH_TEXT_DIM);
    r.top += S(12);

    fxv_cs_info(rl, FXV_CSI_BLURB, buf);
    if (*buf) {
        cs_para(hdc, buf, &r, TH_TEXT);
        r.top += S(16);
    }

    /* what the role allows, from the game's own rules */
    SelectObject(hdc, head);
    cs_para(hdc, "Races", &r, TH_ACCENT);
    SelectObject(hdc, body);
    fxv_cs_info(rl, FXV_CSI_RACES, buf);
    cs_para(hdc, buf, &r, TH_TEXT);
    r.top += S(8);

    SelectObject(hdc, head);
    cs_para(hdc, "Alignments", &r, TH_ACCENT);
    SelectObject(hdc, body);
    fxv_cs_info(rl, FXV_CSI_ALIGNS, buf);
    cs_para(hdc, buf, &r, TH_TEXT);
    fxv_cs_info(rl, FXV_CSI_GENDER, buf);
    if (*buf)
        cs_para(hdc, buf, &r, TH_TEXT_DIM);
    r.top += S(8);

    SelectObject(hdc, head);
    cs_para(hdc, "Gods", &r, TH_ACCENT);
    SelectObject(hdc, body);
    fxv_cs_info(rl, FXV_CSI_GODS, buf);
    cs_para(hdc, buf, &r, TH_TEXT);

    /* pointing at a choice that doesn't fit: say why */
    if (cs_hg >= G_RACE && cs_hg < G_NGRP && cs_hi >= 0
        && !cs_ok(cs_hg, cs_hi)) {
        cs_why_not(cs_hg, cs_hi, buf);
        SelectObject(hdc, note);
        line = cs_info;
        line.left += S(22), line.right -= S(22);
        line.bottom -= S(16);
        line.top = line.bottom - S(26);
        SetTextColor(hdc, RGB(232, 128, 106));
        DrawTextA(hdc, buf, -1, &line,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX
                      | DT_END_ELLIPSIS);
    }
}

static void
cs_paint_char(HDC hdc, int w, int h, double s)
{
    HFONT big = mswin_ms_font(S(48), FW_BOLD, TRUE),
          sub = mswin_ms_font(max(10, S(19)), FW_NORMAL, FALSE),
          head = mswin_ms_font(max(10, S(16)), FW_BOLD, FALSE),
          row = mswin_ms_font(max(10, S(17)), FW_NORMAL, FALSE),
          rowb = mswin_ms_font(max(10, S(17)), FW_SEMIBOLD, FALSE),
          name = mswin_ms_font(S(32), FW_BOLD, TRUE),
          body = mswin_ms_font(max(10, S(15)), FW_NORMAL, FALSE),
          btn = mswin_ms_font(max(11, S(19)), FW_SEMIBOLD, FALSE),
          small = mswin_ms_font(max(10, S(14)), FW_NORMAL, FALSE);
    HGDIOBJ of;
    RECT r;
    int top, g;
    char buf[BUFSZ];

    cs_layout(w, h, &top);

    of = SelectObject(hdc, big);
    SetRect(&r, 0, top, w, top + S(62));
    mswin_ms_text(hdc, "Create your hero", &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE, TH_ACCENT,
                  max(2, S(3)));
    /* the hero as chosen so far */
    SelectObject(hdc, sub);
    fxv_cs_hero_line(buf);
    SetRect(&r, 0, top + S(64), w, top + S(94));
    mswin_ms_text(hdc, buf, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                  TH_TEXT, 1);

    for (g = 0; g < G_NGRP; g++)
        cs_paint_group(hdc, g, s, head, row, rowb);
    cs_paint_info(hdc, s, name, head, body, rowb);

    SelectObject(hdc, btn);
    cs_paint_buttons(hdc, s);

    SelectObject(hdc, small);
    cs_hints(hdc, w, h, s,
             "Arrows or mouse: choose      Tab: next      Enter: play      "
             "*: random      Esc: back      Ctrl+M: music");

    SelectObject(hdc, of);
    DeleteObject(big);
    DeleteObject(sub);
    DeleteObject(head);
    DeleteObject(row);
    DeleteObject(rowb);
    DeleteObject(name);
    DeleteObject(body);
    DeleteObject(btn);
    DeleteObject(small);
}

static void
cs_paint(HWND hWnd, HDC hdc)
{
    RECT cr;
    int w, h;
    double s;
    HDC mdc;
    HBITMAP mbm;
    HGDIOBJ omb;

    GetClientRect(hWnd, &cr);
    w = cr.right, h = cr.bottom;
    if (w <= 0 || h <= 0)
        return;
    s = cs_scale(w, h);

    /* draw off screen, then copy, so nothing flickers */
    mdc = CreateCompatibleDC(hdc);
    mbm = CreateCompatibleBitmap(hdc, w, h);
    omb = SelectObject(mdc, mbm);
    mswin_ms_backdrop(hdc, mdc, w, h, cs_mode == CS_CHAR ? 150 : 40);
    SetBkMode(mdc, TRANSPARENT);
    if (cs_mode == CS_NAME)
        cs_paint_name(mdc, w, h, s);
    else
        cs_paint_char(mdc, w, h, s);
    mswin_ms_sound_rect(w, s, &cs_snd);
    mswin_ms_sound_icon(mdc, &cs_snd, cs_hg == G_SND, s);
    BitBlt(hdc, 0, 0, w, h, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, omb);
    DeleteObject(mbm);
    DeleteDC(mdc);
}

#undef S

/*
 * input
 */

/* what's at (x, y): sets *g to a group, G_BTN or G_BOX; returns the
   row or button, or -1 for nothing */
static int
cs_hit(int x, int y, int *g)
{
    POINT pt;
    int i, k;

    pt.x = x, pt.y = y;
    if (PtInRect(&cs_snd, pt)) {
        *g = G_SND;
        return 0;
    }
    for (i = 0; i < B_N; i++)
        if (cs_btn_used(i) && PtInRect(&cs_btn[i], pt)) {
            *g = G_BTN;
            return i;
        }
    if (cs_mode == CS_NAME) {
        if (PtInRect(&cs_box, pt)) {
            *g = G_BOX;
            return 0;
        }
    } else {
        for (k = 0; k < G_NGRP; k++)
            for (i = 0; i < cs_n[k] && i < MAXROWS; i++)
                if (PtInRect(&cs_row[k][i], pt)) {
                    *g = k;
                    return i;
                }
    }
    *g = -1;
    return -1;
}

static void
cs_type(WPARAM ch)
{
    size_t n = strlen(cs_name);

    if (ch == 8) { /* Backspace */
        if (n)
            cs_name[n - 1] = '\0';
    } else if (ch == 127) { /* Ctrl+Backspace: the lot */
        cs_name[0] = '\0';
    } else if (ch == 22) { /* Ctrl+V */
        if (OpenClipboard(cs_hwnd)) {
            HANDLE hd = GetClipboardData(CF_TEXT);
            const char *p = hd ? (const char *) GlobalLock(hd) : NULL;

            if (p) {
                for (; *p && n < sizeof cs_name - 1; p++)
                    if (*p >= ' ' && *p <= '~')
                        cs_name[n++] = *p;
                cs_name[n] = '\0';
                GlobalUnlock(hd);
            }
            CloseClipboard();
        }
    } else if (ch >= ' ' && ch <= '~') {
        if (n == 0 && ch == ' ')
            return;
        if (n < sizeof cs_name - 1)
            cs_name[n] = (char) ch, cs_name[n + 1] = '\0';
    } else {
        return;
    }
    cs_fg = G_BOX;
    cs_caret = TRUE;
    SetTimer(cs_hwnd, 1, GetCaretBlinkTime(), NULL); /* restart blink */
    InvalidateRect(cs_hwnd, NULL, FALSE);
}

/* a letter on the character screen: the next choice in the focused
   group (the roles, from the buttons) that starts with it */
static void
cs_letter(int c)
{
    int g = (cs_fg < G_NGRP) ? cs_fg : G_ROLE,
        i = fxv_cs_letter(g, c);

    if (i >= 0) {
        cs_pick(g, i);
        cs_focus_group(g);
        InvalidateRect(cs_hwnd, NULL, FALSE);
    }
}

static void
cs_key(WPARAM vk)
{
    boolean shift = (GetKeyState(VK_SHIFT) < 0);
    int g = cs_fg;

    switch (vk) {
    case VK_ESCAPE:
        cs_activate(B_BACK);
        return;
    case VK_RETURN:
        cs_activate(g == G_BTN ? cs_fb : B_PLAY);
        return;
    case VK_SPACE:
        if (g == G_BTN)
            cs_activate(cs_fb);
        return; /* in the name box, WM_CHAR types it */
    case VK_TAB:
        cs_tab(shift ? -1 : 1);
        break;
    case VK_UP:
        if (g == G_BTN)
            cs_fg = (cs_mode == CS_NAME) ? G_BOX : cs_lastgrp;
        else if (g < G_NGRP && !cs_move(g, -1)) {
            if (g == G_GEND)
                cs_focus_group(G_RACE);
            else if (g == G_ALIGN)
                cs_focus_group(G_GEND);
        }
        break;
    case VK_DOWN:
        if (g == G_BOX) {
            cs_focus_btn(B_PLAY);
        } else if (g < G_NGRP && !cs_move(g, 1)) {
            if (g == G_RACE)
                cs_focus_group(G_GEND);
            else if (g == G_GEND)
                cs_focus_group(G_ALIGN);
            else
                cs_focus_btn(B_PLAY);
        }
        break;
    case VK_LEFT:
        if (g == G_BTN)
            cs_focus_btn(cs_next_btn(-1));
        else if (g > G_ROLE && g < G_NGRP)
            cs_focus_group(G_ROLE);
        break;
    case VK_RIGHT:
        if (g == G_BTN)
            cs_focus_btn(cs_next_btn(1));
        else if (g == G_ROLE)
            cs_focus_group(cs_lastmid);
        break;
    case 'M':
        if (GetKeyState(VK_CONTROL) >= 0)
            return; /* a plain M is typed or picks a role, in WM_CHAR */
        mswin_menu_music_toggle();
        break;
    case VK_HOME:
    case VK_END:
        if (g < G_NGRP) {
            int i = (vk == VK_HOME) ? cs_step(g, -1, 1)
                                    : cs_step(g, cs_n[g], -1);

            if (i >= 0)
                cs_pick(g, i);
        }
        break;
    default:
        return;
    }
    InvalidateRect(cs_hwnd, NULL, FALSE);
}

static LRESULT CALLBACK
cs_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        cs_paint(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (cs_mode == CS_NAME) {
            cs_caret = !cs_caret;
            InvalidateRect(hWnd, &cs_box, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE: {
        int g, i = cs_hit(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &g);

        if (g != cs_hg || i != cs_hi) {
            TRACKMOUSEEVENT tme;

            cs_hg = g, cs_hi = i;
            if (g == G_BTN && cs_dg < 0)
                cs_focus_btn(i); /* as on the title screen */
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
        if (cs_hg >= 0) {
            cs_hg = cs_hi = -1;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            LPCTSTR cur = IDC_ARROW;

            if (cs_hg == G_BOX)
                cur = IDC_IBEAM;
            else if (cs_hg == G_BTN || cs_hg == G_SND
                     || (cs_hg >= 0 && cs_hg < G_NGRP
                         && cs_ok(cs_hg, cs_hi)))
                cur = IDC_HAND;
            SetCursor(LoadCursor(NULL, cur));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        SetFocus(hWnd);
        cs_di = cs_hit(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &cs_dg);
        if (cs_dg == G_BOX) {
            cs_fg = G_BOX;
            cs_dg = cs_di = -1;
        } else if (cs_dg >= 0 && cs_dg < G_NGRP) {
            /* a choice takes at once, if it fits */
            if (cs_pick(cs_dg, cs_di))
                cs_focus_group(cs_dg);
            else
                MessageBeep(0);
            cs_dg = cs_di = -1;
        } else if (cs_dg == G_BTN || cs_dg == G_SND) {
            SetCapture(hWnd);
        }
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int wasg = cs_dg, was = cs_di, g,
            at = cs_hit(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &g);

        cs_dg = cs_di = -1;
        if (GetCapture() == hWnd)
            ReleaseCapture();
        InvalidateRect(hWnd, NULL, FALSE);
        if (wasg == G_BTN && g == G_BTN && was == at)
            cs_activate(at);
        else if (wasg == G_SND && g == G_SND)
            mswin_menu_music_toggle();
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        cs_key(wParam);
        return 0;
    case WM_CHAR:
        if (wParam == '\r' || wParam == '\t' || wParam == 27)
            return 0; /* done in WM_KEYDOWN */
        if (cs_mode == CS_NAME) {
            if (wParam == ' ' && cs_fg == G_BTN)
                return 0; /* Space pressed a button */
            cs_type(wParam);
        } else if (wParam == '*') {
            cs_activate(B_RANDOM);
        } else if (wParam < 128 && isalpha((int) wParam)) {
            cs_letter(lowc((char) wParam));
        }
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void
mswin_char_screen_place(HWND main)
{
    HWND sw = mswin_saves_hwnd();
    RECT rc;

    GetClientRect(main, &rc);
    if (sw && IsWindow(sw)) /* (the saved heroes screen) */
        SetWindowPos(sw, HWND_TOP, 0, 0, rc.right, rc.bottom, 0);
    if (!cs_hwnd || !IsWindow(cs_hwnd))
        return;
    SetWindowPos(cs_hwnd, HWND_TOP, 0, 0, rc.right, rc.bottom, 0);
}

/* show the screen and wait for an answer; -1 if it couldn't be shown */
static int
cs_run(int mode)
{
    static boolean registered;
    HWND main = GetNHApp()->hMainWnd, bar;
    MSG msg;

    if (!registered) {
        WNDCLASS wc;

        ZeroMemory(&wc, sizeof wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = cs_proc;
        wc.hInstance = GetNHApp()->hApp;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = TEXT("MSNHCharScreen");
        if (!RegisterClass(&wc))
            return -1;
        registered = TRUE;
    }
    if (!main)
        return -1;

    cs_mode = mode;
    cs_result = -1;
    cs_hg = cs_hi = cs_dg = cs_di = -1;
    cs_caret = TRUE;
    if (mode == CS_NAME) {
        cs_fg = G_BOX;
    } else {
        cs_fg = cs_lastgrp = G_ROLE;
        cs_lastmid = G_RACE;
    }
    cs_fb = B_PLAY;

    bar = FindWindowEx(main, NULL, TEXT("MSNHTopBar"), NULL);
    if (bar)
        ShowWindow(bar, SW_HIDE);
    cs_hwnd = CreateWindow(TEXT("MSNHCharScreen"), NULL,
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 1,
                           1, main, NULL, GetNHApp()->hApp, NULL);
    if (!cs_hwnd) {
        if (bar)
            ShowWindow(bar, SW_SHOW);
        return -1;
    }
    mswin_char_screen_place(main);
    GetNHApp()->hPopupWnd = cs_hwnd; /* the main window sends focus here */
    SetForegroundWindow(main);
    SetFocus(cs_hwnd);
    if (mode == CS_NAME)
        SetTimer(cs_hwnd, 1, GetCaretBlinkTime(), NULL);

    while (cs_result < 0 && GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GetNHApp()->hPopupWnd = NULL;
    KillTimer(cs_hwnd, 1);
    DestroyWindow(cs_hwnd);
    cs_hwnd = NULL;
    mswin_ms_backdrop_free();
    if (bar)
        ShowWindow(bar, SW_SHOW);
    SetFocus(main);
    return cs_result < 0 ? 0 : cs_result;
}

boolean
mswin_name_screen(char *name, int size)
{
    int r;

    cs_name[0] = '\0';
    if (name && *name)
        (void) strncpy(cs_name, name, sizeof cs_name - 1),
            cs_name[sizeof cs_name - 1] = '\0';
    r = cs_run(CS_NAME);
    if (r < 0) /* couldn't show it; fall back on the old box */
        return mswin_getlin_window("Who are you?", name, size) != IDCANCEL;
    if (r) {
        char t[PL_NSIZ];

        cs_trimmed(t);
        (void) strncpy(name, t, size - 1);
        name[size - 1] = '\0';
    }
    return r == 1;
}

int
mswin_char_screen(void (*randomize)(void *), void *arg)
{
    fxv_cs_count();
    cs_rnd = randomize;
    cs_rnd_arg = arg;
    cs_fix(); /* should already fit; make sure */

    return cs_run(CS_CHAR);
}

/*
 * the saved heroes screen (as the SDL port's; fxview.c: fxv_saves_*)
 */

static HWND sv_hwnd;

HWND
mswin_saves_hwnd(void)
{
    return sv_hwnd;
}

static int sv_result = -2;  /* 1 go on, 0 new hero, -1 back; -2 waiting */
static int sv_sel;          /* the card chosen */
static int sv_fb = -1;      /* the button with the keyboard, -1: cards */
static int sv_hot = -1, sv_hotcard = -1, sv_chot = -1;
static boolean sv_snd_hot, sv_confirm;
static int sv_cfocus;       /* the confirmation's button with the keys */
static struct fxv_sv_layout sv_L;

static const char *const sv_btn_label[FXV_SVB_N] = {
    "Back", "Delete", "New hero", "Continue"
};

#define S(v) ((int) ((v) * s + 0.5))

static void
sv_rect(RECT *r, const int q[4])
{
    SetRect(r, q[0], q[1], q[2], q[3]);
}

static void
sv_paint(HWND hWnd, HDC hdc)
{
    RECT cr, r;
    int w, h, i;
    double s;
    HDC mdc, tdc;
    HBITMAP mbm;
    HGDIOBJ omb, of, otb;
    HFONT big, sub, name, body, small;

    GetClientRect(hWnd, &cr);
    w = cr.right, h = cr.bottom;
    if (w <= 0 || h <= 0)
        return;
    fxv_saves_layout(&sv_L, w, h, sv_sel);
    s = sv_L.s;
    big = mswin_ms_font(S(52), FW_BOLD, TRUE);
    sub = mswin_ms_font(max(10, S(17)), FW_NORMAL, FALSE);
    name = mswin_ms_font(max(11, S(21)), FW_SEMIBOLD, FALSE);
    body = mswin_ms_font(max(9, S(14)), FW_NORMAL, FALSE);
    small = mswin_ms_font(max(9, S(14)), FW_NORMAL, FALSE);

    /* draw off screen, then copy, so nothing flickers */
    mdc = CreateCompatibleDC(hdc);
    mbm = CreateCompatibleBitmap(hdc, w, h);
    omb = SelectObject(mdc, mbm);
    mswin_ms_backdrop(hdc, mdc, w, h, 150);
    SetBkMode(mdc, TRANSPARENT);

    of = SelectObject(mdc, big);
    SetRect(&r, 0, sv_L.top, w, sv_L.top + S(66));
    mswin_ms_text(mdc, "Choose your hero", &r,
                  DT_CENTER | DT_TOP | DT_SINGLELINE, TH_ACCENT,
                  max(2, S(2)));
    SelectObject(mdc, sub);
    SetRect(&r, 0, sv_L.top + S(70), w, sv_L.top + S(96));
    mswin_ms_text(mdc, "Go on with a saved game, or start a new hero.", &r,
                  DT_CENTER | DT_TOP | DT_SINGLELINE, TH_TEXT, 1);

    /* the cards */
    tdc = CreateCompatibleDC(hdc);
    otb = SelectObject(tdc, GetNHApp()->bmpMapTiles);
    for (i = 0; i < fxv_sv_n; i++) {
        const struct fxv_save *v = &fxv_sv[i];
        const char *badge = fxv_saves_mode_name(v->mode);
        boolean on = (i == sv_sel), hot = (i == sv_hotcard);
        int pad = S(12), ph;
        RECT c, p;

        if (sv_L.card[i][2] <= sv_L.card[i][0])
            continue; /* (scrolled out of view) */
        sv_rect(&c, sv_L.card[i]);
        ph = (c.bottom - c.top) - 2 * pad;
        cs_round(mdc, &c, hot ? TH_BG_HEADER : TH_BG_PANEL,
                 on ? TH_ACCENT : TH_SEPARATOR, on ? max(2, S(2)) : 1, 0);
        /* the portrait: the hero as on the map */
        SetRect(&p, c.left + pad, c.top + pad, c.left + pad + ph,
                c.top + pad + ph);
        cs_round(mdc, &p, TH_BG_APP, TH_SEPARATOR, 1, 0);
        if (v->tile >= 0 && GetNHApp()->bmpMapTiles) {
            int t = v->tile,
                tx = (t % GetNHApp()->mapTilesPerLine) * GetNHApp()->mapTile_X,
                ty = (t / GetNHApp()->mapTilesPerLine) * GetNHApp()->mapTile_Y;

            (*GetNHApp()->lpfnTransparentBlt)(
                mdc, p.left + S(4), p.top + S(4), ph - S(8), ph - S(8), tdc,
                tx, ty, GetNHApp()->mapTile_X, GetNHApp()->mapTile_Y,
                TILE_BK_COLOR);
        }
        SelectObject(mdc, name);
        SetRect(&r, p.right + S(14), c.top + S(12), c.right - S(10),
                c.top + S(44));
        mswin_ms_text(mdc, v->name, &r,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS,
                      on ? TH_ACCENT : TH_TEXT, 0);
        SelectObject(mdc, body);
        SetRect(&r, p.right + S(14), c.top + S(48), c.right - S(10),
                c.top + S(70));
        mswin_ms_text(mdc, *v->who ? v->who : "(an older save)", &r,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS,
                      TH_TEXT_DIM, 0);
        if (*badge) { /* how it was played */
            COLORREF bc = (v->mode == 'D') ? RGB(200, 120, 235)
                                           : RGB(242, 152, 62);
            SIZE sz;
            RECT b;

            GetTextExtentPoint32A(mdc, badge, (int) strlen(badge), &sz);
            SetRect(&b, c.right - S(10) - sz.cx - S(14), c.top + S(10),
                    c.right - S(10), c.top + S(32));
            cs_round(mdc, &b, TH_BG_HEADER, bc, 1, 0);
            mswin_ms_text(mdc, badge, &b,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE, bc, 0);
        }
    }
    SelectObject(tdc, otb);
    DeleteDC(tdc);

    /* the scroll bar, when they don't all fit */
    if (sv_L.track[2] > sv_L.track[0]) {
        RECT t;

        sv_rect(&t, sv_L.track);
        cs_round(mdc, &t, TH_BG_HEADER, TH_BG_HEADER, 1, 0);
        sv_rect(&t, sv_L.thumb);
        cs_round(mdc, &t, TH_TEXT_DIM, TH_TEXT_DIM, 1, 0);
    }
    /* more above or below */
    SelectObject(mdc, small);
    if (sv_L.first > 0
        || sv_L.first + sv_L.cols * sv_L.rows < fxv_sv_n
        || fxv_sv_total > fxv_sv_n) {
        char more[BUFSZ];

        fxv_saves_more(more);
        SetRect(&r, 0, sv_L.btn[0][1] - S(30), w, sv_L.btn[0][1] - S(8));
        mswin_ms_text(mdc, more, &r, DT_CENTER | DT_SINGLELINE,
                      TH_TEXT_DIM, 1);
    }
    for (i = 0; i < FXV_SVB_N; i++) {
        RECT b;

        sv_rect(&b, sv_L.btn[i]);
        mswin_ms_button(mdc, &b, sv_btn_label[i], i == FXV_SVB_PLAY,
                        !sv_confirm && sv_hot == i,
                        !sv_confirm && sv_fb == i, FALSE, TRUE, s);
    }
    SelectObject(mdc, small);
    cs_hints(mdc, w, h, s, sv_confirm
             ? "Y: delete   N or Esc: keep it"
             : "Arrows or mouse: choose   Enter: continue   N: new hero"
               "   Del: delete   Esc: back");
    mswin_ms_sound_rect(w, s, &cs_snd);
    mswin_ms_sound_icon(mdc, &cs_snd, sv_snd_hot, s);

    if (sv_confirm) { /* "Delete ... for good?" */
        HBITMAP one = CreateCompatibleBitmap(hdc, 1, 1);
        HDC odc = CreateCompatibleDC(hdc);
        HGDIOBJ oo = SelectObject(odc, one);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 140, 0 };
        char q[BUFSZ];
        RECT px, pr, b;

        SetRect(&px, 0, 0, 1, 1);
        FillRect(odc, &px, (HBRUSH) GetStockObject(BLACK_BRUSH));
        (void) GdiAlphaBlend(mdc, 0, 0, w, h, odc, 0, 0, 1, 1, bf);
        SelectObject(odc, oo);
        DeleteObject(one);
        DeleteDC(odc);
        sv_rect(&pr, sv_L.confirm);
        cs_round(mdc, &pr, RGB(20, 23, 29), TH_ACCENT, 1, 0);
        Snprintf(q, sizeof q, "Delete %s for good?", fxv_sv[sv_sel].name);
        SelectObject(mdc, name);
        SetRect(&r, pr.left, pr.top + S(22), pr.right, pr.top + S(54));
        mswin_ms_text(mdc, q, &r, DT_CENTER | DT_SINGLELINE, TH_TEXT, 0);
        SelectObject(mdc, body);
        SetRect(&r, pr.left, pr.top + S(60), pr.right, pr.top + S(84));
        mswin_ms_text(mdc, "The saved game is gone; it can't be brought "
                           "back.", &r, DT_CENTER | DT_SINGLELINE,
                      TH_TEXT_DIM, 0);
        sv_rect(&b, sv_L.cbtn[FXV_SVC_CANCEL]);
        mswin_ms_button(mdc, &b, "Keep it", FALSE,
                        sv_chot == FXV_SVC_CANCEL,
                        sv_cfocus == FXV_SVC_CANCEL, FALSE, TRUE, s);
        sv_rect(&b, sv_L.cbtn[FXV_SVC_DELETE]);
        mswin_ms_button(mdc, &b, "Delete", TRUE, sv_chot == FXV_SVC_DELETE,
                        sv_cfocus == FXV_SVC_DELETE, FALSE, TRUE, s);
    }

    SelectObject(mdc, of);
    BitBlt(hdc, 0, 0, w, h, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, omb);
    DeleteObject(mbm);
    DeleteDC(mdc);
    DeleteObject(big);
    DeleteObject(sub);
    DeleteObject(name);
    DeleteObject(body);
    DeleteObject(small);
}

#undef S

static void
sv_press(int b)
{
    switch (b) {
    case FXV_SVB_BACK:
        sv_result = -1;
        break;
    case FXV_SVB_NEW:
        sv_result = 0;
        break;
    case FXV_SVB_PLAY:
        if (fxv_sv_n) {
            fxv_saves_choose(sv_sel);
            sv_result = 1;
        }
        break;
    case FXV_SVB_DELETE:
        if (fxv_sv_n)
            sv_confirm = TRUE, sv_cfocus = FXV_SVC_CANCEL, sv_chot = -1;
        break;
    }
    if (sv_hwnd)
        InvalidateRect(sv_hwnd, NULL, FALSE);
}

static void
sv_answer(boolean yes)
{
    sv_confirm = FALSE;
    if (yes) {
        fxv_saves_delete(sv_sel);
        sv_sel = min(sv_sel, max(0, fxv_sv_n - 1));
        if (!fxv_sv_n)
            sv_result = 0; /* none left: on to a new hero */
    }
    if (sv_hwnd)
        InvalidateRect(sv_hwnd, NULL, FALSE);
}

static void
sv_key(WPARAM vk)
{
    int cols = max(1, sv_L.cols);

    if (sv_confirm) {
        if (vk == 'Y')
            sv_answer(TRUE);
        else if (vk == 'N' || vk == VK_ESCAPE)
            sv_answer(FALSE);
        else if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_TAB)
            sv_cfocus = !sv_cfocus;
        else if (vk == VK_RETURN || vk == VK_SPACE)
            sv_answer(sv_cfocus == FXV_SVC_DELETE);
        InvalidateRect(sv_hwnd, NULL, FALSE);
        return;
    }
    switch (vk) {
    case VK_ESCAPE:
    case VK_BACK:
        sv_press(FXV_SVB_BACK);
        return;
    case 'N':
        sv_press(FXV_SVB_NEW);
        return;
    case VK_DELETE:
    case 'D':
        sv_press(FXV_SVB_DELETE);
        return;
    case 'M':
        if (GetKeyState(VK_CONTROL) & 0x8000)
            mswin_menu_music_toggle();
        break;
    case VK_RETURN:
    case VK_SPACE:
        sv_press(sv_fb >= 0 ? sv_fb : FXV_SVB_PLAY);
        return;
    case VK_TAB: /* the cards, then the buttons from Continue back */
        sv_fb = (sv_fb < 0) ? FXV_SVB_PLAY : sv_fb - 1;
        break;
    case VK_LEFT:
        if (sv_fb >= 0)
            sv_fb = max(0, sv_fb - 1);
        else
            sv_sel = max(0, sv_sel - 1);
        break;
    case VK_RIGHT:
        if (sv_fb >= 0)
            sv_fb = min(FXV_SVB_N - 1, sv_fb + 1);
        else
            sv_sel = min(fxv_sv_n - 1, sv_sel + 1);
        break;
    case VK_UP:
        if (sv_fb >= 0)
            sv_fb = -1;
        else if (sv_sel >= cols)
            sv_sel -= cols;
        break;
    case VK_DOWN:
        if (sv_fb < 0 && sv_sel + cols < fxv_sv_n)
            sv_sel += cols;
        else if (sv_fb < 0)
            sv_fb = FXV_SVB_PLAY;
        break;
    case VK_HOME:
        sv_sel = 0, sv_fb = -1;
        break;
    case VK_END:
        sv_sel = fxv_sv_n - 1, sv_fb = -1;
        break;
    default:
        return;
    }
    InvalidateRect(sv_hwnd, NULL, FALSE);
}

#define SV_IN(q, x, y) ((x) >= (q)[0] && (x) < (q)[2] && (y) >= (q)[1] \
                        && (y) < (q)[3])

static LRESULT CALLBACK
sv_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    int x, y, i;

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        sv_paint(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int hot = -1, hotcard = -1, chot = -1;
        boolean snd;
        POINT pt;

        x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        pt.x = x, pt.y = y;
        snd = PtInRect(&cs_snd, pt);
        if (sv_confirm) {
            for (i = 0; i < 2; i++)
                if (SV_IN(sv_L.cbtn[i], x, y))
                    chot = i;
        } else {
            for (i = 0; i < FXV_SVB_N; i++)
                if (SV_IN(sv_L.btn[i], x, y))
                    hot = i;
            for (i = 0; i < fxv_sv_n; i++)
                if (SV_IN(sv_L.card[i], x, y))
                    hotcard = i;
        }
        if (hot != sv_hot || hotcard != sv_hotcard || chot != sv_chot
            || snd != sv_snd_hot) {
            TRACKMOUSEEVENT tme;

            sv_hot = hot, sv_hotcard = hotcard, sv_chot = chot;
            sv_snd_hot = snd;
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
        sv_hot = sv_hotcard = sv_chot = -1, sv_snd_hot = FALSE;
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, (sv_hot >= 0 || sv_hotcard >= 0
                                        || sv_chot >= 0 || sv_snd_hot)
                                           ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(hWnd);
        x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        if (sv_confirm) {
            for (i = 0; i < 2; i++)
                if (SV_IN(sv_L.cbtn[i], x, y))
                    sv_answer(i == FXV_SVC_DELETE);
            return 0;
        }
        if (sv_snd_hot) {
            mswin_menu_music_toggle();
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        for (i = 0; i < FXV_SVB_N; i++)
            if (SV_IN(sv_L.btn[i], x, y)) {
                sv_fb = i;
                sv_press(i);
                return 0;
            }
        for (i = 0; i < fxv_sv_n; i++)
            if (SV_IN(sv_L.card[i], x, y)) {
                sv_fb = -1;
                if (msg == WM_LBUTTONDBLCLK && i == sv_sel)
                    sv_press(FXV_SVB_PLAY); /* a double click: go on */
                sv_sel = i;
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
        return 0;
    case WM_MOUSEWHEEL:
        if (!sv_confirm) {
            int cols = max(1, sv_L.cols);

            sv_sel += (GET_WHEEL_DELTA_WPARAM(wParam) > 0) ? -cols : cols;
            sv_sel = max(0, min(sv_sel, fxv_sv_n - 1));
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        sv_key(wParam);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

#undef SV_IN

int
mswin_saves_screen(void)
{
    static boolean registered;
    HWND main = GetNHApp()->hMainWnd, bar;
    MSG msg;
    RECT rc;

    if (!registered) {
        WNDCLASS wc;

        ZeroMemory(&wc, sizeof wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = sv_proc;
        wc.hInstance = GetNHApp()->hApp;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = TEXT("MSNHSavesScreen");
        if (!RegisterClass(&wc))
            return 0;
        registered = TRUE;
    }
    if (!main)
        return 0;
    sv_result = -2;
    sv_sel = 0, sv_fb = -1;
    sv_hot = sv_hotcard = sv_chot = -1;
    sv_snd_hot = sv_confirm = FALSE;

    bar = FindWindowEx(main, NULL, TEXT("MSNHTopBar"), NULL);
    if (bar)
        ShowWindow(bar, SW_HIDE);
    GetClientRect(main, &rc);
    sv_hwnd = CreateWindow(TEXT("MSNHSavesScreen"), NULL,
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                           rc.right, rc.bottom, main, NULL,
                           GetNHApp()->hApp, NULL);
    if (!sv_hwnd) {
        if (bar)
            ShowWindow(bar, SW_SHOW);
        return 0;
    }
    SetWindowPos(sv_hwnd, HWND_TOP, 0, 0, rc.right, rc.bottom, 0);
    GetNHApp()->hPopupWnd = sv_hwnd; /* the main window sends focus here */
    SetForegroundWindow(main);
    SetFocus(sv_hwnd);

    while (sv_result == -2 && GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GetNHApp()->hPopupWnd = NULL;
    DestroyWindow(sv_hwnd);
    sv_hwnd = NULL;
    mswin_ms_backdrop_free();
    if (bar)
        ShowWindow(bar, SW_SHOW);
    SetFocus(main);
    return sv_result == -2 ? -1 : sv_result;
}
