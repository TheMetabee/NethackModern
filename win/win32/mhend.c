/* NetHack Modern: the death screen and the score list.
 * NetHack may be freely redistributed.  See license for details.
 *
 * These take the place of the tombstone dialog and the plain text score
 * list.  Like the title and character screens they fill the game
 * window and work with the mouse or the keyboard alone.
 *
 * The death screen shows a tombstone, the messages of the hero's last
 * turn (taken as the hero dies, before the end-of-game questions) and
 * how far the hero got, over the title art drained of its colour.
 *
 * The score list is built from the lines topten() writes, so the core
 * is left alone: each entry's line is taken apart into its rank,
 * points, hero, fate and hit points, and drawn as a row of a table.
 */

#include "win10.h"
#include "winMS.h"
#include "hack.h"
#include "mhsplash.h"
#include "mhmsgwnd.h"
#include "mhtheme.h"
#include "mhend.h"

PNHWinApp GetNHApp(void);

#define ES_DEATH 0
#define ES_SCORES 1

#define BONE RGB(232, 226, 212)       /* the death screen's pale text */
#define FAINT RGB(104, 110, 122)      /* dimmer than TH_TEXT_DIM */

static HWND es_hwnd;
static int es_mode;
static boolean es_done;
static boolean es_hot, es_down; /* the mouse over / pressing the button */
static RECT es_btn;

/*
 * the hero's last messages
 */

#define LASTN 4
static char es_last[LASTN][BUFSZ];
static int es_nlast;

static void
es_strip(char *s, const char *what)
{
    char *p;
    size_t n = strlen(what);

    while ((p = strstri(s, what)) != 0)
        memmove(p, p + n, strlen(p + n) + 1);
}

void
mswin_end_snapshot(void)
{
    char buf[BUFSZ], got[LASTN][BUFSZ];
    COLORREF c;
    long turn, t0 = 0L;
    boolean recent, scrolled;
    int back, n = 0, i;

    for (back = 0; back < 16 && n < LASTN; back++) {
        if (!mswin_msg_overlay_line(back, buf, sizeof buf, &c, &turn,
                                    &recent, &scrolled))
            break;
        if (back == 0)
            t0 = turn;
        else if (turn != t0)
            break; /* only the last turn's */
        es_strip(buf, "--More--");
        (void) mungspaces(buf);
        if (!*buf || !strncmp(buf, "Die?", 4))
            continue; /* wizard mode's question */
        Strcpy(got[n++], buf);
    }
    for (i = 0; i < n; i++)
        Strcpy(es_last[i], got[n - 1 - i]); /* oldest first */
    es_nlast = n;
}

/*
 * what the death screen shows, gathered when it opens
 */

static char es_title[BUFSZ], es_cause[BUFSZ], es_stone[4][BUFSZ];
static int es_nstone;
static struct {
    const char *label;
    char value[32];
} es_stat[6];

static void
es_lines(const char *text, char out[][BUFSZ], int max, int *n)
{
    const char *p = text, *e;

    *n = 0;
    while (p && *p && *n < max) {
        size_t len;

        e = strchr(p, '\n');
        len = e ? (size_t) (e - p) : strlen(p);
        if (len >= BUFSZ)
            len = BUFSZ - 1;
        memcpy(out[*n], p, len);
        out[*n][len] = '\0';
        while (len && (out[*n][len - 1] == '\r' || out[*n][len - 1] == ' '))
            out[*n][--len] = '\0';
        (*n)++;
        p = e ? e + 1 : 0;
    }
}

static void
es_gather(const char *rip, const char *summary)
{
    char lines[8][BUFSZ];
    int n, i;
    char where[BUFSZ];

    /* the tombstone: name, gold, cause, year */
    es_nstone = 0;
    if (rip)
        es_lines(rip, es_stone, 4, &es_nstone);

    /* the heading: the game's own farewell ("Goodbye ... the Wizard") */
    es_title[0] = '\0';
    if (summary) {
        es_lines(summary, lines, 8, &n);
        for (i = 0; i < n; i++)
            if (lines[i][0]) {
                Strcpy(es_title, lines[i]);
                break;
            }
    }
    if (!es_title[0])
        Sprintf(es_title, "Goodbye %s...", svp.plname);

    /* what killed the hero, and where */
    if (In_endgame(&u.uz))
        Strcpy(where, "on the Elemental Planes");
    else if (Is_knox(&u.uz))
        Sprintf(where, "in %s", svd.dungeons[u.uz.dnum].dname);
    else
        Sprintf(where, "on level %d of %s", depth(&u.uz),
                svd.dungeons[u.uz.dnum].dname);
    if (es_nstone >= 3 && es_stone[2][0])
        Sprintf(es_cause, "%c%s, %s", highc(es_stone[2][0]),
                es_stone[2] + 1, where);
    else
        Sprintf(es_cause, "Died %s", where);

    es_stat[0].label = "POINTS";
    Sprintf(es_stat[0].value, "%ld", (long) u.urexp);
    es_stat[1].label = "GOLD";
    Sprintf(es_stat[1].value, "%ld", gd.done_money);
    es_stat[2].label = "TURNS";
    Sprintf(es_stat[2].value, "%ld", svm.moves);
    es_stat[3].label = "EXPERIENCE LEVEL";
    Sprintf(es_stat[3].value, "%d", u.ulevel);
    es_stat[4].label = "MAXIMUM HP";
    Sprintf(es_stat[4].value, "%d", u.uhpmax);
    es_stat[5].label = "DEEPEST LEVEL";
    Sprintf(es_stat[5].value, "%d", deepest_lev_reached(FALSE));
}

/*
 * the score list, from topten()'s lines
 */

#define MAXLINES 400
#define MAXENTRIES 120

