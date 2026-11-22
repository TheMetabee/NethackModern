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

PNHWinApp GetNHApp(void);

#define CS_NAME 0 /* asking for a name */
#define CS_CHAR 1 /* choosing role, race, gender, alignment */

/* the groups of choices, then the other things that can have focus */
#define G_ROLE 0
#define G_RACE 1
#define G_GEND 2
#define G_ALIGN 3
#define G_NGRP 4
#define G_BTN 4 /* the row of buttons */
#define G_BOX 5 /* the name box */

#define B_BACK 0
#define B_RANDOM 1
#define B_PLAY 2
#define B_N 3

#define MAXROWS 32

static HWND cs_hwnd;
static int cs_mode;
static int cs_result = -1;      /* 1 go on, 0 back; -1 while waiting */
static int cs_fg = G_ROLE;      /* the focus: a group, G_BTN or G_BOX */
static int cs_fb = B_PLAY;      /* ... the button, when it's G_BTN */
static int cs_lastgrp = G_ROLE; /* where Up from the buttons goes */
static int cs_lastmid = G_RACE; /* where Right from the roles goes */
static int cs_hg = -1, cs_hi = -1; /* what's under the mouse */
static int cs_dg = -1, cs_di = -1; /* what the mouse is pressing */
static int cs_n[G_NGRP];           /* how many choices in each group */
static RECT cs_row[G_NGRP][MAXROWS], cs_panel[G_NGRP], cs_info;
static RECT cs_btn[B_N], cs_box;
static char cs_name[PL_NSIZ];
static boolean cs_caret = TRUE;
static void (*cs_rnd)(void *);
static void *cs_rnd_arg;

static const char *const cs_btn_label[2][B_N] = {
    { "Back", "", "Continue" },
    { "Back", "Random", "Play" },
};

/* a few words on each role, by its file code */
static const struct {
    const char *code, *text;
} cs_blurb[] = {
    { "Arc", "A scholar of ruins and lost places: quick on foot, "
             "sharp-eyed for traps, and never without a pick-axe." },
    { "Bar", "A warrior from the wild lands: strong, hardy, resistant "
             "to poison, and deadly with a two-handed sword or axe." },
    { "Cav", "A primitive brawler armed with a club and a sling, "
             "tough as old leather, with a loyal dog at heel." },
    { "Hea", "A physician with healing spells, a stethoscope and a "
             "full purse, but little skill at arms. A slow start." },
    { "Kni", "A mounted warrior on a pony, with lance and long sword, "
             "bound by a strict code of honor." },
    { "Mon", "A master of the martial arts who fights barehanded, "
             "moves fast, and is best without armor." },
    { "Pri", "A servant of the gods who sees at a glance whether things "
             "are blessed or cursed, and starts with clerical spells." },
    { "Rog", "A thief and knife-thrower who prefers to strike from the "
             "shadows, and is at home among locks and traps." },
    { "Ran", "A wilderness scout and expert archer, cloaked so that "
             "foes are never quite sure where to strike." },
    { "Sam", "A disciplined warrior of the East: fast, well armored, "
             "and deadly with the katana." },
    { "Tou", "A sightseer with a camera, a loud shirt and plenty of "
             "gold. Weak and naive: the hardest way to play." },
    { "Val", "A shieldmaiden of the north, stealthy, resistant to cold, "
             "and strong. A good first hero." },
    { "Wiz", "A student of the arcane with a magic quarterstaff, a "
             "cloak, and spells, wands and rings to begin with." },
};

/*
 * the rules
 */

static int
cs_value(int g)
{
    switch (g) {
    case G_ROLE:
        return flags.initrole;
    case G_RACE:
        return flags.initrace;
    case G_GEND:
        return flags.initgend;
    default:
        return flags.initalign;
    }
}

static void
cs_set(int g, int v)
{
    switch (g) {
    case G_ROLE:
        flags.initrole = v;
        break;
    case G_RACE:
        flags.initrace = v;
        break;
    case G_GEND:
        flags.initgend = v;
        break;
    default:
        flags.initalign = v;
        break;
    }
}

/* can choice i of group g go with the choices before it? */
static boolean
cs_ok(int g, int i)
{
    if (i < 0 || i >= cs_n[g])
        return FALSE;
    switch (g) {
    case G_ROLE:
        return ok_role(i, ROLE_RANDOM, ROLE_RANDOM, ROLE_RANDOM);
    case G_RACE:
        return ok_race(flags.initrole, i, ROLE_RANDOM, ROLE_RANDOM);
    case G_GEND:
        return ok_gend(flags.initrole, flags.initrace, i, ROLE_RANDOM);
    default:
        return ok_align(flags.initrole, flags.initrace, flags.initgend, i);
    }
}

/* after a change, move any later choice that no longer fits */
static void
cs_fix(void)
{
    int g, i;

    for (g = G_RACE; g < G_NGRP; g++) {
        if (cs_ok(g, cs_value(g)))
            continue;
        for (i = 0; i < cs_n[g]; i++)
            if (cs_ok(g, i))
                break;
        if (i < cs_n[g]) {
            cs_set(g, i);
        } else { /* shouldn't happen; let the game pick */
            switch (g) {
            case G_RACE:
                flags.initrace = pick_race(flags.initrole, ROLE_RANDOM,
                                           ROLE_RANDOM, PICK_RANDOM);
                break;
            case G_GEND:
                flags.initgend = pick_gend(flags.initrole, flags.initrace,
                                           ROLE_RANDOM, PICK_RANDOM);
                break;
            default:
                flags.initalign = pick_align(flags.initrole, flags.initrace,
                                             flags.initgend, PICK_RANDOM);
                break;
            }
        }
    }
}

static boolean
cs_pick(int g, int i)
{
    if (!cs_ok(g, i))
        return FALSE;
    cs_set(g, i);
    cs_fix();
    return TRUE;
}

static const char *
cs_role_name(int r)
{
    return (flags.initgend == 1 && roles[r].name.f) ? roles[r].name.f
                                                     : roles[r].name.m;
}

static const char *
cs_label(int g, int i, char *buf)
{
    const char *s;

    switch (g) {
    case G_ROLE:
        s = cs_role_name(i);
        break;
    case G_RACE:
        s = races[i].noun;
        break;
    case G_GEND:
        s = genders[i].adj;
        break;
    default:
        s = aligns[i].adj;
        break;
    }
    Strcpy(buf, s);
    buf[0] = highc(buf[0]);
    return buf;
}

/* why choice i of group g can't be had, for the info panel */
static void
cs_why_not(int g, int i, char *buf)
{
    char who[BUFSZ];

    Strcpy(who, makeplural(cs_role_name(flags.initrole)));
    switch (g) {
    case G_RACE:
        Sprintf(buf, "%s can't be %s.", who, makeplural(races[i].noun));
        break;
    case G_GEND:
        Sprintf(buf, "%s can't be %s.", who, genders[i].adj);
        break;
    case G_ALIGN:
        Sprintf(buf, "%s %s can't be %s.", races[flags.initrace].adj, who,
                aligns[i].adj);
        buf[0] = highc(buf[0]);
        break;
    default:
        buf[0] = '\0';
        break;
    }
}

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

/* the next choice that fits, from the current one, or -1 */
static int
cs_step(int g, int from, int dir)
{
    int i;

    for (i = from + dir; i >= 0 && i < cs_n[g]; i += dir)
        if (cs_ok(g, i))
            return i;
    return -1;
}

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
 * where things go; sizes are for a 740-pixel-high window, scaled
 */

static double
cs_scale(int w, int h)
{
    double s = min(h / 740.0, w / 960.0);

    return s < 0.5 ? 0.5 : s > 2.2 ? 2.2 : s;
}

#define S(v) ((int) ((v) * s + 0.5))