static struct {
    int attr;
    char *text;
} sl_line[MAXLINES];
static int sl_nline;

static struct sl_entry {
    int rank;                 /* 0: the hero's, when not on the list */
    long points;
    char name[PL_NSIZ];
    char who[BUFSZ];          /* "chaotic female human Wizard" */
    char fate[BUFSZ];         /* "Killed by a jackal" */
    char place[BUFSZ];        /* "Died in The Dungeons of Doom on level 2" */
    char hp[16];
    int maxhp;
    boolean hero;
} sl_ent[MAXENTRIES];
static int sl_nent;
static char sl_note[BUFSZ];   /* "You made the top ten list!" and such */
static int sl_top;            /* the first row showing */
static int sl_fit = 1;        /* how many rows fit */

void
mswin_scores_add(int attr, const char *text)
{
    if (sl_nline >= MAXLINES || !text)
        return;
    sl_line[sl_nline].attr = attr;
    sl_line[sl_nline].text = dupstr(text);
    sl_nline++;
}

void
mswin_scores_clear(void)
{
    int i;

    for (i = 0; i < sl_nline; i++)
        free(sl_line[i].text);
    sl_nline = 0;
    sl_nent = 0;
    sl_note[0] = '\0';
}

/* is this the first line of an entry: rank (or blanks), then points? */
static boolean
sl_starts_entry(const char *s)
{
    int i;
    boolean seen = FALSE;

    if (strlen(s) < 17)
        return FALSE;
    for (i = 0; i < 3; i++)
        if (s[i] != ' ' && !digit(s[i]))
            return FALSE;
    for (i = 4; i < 14; i++) {
        if (digit(s[i]))
            seen = TRUE;
        else if (s[i] != ' ')
            return FALSE;
    }
    return seen && s[14] == ' ' && s[15] == ' ' && s[16] != ' ';
}

static boolean
sl_continues(const char *s)
{
    int i;

    for (i = 0; i < 16; i++)
        if (s[i] != ' ')
            return FALSE;
    return s[16] && s[16] != ' ';
}

/* "Wiz", "Hum", "Fem", "Cha" into "chaotic female human Wizard" */
static void
sl_who(struct sl_entry *e, char *code)
{
    char *part[8], *p;
    int np = 0, i, k;
    const char *role = 0, *race = 0, *gend = 0, *align = 0;
    boolean fem = FALSE;

    /* the name can hold dashes; the codes are the last parts */
    for (p = code; np < 8; ) {
        part[np++] = p;
        if ((p = strchr(p, '-')) == 0)
            break;
        *p++ = '\0';
    }
    k = np;
    while (k > 1) {
        const char *c = part[k - 1];
        boolean took = FALSE;

        if (!align)
            for (i = 0; i < ROLE_ALIGNS; i++)
                if (!strcmpi(c, aligns[i].filecode))
                    align = aligns[i].adj, took = TRUE;
        if (!took && !gend)
            for (i = 0; i < ROLE_GENDERS; i++)
                if (!strcmpi(c, genders[i].filecode))
                    gend = genders[i].adj, fem = (i == 1), took = TRUE;
        if (!took && !race)
            for (i = 0; races[i].noun; i++)
                if (!strcmpi(c, races[i].filecode))
                    race = races[i].adj, took = TRUE;
        if (!took && !role)
            for (i = 0; roles[i].name.m; i++)
                if (!strcmpi(c, roles[i].filecode))
                    role = c, took = TRUE;
        if (!took)
            break;
        k--;
    }
    /* what's left is the name */
    e->name[0] = '\0';
    for (i = 0; i < k; i++) {
        if (i)
            (void) strncat(e->name, "-", sizeof e->name - strlen(e->name) - 1);
        (void) strncat(e->name, part[i], sizeof e->name - strlen(e->name) - 1);
    }
    e->who[0] = '\0';
    if (align)
        Sprintf(eos(e->who), "%s ", align);
    if (gend)
        Sprintf(eos(e->who), "%s ", gend);
    if (race)
        Sprintf(eos(e->who), "%s ", race);
    if (role) {
        for (i = 0; roles[i].name.m; i++)
            if (!strcmpi(role, roles[i].filecode)) {
                Strcat(e->who, (fem && roles[i].name.f) ? roles[i].name.f
                                                        : roles[i].name.m);
                break;
            }
    }
    (void) mungspaces(e->who);
}

static void
sl_cap(char *s)
{
    size_t n = strlen(s);

    while (n && (s[n - 1] == '.' || s[n - 1] == ' '))
        s[--n] = '\0';
    if (*s)
        *s = highc(*s);
}

/* take apart one entry: 'first' is its first line, 'rest' the text of
   its other lines */
static void
sl_entry(const char *first, const char *rest, boolean hero)
{
    struct sl_entry *e;
    char buf[BUFSZ * 3], *p, *q, *code;
    size_t n;

    if (sl_nent >= MAXENTRIES)
        return;
    e = &sl_ent[sl_nent];
    memset(e, 0, sizeof *e);
    e->hero = hero;
    e->rank = atoi(first);
    e->points = atol(first + 3);
    Snprintf(buf, sizeof buf, "%s%s%s", first + 16, *rest ? " " : "", rest);
    (void) mungspaces(buf);

    /* the hit points at the end: "12 [12]" or "- [12]" */
    n = strlen(buf);
    if (n && buf[n - 1] == ']' && (p = strrchr(buf, '[')) != 0) {
        for (q = p + 1; digit(*q); q++)
            continue;
        if (q > p + 1 && *q == ']') {
            e->maxhp = atoi(p + 1);
            q = p;
            while (q > buf && q[-1] == ' ')
                q--;
            *q = '\0';
            if ((p = strrchr(buf, ' ')) != 0
                && (!strcmp(p + 1, "-") || digit(p[1]))) {
                (void) strncpy(e->hp, p + 1, sizeof e->hp - 1);
                *p = '\0';
            } else {
                Strcpy(e->hp, "?");
            }
        }
    }
    if (!e->maxhp)
        Strcpy(e->hp, "");

    /* the hero: "test1-Wiz-Hum-Fem-Cha" */
    code = buf;
    if ((p = strchr(buf, ' ')) != 0)
        *p++ = '\0';
    else
        p = eos(buf);
    sl_who(e, code);

    /* the fate: "died in X on level 2.  Killed by a jackal." */
    q = strstr(p, ". ");
    if (q && q[2]) {
        *q = '\0';
        (void) strncpy(e->place, p, sizeof e->place - 1);
        (void) strncpy(e->fate, q + 2, sizeof e->fate - 1);
    } else {
        (void) strncpy(e->fate, p, sizeof e->fate - 1);
    }
    sl_cap(e->fate);
    sl_cap(e->place);
    sl_nent++;
}