static void
cs_layout_buttons(int w, int top, double s)
{
    int bw = S(150), pw = S(cs_mode == CS_NAME ? 254 : 220), bh = S(46),
        gap = S(16), x;

    if (cs_mode == CS_NAME) {
        x = (w - (bw + gap + pw)) / 2;
        SetRect(&cs_btn[B_BACK], x, top, x + bw, top + bh);
        SetRectEmpty(&cs_btn[B_RANDOM]);
        x += bw + gap;
        SetRect(&cs_btn[B_PLAY], x, top, x + pw, top + bh);
    } else {
        x = (w - (2 * bw + 2 * gap + pw)) / 2;
        SetRect(&cs_btn[B_BACK], x, top, x + bw, top + bh);
        x += bw + gap;
        SetRect(&cs_btn[B_RANDOM], x, top, x + bw, top + bh);
        x += bw + gap;
        SetRect(&cs_btn[B_PLAY], x, top, x + pw, top + bh);
    }
}

/* the character screen: roles on the left; race, gender and alignment
   stacked in the middle; about the role on the right; buttons below */
static void
cs_layout_char(int w, int h, double s, int *ptop)
{
    int x0 = (w - S(900)) / 2, top = max(S(12), (h - S(710)) / 2);
    int ptop0 = top + S(112), hh = S(38), rh = S(30), gap = S(16);
    int g, i, y, colh = 0;

    *ptop = top;
    /* the middle column, top to bottom */
    y = ptop0;
    for (g = G_RACE; g < G_NGRP; g++) {
        int ph = hh + cs_n[g] * rh + S(10);

        SetRect(&cs_panel[g], x0 + S(272), y, x0 + S(482), y + ph);
        y += ph + gap;
    }
    colh = y - gap - ptop0;
    SetRect(&cs_panel[G_ROLE], x0, ptop0, x0 + S(250),
            ptop0 + max(colh, hh + cs_n[G_ROLE] * rh + S(10)));
    colh = cs_panel[G_ROLE].bottom - ptop0;
    SetRect(&cs_info, x0 + S(504), ptop0, x0 + S(900), ptop0 + colh);

    for (g = 0; g < G_NGRP; g++)
        for (i = 0; i < cs_n[g] && i < MAXROWS; i++) {
            RECT *p = &cs_panel[g];

            SetRect(&cs_row[g][i], p->left + S(8), p->top + hh + i * rh,
                    p->right - S(8), p->top + hh + (i + 1) * rh - S(2));
        }
    cs_layout_buttons(w, ptop0 + colh + S(26), s);
}

/* the name screen: a heading, the box, and buttons, low in the window
   like the title screen's */