static void
sl_parse(void)
{
    char rest[BUFSZ * 3];
    const char *first = 0;
    boolean hero = FALSE;
    int i;

    sl_nent = 0;
    sl_note[0] = rest[0] = '\0';
    for (i = 0; i <= sl_nline; i++) {
        const char *s = (i < sl_nline) ? sl_line[i].text : "";
        boolean bold = (i < sl_nline && sl_line[i].attr == ATR_BOLD);

        if (first && sl_continues(s)) {
            if (*rest)
                (void) strncat(rest, " ", sizeof rest - strlen(rest) - 1);
            (void) strncat(rest, s + 16, sizeof rest - strlen(rest) - 1);
            hero = hero || bold;
            continue;
        }
        if (first) { /* the entry before is complete */
            sl_entry(first, rest, hero);
            first = 0;
        }
        if (i == sl_nline)
            break;
        if (sl_starts_entry(s)) {
            first = s;
            rest[0] = '\0';
            hero = bold;
        } else if (!strncmp(s, " No  Points", 11)) {
            continue; /* the header; the table has its own */
        } else {
            char t[BUFSZ];

            (void) strncpy(t, s, sizeof t - 1);
            t[sizeof t - 1] = '\0';
            (void) mungspaces(t);
            if (*t) {
                if (*sl_note)
                    (void) strncat(sl_note, "  ",
                                   sizeof sl_note - strlen(sl_note) - 1);
                (void) strncat(sl_note, t,
                               sizeof sl_note - strlen(sl_note) - 1);
            }
        }
    }
}

/*
 * drawing
 */

static double
es_scale(int w, int h)
{
    double s = min(h / 740.0, w / 960.0);

    return s < 0.5 ? 0.5 : s > 2.2 ? 2.2 : s;
}

#define S(v) ((int) ((v) * s + 0.5))

static HFONT
es_font(int px, int weight, boolean serif, boolean italic)
{
    return CreateFontA(-px, 0, 0, 0, weight, italic, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       VARIABLE_PITCH | (serif ? FF_ROMAN : FF_SWISS),
                       serif ? "Palatino Linotype" : TH_FONT_UI);
}

static void
es_round(HDC hdc, const RECT *r, COLORREF fill, COLORREF edge, int pw,
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

static void
es_fill(HDC hdc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);

    FillRect(hdc, r, b);
    DeleteObject(b);
}

static void
es_draw(HDC hdc, const char *t, RECT *r, UINT how, COLORREF c)
{
    SetTextColor(hdc, c);
    DrawTextA(hdc, t, -1, r, how | DT_NOPREFIX);
}

/* a darkened layer over a rectangle: lets the art show through panels */
static void
es_shade(HDC hdc, const RECT *r, COLORREF c, int alpha)
{
    HDC one = CreateCompatibleDC(hdc);
    HBITMAP bm = CreateCompatibleBitmap(hdc, 1, 1);
    HGDIOBJ ob = SelectObject(one, bm);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE) alpha, 0 };
    RECT px;

    SetRect(&px, 0, 0, 1, 1);
    es_fill(one, &px, c);
    (void) GdiAlphaBlend(hdc, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, one, 0, 0, 1, 1, bf);
    SelectObject(one, ob);
    DeleteObject(bm);
    DeleteDC(one);
}

/* the backdrop: the title art; for the death screen drained of colour,
   cold and dark at the edges.  Kept until the size changes. */
static HBITMAP es_bg;
static int es_bgw, es_bgh, es_bgmode = -1;