static void
cs_layout_name(int w, int h, double s, int *ptop)
{
    int bw = S(420), bh = S(56);
    int top = (int) (h * 0.50);

    if (top + S(300) > h)
        top = h - S(300);
    *ptop = top;
    SetRect(&cs_box, (w - bw) / 2, top + S(96), (w + bw) / 2,
            top + S(96) + bh);
    cs_layout_buttons(w, cs_box.bottom + S(54), s);
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

    RoundRect(hdc, r->left, r->top, r->right, r->bottom, rad, rad);
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

    cs_layout_name(w, h, s, &top);

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
             "Type a name        Enter: continue        Esc: back");

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
    const struct Role *R = &roles[rl];
    boolean fem = (flags.initgend == 1);
    char buf[BUFSZ];
    const char *blurb = NULL;
    int i, n;

    cs_round(hdc, &r, TH_BG_PANEL, TH_SEPARATOR, 1, S(14));
    r.left += S(22), r.right -= S(22), r.top += S(16);

    SelectObject(hdc, name);
    line = r;
    line.bottom = line.top + S(42);
    SetTextColor(hdc, TH_ACCENT);
    DrawTextA(hdc, cs_role_name(rl), -1, &line,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    r.top = line.bottom;

    SelectObject(hdc, body);
    Sprintf(buf, "Starts out as %s",
            an((fem && R->rank[0].f) ? R->rank[0].f : R->rank[0].m));
    cs_para(hdc, buf, &r, TH_TEXT_DIM);
    r.top += S(12);

    for (i = 0; i < (int) SIZE(cs_blurb); i++)
        if (!strcmp(cs_blurb[i].code, R->filecode))
            blurb = cs_blurb[i].text;
    if (blurb) {
        cs_para(hdc, blurb, &r, TH_TEXT);
        r.top += S(16);
    }

    /* what the role allows, from the game's own rules */
    SelectObject(hdc, head);
    cs_para(hdc, "Races", &r, TH_ACCENT);
    SelectObject(hdc, body);
    buf[0] = '\0';
    for (i = n = 0; i < cs_n[G_RACE]; i++)
        if (ok_race(rl, i, ROLE_RANDOM, ROLE_RANDOM)) {
            if (n++)
                Strcat(buf, ", ");
            Strcat(buf, races[i].noun);
        }
    cs_para(hdc, buf, &r, TH_TEXT);
    r.top += S(8);

    SelectObject(hdc, head);
    cs_para(hdc, "Alignments", &r, TH_ACCENT);
    SelectObject(hdc, body);
    buf[0] = '\0';
    for (i = n = 0; i < cs_n[G_ALIGN]; i++)
        if (ok_align(rl, ROLE_RANDOM, ROLE_RANDOM, i)) {
            if (n++)
                Strcat(buf, ", ");
            Strcat(buf, aligns[i].adj);
        }
    cs_para(hdc, buf, &r, TH_TEXT);
    if (ok_gend(rl, ROLE_RANDOM, 0, ROLE_RANDOM)
        != ok_gend(rl, ROLE_RANDOM, 1, ROLE_RANDOM)) {
        Sprintf(buf, "Always %s",
                genders[ok_gend(rl, ROLE_RANDOM, 1, ROLE_RANDOM)].adj);
        cs_para(hdc, buf, &r, TH_TEXT_DIM);
    }
    r.top += S(8);

    SelectObject(hdc, head);
    cs_para(hdc, "Gods", &r, TH_ACCENT);
    SelectObject(hdc, body);
    if (R->lgod) {
#define GOD(g) ((g)[0] == '_' ? (g) + 1 : (g))
        Sprintf(buf, "%s (lawful), %s (neutral), %s (chaotic)",
                GOD(R->lgod), GOD(R->ngod), GOD(R->cgod));
#undef GOD
    } else {
        Strcpy(buf, "Another role's pantheon, chosen at random");
    }
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
    const struct Role *R = &roles[flags.initrole];
    boolean fem = (flags.initgend == 1);

    cs_layout_char(w, h, s, &top);

    of = SelectObject(hdc, big);
    SetRect(&r, 0, top, w, top + S(62));
    mswin_ms_text(hdc, "Create your hero", &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE, TH_ACCENT,
                  max(2, S(3)));
    /* the hero as chosen so far */
    SelectObject(hdc, sub);
    Sprintf(buf, "%s the %s, %s %s %s %s", svp.plname,
            (fem && R->rank[0].f) ? R->rank[0].f : R->rank[0].m,
            aligns[flags.initalign].adj, genders[flags.initgend].adj,
            races[flags.initrace].adj, cs_role_name(flags.initrole));
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
             "Arrows or mouse: choose        Tab: next        "
             "Enter: play        *: random        Esc: back");

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
    int g = (cs_fg < G_NGRP) ? cs_fg : G_ROLE, i, k;
    char buf[BUFSZ];

    for (k = 1; k <= cs_n[g]; k++) {
        i = (cs_value(g) + k) % cs_n[g];
        if (cs_ok(g, i) && lowc(*cs_label(g, i, buf)) == c) {
            cs_pick(g, i);
            cs_focus_group(g);
            InvalidateRect(cs_hwnd, NULL, FALSE);
            return;
        }
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
            else if (cs_hg == G_BTN
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
        } else if (cs_dg == G_BTN) {
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
    RECT rc;

    if (!cs_hwnd || !IsWindow(cs_hwnd))
        return;
    GetClientRect(main, &rc);
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
    int i;

    for (i = 0; roles[i].name.m && i < MAXROWS; i++)
        continue;
    cs_n[G_ROLE] = i;
    for (i = 0; races[i].noun && i < MAXROWS; i++)
        continue;
    cs_n[G_RACE] = i;
    cs_n[G_GEND] = ROLE_GENDERS;
    cs_n[G_ALIGN] = ROLE_ALIGNS;
    cs_rnd = randomize;
    cs_rnd_arg = arg;
    cs_fix(); /* should already fit; make sure */

    return cs_run(CS_CHAR);
}