static void
es_backdrop(HDC hdc, HDC dst, int w, int h)
{
    if (!es_bg || es_bgw != w || es_bgh != h || es_bgmode != es_mode) {
        BITMAPINFO bi;
        DWORD *px = 0;
        HDC bdc;
        HGDIOBJ ob;

        if (es_bg)
            DeleteObject(es_bg), es_bg = NULL;
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; /* top down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        es_bg = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void **) &px,
                                 NULL, 0);
        if (!es_bg) {
            mswin_ms_backdrop(hdc, dst, w, h, 170);
            return;
        }
        bdc = CreateCompatibleDC(hdc);
        ob = SelectObject(bdc, es_bg);
        mswin_ms_backdrop(hdc, bdc, w, h, es_mode == ES_DEATH ? 0 : 195);
        GdiFlush();
        if (es_mode == ES_DEATH && px) {
            double cx = w / 2.0, cy = h * 0.45;
            double rx = w * 0.62, ry = h * 0.70;
            int x, y;

            for (y = 0; y < h; y++) {
                double dy = (y - cy) / ry;

                for (x = 0; x < w; x++) {
                    DWORD v = px[(size_t) y * w + x];
                    int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
                    double dx = (x - cx) / rx, d = dx * dx + dy * dy;
                    double f = 0.40 * (1.0 - 0.75 * min(d, 1.0)), l;

                    l = (0.30 * r + 0.59 * g + 0.11 * b) * f;
                    r = (int) (l * 0.90);
                    g = (int) (l * 0.96);
                    b = (int) min(l * 1.10 + 4, 255.0);
                    px[(size_t) y * w + x] = ((DWORD) r << 16)
                                             | ((DWORD) g << 8) | (DWORD) b;
                }
            }
        }
        SelectObject(bdc, ob);
        DeleteDC(bdc);
        es_bgw = w, es_bgh = h, es_bgmode = es_mode;
    }
    {
        HDC bdc = CreateCompatibleDC(hdc);
        HGDIOBJ ob = SelectObject(bdc, es_bg);

        BitBlt(dst, 0, 0, w, h, bdc, 0, 0, SRCCOPY);
        SelectObject(bdc, ob);
        DeleteDC(bdc);
    }
}

/* text cut into the stone: dark, with a lit lower edge */
static void
es_carve(HDC hdc, const char *t, RECT *r, UINT how, double s)
{
    int k = max(1, S(1));

    OffsetRect(r, k, k);
    es_draw(hdc, t, r, how, RGB(178, 178, 174));
    OffsetRect(r, -k, -k);
    es_draw(hdc, t, r, how, RGB(46, 47, 50));
}

/* the tombstone, with the game's four lines carved in it */
static void
es_paint_stone(HDC hdc, const RECT *box, double s)
{
    int w = box->right - box->left, i;
    HRGN cap, body, stone;
    RECT r, mound;
    TRIVERTEX v[2];
    GRADIENT_RECT gr = { 0, 1 };
    HBRUSH edge;
    unsigned long seed = 0x2545F491UL;
    HFONT rip = es_font(S(66), FW_BOLD, TRUE, FALSE),
          name = es_font(S(27), FW_BOLD, TRUE, FALSE),
          line = es_font(S(19), FW_SEMIBOLD, TRUE, FALSE);
    HGDIOBJ of;

    /* the earth it stands in */
    SetRect(&mound, box->left - S(50), box->bottom - S(26),
            box->right + S(50), box->bottom + S(26));
    {
        HBRUSH b = CreateSolidBrush(RGB(14, 16, 15));
        HPEN p = CreatePen(PS_SOLID, 1, RGB(14, 16, 15));
        HGDIOBJ ob = SelectObject(hdc, b), op = SelectObject(hdc, p);

        Ellipse(hdc, mound.left, mound.top, mound.right, mound.bottom);
        SelectObject(hdc, op);
        SelectObject(hdc, ob);
        DeleteObject(p);
        DeleteObject(b);
    }

    /* the stone: a rounded head on a block */
    cap = CreateEllipticRgn(box->left, box->top, box->right, box->top + w);
    body = CreateRectRgn(box->left, box->top + w / 2, box->right,
                         box->bottom);
    stone = CreateRectRgn(0, 0, 1, 1);
    CombineRgn(stone, cap, body, RGN_OR);
    SelectClipRgn(hdc, stone);

    /* lit from the upper left */
    v[0].x = box->left, v[0].y = box->top;
    v[0].Red = 0xA400, v[0].Green = 0xA400, v[0].Blue = 0xA000, v[0].Alpha = 0;
    v[1].x = box->right, v[1].y = box->bottom;
    v[1].Red = 0x5C00, v[1].Green = 0x5E00, v[1].Blue = 0x6200, v[1].Alpha = 0;
    GdiGradientFill(hdc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);
    /* weathering: specks, and moss creeping up from the ground */
    for (i = 0; i < 520; i++) {
        int x, y, sz, g;
        COLORREF c;

        seed = seed * 1103515245UL + 12345UL;
        x = box->left + (int) ((seed >> 8) % (unsigned long) w);
        seed = seed * 1103515245UL + 12345UL;
        y = box->top + (int) ((seed >> 8)
                              % (unsigned long) (box->bottom - box->top));
        sz = max(1, S((seed >> 4) % 3 ? 1 : 2));
        if (i < 380) {
            /* just darker or lighter than the stone around it */
            g = (int) ((seed >> 12) % 24) + 8;
            c = ((seed >> 20) & 1) ? RGB(88 - g, 90 - g, 94 - g)
                                   : RGB(124 + g, 124 + g, 121 + g);
        } else {
            /* moss: low down, thicker toward the ground */
            unsigned long k = (seed >> 9) % (unsigned long) S(70);

            y = box->bottom - 1 - (int) (k * k / (unsigned long) S(70));
            c = RGB(70 + (int) (seed % 16), 88 + (int) (seed % 18), 60);
            sz = max(2, S(3));
        }
        SetRect(&r, x, y, x + sz, y + sz);
        es_fill(hdc, &r, c);
    }
    /* a crack */
    {
        HPEN p = CreatePen(PS_SOLID, max(1, S(1)), RGB(58, 59, 62));
        HGDIOBJ op = SelectObject(hdc, p);
        int x0 = box->right - w / 5, y0 = box->top + w / 3;

        MoveToEx(hdc, x0, y0, NULL);
        LineTo(hdc, x0 - S(9), y0 + S(26));
        LineTo(hdc, x0 - S(4), y0 + S(44));
        LineTo(hdc, x0 - S(16), y0 + S(70));
        SelectObject(hdc, op);
        DeleteObject(p);
    }
    SelectClipRgn(hdc, NULL);
    edge = CreateSolidBrush(RGB(40, 42, 45));
    FrameRgn(hdc, stone, edge, max(2, S(2)), max(2, S(2)));
    DeleteObject(edge);
    DeleteObject(cap);
    DeleteObject(body);
    DeleteObject(stone);

    /* the words */
    SetBkMode(hdc, TRANSPARENT);
    of = SelectObject(hdc, rip);
    SetRect(&r, box->left, box->top + S(34), box->right, box->top + S(110));
    es_carve(hdc, "RIP", &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE, s);
    {
        HPEN p = CreatePen(PS_SOLID, max(1, S(2)), RGB(70, 71, 74));
        HGDIOBJ op = SelectObject(hdc, p);
        int y = box->top + S(122);

        MoveToEx(hdc, box->left + S(46), y, NULL);
        LineTo(hdc, box->right - S(46), y);
        SelectObject(hdc, op);
        DeleteObject(p);
    }
    r.left = box->left + S(22), r.right = box->right - S(22);
    r.top = box->top + S(138);
    for (i = 0; i < es_nstone; i++) {
        RECT m = r;
        UINT how = DT_CENTER | DT_WORDBREAK;

        SelectObject(hdc, i == 0 ? name : line);
        m.bottom = box->bottom - S(14);
        DrawTextA(hdc, es_stone[i], -1, &m,
                  DT_CALCRECT | how | DT_NOPREFIX);
        m.left = r.left, m.right = r.right;
        es_carve(hdc, es_stone[i], &m, how, s);
        r.top = m.bottom + S(i == 0 ? 10 : 6);
    }
    SelectObject(hdc, of);
    DeleteObject(rip);
    DeleteObject(name);
    DeleteObject(line);
}

static void
es_paint_death(HDC hdc, int w, int h, double s)
{
    HFONT kicker = es_font(max(10, S(15)), FW_SEMIBOLD, FALSE, FALSE),
          big = es_font(S(44), FW_BOLD, TRUE, FALSE),
          sub = es_font(max(10, S(20)), FW_NORMAL, TRUE, TRUE),
          head = es_font(max(10, S(15)), FW_BOLD, FALSE, FALSE),
          words = es_font(max(11, S(23)), FW_NORMAL, TRUE, TRUE),
          num = es_font(S(30), FW_BOLD, TRUE, FALSE),
          label = es_font(max(9, S(12)), FW_SEMIBOLD, FALSE, FALSE),
          btn = es_font(max(11, S(19)), FW_SEMIBOLD, FALSE, FALSE),
          small = es_font(max(10, S(14)), FW_NORMAL, FALSE, FALSE),
          quote = es_font(S(72), FW_BOLD, TRUE, FALSE);
    HGDIOBJ of;
    int x0 = (w - S(900)) / 2, top = max(S(14), (h - S(640)) / 2);
    RECT r, stone, panel;
    int i, y;

    of = SelectObject(hdc, kicker);
    SetRect(&r, 0, top, w, top + S(22));
    mswin_ms_text(hdc, "R E S T    I N    P E A C E", &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(150, 60, 52),
                  1);
    SelectObject(hdc, big);
    SetRect(&r, S(20), top + S(26), w - S(20), top + S(86));
    mswin_ms_text(hdc, es_title, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                  BONE, max(2, S(3)));
    SelectObject(hdc, sub);
    SetRect(&r, S(20), top + S(88), w - S(20), top + S(118));
    mswin_ms_text(hdc, es_cause, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                  RGB(190, 192, 198), 1);

    /* the stone on the left, the last words and the tally on the right */
    SetRect(&stone, x0 + S(40), top + S(156), x0 + S(310), top + S(510));
    es_paint_stone(hdc, &stone, s);

    SetRect(&panel, x0 + S(370), top + S(156), x0 + S(900), top + S(510));
    es_shade(hdc, &panel, RGB(8, 9, 12), 190);
    {
        HPEN p = CreatePen(PS_SOLID, 1, RGB(64, 68, 78));
        HGDIOBJ op = SelectObject(hdc, p),
                ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));

        RoundRect(hdc, panel.left, panel.top, panel.right, panel.bottom,
                  S(14), S(14));
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(p);
    }

    SelectObject(hdc, head);
    SetRect(&r, panel.left + S(26), panel.top + S(16), panel.right - S(20),
            panel.top + S(40));
    es_draw(hdc, "FINAL WORDS", &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE,
            TH_ACCENT);

    /* the words themselves, under a large opening quote */
    SelectObject(hdc, quote);
    SetRect(&r, panel.left + S(20), panel.top + S(32), panel.left + S(80),
            panel.top + S(112));
    SetTextColor(hdc, RGB(120, 50, 44));
    {
        static const WCHAR lq[2] = { 0x201C, 0 }; /* an opening quote */

        DrawTextW(hdc, lq, 1, &r,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(hdc, words);
    {
        RECT area;
        int n0 = 0, hsum;

        SetRect(&area, panel.left + S(70), panel.top + S(52),
                panel.right - S(28), panel.top + S(196));
        if (!es_nlast) {
            es_draw(hdc, "(nothing more was said)", &area,
                    DT_LEFT | DT_WORDBREAK, FAINT);
        } else {
            /* drop the oldest until the rest fit */
            for (;;) {
                hsum = 0;
                for (i = n0; i < es_nlast; i++) {
                    RECT m = area;

                    DrawTextA(hdc, es_last[i], -1, &m,
                              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                    hsum += (m.bottom - m.top) + S(6);
                }
                if (hsum <= area.bottom - area.top || n0 >= es_nlast - 1)
                    break;
                n0++;
            }
            y = area.top;
            for (i = n0; i < es_nlast; i++) {
                RECT m = area;

                m.top = y;
                DrawTextA(hdc, es_last[i], -1, &m,
                          DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                m.right = area.right;
                if (m.bottom > area.bottom)
                    m.bottom = area.bottom;
                es_draw(hdc, es_last[i], &m, DT_LEFT | DT_WORDBREAK,
                        i == es_nlast - 1 ? BONE : TH_TEXT_DIM);
                y = m.bottom + S(6);
            }
        }
    }

    /* the tally */
    {
        HPEN p = CreatePen(PS_SOLID, 1, RGB(60, 64, 74));
        HGDIOBJ op = SelectObject(hdc, p);

        y = panel.top + S(212);
        MoveToEx(hdc, panel.left + S(26), y, NULL);
        LineTo(hdc, panel.right - S(26), y);
        SelectObject(hdc, op);
        DeleteObject(p);
    }
    for (i = 0; i < 6; i++) {
        int cw = (panel.right - panel.left - S(52)) / 3;
        int cx = panel.left + S(26) + (i % 3) * cw;
        int cy = panel.top + S(226) + (i / 3) * S(62);

        SelectObject(hdc, num);
        SetRect(&r, cx, cy, cx + cw, cy + S(36));
        es_draw(hdc, es_stat[i].value, &r,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE, BONE);
        SelectObject(hdc, label);
        SetRect(&r, cx, cy + S(36), cx + cw, cy + S(54));
        es_draw(hdc, es_stat[i].label, &r,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE, TH_TEXT_DIM);
    }

    SelectObject(hdc, btn);
    SetRect(&es_btn, (w - S(240)) / 2, top + S(548), (w + S(240)) / 2,
            top + S(594));
    mswin_ms_button(hdc, &es_btn, "Continue", TRUE, es_hot, TRUE, es_down,
                    TRUE, s);
    SelectObject(hdc, small);
    SetRect(&r, 0, h - S(34), w, h - S(10));
    mswin_ms_text(hdc, "Enter, Space or Esc: continue", &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE, TH_TEXT_DIM, 1);

    SelectObject(hdc, of);
    DeleteObject(kicker);
    DeleteObject(big);
    DeleteObject(sub);
    DeleteObject(head);
    DeleteObject(words);
    DeleteObject(num);
    DeleteObject(label);
    DeleteObject(btn);
    DeleteObject(small);
    DeleteObject(quote);
}

/* a rank in a circle: gold, silver and bronze for the first three */
static void
sl_badge(HDC hdc, int cx, int cy, int rank, double s)
{
    static const COLORREF medal[3] = {
        RGB(229, 186, 96), RGB(196, 202, 212), RGB(196, 132, 84)
    };
    int rad = S(17);
    RECT r;
    char buf[16];

    SetRect(&r, cx - rad, cy - rad, cx + rad, cy + rad);
    if (rank >= 1 && rank <= 3) {
        HBRUSH b = CreateSolidBrush(medal[rank - 1]);
        HPEN p = CreatePen(PS_SOLID, 1, medal[rank - 1]);
        HGDIOBJ ob = SelectObject(hdc, b), op = SelectObject(hdc, p);

        Ellipse(hdc, r.left, r.top, r.right, r.bottom);
        SelectObject(hdc, op);
        SelectObject(hdc, ob);
        DeleteObject(p);
        DeleteObject(b);
    } else {
        HPEN p = CreatePen(PS_SOLID, max(1, S(1)), TH_SEPARATOR);
        HGDIOBJ op = SelectObject(hdc, p),
                ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));

        Ellipse(hdc, r.left, r.top, r.right, r.bottom);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(p);
    }
    if (rank > 0)
        Sprintf(buf, "%d", rank);
    else
        Strcpy(buf, "-");
    es_draw(hdc, buf, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE,
            (rank >= 1 && rank <= 3) ? TH_BG_APP : TH_TEXT_DIM);
}

static void
es_paint_scores(HDC hdc, int w, int h, double s)
{
    HFONT big = es_font(S(48), FW_BOLD, TRUE, FALSE),
          sub = es_font(max(10, S(19)), FW_NORMAL, FALSE, FALSE),
          head = es_font(max(9, S(13)), FW_BOLD, FALSE, FALSE),
          rankf = es_font(max(10, S(16)), FW_BOLD, FALSE, FALSE),
          pts = es_font(max(11, S(22)), FW_BOLD, TRUE, FALSE),
          name = es_font(max(10, S(18)), FW_SEMIBOLD, FALSE, FALSE),
          body = es_font(max(10, S(16)), FW_NORMAL, FALSE, FALSE),
          small = es_font(max(9, S(14)), FW_NORMAL, FALSE, FALSE),
          btn = es_font(max(11, S(19)), FW_SEMIBOLD, FALSE, FALSE);
    HGDIOBJ of;
    int pw = min(w - S(48), S(1040)), pl = (w - pw) / 2, pr = pl + pw;
    int top = max(S(14), (h - S(700)) / 2), rh = S(64), hh = S(36);
    int ptop = top + (sl_note[0] ? S(118) : S(90)),
        pbot = h - S(34) - S(46) - S(34);
    int xpts = pl + S(196), xwho = pl + S(222), xfate = pl + S(222) + S(260),
        xhp = pr - S(110);
    RECT r, panel;
    int i, y;

    of = SelectObject(hdc, big);
    SetRect(&r, 0, top, w, top + S(62));
    mswin_ms_text(hdc, "High Scores", &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE, TH_ACCENT,
                  max(2, S(3)));
    SelectObject(hdc, sub);
    SetRect(&r, S(20), top + S(66), w - S(20), top + S(96));
    mswin_ms_text(hdc, sl_note, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                  TH_TEXT, 1);

    /* the table */
    if (sl_nent) {
        int need = hh + sl_nent * rh + S(10);

        if (ptop + need < pbot)
            pbot = ptop + need;
    } else {
        pbot = min(pbot, ptop + S(120));
    }
    SetRect(&panel, pl, ptop, pr, pbot);
    es_shade(hdc, &panel, RGB(10, 11, 14), 205);
    {
        HPEN p = CreatePen(PS_SOLID, 1, TH_SEPARATOR);
        HGDIOBJ op = SelectObject(hdc, p),
                ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));

        RoundRect(hdc, panel.left, panel.top, panel.right, panel.bottom,
                  S(14), S(14));
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(p);
    }

    if (!sl_nent) {
        SelectObject(hdc, body);
        r = panel;
        InflateRect(&r, -S(30), 0);
        es_draw(hdc, "No scores to show.", &r,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE, TH_TEXT_DIM);
    } else {
        SelectObject(hdc, head);
        y = ptop + S(10);
        SetRect(&r, pl + S(18), y, pl + S(70), y + S(20));
        es_draw(hdc, "RANK", &r, DT_LEFT | DT_SINGLELINE, TH_TEXT_DIM);
        SetRect(&r, pl, y, xpts, y + S(20));
        es_draw(hdc, "POINTS", &r, DT_RIGHT | DT_SINGLELINE, TH_TEXT_DIM);
        SetRect(&r, xwho, y, xfate, y + S(20));
        es_draw(hdc, "HERO", &r, DT_LEFT | DT_SINGLELINE, TH_TEXT_DIM);
        SetRect(&r, xfate, y, xhp, y + S(20));
        es_draw(hdc, "FATE", &r, DT_LEFT | DT_SINGLELINE, TH_TEXT_DIM);
        SetRect(&r, xhp, y, pr - S(20), y + S(20));
        es_draw(hdc, "HP", &r, DT_RIGHT | DT_SINGLELINE, TH_TEXT_DIM);

        sl_fit = max(1, (pbot - ptop - hh - S(6)) / rh);
        if (sl_top > sl_nent - sl_fit)
            sl_top = max(0, sl_nent - sl_fit);
        for (i = sl_top; i < sl_nent && i < sl_top + sl_fit; i++) {
            struct sl_entry *e = &sl_ent[i];
            RECT row;
            char buf[64];

            y = ptop + hh + (i - sl_top) * rh;
            SetRect(&row, pl + S(6), y, pr - S(6), y + rh - S(4));
            if (e->hero) {
                es_round(hdc, &row, RGB(52, 44, 26), RGB(120, 96, 50), 1,
                         S(10));
                SetRect(&r, row.left, row.top + S(8), row.left + S(4),
                        row.bottom - S(8));
                es_fill(hdc, &r, TH_ACCENT);
            } else if ((i - sl_top) % 2) {
                es_round(hdc, &row, RGB(27, 30, 36), RGB(27, 30, 36), 1,
                         S(10));
            }
            SelectObject(hdc, rankf);
            sl_badge(hdc, pl + S(36), (row.top + row.bottom) / 2, e->rank, s);

            SelectObject(hdc, pts);
            Sprintf(buf, "%ld", e->points);
            SetRect(&r, pl + S(60), row.top, xpts, row.bottom);
            es_draw(hdc, buf, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE,
                    e->hero ? TH_ACCENT : TH_TEXT);

            SelectObject(hdc, name);
            SetRect(&r, xwho, row.top + S(9), xfate - S(16),
                    row.top + S(33));
            es_draw(hdc, e->name, &r,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                    e->hero ? TH_ACCENT : TH_TEXT);
            SelectObject(hdc, small);
            SetRect(&r, xwho, row.top + S(33), xfate - S(16),
                    row.top + S(54));
            es_draw(hdc, e->who, &r,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                    TH_TEXT_DIM);

            SelectObject(hdc, body);
            SetRect(&r, xfate, row.top + (e->place[0] ? S(9) : 0),
                    xhp - S(16), e->place[0] ? row.top + S(33) : row.bottom);
            es_draw(hdc, e->fate, &r,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                    TH_TEXT);
            if (e->place[0]) {
                SelectObject(hdc, small);
                SetRect(&r, xfate, row.top + S(33), xhp - S(16),
                        row.top + S(54));
                es_draw(hdc, e->place, &r,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE
                            | DT_END_ELLIPSIS,
                        TH_TEXT_DIM);
            }

            SelectObject(hdc, body);
            if (e->maxhp)
                Sprintf(buf, "%s / %d", !strcmp(e->hp, "-") ? "0" : e->hp,
                        e->maxhp);
            else
                buf[0] = '\0';
            SetRect(&r, xhp, row.top, pr - S(20), row.bottom);
            es_draw(hdc, buf, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE,
                    TH_TEXT_DIM);
        }

        /* more above or below */
        SelectObject(hdc, small);
        if (sl_top > 0) {
            SetRect(&r, pl, ptop - S(22), pr, ptop - S(2));
            es_draw(hdc, "more above  (scroll or Up)", &r,
                    DT_CENTER | DT_SINGLELINE, TH_TEXT_DIM);
        }
        if (sl_top + sl_fit < sl_nent) {
            SetRect(&r, pl, pbot + S(2), pr, pbot + S(22));
            es_draw(hdc, "more below  (scroll or Down)", &r,
                    DT_CENTER | DT_SINGLELINE, TH_TEXT_DIM);
        }
    }

    SelectObject(hdc, btn);
    y = max(pbot + S(30), min(h - S(34) - S(56), pbot + S(40)));
    SetRect(&es_btn, (w - S(240)) / 2, y, (w + S(240)) / 2, y + S(46));
    mswin_ms_button(hdc, &es_btn, "Done", TRUE, es_hot, TRUE, es_down, TRUE,
                    s);
    SelectObject(hdc, small);
    SetRect(&r, 0, h - S(34), w, h - S(10));
    mswin_ms_text(hdc,
                  sl_nent > sl_fit
                      ? "Up, Down or wheel: scroll      Enter or Esc: done"
                      : "Enter or Esc: done",
                  &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE, TH_TEXT_DIM, 1);

    SelectObject(hdc, of);
    DeleteObject(big);
    DeleteObject(sub);
    DeleteObject(head);
    DeleteObject(rankf);
    DeleteObject(pts);
    DeleteObject(name);
    DeleteObject(body);
    DeleteObject(small);
    DeleteObject(btn);
}

static void
es_paint(HWND hWnd, HDC hdc)
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
    s = es_scale(w, h);

    mdc = CreateCompatibleDC(hdc);
    mbm = CreateCompatibleBitmap(hdc, w, h);
    omb = SelectObject(mdc, mbm);
    es_backdrop(hdc, mdc, w, h);
    SetBkMode(mdc, TRANSPARENT);
    if (es_mode == ES_DEATH)
        es_paint_death(mdc, w, h, s);
    else
        es_paint_scores(mdc, w, h, s);
    BitBlt(hdc, 0, 0, w, h, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, omb);
    DeleteObject(mbm);
    DeleteDC(mdc);
}

#undef S

/*
 * input
 */

static void
sl_scroll(int by)
{
    int was = sl_top;

    sl_top += by;
    if (sl_top > sl_nent - sl_fit)
        sl_top = sl_nent - sl_fit;
    if (sl_top < 0)
        sl_top = 0;
    if (sl_top != was && es_hwnd)
        InvalidateRect(es_hwnd, NULL, FALSE);
}

static LRESULT CALLBACK
es_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        es_paint(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt;
        boolean hot;

        pt.x = GET_X_LPARAM(lParam), pt.y = GET_Y_LPARAM(lParam);
        hot = PtInRect(&es_btn, pt) ? TRUE : FALSE;
        if (hot != es_hot) {
            TRACKMOUSEEVENT tme;

            es_hot = hot;
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
        if (es_hot) {
            es_hot = FALSE;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, es_hot ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        SetFocus(hWnd);
        if (es_hot) {
            es_down = TRUE;
            SetCapture(hWnd);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hWnd)
            ReleaseCapture();
        if (es_down) {
            es_down = FALSE;
            InvalidateRect(hWnd, NULL, FALSE);
            if (es_hot)
                es_done = TRUE;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (es_mode == ES_SCORES)
            sl_scroll(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        switch (wParam) {
        case VK_RETURN:
        case VK_ESCAPE:
        case VK_SPACE:
            es_done = TRUE;
            break;
        case VK_UP:
            sl_scroll(-1);
            break;
        case VK_DOWN:
            sl_scroll(1);
            break;
        case VK_PRIOR:
            sl_scroll(-sl_fit);
            break;
        case VK_NEXT:
            sl_scroll(sl_fit);
            break;
        case VK_HOME:
            sl_scroll(-sl_nent);
            break;
        case VK_END:
            sl_scroll(sl_nent);
            break;
        }
        return 0;
    case WM_COMMAND: /* the main window cancelling its popup */
        if (LOWORD(wParam) == IDCANCEL || LOWORD(wParam) == IDOK)
            es_done = TRUE;
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void
mswin_end_screen_place(HWND main)
{
    RECT rc;

    if (!es_hwnd || !IsWindow(es_hwnd))
        return;
    GetClientRect(main, &rc);
    SetWindowPos(es_hwnd, HWND_TOP, 0, 0, rc.right, rc.bottom, 0);
}

static boolean
es_run(int mode)
{
    static boolean registered;
    HWND main = GetNHApp()->hMainWnd, bar;
    MSG msg;

    if (!registered) {
        WNDCLASS wc;

        ZeroMemory(&wc, sizeof wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = es_proc;
        wc.hInstance = GetNHApp()->hApp;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = TEXT("MSNHEndScreen");
        if (!RegisterClass(&wc))
            return FALSE;
        registered = TRUE;
    }
    if (!main || !IsWindow(main))
        return FALSE;

    es_mode = mode;
    es_done = FALSE;
    es_hot = es_down = FALSE;
    SetRectEmpty(&es_btn);
    bar = FindWindowEx(main, NULL, TEXT("MSNHTopBar"), NULL);
    if (bar)
        ShowWindow(bar, SW_HIDE);
    es_hwnd = CreateWindow(TEXT("MSNHEndScreen"), NULL,
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 1,
                           1, main, NULL, GetNHApp()->hApp, NULL);
    if (!es_hwnd) {
        if (bar)
            ShowWindow(bar, SW_SHOW);
        return FALSE;
    }
    mswin_end_screen_place(main);
    GetNHApp()->hPopupWnd = es_hwnd;
    SetForegroundWindow(main);
    SetFocus(es_hwnd);

    while (!es_done && IsWindow(es_hwnd) && GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GetNHApp()->hPopupWnd = NULL;
    if (IsWindow(es_hwnd))
        DestroyWindow(es_hwnd);
    es_hwnd = NULL;
    if (es_bg)
        DeleteObject(es_bg), es_bg = NULL;
    es_bgmode = -1;
    mswin_ms_backdrop_free();
    if (bar && IsWindow(bar))
        ShowWindow(bar, SW_SHOW);
    if (IsWindow(main))
        SetFocus(main);
    return TRUE;
}

boolean
mswin_death_screen(const char *rip, const char *summary)
{
    es_gather(rip, summary);
    return es_run(ES_DEATH);
}

boolean
mswin_scores_screen(void)
{
    int i;
    boolean ok;

    sl_parse();
    /* start with the hero's entry in view */
    sl_top = 0;
    sl_fit = 6;
    for (i = 0; i < sl_nent; i++)
        if (sl_ent[i].hero) {
            sl_top = max(0, i - 2);
            break;
        }
    ok = es_run(ES_SCORES);
    mswin_scores_clear();
    return ok;
}
