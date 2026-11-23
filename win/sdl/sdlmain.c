/* NetHack Modern sdlmain.c */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * An SDL3 window port: the map drawn with the tile sheet on the GPU,
 * text with SDL3_ttf, and the effects every tiles front end shares
 * (win/share/fxview.c). It runs on Linux, macOS and Windows alike.
 *
 * Layout: messages along the top, the map in the middle (centred on the
 * hero), the two status lines along the bottom; menus and text windows
 * open over the map. Everything is keyboard driven.
 *
 * Files it reads from the game's directory: tiles.bmp (made by
 * util/tile2bmp), and a TrueType font: $NETHACK_FONT, then
 * fonts/DejaVuSansMono.ttf there, then a common system monospace font.
 */

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "hack.h"
#include "func_tab.h"
#include "fxview.h"
#if !defined(VERSION_MAJOR)
#include "patchlevel.h"
#endif
#ifdef SND_LIB_MINIAUDIO
#include "../sound/miniaudio/nhminiaudio.h"
#endif

#include <math.h>

#ifdef SDL_GRAPHICS

/* ---- the port's state --------------------------------------------- */

#define SDL_TILE 16            /* tile sheet: pixels per tile */
#define SDL_MAXWIN 24
#define SDL_MSGKEEP 200        /* message history */
#define SDL_MSGSHOW 3          /* message lines on screen */
#define SDL_TEXTMAX 1000       /* lines in a text or menu window */
#define SDL_KEYQ 64
/* what the name and character screens' own keys and clicks push */
#define SDL_CSK_UP 0x300
#define SDL_CSK_DOWN 0x301
#define SDL_CSK_LEFT 0x302
#define SDL_CSK_RIGHT 0x303
#define SDL_CSK_HOME 0x304
#define SDL_CSK_END 0x305
#define SDL_CSK_TAB 0x306
#define SDL_CSK_BACKTAB 0x307
#define SDL_CSK_MUSIC 0x308
#define SDL_CSK_CLEAR 0x309      /* Ctrl+Backspace: the whole name */
#define SDL_CSK_PGUP 0x30a
#define SDL_CSK_PGDN 0x30b
#define SDL_CSK_BTN 0x310        /* + the button clicked */
#define CS_IN(r, x, y) ((x) >= (r)[0] && (x) < (r)[2] && (y) >= (r)[1] \
                        && (y) < (r)[3])
#define SDL_ES_DEATH 0           /* the death screen */
#define SDL_ES_SCORES 1          /* the score list */
/* the name and character screens, and their groups beyond the choices
   (FXV_CS_ROLE..FXV_CS_ALIGN) */
#define SDL_CS_NAME 0 /* asking for a name */
#define SDL_CS_CHAR 1 /* choosing role, race, gender, alignment */
#define SDL_CS_BTN FXV_CS_NGRP       /* the row of buttons */
#define SDL_CS_BOX (FXV_CS_NGRP + 1) /* the name box */
#define SDL_CS_SND (FXV_CS_NGRP + 2) /* the speaker: the menu music */

struct sdl_item {              /* a line of a menu or text window */
    anything id;               /* a_void == 0: not selectable */
    char acc;                  /* accelerator */
    char gacc;                 /* group accelerator */
    int attr, color;
    boolean selected;
    int tile;                  /* its tile, -1 none */
    char *text;
};

struct sdl_win {
    int type;                  /* NHW_*, 0 = free slot */
    struct sdl_item *items;
    int nitems, maxitems;
    char *prompt;              /* end_menu() prompt */
    char status[2][BUFSZ];     /* NHW_STATUS: its two lines */
    int cury;
    boolean perminv;           /* the persistent inventory (perm_invent) */
    boolean rip;               /* the end of the game: the tombstone's
                                  lines, then the farewell */
};

static struct {
    SDL_Window *window;
    SDL_Renderer *ren;
    TTF_Font *font;
    SDL_Texture *tiles;        /* the tile sheet, key colour cut out */
    SDL_Texture *tiles_solid;  /* the same, solid: floors, backgrounds */
    SDL_Surface *tiles_px;     /* the solid sheet's pixels (missiles) */
    int tiles_across;
    int cw, lh;                /* character width, line height */
    float text_px;             /* the text size, pixels (Alt+= / Alt+-) */
    float auto_text_px;        /* the size that suits the screen */
    int text_set, zoom_set;    /* fx_text_size, fx_map_zoom as applied */
    int bar_open, bar_hot;     /* menu bar: open topic, topic under mouse */
    int bar_item;              /* the drop-down item under the mouse */
    int zoom;                  /* index into sdl_zooms[], -1 = automatic */
    int zoom_used;             /* the step the last layout used */
    boolean recentre;          /* put the hero mid-view next layout */
    int viewx, viewy;          /* first square shown, when it scrolls */
    int herox, heroy;          /* where cliparound() says the hero is */
    int curx, cury;            /* map cursor (getpos) */
    glyph_info map[COLNO][ROWNO], bkmap[COLNO][ROWNO];
    struct sdl_win win[SDL_MAXWIN];
    char *msgs[SDL_MSGKEEP];
    long msg_turn[SDL_MSGKEEP];      /* the turn each came on */
    fxv_color msg_hl[SDL_MSGKEEP];   /* its highlight colour... */
    boolean msg_has_hl[SDL_MSGKEEP]; /* ...if it has one */
    TTF_Font *msgfont;         /* bold, for messages on the map */
    SDL_Rect maprect;          /* the level as last drawn (column 1 on) */
    int nmsgs;                 /* total ever added */
    int msg_seen;              /* messages before this one are old */
    const char *prompt;        /* a question being asked, shown on top */
    int keyq[SDL_KEYQ], kq_head, kq_tail;
    boolean skip_text;         /* the key already gave us this character */
    boolean dirty;
    boolean animating;         /* auras or flickering light on show */
    SDL_Texture *disc, *light; /* for dots, and the shade layer */
    TTF_Font *fxfont[3];       /* floating text, at sdl_fx_font() sizes */
    int fx_ts;                 /* the tile size those fonts suit */
    char fontpath[BUFSZ];      /* the font file in use */
    struct {                   /* the menu or text window open, if any */
        struct sdl_win *w;
        int first, how;
        int bx, by, bw, bh;    /* its box on screen, as last drawn */
        int items_y;           /* where the line for item "first" is */
        int hover;             /* the item under the mouse, -1 none */
    } ovl;
    struct {                   /* the title screen, while it's up */
        boolean up;
        int focus, hot, down;  /* FXV_TITLE_*, or SDL_TITLE_SOUND */
        int btn[FXV_TITLE_NBTN][4], snd[4]; /* as last laid out */
    } title;
    struct {                   /* the name and character screens */
        int mode;              /* SDL_CS_NAME or _CHAR; -1 when not up */
        int fg, fb;            /* the focus: a group, or _BTN / _BOX */
        int lastgrp, lastmid;  /* where Up from the buttons, Right from
                                  the roles go */
        int hg, hi, dg, di;    /* what's under, and pressed by, the mouse */
        char name[PL_NSIZ];
        struct fxv_cs_layout L; /* as last laid out */
    } cs;
    struct {                   /* the death screen and the score list */
        int mode;              /* SDL_ES_DEATH or _SCORES; -1 when not up */
        boolean hot, down;     /* the mouse over / pressing the button */
        int btn[4];            /* the button, as last drawn */
        int top, fit;          /* scores: the first row showing, how many
                                  rows fit */
    } es;
    boolean closing;           /* the window was closed: don't come back
                                  to the title screen */
    SDL_Texture *art;          /* the title painting (none if missing) */
    SDL_Surface *art_px;       /* ... its pixels, for the death screen */
    SDL_Texture *art_grey;     /* the death screen's: drained of colour */
    SDL_Texture *stone;        /* the tombstone, at stone_w x stone_h */
    int stone_w, stone_h;
    float art_w, art_h;
    TTF_Font *face[3];         /* the screens' fonts (SDL_FACE_*) */
    struct {                   /* the inventory panel (perm_invent) */
        int first;             /* the line at its top */
        boolean updating;      /* update_inventory() is filling it */
    } inv;
} sdl;

static void sdl_render(void);
static boolean sdl_play_again; /* Play was chosen: a finished game
                                  comes back to the title screen */
static void sdl_zoom(int);
static int sdl_page_lines(void);
static int sdl_status_h(void);
static int sdl_inv_w(void);
static int sdl_inv_lines(void);
static int sdl_map_top(void);
static void sdl_draw_inventory(void);
static int sdl_title_hit(float, float);
static int sdl_cs_hit(float, float, int *);
static void sdl_draw_cs(void);
static void sdl_draw_es(void);
static void sdl_end_screens(struct sdl_win *, boolean);
static void sdl_outrip(winid, int, time_t);
static winid sdl_create_nhwindow(int);
static void sdl_putstr(winid, int, const char *);
static void sdl_destroy_nhwindow(winid);
static void sdl_exit_nhwindows(const char *);
static void sdl_draw_title(void);
static void sdl_music_end(void);
static int sdl_text_w(const char *);
static SDL_Color sdl_col(fxv_color);
static struct sdl_win *sdl_getwin(winid);

/* the character panel's action buttons, as last drawn */
#define SDL_MAXBTN 8
static struct {
    SDL_Rect r;
    int key;
} sdl_btn[SDL_MAXBTN];
static int sdl_nbtn, sdl_btn_hot = -1, sdl_actx;

/* the action button at a point, or -1 */
static int
sdl_btn_at(float mx, float my)
{
    int i;

    for (i = 0; i < sdl_nbtn; i++)
        if (mx >= sdl_btn[i].r.x && mx < sdl_btn[i].r.x + sdl_btn[i].r.w
            && my >= sdl_btn[i].r.y && my < sdl_btn[i].r.y + sdl_btn[i].r.h)
            return i;
    return -1;
}
static int sdl_wait_key(void);
static void sdl_add_msg(const char *);

/* ---- text ---------------------------------------------------------- */

static const SDL_Color sdl_white = { 230, 230, 230, 255 };
static const SDL_Color sdl_dim = { 140, 140, 150, 255 };
static const SDL_Color sdl_gold = { 240, 200, 90, 255 };

static SDL_Color
sdl_nh_color(int clr)
{
    fxv_color c = fxv_nh_color(clr);
    SDL_Color s;

    if (clr == NO_COLOR || clr < 0 || clr >= CLR_MAX)
        return sdl_white;
    s.r = (Uint8) FXV_R(c), s.g = (Uint8) FXV_G(c), s.b = (Uint8) FXV_B(c);
    s.a = 255;
    return s;
}

/* draw a line of text with its top left at x,y; returns its width */
static int
sdl_text(int x, int y, const char *s, SDL_Color c)
{
    SDL_Surface *surf;
    SDL_Texture *tex;
    SDL_FRect dst;
    int w;

    if (!s || !*s)
        return 0;
    surf = TTF_RenderText_Blended(sdl.font, s, 0, c);
    if (!surf)
        return 0;
    tex = SDL_CreateTextureFromSurface(sdl.ren, surf);
    w = surf->w;
    dst.x = (float) x, dst.y = (float) y;
    dst.w = (float) surf->w, dst.h = (float) surf->h;
    SDL_DestroySurface(surf);
    if (tex) {
        SDL_RenderTexture(sdl.ren, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    return w;
}

static void
sdl_fill(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_FRect rr;

    rr.x = (float) x, rr.y = (float) y, rr.w = (float) w, rr.h = (float) h;
    SDL_SetRenderDrawBlendMode(sdl.ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl.ren, r, g, b, a);
    SDL_RenderFillRect(sdl.ren, &rr);
}

/* set the text size (messages, status, menus), in pixels */
static void
sdl_text_size(float px)
{
    int w = 8, h = 16;

    sdl.text_px = max(10.0f, min(72.0f, px));
    if (!sdl.font)
        return;
    (void) TTF_SetFontSize(sdl.font, sdl.text_px);
    (void) TTF_GetStringSize(sdl.font, "M", 1, &w, &h);
    sdl.cw = w;
    sdl.lh = TTF_GetFontLineSkip(sdl.font);
    if (sdl.msgfont)
        (void) TTF_SetFontSize(sdl.msgfont, sdl.text_px);
    sdl.dirty = TRUE;
}

/* the first readable TrueType monospace font we can find */
static TTF_Font *
sdl_open_font(float pt)
{
    static const char *const tries[] = {
        "fonts/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "C:\\Windows\\Fonts\\consola.ttf",
    };
    const char *env = getenv("NETHACK_FONT");
    TTF_Font *f;
    int i;

    if (env && *env && (f = TTF_OpenFont(env, pt)) != 0) {
        Snprintf(sdl.fontpath, sizeof sdl.fontpath, "%s", env);
        return f;
    }
    for (i = 0; i < SIZE(tries); i++)
        if ((f = TTF_OpenFont(tries[i], pt)) != 0) {
            Snprintf(sdl.fontpath, sizeof sdl.fontpath, "%s", tries[i]);
            return f;
        }
    return (TTF_Font *) 0;
}

/* ---- keys ---------------------------------------------------------- */

static void
sdl_push_key(int c)
{
    int next = (sdl.kq_tail + 1) % SDL_KEYQ;

    (void) fxv_snap(); /* a key snaps every sprite back to its square */
    if (next != sdl.kq_head) {
        sdl.keyq[sdl.kq_tail] = c;
        sdl.kq_tail = next;
    }
}

/* a movement key for a direction, honouring number_pad */
static int
sdl_dir_key(int dx, int dy)
{
    static const char vi[3][3] = { { 'y', 'k', 'u' },
                                   { 'h', '.', 'l' },
                                   { 'b', 'j', 'n' } };
    static const char pad[3][3] = { { '7', '8', '9' },
                                    { '4', '5', '6' },
                                    { '1', '2', '3' } };

    return iflags.num_pad ? pad[dy + 1][dx + 1] : vi[dy + 1][dx + 1];
}

/* ---- the menu bar along the top (as NetHackW's Modern bar) ---------- */

enum sdl_bar_kind {
    BAR_KEYS,      /* type these keys into the game ("\x1b" c: Meta c) */
    BAR_ZOOM,      /* map zoom: +1, -1, 0 automatic */
    BAR_TEXT,      /* text size: +1 larger, -1 smaller */
    BAR_VOLUME,    /* 1/2 effects up/down, 3/4 ambience up/down */
    BAR_SHADE,     /* fx_shade 0..3, or 4: lighting off */
    BAR_STYLE,     /* fx_bar_style 0..4 */
    BAR_BORDER,    /* fx_bar_border on or off */
    BAR_INVEN,     /* the inventory panel shown or hidden */
    BAR_ACTIONS,   /* the action buttons shown or hidden */
    BAR_SEP
};

struct sdl_bar_item {
    const char *label, *hint; /* hint: the key, as shown */
    int kind;
    const char *keys;
    int arg;
};

#define BK(l, h, k) { l, h, BAR_KEYS, k, 0 }
#define BSEP { NULL, NULL, BAR_SEP, NULL, 0 }
#define BMETA(c) "\x1b" c

static const struct sdl_bar_item bar_game[] = {
    BK("Save and quit", "S", "S"),
    BK("Quit without saving", "#quit", "#quit\n"),
    BSEP,
    BK("Options", "O", "O"),
    BK("All options", "m O", "mO"),
    BSEP,
    { "Sound louder", "", BAR_VOLUME, NULL, 1 },
    { "Sound quieter", "", BAR_VOLUME, NULL, 2 },
    { "Ambience louder", "", BAR_VOLUME, NULL, 3 },
    { "Ambience quieter", "", BAR_VOLUME, NULL, 4 },
};
static const struct sdl_bar_item bar_view[] = {
    { "Zoom in", "Ctrl+=", BAR_ZOOM, NULL, 1 },
    { "Zoom out", "Ctrl+-", BAR_ZOOM, NULL, -1 },
    { "Whole level", "Ctrl+0", BAR_ZOOM, NULL, 0 },
    BSEP,
    { "Larger text", "Alt+=", BAR_TEXT, NULL, 1 },
    { "Smaller text", "Alt+-", BAR_TEXT, NULL, -1 },
    BSEP,
    { "Shade: light", "", BAR_SHADE, NULL, 0 },
    { "Shade: medium", "", BAR_SHADE, NULL, 1 },
    { "Shade: dark", "", BAR_SHADE, NULL, 2 },
    { "Shade: deep", "", BAR_SHADE, NULL, 3 },
    { "Lighting off", "", BAR_SHADE, NULL, 4 },
    BSEP,
    { "Bars: flat", "", BAR_STYLE, NULL, 0 },
    { "Bars: smooth", "", BAR_STYLE, NULL, 1 },
    { "Bars: gloss", "", BAR_STYLE, NULL, 2 },
    { "Bars: striped", "", BAR_STYLE, NULL, 3 },
    { "Bars: bevel", "", BAR_STYLE, NULL, 4 },
    { "Bar border", "", BAR_BORDER, NULL, 0 },
    BSEP,
    { "Inventory panel", "", BAR_INVEN, NULL, 0 },
    { "Action buttons", "", BAR_ACTIONS, NULL, 0 },
    BK("Previous messages", "Ctrl+P", "\x10"),
    BK("Dungeon overview", "Ctrl+O", "\x0f"),
};
static const struct sdl_bar_item bar_items[] = {
    BK("Inventory", "i", "i"),
    BK("Pick up", ",", ","),
    BK("Drop", "d", "d"),
    BK("Drop several", "D", "D"),
    BSEP,
    BK("Eat", "e", "e"),
    BK("Quaff", "q", "q"),
    BK("Read", "r", "r"),
    BK("Apply", "a", "a"),
    BK("Throw", "t", "t"),
    BK("Fire", "f", "f"),
    BSEP,
    BK("Loot", "M-l", BMETA("l")),
    BK("Dip", "M-d", BMETA("d")),
    BK("Name or call", "M-n", BMETA("n")),
    BK("Adjust letters", "M-a", BMETA("a")),
};
static const struct sdl_bar_item bar_gear[] = {
    BK("Wield", "w", "w"),
    BK("Swap weapons", "x", "x"),
    BK("Quiver", "Q", "Q"),
    BSEP,
    BK("Wear", "W", "W"),
    BK("Take off", "T", "T"),
    BK("Put on", "P", "P"),
    BK("Remove", "R", "R"),
    BSEP,
    BK("Show equipment", "*", "*"),
};
/* (no movement here: the hero is moved only by keys) */
static const struct sdl_bar_item bar_act[] = {
    BK("Search", "s", "s"),
    BK("Rest a turn", ".", "."),
    BK("Kick", "Ctrl+D", "\x04"),
    BK("Sit", "M-s", BMETA("s")),
    BSEP,
    BK("Open", "o", "o"),
    BK("Close", "c", "c"),
    BK("Force a lock", "M-f", BMETA("f")),
    BK("Untrap", "M-u", BMETA("u")),
    BK("Engrave", "E", "E"),
    BSEP,
    BK("Chat", "M-c", BMETA("c")),
    BK("Pay", "p", "p"),
    BK("Pray", "M-p", BMETA("p")),
    BK("Offer", "M-o", BMETA("o")),
};
static const struct sdl_bar_item bar_magic[] = {
    BK("Cast a spell", "Z", "Z"),
    BK("Known spells", "+", "+"),
    BK("Zap a wand", "z", "z"),
    BSEP,
    BK("Turn undead", "M-t", BMETA("t")),
    BK("Invoke", "M-i", BMETA("i")),
    BK("Enhance skills", "M-e", BMETA("e")),
};
static const struct sdl_bar_item bar_info[] = {
    BK("Attributes", "Ctrl+X", "\x18"),
    BK("Look here", ":", ":"),
    BK("Far look", ";", ";"),
    BK("What is a symbol", "/", "/"),
    BK("Discoveries", "\\", "\\"),
    BK("Conduct", "M-C", BMETA("C")),
};
static const struct sdl_bar_item bar_help[] = {
    BK("Help", "?", "?"),
    BK("Extended commands", "#?", "#?\n"),
    BK("Game options", "O", "O"),
};
#undef BK
#undef BSEP
#undef BMETA

static const struct sdl_bar_topic {
    const char *name;
    const struct sdl_bar_item *items;
    int n;
} bar_topics[] = {
    { "Game", bar_game, SIZE(bar_game) },
    { "View", bar_view, SIZE(bar_view) },
    { "Items", bar_items, SIZE(bar_items) },
    { "Gear", bar_gear, SIZE(bar_gear) },
    { "Act", bar_act, SIZE(bar_act) },
    { "Magic", bar_magic, SIZE(bar_magic) },
    { "Info", bar_info, SIZE(bar_info) },
    { "Help", bar_help, SIZE(bar_help) },
};
#define BAR_NTOPIC SIZE(bar_topics)

static int bar_x[BAR_NTOPIC + 1]; /* topic left edges, and the end */
static int bar_inv[2];            /* the inventory toggle's left, right */
static void sdl_toggle_inven(void);

/* is this item a setting that's on now? (it gets a tick) */
static boolean
sdl_bar_checked(const struct sdl_bar_item *it)
{
    switch (it->kind) {
    case BAR_SHADE:
        return (it->arg >= 4) ? !iflags.fx_lighting
                              : (iflags.fx_lighting
                                 && iflags.fx_shade == it->arg);
    case BAR_ZOOM:
        return it->arg == 0 && sdl.zoom < 0; /* "Whole level": auto */
    case BAR_STYLE:
        return iflags.fx_bar_style == it->arg;
    case BAR_BORDER:
        return iflags.fx_bar_border;
    case BAR_INVEN:
        return iflags.perm_invent && !iflags.fx_hide_inventory;
    case BAR_ACTIONS:
        return iflags.fx_action_buttons;
    default:
        return FALSE;
    }
}

/* the bar's height; the messages start below it */
static int
sdl_bar_h(void)
{
    return sdl.lh + 8;
}

/* the open topic's drop-down box, as drawn */
static void
sdl_bar_box(int *x, int *y, int *w, int *h)
{
    const struct sdl_bar_topic *tp = &bar_topics[sdl.bar_open];
    int i, wl = 0, wh = 0;

    for (i = 0; i < tp->n; i++)
        if (tp->items[i].label) {
            wl = max(wl, (int) strlen(tp->items[i].label));
            wh = max(wh, (int) strlen(tp->items[i].hint));
        }
    *x = bar_x[sdl.bar_open], *y = sdl_bar_h();
    /* room for a tick, the label, and the key (or a volume like 100%) */
    *w = (2 + wl + max(wh, 4) + 4) * sdl.cw + 24;
    *h = 0;
    for (i = 0; i < tp->n; i++)
        *h += tp->items[i].kind == BAR_SEP ? sdl.lh / 2 : sdl.lh;
    *h += 12;
}

/* the drop-down item at a point, or -1 */
static int
sdl_bar_item_at(float mx, float my)
{
    const struct sdl_bar_topic *tp;
    int x, y, w, h, i, iy;

    if (sdl.bar_open < 0)
        return -1;
    tp = &bar_topics[sdl.bar_open];
    sdl_bar_box(&x, &y, &w, &h);
    if (mx < x || mx >= x + w || my < y + 6 || my >= y + h)
        return -1;
    for (i = 0, iy = y + 6; i < tp->n; i++) {
        int ih = tp->items[i].kind == BAR_SEP ? sdl.lh / 2 : sdl.lh;

        if (my >= iy && my < iy + ih)
            return tp->items[i].kind == BAR_SEP ? -1 : i;
        iy += ih;
    }
    return -1;
}

/* the topic at a point on the bar, or -1 */
static int
sdl_bar_topic_at(float mx, float my)
{
    int i;

    if (my < 0 || my >= sdl_bar_h())
        return -1;
    for (i = 0; i < (int) BAR_NTOPIC; i++)
        if (mx >= bar_x[i] && mx < bar_x[i + 1])
            return i;
    if (iflags.perm_invent && mx >= bar_inv[0] && mx < bar_inv[1])
        return BAR_NTOPIC; /* the inventory toggle */
    return -1;
}

/* show or hide the inventory panel (fx_hide_inventory, kept in
   nethackmodern.rc at once, as NetHackW does) */
static void
sdl_toggle_inven(void)
{
    iflags.fx_hide_inventory = !iflags.fx_hide_inventory;
    fx_options_remember();
    sdl.recentre = TRUE;
    sdl.dirty = TRUE;
}

static void
sdl_draw_bar(void)
{
    SDL_Color hi = { 240, 200, 90, 255 }, txt = { 205, 205, 215, 255 };
    int i, x = 8, w, h;

    SDL_GetRenderOutputSize(sdl.ren, &w, &h);
    sdl_fill(0, 0, w, sdl_bar_h(), 22, 24, 34, 255);
    sdl_fill(0, sdl_bar_h() - 1, w, 1, 60, 66, 90, 255);
    for (i = 0; i < (int) BAR_NTOPIC; i++) {
        int tw = (int) strlen(bar_topics[i].name) * sdl.cw + 2 * sdl.cw;

        bar_x[i] = x;
        if (i == sdl.bar_open || i == sdl.bar_hot)
            sdl_fill(x, 2, tw, sdl_bar_h() - 4, 50, 58, 88, 255);
        (void) sdl_text(x + sdl.cw, 4, bar_topics[i].name,
                        i == sdl.bar_open ? hi : txt);
        x += tw;
    }
    bar_x[BAR_NTOPIC] = x;
    bar_inv[0] = bar_inv[1] = 0;
    if (iflags.perm_invent) {
        /* the inventory toggle, at the right end (as NetHackW's) */
        const char *lab = iflags.fx_hide_inventory ? "Show inventory"
                                                   : "Hide inventory";
        int tw = (int) strlen(lab) * sdl.cw + 2 * sdl.cw;

        bar_inv[0] = w - 8 - tw, bar_inv[1] = w - 8;
        sdl_fill(bar_inv[0], 3, tw, sdl_bar_h() - 6,
                 sdl.bar_hot == (int) BAR_NTOPIC ? 50 : 34,
                 sdl.bar_hot == (int) BAR_NTOPIC ? 58 : 38,
                 sdl.bar_hot == (int) BAR_NTOPIC ? 88 : 52, 255);
        (void) sdl_text(bar_inv[0] + sdl.cw, 4, lab,
                        sdl.bar_hot == (int) BAR_NTOPIC ? hi : sdl_dim);
    }
    if (sdl.bar_open >= 0) {
        const struct sdl_bar_topic *tp = &bar_topics[sdl.bar_open];
        int bx, by, bw, bh, iy;

        sdl_bar_box(&bx, &by, &bw, &bh);
        sdl_fill(bx, by, bw, bh, 16, 18, 28, 245);
        sdl_fill(bx, by, bw, 2, 90, 110, 150, 255);
        for (i = 0, iy = by + 6; i < tp->n; i++) {
            const struct sdl_bar_item *it = &tp->items[i];

            if (it->kind == BAR_SEP) {
                sdl_fill(bx + 8, iy + sdl.lh / 4, bw - 16, 1, 60, 66, 90,
                         255);
                iy += sdl.lh / 2;
                continue;
            }
            if (i == sdl.bar_item)
                sdl_fill(bx + 4, iy, bw - 8, sdl.lh, 70, 92, 140, 150);
            /* settings show which one is on */
            if (sdl_bar_checked(it))
                (void) sdl_text(bx + 10, iy, "\xE2\x9C\x93", hi); /* tick */
            (void) sdl_text(bx + 12 + 2 * sdl.cw, iy, it->label, txt);
            {
                char level[16];
                const char *hint = it->hint;

                if (it->kind == BAR_VOLUME) { /* the level it's at now */
                    Snprintf(level, sizeof level, "%d%%",
                             it->arg <= 2 ? iflags.fx_volume
                                          : iflags.fx_ambience_volume);
                    hint = level;
                }
                if (*hint)
                    (void) sdl_text(bx + bw - 12
                                        - (int) strlen(hint) * sdl.cw,
                                    iy, hint, sdl_dim);
            }
            iy += sdl.lh;
        }
    }
}

/* do what a drop-down item says */
static void
sdl_bar_do(const struct sdl_bar_item *it)
{
    const char *k;

    switch (it->kind) {
    case BAR_KEYS:
        for (k = it->keys; *k; k++) {
            if (*k == '\x1b' && k[1])
                sdl_push_key(M((int) *++k));
            else
                sdl_push_key(*k);
        }
        break;
    case BAR_ZOOM:
        sdl_zoom(it->arg);
        break;
    case BAR_TEXT:
        sdl_text_size(sdl.text_px * (it->arg > 0 ? 1.1f : 0.9f));
        iflags.fx_text_size = sdl.text_set = (int) (sdl.text_px + 0.5f);
        break;
    case BAR_VOLUME: {
        int *v = (it->arg <= 2) ? &iflags.fx_volume
                                : &iflags.fx_ambience_volume;

        *v = max(0, min(100, *v + ((it->arg & 1) ? 10 : -10)));
#ifdef SND_LIB_MINIAUDIO
        {
            extern void ma_apply_volumes(void); /* masound.c */

            ma_apply_volumes();
        }
#endif
        break;
    }
    case BAR_SHADE:
        if (it->arg >= 4) {
            iflags.fx_lighting = FALSE;
        } else {
            iflags.fx_lighting = TRUE;
            iflags.fx_shade = it->arg;
        }
        break;
    case BAR_STYLE: /* the HP and Pw bars' look (kept in nethackmodern.rc) */
        iflags.fx_bar_style = it->arg;
        break;
    case BAR_BORDER:
        iflags.fx_bar_border = !iflags.fx_bar_border;
        break;
    case BAR_INVEN:
        /* the panel is the core's persistent inventory, so it needs the
           perm_invent option; this only shows or hides it, as NetHackW's
           toolbar button does */
        if (!iflags.perm_invent)
            sdl_add_msg("The inventory panel needs the perm_invent option"
                        " (set it with O).");
        else
            sdl_toggle_inven();
        break;
    case BAR_ACTIONS: /* (kept in nethackmodern.rc at once) */
        iflags.fx_action_buttons = !iflags.fx_action_buttons;
        fx_options_remember();
        break;
    default:
        break;
    }
    sdl.dirty = TRUE;
}

/* a mouse event for the bar; TRUE if the bar took it */
static boolean
sdl_bar_mouse(const SDL_Event *ev)
{
    float mx, my;
    int t, i;
    boolean busy = (sdl.ovl.w || sdl.prompt); /* the game is asking */

    if (ev->type == SDL_EVENT_MOUSE_MOTION) {
        mx = ev->motion.x, my = ev->motion.y;
        t = sdl_bar_topic_at(mx, my);
        if (t != sdl.bar_hot)
            sdl.bar_hot = t, sdl.dirty = TRUE;
        if (sdl.bar_open >= 0) {
            if (t >= 0 && t < (int) BAR_NTOPIC
                && t != sdl.bar_open) /* slide along the bar */
                sdl.bar_open = t, sdl.bar_item = -1;
            i = sdl_bar_item_at(mx, my);
            if (i != sdl.bar_item)
                sdl.bar_item = i;
            sdl.dirty = TRUE;
            return TRUE;
        }
        return FALSE;
    }
    if (ev->type != SDL_EVENT_MOUSE_BUTTON_DOWN)
        return FALSE;
    mx = ev->button.x, my = ev->button.y;
    t = sdl_bar_topic_at(mx, my);
    if (t == (int) BAR_NTOPIC) { /* the inventory toggle: only a view */
        sdl.bar_open = sdl.bar_item = -1;
        if (ev->button.button == SDL_BUTTON_LEFT)
            sdl_toggle_inven();
        return TRUE;
    }
    if (t >= 0) {
        if (!busy)
            sdl.bar_open = (sdl.bar_open == t) ? -1 : t;
        sdl.bar_item = -1;
        sdl.dirty = TRUE;
        return TRUE;
    }
    if (sdl.bar_open < 0)
        return FALSE;
    i = sdl_bar_item_at(mx, my);
    t = sdl.bar_open;
    sdl.bar_open = -1, sdl.bar_item = -1, sdl.dirty = TRUE;
    if (i >= 0 && ev->button.button == SDL_BUTTON_LEFT && !busy)
        sdl_bar_do(&bar_topics[t].items[i]);
    return TRUE; /* a click anywhere else just closes it */
}

/* the menu item (one with an identifier) under a point of the open menu
   or text window, as last drawn; -1 if none */
static int
sdl_ovl_item_at(float mx, float my)
{
    struct sdl_win *w = sdl.ovl.w;
    int i;

    if (!w || mx < sdl.ovl.bx || mx >= sdl.ovl.bx + sdl.ovl.bw
        || my < sdl.ovl.items_y || my >= sdl.ovl.by + sdl.ovl.bh)
        return -1;
    i = sdl.ovl.first + (int) ((my - sdl.ovl.items_y) / sdl.lh);
    if (i >= w->nitems || i >= sdl.ovl.first + sdl_page_lines()
        || !w->items[i].id.a_void)
        return -1;
    return i;
}

static void
sdl_handle_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_QUIT:
        if (sdl.title.up || sdl.cs.mode >= 0) { /* nothing has started:
                                                    just leave */
            sdl_push_key('x');
            break;
        }
        /* closing the window: save, as a hangup would, and don't come
           back to the title screen */
        sdl.closing = TRUE;
        sdl_push_key('\033');
        if (program_state.in_moveloop)
            sdl_push_key('S'), sdl_push_key('y');
        break;
    default:
        /* shown, exposed, resized, moved, focused...: draw it again (on
           some desktops the first frame lands before the window is up) */
        if (e->type >= SDL_EVENT_WINDOW_FIRST
            && e->type <= SDL_EVENT_WINDOW_LAST)
            sdl.dirty = TRUE;
        /* the player resized the window: kept as fx_window_size (and so
           in nethackmodern.rc) */
        if (e->type == SDL_EVENT_WINDOW_RESIZED && e->window.data1 > 0
            && e->window.data2 > 0)
            iflags.fx_window_w = e->window.data1,
            iflags.fx_window_h = e->window.data2;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        /* the wheel zooms the map, or turns a menu's pages */
        if (e->wheel.y == 0.0f)
            break;
        if (sdl.ovl.w) {
            sdl_push_key(e->wheel.y > 0 ? '<' : '>');
        } else if (sdl.es.mode >= 0) {
            sdl_push_key(e->wheel.y > 0 ? SDL_CSK_UP : SDL_CSK_DOWN);
        } else if (sdl.cs.mode >= 0 || sdl.title.up) {
            ; /* nothing to zoom */
        } else {
            /* over the inventory panel it scrolls that instead */
            SDL_Event ev = *e;
            int sw, sh;

            (void) SDL_ConvertEventToRenderCoordinates(sdl.ren, &ev);
            SDL_GetRenderOutputSize(sdl.ren, &sw, &sh);
            if (sdl_inv_w() > 0 && ev.wheel.mouse_x >= sw - sdl_inv_w()) {
                sdl.inv.first += (e->wheel.y > 0) ? -3 : 3;
                sdl.dirty = TRUE; /* (kept in range when drawn) */
            } else {
                sdl_zoom(e->wheel.y > 0 ? 1 : -1);
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        /* the mouse never moves the hero: clicks only work menus and
           text windows (left picks an item or goes on, right closes) */
        SDL_Event ev = *e;
        struct sdl_win *w = sdl.ovl.w;
        float mx, my;

        (void) SDL_ConvertEventToRenderCoordinates(sdl.ren, &ev);
        if (sdl.es.mode >= 0 && !w) { /* the end screens' button */
            if (ev.button.button == SDL_BUTTON_LEFT && sdl.es.hot)
                sdl.es.down = TRUE, sdl.dirty = TRUE;
            break;
        }
        if (sdl.cs.mode >= 0 && !w) { /* the name or character screen */
            int g, i;

            if (ev.button.button != SDL_BUTTON_LEFT)
                break;
            i = sdl_cs_hit(ev.button.x, ev.button.y, &g);
            sdl.cs.dg = sdl.cs.di = -1;
            if (g == SDL_CS_BOX) {
                sdl.cs.fg = SDL_CS_BOX;
            } else if (g >= 0 && g < FXV_CS_NGRP) {
                /* a choice takes at once, if it fits */
                if (fxv_cs_pick(g, i)) {
                    sdl.cs.fg = sdl.cs.lastgrp = g;
                    if (g != FXV_CS_ROLE)
                        sdl.cs.lastmid = g;
                }
            } else if (g == SDL_CS_BTN || g == SDL_CS_SND) {
                sdl.cs.dg = g, sdl.cs.di = i;
            }
            sdl.dirty = TRUE;
            break;
        }
        if (sdl.title.up && !w) { /* a title screen button, pressed */
            if (ev.button.button == SDL_BUTTON_LEFT) {
                sdl.title.down = sdl_title_hit(ev.button.x, ev.button.y);
                sdl.dirty = TRUE;
            }
            break;
        }
        if (sdl_bar_mouse(&ev)) /* the menu bar first */
            break;
        if (!w) {
            /* an action button types its key, unless the game is
               asking something */
            int b = sdl_btn_at(ev.button.x, ev.button.y);

            if (b >= 0 && ev.button.button == SDL_BUTTON_LEFT
                && !sdl.prompt)
                sdl_push_key(sdl_btn[b].key);
            break;
        }
        mx = ev.button.x, my = ev.button.y;
        if (ev.button.button == SDL_BUTTON_RIGHT) {
            sdl_push_key('\033');
            break;
        }
        if (ev.button.button != SDL_BUTTON_LEFT || mx < sdl.ovl.bx
            || mx >= sdl.ovl.bx + sdl.ovl.bw || my < sdl.ovl.by
            || my >= sdl.ovl.by + sdl.ovl.bh)
            break;
        if (sdl.ovl.how == PICK_NONE) {
            sdl_push_key(' ');
        } else {
            int i = sdl_ovl_item_at(mx, my);

            if (i >= 0 && w->items[i].acc)
                sdl_push_key(w->items[i].acc);
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (sdl.es.mode >= 0 && !sdl.ovl.w
            && e->button.button == SDL_BUTTON_LEFT) {
            if (sdl.es.down) {
                sdl.es.down = FALSE, sdl.dirty = TRUE;
                if (sdl.es.hot)
                    sdl_push_key('\n');
            }
            break;
        }
        if (sdl.cs.mode >= 0 && !sdl.ovl.w
            && e->button.button == SDL_BUTTON_LEFT) {
            /* a button: chosen on letting go over it */
            SDL_Event ev = *e;
            int g, at, wasg = sdl.cs.dg, was = sdl.cs.di;

            (void) SDL_ConvertEventToRenderCoordinates(sdl.ren, &ev);
            at = sdl_cs_hit(ev.button.x, ev.button.y, &g);
            sdl.cs.dg = sdl.cs.di = -1, sdl.dirty = TRUE;
            if (wasg == SDL_CS_BTN && g == SDL_CS_BTN && was == at)
                sdl_push_key(SDL_CSK_BTN + at);
            else if (wasg == SDL_CS_SND && g == SDL_CS_SND)
                sdl_push_key(SDL_CSK_MUSIC);
            break;
        }
        if (sdl.title.up && !sdl.ovl.w
            && e->button.button == SDL_BUTTON_LEFT) {
            /* a title screen button: chosen on letting go over it */
            SDL_Event ev = *e;
            int at, was = sdl.title.down;

            (void) SDL_ConvertEventToRenderCoordinates(sdl.ren, &ev);
            at = sdl_title_hit(ev.button.x, ev.button.y);
            sdl.title.down = -1, sdl.dirty = TRUE;
            if (was >= 0 && was == at)
                sdl_push_key(at == FXV_TITLE_PLAY ? 'p'
                             : at == FXV_TITLE_ABOUT ? 'a'
                             : at == FXV_TITLE_EXIT ? 'x' : 'm');
        }
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        /* the item under the mouse is highlighted */
        SDL_Event ev = *e;
        int i;

        (void) SDL_ConvertEventToRenderCoordinates(sdl.ren, &ev);
        if (sdl.es.mode >= 0 && !sdl.ovl.w) { /* the button lights up */
            boolean hot = CS_IN(sdl.es.btn, ev.motion.x, ev.motion.y);

            if (hot != sdl.es.hot)
                sdl.es.hot = hot, sdl.dirty = TRUE;
            break;
        }
        if (sdl.cs.mode >= 0 && !sdl.ovl.w) { /* what's pointed at */
            int g;

            i = sdl_cs_hit(ev.motion.x, ev.motion.y, &g);
            if (g != sdl.cs.hg || i != sdl.cs.hi) {
                sdl.cs.hg = g, sdl.cs.hi = i;
                if (g == SDL_CS_BTN && sdl.cs.dg < 0) /* as the title's */
                    sdl.cs.fg = SDL_CS_BTN, sdl.cs.fb = i;
                sdl.dirty = TRUE;
            }
            break;
        }
        if (sdl.title.up && !sdl.ovl.w) { /* a title button lights up */
            i = sdl_title_hit(ev.motion.x, ev.motion.y);
            if (i != sdl.title.hot) {
                sdl.title.hot = i;
                if (i >= 0 && i < FXV_TITLE_NBTN)
                    sdl.title.focus = i;
                sdl.dirty = TRUE;
            }
            break;
        }
        if (sdl_bar_mouse(&ev)) /* the menu bar first */
            break;
        i = sdl.ovl.w ? -1 : sdl_btn_at(ev.motion.x, ev.motion.y);
        if (i != sdl_btn_hot) /* the action button under the mouse */
            sdl_btn_hot = i, sdl.dirty = TRUE;
        if (!sdl.ovl.w)
            break;
        i = sdl_ovl_item_at(ev.motion.x, ev.motion.y);
        if (i != sdl.ovl.hover)
            sdl.ovl.hover = i, sdl.dirty = TRUE;
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (sdl.skip_text) {
            sdl.skip_text = FALSE;
            break;
        }
        if (e->text.text && (unsigned char) e->text.text[0] < 0x80
            && e->text.text[0])
            sdl_push_key(e->text.text[0]);
        break;
    case SDL_EVENT_KEY_DOWN: {
        SDL_Keycode k = e->key.key;
        SDL_Keymod m = e->key.mod;
        boolean numlock = (m & SDL_KMOD_NUM) != 0;

        sdl.skip_text = FALSE;
        if ((sdl.cs.mode >= 0 || sdl.es.mode >= 0) && !sdl.ovl.w) {
            int c = 0;

            if ((m & SDL_KMOD_CTRL) && k == SDLK_M) {
                c = SDL_CSK_MUSIC;
            } else if ((m & SDL_KMOD_CTRL) && k == SDLK_BACKSPACE) {
                c = SDL_CSK_CLEAR;
            } else if ((m & SDL_KMOD_CTRL) && k == SDLK_V) {
                /* paste a name */
                char *t = SDL_GetClipboardText(), *q;

                for (q = t; q && *q; q++)
                    if (*q >= ' ' && *q <= '~')
                        sdl_push_key(*q);
                SDL_free(t);
                sdl.skip_text = TRUE;
                break;
            } else {
                switch (k) {
                case SDLK_UP: c = SDL_CSK_UP; break;
                case SDLK_DOWN: c = SDL_CSK_DOWN; break;
                case SDLK_LEFT: c = SDL_CSK_LEFT; break;
                case SDLK_RIGHT: c = SDL_CSK_RIGHT; break;
                case SDLK_HOME: c = SDL_CSK_HOME; break;
                case SDLK_END: c = SDL_CSK_END; break;
                case SDLK_PAGEUP: c = SDL_CSK_PGUP; break;
                case SDLK_PAGEDOWN: c = SDL_CSK_PGDN; break;
                case SDLK_TAB:
                    c = (m & SDL_KMOD_SHIFT) ? SDL_CSK_BACKTAB : SDL_CSK_TAB;
                    break;
                case SDLK_ESCAPE: c = '\033'; break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER: c = '\n'; break;
                case SDLK_BACKSPACE: c = '\b'; break;
                default: break; /* letters come as text */
                }
            }
            if (c) {
                sdl_push_key(c);
                if (c == SDL_CSK_MUSIC || c == SDL_CSK_CLEAR)
                    sdl.skip_text = TRUE;
            }
            break;
        }
        if (sdl.title.up && !sdl.ovl.w) {
            int f = sdl.title.focus;

            if ((m & SDL_KMOD_CTRL) && k == SDLK_M) {
                sdl_push_key('m'), sdl.skip_text = TRUE;
                break;
            }
            switch (k) {
            case SDLK_LEFT:
            case SDLK_UP:
                f = (f + FXV_TITLE_NBTN - 1) % FXV_TITLE_NBTN;
                break;
            case SDLK_RIGHT:
            case SDLK_DOWN:
            case SDLK_TAB:
                f = (f + 1) % FXV_TITLE_NBTN;
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                sdl_push_key('\n');
                break;
            default:
                break; /* letters come as text */
            }
            if (f != sdl.title.focus)
                sdl.title.focus = f, sdl.dirty = TRUE;
            break;
        }
        if (sdl.bar_open >= 0) {
            /* a drop-down is open: Escape (or any key) just closes it */
            sdl.bar_open = sdl.bar_item = -1;
            sdl.dirty = TRUE;
            if (k == SDLK_ESCAPE)
                break;
        }
        /* the map's zoom: Ctrl+= in, Ctrl+- out, Ctrl+0 automatic (also
           on the keypad); these never reach the game */
        if (m & SDL_KMOD_CTRL) {
            int dir = 2; /* not a zoom key */

            if (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS)
                dir = 1;
            else if (k == SDLK_MINUS || k == SDLK_KP_MINUS)
                dir = -1;
            else if (k == SDLK_0 || k == SDLK_KP_0)
                dir = 0;
            if (dir != 2) {
                sdl_zoom(dir);
                sdl.skip_text = TRUE;
                break;
            }
        }
        if ((m & SDL_KMOD_CTRL) && k >= 'a' && k <= 'z') {
            sdl_push_key(C((int) k));
            break;
        }
        /* the text's size: Alt+= larger, Alt+- smaller (not commands) */
        if ((m & SDL_KMOD_ALT)
            && (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS
                || k == SDLK_MINUS || k == SDLK_KP_MINUS)) {
            sdl_text_size(sdl.text_px
                          * ((k == SDLK_MINUS || k == SDLK_KP_MINUS) ? 0.9f
                                                                     : 1.1f));
            /* kept as fx_text_size (and so in nethackmodern.rc) */
            iflags.fx_text_size = sdl.text_set = (int) (sdl.text_px + 0.5f);
            sdl.skip_text = TRUE;
            break;
        }
        if ((m & SDL_KMOD_ALT) && k >= 'a' && k <= 'z') {
            sdl_push_key(M((int) k));
            sdl.skip_text = TRUE; /* in case a character comes too */
            break;
        }
        switch (k) {
        case SDLK_ESCAPE: sdl_push_key('\033'); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: sdl_push_key('\n'); break;
        case SDLK_BACKSPACE: sdl_push_key('\b'); break;
        case SDLK_TAB: sdl_push_key('\t'); break;
        case SDLK_UP: sdl_push_key(sdl_dir_key(0, -1)); break;
        case SDLK_DOWN: sdl_push_key(sdl_dir_key(0, 1)); break;
        case SDLK_LEFT: sdl_push_key(sdl_dir_key(-1, 0)); break;
        case SDLK_RIGHT: sdl_push_key(sdl_dir_key(1, 0)); break;
        case SDLK_HOME: sdl_push_key(sdl_dir_key(-1, -1)); break;
        case SDLK_PAGEUP: sdl_push_key(sdl_dir_key(1, -1)); break;
        case SDLK_END: sdl_push_key(sdl_dir_key(-1, 1)); break;
        case SDLK_PAGEDOWN: sdl_push_key(sdl_dir_key(1, 1)); break;
        default:
            /* the keypad without Num Lock moves; with it, the digits
               come as text */
            if (!numlock && k >= SDLK_KP_1 && k <= SDLK_KP_9) {
                int n = (int) (k - SDLK_KP_1); /* 0..8, 1 is bottom left */

                sdl_push_key(sdl_dir_key(n % 3 - 1, 1 - n / 3));
            }
            break;
        }
        break;
    }
    }
}

static boolean
sdl_effects_moving(void)
{
    return (sdl.animating || fxv.nparts || fxv.nfloaters || fxv.nanims
            || fxv.shake_dur);
}

/* wait for a key, drawing (and animating) meanwhile */
static int
sdl_wait_key(void)
{
    SDL_Event e;
    int c;

    while (sdl.kq_head == sdl.kq_tail) {
        if (sdl.dirty || sdl_effects_moving())
            sdl_render();
        /* while idle, redraw now and then anyway, in case the desktop
           lost a frame */
        if (SDL_WaitEventTimeout(&e, sdl_effects_moving() ? 16 : 500))
            sdl_handle_event(&e);
        else
            sdl.dirty = TRUE;
        while (SDL_PollEvent(&e))
            sdl_handle_event(&e);
    }
    c = sdl.keyq[sdl.kq_head];
    sdl.kq_head = (sdl.kq_head + 1) % SDL_KEYQ;
    return c;
}

/* ---- drawing ------------------------------------------------------- */

static void
sdl_tile_from(SDL_Texture *sheet, int tile, float x, float y, float size)
{
    SDL_FRect src, dst;

    if (tile < 0 || !sheet)
        return;
    src.x = (float) ((tile % sdl.tiles_across) * SDL_TILE);
    src.y = (float) ((tile / sdl.tiles_across) * SDL_TILE);
    src.w = src.h = (float) SDL_TILE;
    dst.x = x, dst.y = y, dst.w = dst.h = size;
    SDL_RenderTexture(sdl.ren, sheet, &src, &dst);
}

/* a creature or object, with its background showing round it */
static void
sdl_tile(int tile, float x, float y, float size)
{
    sdl_tile_from(sdl.tiles, tile, x, y, size);
}

/* a floor or other background, solid (the key colour is its colour) */
static void
sdl_tile_solid(int tile, float x, float y, float size)
{
    sdl_tile_from(sdl.tiles_solid ? sdl.tiles_solid : sdl.tiles, tile, x, y,
                  size);
}

/* zoom steps: a map square's size on screen, in pixels */
static const int sdl_zooms[] = { 12, 16, 20, 24, 32, 40, 48, 64, 80 };
#define SDL_NZOOM ((int) (sizeof sdl_zooms / sizeof sdl_zooms[0]))

/* keep the hero in view along one axis, as NetHackW does: the view only
   moves when the hero comes within the scroll margin of its edge (or
   is re-centred, after a zoom or a new level); *v is the first square
   shown, page how many fit, lo..hi the squares there are */
static void
sdl_follow(int *v, int hero, int page, int lo, int hi)
{
    int m = iflags.wc_scroll_margin > 0 ? iflags.wc_scroll_margin : 5;
    int amount = max(1, iflags.wc_scroll_amount);

    m = min(m, max(1, page / 3));
    if (sdl.recentre)
        *v = hero - page / 2;
    else if (hero < *v + m)
        *v = hero - (m + amount - 1);
    else if (hero > *v + page - 1 - m)
        *v = hero - page + 1 + (m + amount - 1);
    *v = max(lo, min(*v, hi - page));
}

/* where the map goes: top and bottom of its area, the size of a square,
   and where square 0,0 is drawn. A level that fits is centred and stays
   put; one that doesn't follows the hero by its edges. */
static void
sdl_layout(int *top, int *bottom, int *ts, int *ox, int *oy)
{
    int w, h, aw, ah, z;

    SDL_GetRenderOutputSize(sdl.ren, &w, &h);
    *top = sdl_map_top();
    *bottom = h - sdl_status_h(); /* the character panel */
    aw = max(1, w - sdl_inv_w()), ah = max(1, *bottom - *top);
    if (sdl.zoom >= 0) {
        z = sdl.zoom;
    } else {
        /* automatic: the largest step that shows the whole level */
        for (z = SDL_NZOOM - 1; z > 0; z--)
            if ((COLNO - 1) * sdl_zooms[z] <= aw
                && ROWNO * sdl_zooms[z] <= ah)
                break;
    }
    if (z != sdl.zoom_used)
        sdl.zoom_used = z, sdl.recentre = TRUE;
    *ts = sdl_zooms[z];
    /* column 0 is never part of the level */
    if ((COLNO - 1) * *ts <= aw) {
        *ox = (aw - (COLNO - 1) * *ts) / 2 - *ts;
    } else {
        sdl_follow(&sdl.viewx, sdl.herox, aw / *ts, 1, COLNO);
        *ox = -sdl.viewx * *ts;
    }
    if (ROWNO * *ts <= ah) {
        *oy = *top + (ah - ROWNO * *ts) / 2;
    } else {
        sdl_follow(&sdl.viewy, sdl.heroy, ah / *ts, 0, ROWNO);
        *oy = *top - sdl.viewy * *ts;
    }
    sdl.recentre = FALSE;
}

/* zoom in (+1), out (-1), or back to automatic (0) */
static void
sdl_zoom(int dir)
{
    if (!dir)
        sdl.zoom = -1;
    else
        sdl.zoom = max(0, min(SDL_NZOOM - 1,
                              (sdl.zoom >= 0 ? sdl.zoom : sdl.zoom_used)
                                  + dir));
    /* kept as fx_map_zoom (and so in nethackmodern.rc) */
    iflags.fx_map_zoom = sdl.zoom_set = (sdl.zoom < 0) ? 0
                                                       : sdl_zooms[sdl.zoom];
    sdl.recentre = TRUE;
    sdl.dirty = TRUE;
}

/* the zoom step nearest a square size in pixels (0: automatic) */
static int
sdl_zoom_for(int px)
{
    int z, best = 0;

    if (px <= 0)
        return -1;
    for (z = 1; z < SDL_NZOOM; z++)
        if (abs(sdl_zooms[z] - px) < abs(sdl_zooms[best] - px))
            best = z;
    return best;
}

/* follow the fx_ options the player may have changed in the options
   menu (or that the keys changed): text size, zoom, window size */
static void
sdl_sync_options(void)
{
    if (iflags.fx_text_size != sdl.text_set) {
        sdl.text_set = iflags.fx_text_size;
        sdl_text_size(sdl.text_set ? (float) sdl.text_set
                                   : sdl.auto_text_px);
    }
    if (iflags.fx_map_zoom != sdl.zoom_set) {
        sdl.zoom_set = iflags.fx_map_zoom;
        sdl.zoom = sdl_zoom_for(sdl.zoom_set);
        sdl.recentre = TRUE;
    }
    if (iflags.fx_window_w && iflags.fx_window_h && sdl.window) {
        int w, h;

        SDL_GetWindowSize(sdl.window, &w, &h);
        if (w != iflags.fx_window_w || h != iflags.fx_window_h)
            (void) SDL_SetWindowSize(sdl.window, iflags.fx_window_w,
                                     iflags.fx_window_h);
    }
}

/* ---- the shared effects (win/share/fxview.c), drawn with SDL ----- */

/* a soft-edged white disc, tinted and stretched for every dot */
static SDL_Texture *
sdl_disc(void)
{
    enum { D = 64 };
    static Uint32 px[D * D];
    int x, y;

    if (sdl.disc)
        return sdl.disc;
    for (y = 0; y < D; y++)
        for (x = 0; x < D; x++) {
            double dx = x + 0.5 - D / 2.0, dy = y + 0.5 - D / 2.0;
            double d = sqrt(dx * dx + dy * dy), a = (D / 2.0 - d) * 1.5;
            Uint32 al = (Uint32) (a <= 0 ? 0 : a >= 1 ? 255 : a * 255);

            px[y * D + x] = (al << 24) | 0xffffffU; /* ARGB8888 */
        }
    sdl.disc = SDL_CreateTexture(sdl.ren, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STATIC, D, D);
    if (sdl.disc) {
        (void) SDL_UpdateTexture(sdl.disc, NULL, px, D * 4);
        (void) SDL_SetTextureScaleMode(sdl.disc, SDL_SCALEMODE_LINEAR);
    }
    return sdl.disc;
}

/* a filled dot of colour c; 'light' adds it onto what's there */
static void
sdl_dot(float x, float y, float r, fxv_color c, boolean light)
{
    SDL_Texture *t = sdl_disc();
    SDL_FRect dst;

    if (!t || r <= 0)
        return;
    dst.x = x - r, dst.y = y - r, dst.w = dst.h = 2 * r + 1;
    (void) SDL_SetTextureColorMod(t, (Uint8) FXV_R(c), (Uint8) FXV_G(c),
                                  (Uint8) FXV_B(c));
    (void) SDL_SetTextureBlendMode(t, light ? SDL_BLENDMODE_ADD
                                            : SDL_BLENDMODE_BLEND);
    (void) SDL_RenderTexture(sdl.ren, t, NULL, &dst);
}

/* the fonts for floating text, sized to the map's tiles: 0 numbers,
   1 big hits and "Level N!", 2 status names and sleeping z's */
static TTF_Font *
sdl_fx_font(int which, int ts)
{
    static const float scale[3] = { 0.55f, 0.80f, 0.42f };
    int i;

    if (sdl.fx_ts != ts) {
        for (i = 0; i < 3; i++) {
            if (sdl.fxfont[i])
                TTF_CloseFont(sdl.fxfont[i]);
            sdl.fxfont[i] = TTF_OpenFont(sdl.fontpath,
                                         max(8.0f, scale[i] * ts));
            if (sdl.fxfont[i] && i != 2)
                TTF_SetFontStyle(sdl.fxfont[i], TTF_STYLE_BOLD);
        }
        sdl.fx_ts = ts;
    }
    return sdl.fxfont[which] ? sdl.fxfont[which] : sdl.font;
}

/* text in a given font: centred on cx,cy (or top left at it), with a
   black outline 'o' pixels thick if o > 0, added on as light if asked */
static void
sdl_fx_text(TTF_Font *f, float cx, float cy, boolean centred,
            const char *s, fxv_color c, int o, boolean light)
{
    SDL_Color col, black = { 0, 0, 0, 255 };
    SDL_Surface *surf;
    SDL_Texture *tex, *out = 0;
    SDL_FRect dst;
    int dx, dy;

    if (!s || !*s || !f)
        return;
    col.r = (Uint8) FXV_R(c), col.g = (Uint8) FXV_G(c);
    col.b = (Uint8) FXV_B(c), col.a = 255;
    if (!(surf = TTF_RenderText_Blended(f, s, 0, col)))
        return;
    tex = SDL_CreateTextureFromSurface(sdl.ren, surf);
    dst.w = (float) surf->w, dst.h = (float) surf->h;
    SDL_DestroySurface(surf);
    if (!tex)
        return;
    dst.x = centred ? cx - dst.w / 2 : cx;
    dst.y = centred ? cy - dst.h / 2 : cy;
    if (o > 0 && (surf = TTF_RenderText_Blended(f, s, 0, black)) != 0) {
        out = SDL_CreateTextureFromSurface(sdl.ren, surf);
        SDL_DestroySurface(surf);
        for (dx = -o; out && dx <= o; dx += o)
            for (dy = -o; dy <= o; dy += o) {
                SDL_FRect r = dst;

                if (!dx && !dy)
                    continue;
                r.x += dx, r.y += dy;
                (void) SDL_RenderTexture(sdl.ren, out, NULL, &r);
            }
        if (out)
            SDL_DestroyTexture(out);
    }
    if (light)
        (void) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_ADD);
    (void) SDL_RenderTexture(sdl.ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
}

/* a tile drawn see-through */
static void
sdl_tile_alpha(int tile, float x, float y, float size, int alpha)
{
    if (!sdl.tiles)
        return;
    (void) SDL_SetTextureAlphaMod(sdl.tiles,
                                  (Uint8) max(0, min(255, alpha)));
    sdl_tile(tile, x, y, size);
    (void) SDL_SetTextureAlphaMod(sdl.tiles, 255);
}

/* the floor tile under a creature on x,y; for plain lit room floor the
   core sends no background glyph, so it's the room-floor tile */
static int
sdl_floor_tile(int x, int y)
{
    glyph_info under;

    if (sdl.bkmap[x][y].glyph != NO_GLYPH
        && sdl.bkmap[x][y].glyph != GLYPH_UNEXPLORED)
        return sdl.bkmap[x][y].gm.tileidx;
    map_glyphinfo(x, y, cmap_to_glyph(S_room), 0, &under);
    return under.gm.tileidx;
}

/* creatures seen though invisible: floor back under them, and they're
   laid on top partly see-through (moving ones are drawn as sprites) */
static void
sdl_draw_translucent(int ox, int oy, int ts, fxv_ms now)
{
    int x, y;

    if (!iflags.fx_auras || Is_rogue_level(&u.uz))
        return;
    for (x = 1; x < COLNO; x++)
        for (y = 0; y < ROWNO; y++) {
            float px = (float) (ox + x * ts), py = (float) (oy + y * ts);

            if (!(sdl.map[x][y].fxauras & FXA_INVIS)
                || !glyph_is_monster(sdl.map[x][y].glyph)
                || fxv_square_busy(x, y, now))
                continue;
            sdl_tile_solid(sdl_floor_tile(x, y), px, py, (float) ts);
            sdl_tile_alpha(sdl.map[x][y].gm.tileidx, px, py, (float) ts,
                           FXV_INVIS_ALPHA);
        }
}

/* creatures drawn off their squares: lunges, shakes, slides, fades */
static void
sdl_draw_sprites(int ox, int oy, int ts, fxv_ms now)
{
    int i;

    for (i = 0; i < fxv.nanims; i++) {
        struct fxv_anim *a = &fxv.anims[i];
        float fx, fy, cx, cy;
        int alpha;

        if (!fxv_anim_pose(a, now, &fx, &fy, &alpha))
            continue;
        cx = (float) (ox + a->x * ts), cy = (float) (oy + a->y * ts);
        if (a->kind == FXM_GHOST || a->kind == FXM_DEATH) {
            sdl_tile_alpha(a->tile, cx + fx * ts, cy + fy * ts, (float) ts,
                           alpha);
            continue;
        }
        /* the floor under it, so it doesn't leave a copy behind */
        sdl_tile_solid(sdl_floor_tile(a->x, a->y), cx, cy, (float) ts);
        if ((sdl.map[a->x][a->y].fxauras & FXA_INVIS) && iflags.fx_auras)
            sdl_tile_alpha(a->tile, cx + fx * ts, cy + fy * ts, (float) ts,
                           FXV_INVIS_ALPHA);
        else
            sdl_tile(a->tile, cx + fx * ts, cy + fy * ts, (float) ts);
    }
}

/* the shade layer, stretched over the map */
static void
sdl_draw_lighting(int ox, int oy, int ts, fxv_ms now, boolean *flicker)
{
    const uint32_t *layer;
    boolean changed, fresh = FALSE;
    SDL_FRect src, dst;

    if (!fxv_light_layer(now, &layer, &changed, flicker))
        return;
    if (!sdl.light) {
        sdl.light = SDL_CreateTexture(sdl.ren, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      FXV_LIGHT_W, FXV_LIGHT_H);
        if (!sdl.light)
            return;
        (void) SDL_SetTextureBlendMode(sdl.light,
                                       SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        (void) SDL_SetTextureScaleMode(sdl.light, SDL_SCALEMODE_LINEAR);
        fresh = TRUE;
    }
    if (changed || fresh)
        (void) SDL_UpdateTexture(sdl.light, NULL, layer, FXV_LIGHT_W * 4);
    /* column 0 is never part of the level */
    src.x = (float) FXV_LIGHT_S, src.y = 0;
    src.w = (float) (FXV_LIGHT_W - FXV_LIGHT_S);
    src.h = (float) FXV_LIGHT_H;
    dst.x = (float) (ox + ts), dst.y = (float) oy;
    dst.w = (float) ((COLNO - 1) * ts), dst.h = (float) (ROWNO * ts);
    (void) SDL_RenderTexture(sdl.ren, sdl.light, &src, &dst);
}

/* the gold corner marks round the hero's square, moving with the hero */
static void
sdl_draw_hero_frame(int ox, int oy, int ts, fxv_ms now)
{
    int r[8][4], k, j, x, y;
    float sx, sy;

    if (!isok(u.ux, u.uy))
        return;
    fxv_hero_slide(now, &sx, &sy);
    x = ox + u.ux * ts + (int) (sx * ts);
    y = oy + u.uy * ts + (int) (sy * ts);
    if (!fxv_hero_frame(x, y, ts, ts, 0, r))
        return;
    for (k = iflags.fx_hero_frame_light ? 1 : 0; k < 2; k++) {
        /* a dark outline one pixel wider, then the gold (the light
           version: no outline, half see-through) */
        fxv_color c = iflags.fx_hero_frame_light ? FXV_FRAME_LIGHT
                      : k ? FXV_FRAME_GOLD : FXV_FRAME_EDGE;

        (void) fxv_hero_frame(x, y, ts, ts, k ? 0 : 1, r);
        for (j = 0; j < 8; j++)
            sdl_fill(r[j][0], r[j][1], r[j][2] - r[j][0], r[j][3] - r[j][1],
                     (Uint8) FXV_R(c), (Uint8) FXV_G(c), (Uint8) FXV_B(c),
                     iflags.fx_hero_frame_light ? 150 : 255);
    }
}

/* the glow primitives the shared aura painter draws through */
static int sdl_glow_ts;

static void
sdl_glow_dot(void *ctx, int x, int y, int r, fxv_color c)
{
    nhUse(ctx);
    sdl_dot((float) x, (float) y, (float) r, c, TRUE);
}

static void
sdl_glow_text(void *ctx, int x, int y, const char *s, fxv_color c)
{
    nhUse(ctx);
    sdl_fx_text(sdl_fx_font(2, sdl_glow_ts), (float) x, (float) y, FALSE,
                s, c, 0, TRUE);
}

static int
sdl_draw_auras(int ox, int oy, int ts, fxv_ms now)
{
    struct fxv_glow g;

    (void) memset(&g, 0, sizeof g);
    g.dot = sdl_glow_dot, g.text = sdl_glow_text;
    sdl_glow_ts = ts;
    return fxv_paint_auras(&g, now, ox, oy, ts, ts);
}

static void
sdl_draw_particles(int ox, int oy, int ts, fxv_ms now)
{
    int i, pass;

    /* a faint glow added on as light, then a solid core on top so it
       shows even over a bright ray */
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < fxv.nparts; i++) {
            struct fxv_part *p = &fxv.parts[i];
            float sx, sy, k, rad;
            fxv_color c;

            fxv_part_at(p, now, &sx, &sy, &k);
            if (k < 0 || k >= 1)
                continue;
            rad = p->size * ts * (1.0f - 0.55f * k);
            c = fxv_lerp_color(p->c0, p->c1, k);
            if (!pass)
                sdl_dot(ox + sx * ts, oy + sy * ts, rad * 2.2f,
                        fxv_lerp_color(FXV_RGB(0, 0, 0), c, 0.35f), TRUE);
            else
                sdl_dot(ox + sx * ts, oy + sy * ts, max(1.0f, rad), c,
                        FALSE);
        }
}

/* damage numbers, status names and the like, rising and fading */
static void
sdl_draw_floaters(int ox, int oy, int ts, fxv_ms now)
{
    int i, o = max(1, ts / 24);

    for (i = 0; i < fxv.nfloaters; i++) {
        struct fxv_floater *f = &fxv.floaters[i];
        float cx, cy, bright;
        char buf[32];
        fxv_color c = f->color;

        if (!fxv_floater_at(f, now, &cx, &cy, &bright))
            continue;
        /* fade by darkening, as NetHackW does */
        if (bright < 1.0f)
            c = FXV_RGB((int) (FXV_R(c) * bright), (int) (FXV_G(c) * bright),
                        (int) (FXV_B(c) * bright));
        fxv_floater_text(f, buf, sizeof buf);
        sdl_fx_text(sdl_fx_font(f->big ? 1 : f->text[0] ? 2 : 0, ts),
                    ox + cx * ts, oy + cy * ts, TRUE, buf, c, o, FALSE);
    }
}

/* a missile in flight on x,y pointing where it's going (fx_turn_missiles):
   its floor, then the tile turned; FALSE if it isn't one, or doesn't
   need turning (round things, or already pointing that way) */
static boolean
sdl_draw_missile(int x, int y, float px, float py, int ts)
{
    /* each tile's measurement, made the first time it flies */
    static struct sdl_missile_cache {
        char state; /* 0 not yet, 1 measured, 2 not a turnable shape */
        struct fxv_missile m;
    } *cache;
    static int ncache;
    const glyph_info *fg = &sdl.map[x][y];
    int tile = fg->gm.tileidx, dx, dy;
    double rot, scale;
    struct sdl_missile_cache *c;

    if (!iflags.fx_turn_missiles || !sdl.tiles_px
        || !glyph_is_object(fg->glyph) || !fx_flight_at(x, y, &dx, &dy))
        return FALSE;
    if (!cache) {
        ncache = (sdl.tiles_px->w / SDL_TILE) * (sdl.tiles_px->h / SDL_TILE);
        cache = (struct sdl_missile_cache *) alloc(ncache * sizeof *cache);
        (void) memset(cache, 0, ncache * sizeof *cache);
    }
    if (tile < 0 || tile >= ncache)
        return FALSE;
    c = &cache[tile];
    if (!c->state) {
        uint32_t px32[SDL_TILE * SDL_TILE];
        unsigned char mask[SDL_TILE * SDL_TILE];
        const Uint8 *row;
        int i, j;

        /* the tile's pixels, out of the solid sheet */
        for (j = 0; j < SDL_TILE; j++) {
            row = (const Uint8 *) sdl.tiles_px->pixels
                  + ((tile / sdl.tiles_across) * SDL_TILE + j)
                        * sdl.tiles_px->pitch
                  + (tile % sdl.tiles_across) * SDL_TILE * 4;
            for (i = 0; i < SDL_TILE; i++)
                px32[j * SDL_TILE + i] = ((const Uint32 *) row)[i];
        }
        c->state = fxv_missile_shape(px32, (const uint32_t *) 0, SDL_TILE,
                                     SDL_TILE, TRUE, mask, &c->m)
                       ? 1 : 2;
    }
    if (c->state != 1 || !fxv_missile_turn(&c->m, dx, dy, &rot, &scale))
        return FALSE;
    {
        /* the floor under it, then the picture turned about its middle,
           which lands on the middle of the square */
        const glyph_info *bg = &sdl.bkmap[x][y];
        SDL_FRect src, dst;
        SDL_FPoint mid;
        float k = (float) (ts * scale) / SDL_TILE;

        if (bg->glyph != NO_GLYPH && bg->glyph != GLYPH_UNEXPLORED)
            sdl_tile_solid(bg->gm.tileidx, px, py, (float) ts);
        else
            sdl_tile_solid(sdl_floor_tile(x, y), px, py, (float) ts);
        src.x = (float) ((tile % sdl.tiles_across) * SDL_TILE);
        src.y = (float) ((tile / sdl.tiles_across) * SDL_TILE);
        src.w = src.h = (float) SDL_TILE;
        mid.x = (float) (c->m.cx + 0.5) * k;
        mid.y = (float) (c->m.cy + 0.5) * k;
        dst.w = dst.h = SDL_TILE * k;
        dst.x = px + ts / 2.0f - mid.x;
        dst.y = py + ts / 2.0f - mid.y;
        (void) SDL_RenderTextureRotated(sdl.ren, sdl.tiles, &src, &dst,
                                        rot * 180.0 / 3.14159265358979,
                                        &mid, SDL_FLIP_NONE);
    }
    return TRUE;
}

static void
sdl_draw_map(void)
{
    int top, bottom, ts, ox, oy, x, y, w, h;
    SDL_Rect clip;
    Uint64 now = SDL_GetTicks();
    boolean flicker = FALSE;
    int auras;

    sdl_layout(&top, &bottom, &ts, &ox, &oy);
    {
        /* the whole view shakes; messages and status stay still */
        float sx, sy;

        if (fxv_shake_at((fxv_ms) now, &sx, &sy))
            ox += (int) (sx * ts), oy += (int) (sy * ts);
    }
    SDL_GetRenderOutputSize(sdl.ren, &w, &h);
    w -= sdl_inv_w(); /* the inventory panel is to the right */
    clip.x = 0, clip.y = top, clip.w = w, clip.h = bottom - top;
    SDL_SetRenderClipRect(sdl.ren, &clip);
    for (x = 1; x < COLNO; x++)
        for (y = 0; y < ROWNO; y++) {
            float px = (float) (ox + x * ts), py = (float) (oy + y * ts);
            const glyph_info *fg = &sdl.map[x][y], *bg = &sdl.bkmap[x][y];

            if (px + ts < 0 || py + ts < top || px > w || py > bottom)
                continue;
            /* as NetHackW: a background is solid and what's on it has
               its key colour cut out; with no background (plain lit
               floor) the tile itself is drawn solid */
            if (sdl_draw_missile(x, y, px, py, ts)) {
                ; /* a missile in flight, pointing where it's going */
            } else if (bg->glyph != NO_GLYPH
                       && bg->glyph != GLYPH_UNEXPLORED) {
                sdl_tile_solid(bg->gm.tileidx, px, py, (float) ts);
                if (fg->glyph != NO_GLYPH)
                    sdl_tile(fg->gm.tileidx, px, py, (float) ts);
            } else if (fg->glyph != NO_GLYPH) {
                sdl_tile_solid(fg->gm.tileidx, px, py, (float) ts);
            }
            {
                /* a hurt creature's health bar (shared shape, fxview.c) */
                int bar[4], fill[4];
                fxv_color c;

                if (fxv_health_bar(fg->fxhealth, (int) px, (int) py, ts, ts,
                                   bar, fill, &c)) {
                    sdl_fill(bar[0], bar[1], bar[2] - bar[0],
                             bar[3] - bar[1], 0, 0, 0, 255);
                    sdl_fill(fill[0], fill[1], fill[2] - fill[0],
                             fill[3] - fill[1], (Uint8) FXV_R(c),
                             (Uint8) FXV_G(c), (Uint8) FXV_B(c), 255);
                }
            }
        }
    /* where the level is, for the messages' shade */
    sdl.maprect.x = ox + ts, sdl.maprect.y = oy;
    sdl.maprect.w = (COLNO - 1) * ts, sdl.maprect.h = ROWNO * ts;
    /* the effects, in NetHackW's order */
    sdl_draw_translucent(ox, oy, ts, (fxv_ms) now);
    sdl_draw_sprites(ox, oy, ts, (fxv_ms) now);
    sdl_draw_lighting(ox, oy, ts, (fxv_ms) now, &flicker);
    sdl_draw_hero_frame(ox, oy, ts, (fxv_ms) now);
    auras = sdl_draw_auras(ox, oy, ts, (fxv_ms) now);
    sdl_draw_particles(ox, oy, ts, (fxv_ms) now);
    sdl_draw_floaters(ox, oy, ts, (fxv_ms) now);
    SDL_SetRenderClipRect(sdl.ren, NULL);
    /* lasting effects and flickering light keep moving by themselves */
    sdl.animating = (auras > 0 || flicker);
    (void) fxv_expire((fxv_ms) now);
}

/* NetHackW's messages on the map: in the corner fx_msg_pos picks,
   newest lowest, each outlined so it reads over any tiles; a soft shade
   behind them where they're over the level (or, with fx_msg_backdrop, a
   panel); older turns fade, then drop away */
static void
sdl_draw_msg_overlay(void)
{
    struct {
        SDL_Texture *tex;
        int w, h, x, y;
        fxv_color c;
    } ln[FXV_MSG_SHOW];
    static const SDL_Color white = { 255, 255, 255, 255 };
    TTF_Font *f = sdl.msgfont ? sdl.msgfont : sdl.font;
    int sw, sh, aw, top, bottom, margin, wmax, ol, gap, n = 0, i, col, y,
        tot = 0;
    SDL_Rect area, block;
    boolean attop;

    SDL_GetRenderOutputSize(sdl.ren, &sw, &sh);
    aw = sw - sdl_inv_w(), top = sdl_map_top();
    bottom = sh - sdl_status_h();
    margin = max(10, sdl.lh / 2);
    wmax = max(40, min(aw * 3 / 5, aw - 2 * margin));
    ol = max(2, sdl.lh / 10); /* outline thickness */
    gap = max(2, sdl.lh / 8);
    attop = fxv_msg_place(&col);
    TTF_SetFontWrapAlignment(f, col == 1 ? TTF_HORIZONTAL_ALIGN_CENTER
                                : col == 2 ? TTF_HORIZONTAL_ALIGN_RIGHT
                                           : TTF_HORIZONTAL_ALIGN_LEFT);

    /* newest first: a question being asked, then the messages */
    for (i = sdl.nmsgs - 1 + (sdl.prompt ? 1 : 0);
         i >= max(0, sdl.nmsgs - SDL_MSGKEEP) && n < FXV_MSG_SHOW; i--) {
        const char *txt;
        fxv_color c = FXV_RGB(222, 226, 233);
        SDL_Surface *surf;

        if (i == sdl.nmsgs) {
            txt = sdl.prompt, c = FXV_RGB(229, 186, 96);
        } else {
            int slot = i % SDL_MSGKEEP;
            double k = fxv_msg_fade(svm.moves - sdl.msg_turn[slot],
                                    i >= sdl.msg_seen);

            if (k <= 0.0)
                break;
            txt = sdl.msgs[slot];
            if (sdl.msg_has_hl[slot])
                c = sdl.msg_hl[slot];
            c = FXV_RGB((int) (FXV_R(c) * k), (int) (FXV_G(c) * k),
                        (int) (FXV_B(c) * k));
        }
        if (!txt || !*txt)
            continue;
        surf = TTF_RenderText_Blended_Wrapped(f, txt, 0, white, wmax);
        if (!surf)
            continue;
        ln[n].tex = SDL_CreateTextureFromSurface(sdl.ren, surf);
        ln[n].w = surf->w, ln[n].h = surf->h, ln[n].c = c;
        SDL_DestroySurface(surf);
        if (ln[n].tex)
            tot += ln[n].h + (n ? gap : 0), n++;
    }
    if (!n)
        return;

    /* stacked up from the bottom line (the newest) */
    y = attop ? top + margin + tot : bottom - margin;
    for (i = 0; i < n; i++) {
        y -= ln[i].h;
        ln[i].y = y;
        ln[i].x = (col == 0) ? margin
                  : (col == 1) ? (aw - ln[i].w) / 2 : aw - margin - ln[i].w;
        y -= gap;
    }
    block.x = ln[0].x, block.y = ln[0].y, block.w = ln[0].w;
    block.h = ln[0].h;
    for (i = 1; i < n; i++) {
        SDL_Rect r;

        r.x = ln[i].x, r.y = ln[i].y, r.w = ln[i].w, r.h = ln[i].h;
        (void) SDL_GetRectUnion(&block, &r, &block);
    }
    area.x = 0, area.y = top, area.w = aw, area.h = bottom - top;
    SDL_SetRenderClipRect(sdl.ren, &area);

    if (iflags.fx_msg_backdrop) {
        /* a soft dark panel behind them */
        int pad = max(6, sdl.lh / 3);

        sdl_fill(block.x - pad, block.y - pad, block.w + 2 * pad,
                 block.h + 2 * pad, 24, 27, 33, 185);
    } else if (SDL_HasRectIntersection(&block, &sdl.maprect)) {
        /* a shade fading out all round, where they're over the level */
        int step = max(6, sdl.lh / 3), fade = 3 * margin, sx, sy;

        for (sy = block.y - fade; sy < block.y + block.h + fade; sy += step)
            for (sx = block.x - fade; sx < block.x + block.w + fade;
                 sx += step) {
                int cx = sx + step / 2, cy = sy + step / 2, a,
                    dx = (cx < block.x) ? block.x - cx
                         : (cx > block.x + block.w) ? cx - block.x - block.w
                                                    : 0,
                    dy = (cy < block.y) ? block.y - cy
                         : (cy > block.y + block.h) ? cy - block.y - block.h
                                                    : 0;

                if ((a = fxv_msg_shade(dx, dy, fade)) != 0)
                    sdl_fill(sx, sy, step, step,
                             (Uint8) FXV_R(FXV_MSG_SHADE),
                             (Uint8) FXV_G(FXV_MSG_SHADE),
                             (Uint8) FXV_B(FXV_MSG_SHADE), (Uint8) a);
            }
    }

    /* a soft shadow down and right, a round dark outline, the text */
    for (i = 0; i < n; i++) {
        SDL_FRect d;
        int dx, dy;

        d.w = (float) ln[i].w, d.h = (float) ln[i].h;
        (void) SDL_SetTextureColorMod(ln[i].tex, 0, 0, 0);
        for (dy = 1; dy <= ol + 1; dy++) {
            d.x = (float) (ln[i].x + dy), d.y = (float) (ln[i].y + dy + 1);
            SDL_RenderTexture(sdl.ren, ln[i].tex, NULL, &d);
        }
        (void) SDL_SetTextureColorMod(ln[i].tex,
                                      (Uint8) FXV_R(FXV_MSG_SHADE),
                                      (Uint8) FXV_G(FXV_MSG_SHADE),
                                      (Uint8) FXV_B(FXV_MSG_SHADE));
        for (dy = -ol; dy <= ol; dy++)
            for (dx = -ol; dx <= ol; dx++) {
                if ((!dx && !dy) || dx * dx + dy * dy > ol * ol + 1)
                    continue; /* round, not square, corners */
                d.x = (float) (ln[i].x + dx), d.y = (float) (ln[i].y + dy);
                SDL_RenderTexture(sdl.ren, ln[i].tex, NULL, &d);
            }
        (void) SDL_SetTextureColorMod(ln[i].tex, (Uint8) FXV_R(ln[i].c),
                                      (Uint8) FXV_G(ln[i].c),
                                      (Uint8) FXV_B(ln[i].c));
        d.x = (float) ln[i].x, d.y = (float) ln[i].y;
        SDL_RenderTexture(sdl.ren, ln[i].tex, NULL, &d);
        SDL_DestroyTexture(ln[i].tex);
    }
    SDL_SetRenderClipRect(sdl.ren, NULL);
}

static void
sdl_draw_messages(void)
{
    int i, first = max(0, sdl.nmsgs - SDL_MSGSHOW), row = 0;
    int y0 = sdl_bar_h(); /* below the menu bar */

    if (iflags.fx_msg_overlay) {
        sdl_draw_msg_overlay();
        return;
    }
    sdl_fill(0, y0, 4000, SDL_MSGSHOW * sdl.lh + 6, 8, 8, 12, 230);
    if (sdl.prompt) /* a question: it takes the bottom line */
        first = max(0, sdl.nmsgs - (SDL_MSGSHOW - 1));
    for (i = first; i < sdl.nmsgs; i++, row++)
        sdl_text(8, y0 + 3 + row * sdl.lh, sdl.msgs[i % SDL_MSGKEEP],
                 i < sdl.msg_seen ? sdl_dim
                 : sdl.msg_has_hl[i % SDL_MSGKEEP]
                     ? sdl_col(sdl.msg_hl[i % SDL_MSGKEEP])
                     : sdl_white);
    if (sdl.prompt)
        sdl_text(8, y0 + 3 + row * sdl.lh, sdl.prompt, sdl_gold);
}

/* ---- the inventory panel to the right (perm_invent, as NetHackW) -- */

/* the persistent inventory window, when it has something to show */
static struct sdl_win *
sdl_inv_win(void)
{
    struct sdl_win *iw;

    if (!iflags.perm_invent || WIN_INVEN == WIN_ERR
        || !program_state.in_moveloop)
        return 0;
    iw = sdl_getwin(WIN_INVEN);
    return (iw && iw->perminv && iw->nitems) ? iw : 0;
}

/* the panel's width, 0 when it isn't shown; it doesn't follow what's
   in it, so the map doesn't change size as things are picked up */
static int
sdl_inv_w(void)
{
    int sw, sh;

    if (iflags.fx_hide_inventory || !sdl_inv_win())
        return 0;
    SDL_GetRenderOutputSize(sdl.ren, &sw, &sh);
    return max(sw / 5, min(sw / 3, 36 * sdl.cw + 40));
}

/* its tiles: 1.5x or so, as NetHackW's lists, in steps of 8 */
static int
sdl_inv_ts(void)
{
    return 8 * max(2, (sdl.lh * 5 / 4 + 4) / 8);
}

static int
sdl_inv_rowh(void)
{
    return max(sdl.lh, sdl_inv_ts()) + 4;
}

/* the top of the map area (and of the panel) */
static int
sdl_map_top(void)
{
    if (iflags.fx_msg_overlay) /* messages float on the map instead */
        return sdl_bar_h();
    return sdl_bar_h() + SDL_MSGSHOW * sdl.lh + 8;
}

/* how many lines fit in the panel */
static int
sdl_inv_lines(void)
{
    int sw, sh;

    SDL_GetRenderOutputSize(sdl.ren, &sw, &sh);
    return max(1, (sh - sdl_status_h() - sdl_map_top() - 8)
                      / sdl_inv_rowh());
}

/* a line's text, cut short with "..." to fit in px */
static void
sdl_fit(char *buf, size_t sz, const char *s, int px)
{
    size_t n;

    Snprintf(buf, sz, "%s", s);
    if (sdl_text_w(buf) <= px)
        return;
    for (n = strlen(buf); n > 1; n--) {
        Snprintf(buf + n - 1, sz - (n - 1), "...");
        if (sdl_text_w(buf) <= px)
            return;
    }
}

static void
sdl_draw_inventory(void)
{
    const SDL_Color text = sdl_col(FXV_RGB(222, 226, 233)),
                    dim = sdl_col(FXV_RGB(138, 146, 160)),
                    accent = sdl_col(FXV_RGB(229, 186, 96));
    struct sdl_win *iw = sdl_inv_win();
    int pw = sdl_inv_w(), sw, sh, x, y, h, rowh = sdl_inv_rowh(),
        ts = sdl_inv_ts(), n = sdl_inv_lines(), i, ty;
    SDL_Rect clip;
    char buf[BUFSZ];

    if (!iw || !pw)
        return;
    SDL_GetRenderOutputSize(sdl.ren, &sw, &sh);
    x = sw - pw, y = sdl_map_top(), h = sh - sdl_status_h() - y;
    sdl_fill(x, y, pw, h, 24, 27, 33, 255);
    sdl_fill(x, y, 1, h, 52, 58, 70, 255);
    sdl.inv.first = max(0, min(sdl.inv.first, iw->nitems - n));
    clip.x = x + 1, clip.y = y, clip.w = pw - 1, clip.h = h;
    SDL_SetRenderClipRect(sdl.ren, &clip);
    y += 4;
    for (i = sdl.inv.first; i < iw->nitems && i < sdl.inv.first + n;
         i++, y += rowh) {
        struct sdl_item *it = &iw->items[i];
        int lx = x + 10;
        boolean heading = (!it->id.a_void && it->attr != ATR_NONE
                           && it->color == NO_COLOR);

        ty = y + (rowh - sdl.lh) / 2;
        if (heading) {
            /* a group heading: a band, in the accent colour */
            sdl_fill(x + 1, y, pw - 1, rowh, 33, 37, 46, 255);
            sdl_fill(x + 1, y + rowh - 1, pw - 1, 1, 52, 58, 70, 255);
            sdl_fit(buf, sizeof buf, it->text, pw - 24);
            (void) sdl_text(lx, ty, buf, accent);
            continue;
        }
        if (it->acc) { /* the letter, softer */
            buf[0] = it->acc, buf[1] = '\0';
            (void) sdl_text(lx, ty, buf, dim);
        }
        lx += 2 * sdl.cw;
        if (it->tile >= 0)
            sdl_tile(it->tile, (float) lx, (float) (y + (rowh - ts) / 2),
                     (float) ts);
        lx += ts + 6;
        sdl_fit(buf, sizeof buf, it->text, x + pw - 14 - lx);
        (void) sdl_text(lx, ty, buf,
                        it->color == NO_COLOR ? text
                                              : sdl_nh_color(it->color));
    }
    SDL_SetRenderClipRect(sdl.ren, NULL);
    if (iw->nitems > n) {
        /* a thin scroll bar: where this page is in the whole */
        int top = sdl_map_top(), th = max(16, h * n / iw->nitems),
            tp = top + (h - th) * sdl.inv.first / max(1, iw->nitems - n);

        sdl_fill(sw - 5, top, 3, h, 33, 37, 46, 255);
        sdl_fill(sw - 5, tp, 3, th, 90, 98, 115, 255);
    }
}

/* ---- the character panel along the bottom (as NetHackW's) ---------- */

/* what the core said for the fields the panel shows as text; the rest
   it reads from the game itself, as NetHackW does */
static char sdl_title[BUFSZ], sdl_leveldesc[BUFSZ];

static void
sdl_status_update(int idx, genericptr_t ptr, int chg, int percent,
                  int color, unsigned long *colormasks)
{
    char *dst = (idx == BL_TITLE) ? sdl_title
                : (idx == BL_LEVELDESC) ? sdl_leveldesc : (char *) 0;

    nhUse(chg), nhUse(percent), nhUse(color), nhUse(colormasks);
    if (idx == BL_FLUSH || idx == BL_RESET) {
        sdl.dirty = TRUE;
        return;
    }
    if (dst && ptr) {
        /* without the padding the core puts round some values */
        int n;

        Snprintf(dst, BUFSZ, "%s", (const char *) ptr);
        (void) mungspaces(dst);
        for (n = 0; dst[n]; n++)
            if (dst[n] == ':' && dst == sdl_leveldesc)
                dst[n] = ' '; /* "Dlvl:1" reads as "Dlvl 1" */
    }
}

static int
sdl_rowh(void)
{
    return sdl.lh + sdl.lh / 4;
}

/* the panel's height */
static int
sdl_status_h(void)
{
    return 2 * sdl_rowh() + 14;
}

static int
sdl_text_w(const char *s)
{
    int w = 0, h = 0;

    if (s && *s)
        (void) TTF_GetStringSize(sdl.font, s, 0, &w, &h);
    return w;
}

static SDL_Color
sdl_col(fxv_color c)
{
    SDL_Color s;

    s.r = (Uint8) FXV_R(c), s.g = (Uint8) FXV_G(c), s.b = (Uint8) FXV_B(c);
    s.a = 255;
    return s;
}

/* a filled box with slightly rounded corners (radius r, small) */
static void
sdl_rbox(int x, int y, int w, int h, int r, fxv_color c)
{
    Uint8 crd = (Uint8) FXV_R(c), cgr = (Uint8) FXV_G(c),
          cbl = (Uint8) FXV_B(c);

    r = min(r, min(w, h) / 2);
    if (r <= 0) {
        sdl_fill(x, y, w, h, crd, cgr, cbl, 255);
        return;
    }
    sdl_fill(x + r, y, w - 2 * r, h, crd, cgr, cbl, 255);
    sdl_fill(x, y + r, r, h - 2 * r, crd, cgr, cbl, 255);
    sdl_fill(x + w - r, y + r, r, h - 2 * r, crd, cgr, cbl, 255);
    sdl_dot((float) (x + r), (float) (y + r), (float) r, c, FALSE);
    sdl_dot((float) (x + w - r - 1), (float) (y + r), (float) r, c, FALSE);
    sdl_dot((float) (x + r), (float) (y + h - r - 1), (float) r, c, FALSE);
    sdl_dot((float) (x + w - r - 1), (float) (y + h - r - 1), (float) r, c,
            FALSE);
}

/* a thin vertical rule between sections */
static void
sdl_rule(int x, int y, int h)
{
    sdl_fill(x, y, 1, h, 52, 58, 70, 255);
}
/* how far row 'row' of a box 'h' high with corners of radius r is
   inset from each side, so the box is drawn with rounded corners */
static int
sdl_round_inset(int row, int h, int r)
{
    double dy;

    r = min(r, h / 2);
    if (r <= 0)
        return 0;
    if (row < r)
        dy = r - row - 0.5;
    else if (row >= h - r)
        dy = row - (h - r) + 0.5;
    else
        return 0;
    return (int) (r - sqrt(max(0.0, (double) r * r - dy * dy)) + 0.5);
}

#define SDL_BAR_RADIUS FXV_CORNER

/* a labelled bar, "HP 29 / 32" over a fill in the chosen fx_bar_style,
   its background, fill and outline all with slightly rounded corners */
static void
sdl_bar(int x, int y, int w, int h, const char *label, int cur, int mx,
        fxv_color fill)
{
    const int r = SDL_BAR_RADIUS;
    char buf[64];
    int pct = (mx > 0) ? (cur * 100) / mx : 0, fw, row;
    fxv_color stripe;

    pct = max(0, min(100, pct));
    fw = (w * pct) / 100;
    if (fw > 0)
        fw = max(fw, 2 * r);
    stripe = fxv_bar_stripe(fill);
    for (row = 0; row < h; row++) {
        int in = sdl_round_inset(row, h, r);
        int fin = sdl_round_inset(row, h, min(r, fw / 2));

        /* the background, then the fill (each row's colour shared,
           fxview.c), then the striped style's darker diagonals */
        sdl_fill(x + in, y + row, w - 2 * in, 1, 33, 37, 46, 255);
        if (fw > 0) {
            fxv_color c = fxv_bar_row(fill, (h > 1) ? (double) row / (h - 1)
                                                    : 0.0,
                                      row, h);
            int x0 = x + fin, x1 = x + fw - fin;

            sdl_fill(x0, y + row, x1 - x0, 1, (Uint8) FXV_R(c),
                     (Uint8) FXV_G(c), (Uint8) FXV_B(c), 255);
            if (stripe) {
                int step = max(7, h / 2), t = max(2, h / 6), sx;

                for (sx = x - h; sx < x + fw; sx += step) {
                    int a = max(x0, sx + h - row), b = min(x1, a + t);

                    if (sx + h - row + t > x0 && b > a)
                        sdl_fill(a, y + row, b - a, 1,
                                 (Uint8) FXV_R(stripe), (Uint8) FXV_G(stripe),
                                 (Uint8) FXV_B(stripe), 255);
                }
            }
        }
        if (iflags.fx_bar_border) {
            /* a crisp 1-pixel outline following the corners */
            if (row == 0 || row == h - 1)
                sdl_fill(x + in, y + row, w - 2 * in, 1, 4, 5, 7, 255);
            else {
                int pin = sdl_round_inset(row - (row < h / 2 ? 1 : -1), h, r);
                int len = max(1, pin - in + 1);

                sdl_fill(x + in, y + row, len, 1, 4, 5, 7, 255);
                sdl_fill(x + w - in - len, y + row, len, 1, 4, 5, 7, 255);
            }
        }
    }
    Snprintf(buf, sizeof buf, "%s  %d / %d", label, cur, mx);
    (void) sdl_text(x + (w - sdl_text_w(buf)) / 2, y + (h - sdl.lh) / 2,
                    buf, sdl_col(FXV_RGB(250, 250, 252)));
}

/* a small triangle, pointing up or down, centred at x,y */
static void
sdl_arrow(float x, float y, float s, boolean up, fxv_color c)
{
    SDL_Vertex v[3];
    SDL_FColor fc;
    int i;

    fc.r = FXV_R(c) / 255.0f, fc.g = FXV_G(c) / 255.0f;
    fc.b = FXV_B(c) / 255.0f, fc.a = 1.0f;
    v[0].position.x = x - s / 2, v[1].position.x = x + s / 2;
    v[2].position.x = x;
    v[0].position.y = v[1].position.y = up ? y + s / 2 : y - s / 2;
    v[2].position.y = up ? y - s / 2 : y + s / 2;
    for (i = 0; i < 3; i++)
        v[i].color = fc, v[i].tex_coord.x = v[i].tex_coord.y = 0;
    (void) SDL_RenderGeometry(sdl.ren, NULL, v, 3, NULL, 0);
}

static void
sdl_draw_status(void)
{
    static const char *const statnm[6] = { "St", "Dx", "Co", "In", "Wi",
                                           "Ch" };
    const SDL_Color text = sdl_col(FXV_RGB(222, 226, 233)),
                    dim = sdl_col(FXV_RGB(138, 146, 160));
    int w, h, x = 12, y1, y2, rowh = sdl_rowh(), i, sw, colw, top;
    int hp = Upolyd ? u.mh : u.uhp, hpmax = Upolyd ? u.mhmax : u.uhpmax;
    int arrow[6];
    char buf[BUFSZ];

    SDL_GetRenderOutputSize(sdl.ren, &w, &h);
    top = h - sdl_status_h();
    sdl_fill(0, top, w, sdl_status_h(), 24, 27, 33, 255);
    sdl_fill(0, top, w, 1, 52, 58, 70, 255);
    if (!program_state.in_moveloop || !sdl_title[0])
        return; /* nothing to show before the game starts */
    y1 = top + 7, y2 = y1 + rowh;

    /* identity: title, then alignment and where */
    (void) sdl_text(x, y1, sdl_title, sdl_gold);
    Snprintf(buf, sizeof buf, "%s   -   %s",
             (u.ualign.type == A_CHAOTIC) ? "Chaotic"
             : (u.ualign.type == A_NEUTRAL) ? "Neutral" : "Lawful",
             sdl_leveldesc);
    (void) sdl_text(x, y2, buf, dim);
    x += max(sdl_text_w(sdl_title), sdl_text_w(buf)) + 18;
    sdl_rule(x - 9, y1, 2 * rowh - 2);

    /* HP and Pw */
    {
        int bw = max(170, sdl.cw * 22), bh = rowh - 6;

        sdl_bar(x, y1 + 2, bw, bh, "HP", hp, hpmax,
                fxv_hp_color(hpmax > 0 ? (hp * 100) / hpmax : 0));
        sdl_bar(x, y2 + 2, bw, bh, "Pw", u.uen, u.uenmax,
                FXV_RGB(70, 120, 220));
        x += bw + 18;
        sdl_rule(x - 9, y1, 2 * rowh - 2);
    }

    /* the six stats, two rows of three, with an arrow for 50 turns after
       one changes (fxview.c) */
    colw = sdl.cw * 11; /* room for "St 18/04" in a monospace font */
    sw = sdl_text_w("Wi ");
    fxv_stat_arrows(arrow);
    for (i = 0; i < 6; i++) {
        int sx = x + (i % 3) * colw, sy = (i < 3) ? y1 : y2, v = ACURR(i);

        (void) sdl_text(sx, sy, statnm[i], dim);
        if (i == A_STR && v > 18) {
            if (v > STR18(100))
                Snprintf(buf, sizeof buf, "%d", v - 100);
            else if (v < STR18(100))
                Snprintf(buf, sizeof buf, "18/%02d", v - 18);
            else
                Strcpy(buf, "18/**");
        } else {
            Snprintf(buf, sizeof buf, "%d", v);
        }
        (void) sdl_text(sx + sw, sy, buf, text);
        if (arrow[i])
            sdl_arrow((float) (sx + sw + sdl_text_w(buf) + 5 + sdl.cw / 2),
                      (float) (sy + sdl.lh / 2), (float) sdl.cw * 0.8f,
                      arrow[i] > 0,
                      arrow[i] > 0 ? FXV_RGB(104, 210, 98)
                                   : FXV_RGB(235, 84, 76));
    }
    x += 3 * colw + 10;
    sdl_rule(x - 9, y1, 2 * rowh - 2);

    /* armour class and level; gold and the turn */
    {
        int w1, w2;

        Snprintf(buf, sizeof buf, "AC %d", u.uac);
        (void) sdl_text(x, y1, buf, text);
        w1 = sdl_text_w(buf) + 16;
        if (Upolyd)
            Snprintf(buf, sizeof buf, "HD %d", mons[u.umonnum].mlevel);
        else if (flags.showexp)
            Snprintf(buf, sizeof buf, "Lvl %d  (%ld xp)", u.ulevel,
                     u.uexp);
        else
            Snprintf(buf, sizeof buf, "Lvl %d", u.ulevel);
        (void) sdl_text(x + w1, y1, buf, text);
        w1 += sdl_text_w(buf);
        Snprintf(buf, sizeof buf, "$ %ld", money_cnt(gi.invent));
        (void) sdl_text(x, y2, buf, sdl_col(FXV_RGB(242, 210, 96)));
        w2 = max(sdl_text_w(buf), sdl_text_w("AC 10")) + 16;
        if (flags.time) {
            Snprintf(buf, sizeof buf, "Turn %ld", svm.moves);
            (void) sdl_text(x + w2, y2, buf, dim);
            w2 += sdl_text_w(buf);
        }
        x += max(w1, w2) + 18;
        sdl_rule(x - 9, y1, 2 * rowh - 2);
    }

    /* what's worn and wielded: the twelve equipment slots, each
       edged red or green once known cursed or blessed (fxview.c) */
    {
        struct obj *slot[12];
        int cell = rowh - 3, gap = 4;

        fxv_equip_slots(slot);
        for (i = 0; i < 12; i++) {
            int cx = x + (i % 6) * (cell + gap), cy = y1 + (i / 6) * rowh;
            struct obj *o = slot[i];

            sdl_rbox(cx, cy, cell, cell, 2, fxv_equip_edge(o));
            sdl_rbox(cx + 1, cy + 1, cell - 2, cell - 2, 1,
                     o ? FXV_RGB(33, 37, 46) : FXV_RGB(24, 27, 33));
            if (o) {
                glyph_info gi_obj;
                int pad = max(1, cell / 10);

                map_glyphinfo(0, 0, obj_to_glyph(o, rn2_on_display_rng), 0,
                              &gi_obj);
                sdl_tile(gi_obj.gm.tileidx, (float) (cx + pad),
                         (float) (cy + pad), (float) (cell - 2 * pad));
            }
        }
        x += 6 * cell + 5 * gap + 18;
        sdl_rule(x - 9, y1, 2 * rowh - 2);
    }

    /* (the action buttons float above the panel: sdl_draw_actions()) */
    sdl_actx = w;

    /* conditions as chips, wrapping onto the second row (fxview.c) */
    {
        struct fxv_chip chip[24];
        int nchip = fxv_status_chips(chip, SIZE(chip)), cx = x, cy = y1;

        for (i = 0; i < nchip; i++) {
            int cw = sdl_text_w(chip[i].name) + 16;

            if (cx + cw > sdl_actx - 12 && cy == y1)
                cx = x, cy = y2;
            if (cx + cw > sdl_actx - 12)
                break;
            sdl_rbox(cx, cy + 1, cw, rowh - 4, 4, FXV_RGB(33, 37, 46));
            (void) sdl_text(cx + 8, cy + 1 + (rowh - 4 - sdl.lh) / 2,
                            chip[i].name, sdl_col(chip[i].c));
            cx += cw + 6;
        }
    }
}

/* the action buttons: what makes sense to do here (fxview.c), in a row
   floating just above the character panel at the right of the map (at
   its left when the messages sit bottom right), so they never crowd the
   panel on a narrow window; fx_action_buttons turns them off */
static void
sdl_draw_actions(void)
{
    struct fxv_action act[SDL_MAXBTN];
    int w, h, n, i, x, y, bh, right, left;
    boolean from_left = (iflags.fx_msg_overlay && iflags.fx_msg_pos == 5);
    const SDL_Color text = sdl_col(FXV_RGB(222, 226, 233)),
                    dim = sdl_col(FXV_RGB(138, 146, 160));

    sdl_nbtn = 0;
    if (!iflags.fx_action_buttons || !program_state.in_moveloop
        || !sdl_title[0])
        return;
    SDL_GetRenderOutputSize(sdl.ren, &w, &h);
    bh = sdl_rowh();
    y = h - sdl_status_h() - bh - 8;
    left = 12, right = w - sdl_inv_w() - 12;
    n = fxv_actions(act, SDL_MAXBTN, TRUE);
    x = from_left ? left : right;
    for (i = 0; i < n; i++) {
        /* from the right, the last action is the rightmost, as before */
        int k = from_left ? i : n - 1 - i;
        int lw = sdl_text_w(act[k].label), hw = sdl_text_w(act[k].hint);
        int bw = lw + hw + 30;
        SDL_Rect *r = &sdl_btn[sdl_nbtn].r;
        boolean hot = (sdl_nbtn == sdl_btn_hot);

        if (from_left ? x + bw > right : x - bw < left)
            break; /* no more room */
        r->x = from_left ? x : x - bw, r->y = y, r->w = bw, r->h = bh;
        sdl_btn[sdl_nbtn].key = act[k].key;
        sdl_rbox(r->x, r->y, r->w, r->h, FXV_CORNER,
                 hot ? FXV_RGB(229, 186, 96) : FXV_RGB(52, 58, 70));
        sdl_rbox(r->x + 1, r->y + 1, r->w - 2, r->h - 2, FXV_CORNER - 1,
                 hot ? FXV_RGB(52, 58, 70) : FXV_RGB(28, 31, 38));
        (void) sdl_text(r->x + 10, r->y + (r->h - sdl.lh) / 2, act[k].label,
                        text);
        (void) sdl_text(r->x + r->w - 10 - hw, r->y + (r->h - sdl.lh) / 2,
                        act[k].hint, hot ? sdl_gold : dim);
        x = from_left ? x + bw + 6 : x - bw - 6;
        sdl_nbtn++;
    }
}

/* the whole screen, without any menu over it */
static void
sdl_draw_screen(void)
{
    SDL_SetRenderDrawColor(sdl.ren, 0, 0, 0, 255);
    SDL_RenderClear(sdl.ren);
    sdl_draw_map();
    sdl_draw_inventory();
    sdl_draw_status();
    sdl_draw_messages();
    sdl_draw_actions(); /* over the map, just above the panel */
    sdl_draw_bar(); /* last: its drop-downs go over everything */
}

/* for testing: NETHACK_SDL_SHOT=file.png saves each frame drawn there */
static void
sdl_present(void)
{
    static const char *shot = (const char *) -1;

    if (shot == (const char *) -1)
        shot = getenv("NETHACK_SDL_SHOT");
    if (shot && *shot) {
        SDL_Surface *s = SDL_RenderReadPixels(sdl.ren, NULL);

        if (s) {
            (void) SDL_SavePNG(s, shot);
            SDL_DestroySurface(s);
        }
    }
    SDL_RenderPresent(sdl.ren);
}

static void sdl_draw_overlay(void);

/* draw everything, with any open menu or text window on top */
static void
sdl_render(void)
{
    if (!sdl.ren)
        return;
    sdl_sync_options();
    if (sdl.es.mode >= 0)
        sdl_draw_es();
    else if (sdl.cs.mode >= 0)
        sdl_draw_cs();
    else if (sdl.title.up)
        sdl_draw_title();
    else
        sdl_draw_screen();
    sdl_draw_overlay();
    sdl_present();
    sdl.dirty = FALSE;
}

/* ---- messages ------------------------------------------------------ */

/* wait for the player to see what's up before going on */
static void
sdl_more(void)
{
    int c;

    sdl.prompt = "--More--";
    sdl_render();
    do
        c = sdl_wait_key();
    while (c != '\n' && c != '\r' && c != ' ' && c != '\033');
    sdl.prompt = 0;
    sdl.msg_seen = sdl.nmsgs;
    sdl.dirty = TRUE;
}

static void
sdl_add_msg(const char *s)
{
    int slot = sdl.nmsgs % SDL_MSGKEEP;

    /* on the map, as NetHackW: once a key's worth of messages fills
       the lines shown, --More-- before the next */
    if (iflags.fx_msg_overlay && program_state.in_moveloop && sdl.ren
        && sdl.nmsgs - sdl.msg_seen >= FXV_MSG_SHOW)
        sdl_more();
    if (sdl.nmsgs >= SDL_MSGKEEP)
        free(sdl.msgs[slot]);
    sdl.msgs[slot] = dupstr(s);
    sdl.msg_turn[slot] = svm.moves;
    sdl.msg_has_hl[slot] = fxv_msg_highlight(s, &sdl.msg_hl[slot]);
    sdl.nmsgs++;
    sdl.dirty = TRUE;
}

/* ---- menus and text windows ---------------------------------------- */

static void
sdl_free_items(struct sdl_win *w)
{
    int i;

    for (i = 0; i < w->nitems; i++)
        free(w->items[i].text);
    w->nitems = 0;
    if (w->prompt)
        free(w->prompt), w->prompt = 0;
}

static struct sdl_item *
sdl_new_item(struct sdl_win *w)
{
    if (w->nitems >= w->maxitems) {
        int n = w->maxitems ? w->maxitems * 2 : 32;
        struct sdl_item *it = (struct sdl_item *) alloc(n * sizeof *it);

        if (w->items) {
            memcpy(it, w->items, w->nitems * sizeof *it);
            free(w->items);
        }
        w->items = it, w->maxitems = n;
    }
    (void) memset(&w->items[w->nitems], 0, sizeof w->items[0]);
    w->items[w->nitems].tile = -1;
    return &w->items[w->nitems++];
}

/* how many lines fit in the overlay */
static int
sdl_page_lines(void)
{
    int w, h;

    SDL_GetRenderOutputSize(sdl.ren, &w, &h);
    return max(5, (h - 4 * sdl.lh) / sdl.lh - 2);
}

/* draw the open menu or text window over the screen */
static void
sdl_draw_overlay(void)
{
    struct sdl_win *w = sdl.ovl.w;
    int first = sdl.ovl.first, how = sdl.ovl.how;
    int sw, sh, i, n = sdl_page_lines(), width = 20, x, y, bw, bh;

    if (!w)
        return;
    SDL_GetRenderOutputSize(sdl.ren, &sw, &sh);
    for (i = 0; i < w->nitems; i++)
        width = max(width, (int) strlen(w->items[i].text) + 6);
    if (w->prompt)
        width = max(width, (int) strlen(w->prompt) + 2);
    bw = min(sw - 20, width * sdl.cw + 24);
    bh = (min(n, w->nitems) + 3) * sdl.lh + 12;
    x = (sw - bw) / 2, y = max(4, (sh - bh) / 2);
    sdl.ovl.bx = x, sdl.ovl.by = y, sdl.ovl.bw = bw, sdl.ovl.bh = bh;
    sdl_fill(x, y, bw, bh, 14, 16, 24, 240);
    sdl_fill(x, y, bw, 2, 90, 110, 150, 255);
    y += 8;
    if (w->prompt) {
        sdl_text(x + 12, y, w->prompt, sdl_gold);
        y += sdl.lh;
    }
    sdl.ovl.items_y = y;
    for (i = first; i < w->nitems && i < first + n; i++, y += sdl.lh) {
        struct sdl_item *it = &w->items[i];
        char buf[BUFSZ + 8];
        SDL_Color c = sdl_nh_color(it->color);

        if (it->id.a_void && how != PICK_NONE)
            Snprintf(buf, sizeof buf, "%c %c %s", it->acc ? it->acc : ' ',
                     it->selected ? '+' : '-', it->text);
        else
            Snprintf(buf, sizeof buf, "%s", it->text);
        if (!it->id.a_void && (it->attr & (ATR_BOLD | ATR_INVERSE)))
            c = sdl_gold;
        if (i == sdl.ovl.hover) /* under the mouse */
            sdl_fill(x + 6, y, bw - 12, sdl.lh, 70, 92, 140, 150);
        sdl_text(x + 12, y, buf, c);
    }
    if (w->nitems > n) {
        char pg[64];

        Snprintf(pg, sizeof pg, "(%d of %d)  > next  < back", first / n + 1,
                 (w->nitems + n - 1) / n);
        sdl_text(x + 12, y + 4, pg, sdl_dim);
    }
}

/* give selectable items without an accelerator a-z, A-Z per page */
static void
sdl_assign_accels(struct sdl_win *w)
{
    static const char accs[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int i, k = 0, n = sdl_page_lines();

    for (i = 0; i < w->nitems; i++) {
        if (i % n == 0)
            k = 0;
        if (w->items[i].id.a_void && !w->items[i].acc)
            w->items[i].acc = (k < (int) sizeof accs - 1) ? accs[k++] : 0;
    }
}

/* run a menu or text window; returns the count picked, -1 if cancelled */
static int
sdl_run_overlay(struct sdl_win *w, int how, menu_item **picks)
{
    int first = 0, n = sdl_page_lines(), c, i, count;
    struct sdl_win *outer = sdl.ovl.w; /* a menu opened from a menu */
    int outer_first = sdl.ovl.first, outer_how = sdl.ovl.how;

    if (how != PICK_NONE)
        sdl_assign_accels(w);
    sdl.ovl.w = w, sdl.ovl.how = how, sdl.ovl.hover = -1;
    for (;;) {
        sdl.ovl.first = first;
        sdl_render();
        c = sdl_wait_key();
        if (c == '\033') {
            if (how == PICK_NONE)
                break;
            sdl.ovl.w = outer, sdl.ovl.first = outer_first;
            sdl.ovl.how = outer_how;
            sdl.dirty = TRUE;
            return -1;
        }
        if (c == '>' || (c == ' ' && first + n < w->nitems)) {
            if (first + n < w->nitems)
                first += n;
            continue;
        }
        if (c == ' ' || c == '\n' || c == '\r')
            break;
        if (c == '<' || c == '^') {
            first = (c == '^') ? 0 : max(0, first - n);
            continue;
        }
        if (how == PICK_NONE)
            continue;
        if (how == PICK_ANY && (c == '.' || c == ',' || c == '-')) {
            for (i = 0; i < w->nitems; i++)
                if (w->items[i].id.a_void
                    && (c == '.' || (i >= first && i < first + n)))
                    w->items[i].selected = (c != '-');
            continue;
        }
        /* an accelerator on this page, or a group accelerator anywhere */
        for (i = 0; i < w->nitems; i++) {
            struct sdl_item *it = &w->items[i];

            if (!it->id.a_void)
                continue;
            if ((it->acc == c && i >= first && i < first + n)
                || (it->gacc && it->gacc == c)) {
                it->selected = !it->selected;
                if (how == PICK_ONE) {
                    int j;

                    for (j = 0; j < w->nitems; j++)
                        if (j != i)
                            w->items[j].selected = FALSE;
                    it->selected = TRUE;
                    goto done;
                }
                if (it->acc == c)
                    break;
            }
        }
    }
 done:
    sdl.ovl.w = outer, sdl.ovl.first = outer_first, sdl.ovl.how = outer_how;
    sdl.dirty = TRUE;
    if (how == PICK_NONE || !picks)
        return 0;
    for (i = count = 0; i < w->nitems; i++)
        if (w->items[i].id.a_void && w->items[i].selected)
            count++;
    *picks = 0;
    if (count) {
        int k = 0;

        *picks = (menu_item *) alloc(count * sizeof (menu_item));
        for (i = 0; i < w->nitems; i++)
            if (w->items[i].id.a_void && w->items[i].selected) {
                (*picks)[k].item = w->items[i].id;
                (*picks)[k].count = -1L;
                k++;
            }
    }
    sdl.dirty = TRUE;
    return count;
}


/* ---- the full-window screens: their fonts, art and buttons ---------- */

/* the screens' fonts, as NetHackW's: a bold serif for titles, a plain
   and a bold sans for the rest; found among the usual places, or the
   game's own fonts/ folder; failing all those, the map's font */
#define SDL_FACE_TITLE 0
#define SDL_FACE_UI 1
#define SDL_FACE_UI_BOLD 2
#define SDL_NFACE 3

static TTF_Font *
sdl_face(int which, int px)
{
    static const char *const names[SDL_NFACE][5] = {
        { "DejaVuSerif-Bold.ttf", "LiberationSerif-Bold.ttf", "palab.ttf",
          "georgiab.ttf", "timesbd.ttf" },
        { "DejaVuSans.ttf", "LiberationSans-Regular.ttf", "segoeui.ttf",
          "Ubuntu-R.ttf", "arial.ttf" },
        { "DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf", "seguisb.ttf",
          "Ubuntu-B.ttf", "arialbd.ttf" },
    };
    static const char *const dirs[] = {
        "fonts/", "/usr/share/fonts/truetype/dejavu/",
        "/usr/share/fonts/dejavu/", "/usr/share/fonts/TTF/",
        "/usr/share/fonts/truetype/liberation/",
        "/usr/share/fonts/liberation-sans/",
        "/usr/share/fonts/truetype/ubuntu/", "/System/Library/Fonts/",
        "C:\\Windows\\Fonts\\",
    };
    static boolean tried[SDL_NFACE];
    TTF_Font *f;

    if (which < 0 || which >= SDL_NFACE)
        which = SDL_FACE_UI;
    if (!sdl.face[which] && !tried[which]) {
        char path[BUFSZ];
        int i, j;

        tried[which] = TRUE;
        for (i = 0; i < SIZE(names[0]) && !sdl.face[which]; i++)
            for (j = 0; j < SIZE(dirs) && !sdl.face[which]; j++) {
                Snprintf(path, sizeof path, "%s%s", dirs[j], names[which][i]);
                sdl.face[which] = TTF_OpenFont(path, 20.0f);
            }
        if (!sdl.face[which] && sdl.fontpath[0]
            && (sdl.face[which] = TTF_OpenFont(sdl.fontpath, 20.0f)) != 0
            && which != SDL_FACE_UI)
            TTF_SetFontStyle(sdl.face[which], TTF_STYLE_BOLD);
    }
    f = sdl.face[which] ? sdl.face[which] : sdl.font;
    (void) TTF_SetFontSize(f, (float) max(8, px));
    TTF_SetFontKerning(f, false); /* (it set "T" apart from what follows) */
    return f;
}

/* a line in one of those fonts: 'how' 0 from x, 1 centred on x, 2 up
   to x; y is its top. A shadow (down and right, black) if 'shadow'.
   Returns its width. */
static int
sdl_text_w_in(TTF_Font *f, const char *t)
{
    int w = 0, h = 0;

    if (t && *t)
        (void) TTF_GetStringSize(f, t, 0, &w, &h);
    return w;
}

static int
sdl_face_text(TTF_Font *f, const char *s, int x, int y, int how,
              fxv_color c, int shadow)
{
    static const SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *surf;
    SDL_Texture *tex;
    SDL_FRect d;
    int w;

    if (!s || !*s || !(surf = TTF_RenderText_Blended(f, s, 0, white)))
        return 0;
    tex = SDL_CreateTextureFromSurface(sdl.ren, surf);
    w = surf->w;
    d.w = (float) surf->w, d.h = (float) surf->h;
    d.x = (float) (how == 1 ? x - w / 2 : how == 2 ? x - w : x);
    d.y = (float) y;
    SDL_DestroySurface(surf);
    if (!tex)
        return 0;
    if (shadow) {
        SDL_FRect sd = d;

        sd.x += shadow, sd.y += shadow;
        (void) SDL_SetTextureColorMod(tex, 0, 0, 0);
        SDL_RenderTexture(sdl.ren, tex, NULL, &sd);
    }
    (void) SDL_SetTextureColorMod(tex, (Uint8) FXV_R(c), (Uint8) FXV_G(c),
                                  (Uint8) FXV_B(c));
    SDL_RenderTexture(sdl.ren, tex, NULL, &d);
    SDL_DestroyTexture(tex);
    return w;
}

/* the painting fills the window, cropped to keep its shape (evenly from
   the sides, more from the bottom than the top so the rider and the
   moon stay in view), shaded towards the bottom (fxv_title_shade) so
   the title and buttons read over it, then darkened all over by 'dim'
   (0..255) for screens with panels over it */
static void
sdl_backdrop(int w, int h, int dim)
{
    static boolean tried;
    int y, step = 3;

    sdl_fill(0, 0, w, h, 14, 16, 20, 255);
    if (!tried) {
        SDL_Surface *s;

        tried = TRUE;
        if ((s = SDL_LoadPNG("splash_art.png")) != 0) {
            sdl.art = SDL_CreateTextureFromSurface(sdl.ren, s);
            sdl.art_w = (float) s->w, sdl.art_h = (float) s->h;
            sdl.art_px = SDL_ConvertSurface(s, SDL_PIXELFORMAT_XRGB8888);
            SDL_DestroySurface(s);
            if (sdl.art)
                (void) SDL_SetTextureScaleMode(sdl.art,
                                               SDL_SCALEMODE_LINEAR);
        }
    }
    if (sdl.art && sdl.art_w > 0 && sdl.art_h > 0) {
        float sc = max((float) w / sdl.art_w, (float) h / sdl.art_h);
        SDL_FRect d;

        d.w = sdl.art_w * sc, d.h = sdl.art_h * sc;
        d.x = (w - d.w) / 2, d.y = (h - d.h) / 3;
        SDL_RenderTexture(sdl.ren, sdl.art, NULL, &d);
    }
    for (y = 0; y < h; y += step) {
        int a = fxv_title_shade(y, h);

        if (a > 0)
            sdl_fill(0, y, w, step, 0, 0, 0, (Uint8) a);
    }
    if (dim > 0)
        sdl_fill(0, 0, w, h, 0, 0, 0, (Uint8) min(dim, 255));
}

/* a box with the panels' small corners and an edge 'pw' wide */
static void
sdl_edged(const int r[4], fxv_color fill, fxv_color edge, int pw)
{
    int w = r[2] - r[0], h = r[3] - r[1];

    sdl_rbox(r[0], r[1], w, h, FXV_CORNER, edge);
    if (pw > 0 && w > 2 * pw && h > 2 * pw)
        sdl_rbox(r[0] + pw, r[1] + pw, w - 2 * pw, h - 2 * pw,
                 max(1, FXV_CORNER - pw), fill);
}

/* a button in the screens' style: 'primary' is the wide gold one, the
   others dark (as NetHackW's mswin_ms_button) */
static void
sdl_button(const int r[4], const char *label, boolean primary, boolean hot,
           boolean focus, boolean down, boolean enabled, double s)
{
    const fxv_color accent = FXV_RGB(229, 186, 96);
    fxv_color fill, ink, edge;
    TTF_Font *f = sdl_face(SDL_FACE_UI_BOLD, max(11, (int) (19 * s)));

    if (!enabled)
        hot = down = FALSE;
    if (primary && enabled) {
        fill = down ? FXV_RGB(196, 152, 70)
               : hot ? FXV_RGB(244, 206, 124) : accent;
        ink = FXV_RGB(14, 16, 20);
        edge = focus ? FXV_RGB(255, 236, 190) : fill;
    } else if (primary) {
        fill = FXV_RGB(92, 80, 58), ink = FXV_RGB(40, 36, 30);
        edge = focus ? accent : fill;
    } else {
        fill = down ? FXV_RGB(52, 58, 70)
               : hot ? FXV_RGB(33, 37, 46) : FXV_RGB(24, 27, 33);
        ink = !enabled ? FXV_RGB(52, 58, 70)
              : (hot || focus) ? FXV_RGB(222, 226, 233)
                               : FXV_RGB(138, 146, 160);
        edge = focus ? accent : FXV_RGB(52, 58, 70);
    }
    sdl_edged(r, fill, edge, focus ? max(2, (int) (2 * s)) : 1);
    (void) sdl_face_text(f, label, (r[0] + r[2]) / 2,
                         (r[1] + r[3]) / 2 - TTF_GetFontHeight(f) / 2, 1,
                         ink, 0);
}

/* a thick line, for the speaker's waves and cross */
static void
sdl_thick_line(float x0, float y0, float x1, float y1, float t, fxv_color c)
{
    float dx, dy;

    SDL_SetRenderDrawColor(sdl.ren, (Uint8) FXV_R(c), (Uint8) FXV_G(c),
                           (Uint8) FXV_B(c), 255);
    for (dx = -t / 2; dx <= t / 2; dx += 0.5f)
        for (dy = -t / 2; dy <= t / 2; dy += 0.5f)
            SDL_RenderLine(sdl.ren, x0 + dx, y0 + dy, x1 + dx, y1 + dy);
}

/* the speaker button for the menu music: waves while it's on, crossed
   out when it's off; pointed at, it says what it does */
static void
sdl_sound_icon(const int r[4], boolean hot, double s)
{
    boolean on = iflags.fx_menu_music;
    float k = (float) (s * 1.05), t = max(2.0f, 2.2f * k);
    float cx = (r[0] + r[2]) / 2.0f - 3 * (float) s,
          cy = (r[1] + r[3]) / 2.0f;
    fxv_color ink = hot ? FXV_RGB(229, 186, 96)
                    : on ? FXV_RGB(222, 226, 233) : FXV_RGB(138, 146, 160);
    SDL_Vertex v[6];
    static const int tri[] = { 0, 1, 4, 0, 4, 5, 1, 2, 3, 1, 3, 4 };
    static const float px[6] = { -10, -4, 3, 3, -4, -10 },
                       py[6] = { -4, -4, -10, 10, 4, 4 };
    int i;

    /* the round button */
    sdl_dot((r[0] + r[2]) / 2.0f, cy, (r[2] - r[0]) / 2.0f,
            hot ? FXV_RGB(229, 186, 96) : FXV_RGB(52, 58, 70), FALSE);
    sdl_dot((r[0] + r[2]) / 2.0f, cy, (r[2] - r[0]) / 2.0f - 1,
            hot ? FXV_RGB(33, 37, 46) : FXV_RGB(24, 27, 33), FALSE);
    /* the speaker itself */
    for (i = 0; i < 6; i++) {
        v[i].position.x = cx + px[i] * k, v[i].position.y = cy + py[i] * k;
        v[i].color.r = FXV_R(ink) / 255.0f, v[i].color.g = FXV_G(ink) / 255.0f;
        v[i].color.b = FXV_B(ink) / 255.0f, v[i].color.a = 1.0f;
        v[i].tex_coord.x = v[i].tex_coord.y = 0.0f;
    }
    (void) SDL_RenderGeometry(sdl.ren, NULL, v, 6, tri, SIZE(tri));
    if (on) {
        int n;

        for (n = 1; n <= 2; n++) {
            float rr = (n == 1 ? 6 : 11) * k, ax = cx + 4 * k, a0, a1;

            for (a0 = -0.85f; a0 < 0.85f; a0 += 0.17f) {
                a1 = a0 + 0.17f;
                sdl_thick_line(ax + rr * (float) cos(a0),
                               cy + rr * (float) sin(a0),
                               ax + rr * (float) cos(a1),
                               cy + rr * (float) sin(a1), t, ink);
            }
        }
    } else {
        sdl_thick_line(cx + 8 * k, cy - 5 * k, cx + 17 * k, cy + 5 * k, t,
                       ink);
        sdl_thick_line(cx + 17 * k, cy - 5 * k, cx + 8 * k, cy + 5 * k, t,
                       ink);
    }
    if (hot) {
        TTF_Font *f = sdl_face(SDL_FACE_UI_BOLD, max(10, (int) (14 * s)));

        (void) sdl_face_text(f, on ? "Music on  (Ctrl+M)"
                                   : "Music off  (Ctrl+M)",
                             r[0] - (int) (10 * s),
                             (int) cy - TTF_GetFontHeight(f) / 2, 2,
                             FXV_RGB(222, 226, 233), 1);
    }
}

/* ---- the menu music (fx_menu_music), as NetHackW's ----------------- */

/* It starts with the title screen, carries on through character
   creation, and fades out as the game begins: when the hero is chosen,
   or failing that (a restored game) at the first wait for a command.
   The speaker button (or Ctrl+M) turns it off and on. */
static boolean mm_on, mm_over;

static void
sdl_music_sync(void)
{
#ifdef SND_LIB_MINIAUDIO
    static char path[BUFSZ];
    static boolean first = TRUE;
    boolean want = !mm_over && iflags.fx_menu_music && iflags.sounds;

    if (want == mm_on)
        return;
    if (want) {
        if (!path[0]) {
            static const char *const tries[] = {
                "sounds/music_title.mp3", "../sound/modern/music_title.mp3"
            };
            int i;

            for (i = 0; i < SIZE(tries) && !path[0]; i++)
                if (SDL_GetPathInfo(tries[i], NULL))
                    Strcpy(path, tries[i]);
            if (!path[0])
                return;
        }
        if (!nhma_init())
            return; /* no audio device */
        nhma_set_master_volume(0.8f);
        nhma_music(path, 0.55f, first ? 2500 : 900, 1);
        first = FALSE;
        mm_on = TRUE;
    } else {
        nhma_music((const char *) 0, 0.0f, mm_over ? 2500 : 700, 0);
        mm_on = FALSE;
    }
#endif
}

/* the game has begun: fade the music out for good */
static void
sdl_music_end(void)
{
    if (mm_over)
        return;
    mm_over = TRUE;
    sdl_music_sync();
}

static void
sdl_music_toggle(void)
{
    iflags.fx_menu_music = !iflags.fx_menu_music;
    cfg_remember_option("fx_menu_music", iflags.fx_menu_music
                                             ? "fx_menu_music"
                                             : "!fx_menu_music");
    sdl_music_sync();
    sdl.dirty = TRUE;
}

/* ---- the title screen (as NetHackW's) ------------------------------ */

#define SDL_TITLE_SOUND FXV_TITLE_NBTN /* the speaker, beside the buttons */
static const char *const sdl_title_label[FXV_TITLE_NBTN] = {
    "Play NetHack Modern", "About", "Exit"
};

/* the button (or the speaker) at a point, or -1 */
static int
sdl_title_hit(float x, float y)
{
    int i;

    for (i = 0; i < FXV_TITLE_NBTN; i++)
        if (x >= sdl.title.btn[i][0] && x < sdl.title.btn[i][2]
            && y >= sdl.title.btn[i][1] && y < sdl.title.btn[i][3])
            return i;
    if (x >= sdl.title.snd[0] && x < sdl.title.snd[2]
        && y >= sdl.title.snd[1] && y < sdl.title.snd[3])
        return SDL_TITLE_SOUND;
    return -1;
}

static void
sdl_draw_title(void)
{
    const fxv_color accent = FXV_RGB(229, 186, 96),
                    text = FXV_RGB(222, 226, 233),
                    dim = FXV_RGB(138, 146, 160);
    int w, h, th, i, y, m;
    double s;
    TTF_Font *f;
    char buf[BUFSZ];

    SDL_GetRenderOutputSize(sdl.ren, &w, &h);
    s = fxv_scr_scale(w, h);
    fxv_title_buttons(w, h, sdl.title.btn);
    fxv_title_sound_rect(w, s, sdl.title.snd);
    sdl_backdrop(w, h, 0);

    /* the title, just above the buttons */
    th = (int) (96 * s);
    f = sdl_face(SDL_FACE_TITLE, th);
    y = sdl.title.btn[FXV_TITLE_PLAY][1] - (int) (22 * s) - th / 4 - th / 6;
    (void) sdl_face_text(f, "NetHack", w / 2, y - TTF_GetFontAscent(f), 1,
                         accent, max(2, (int) (3 * s)));
    f = sdl_face(SDL_FACE_UI_BOLD, max(10, th / 4));
    (void) sdl_face_text(f, "M  O  D  E  R  N", w / 2, y + th / 10, 1, text,
                         max(1, (int) (2 * s)));

    /* the buttons, and the music */
    for (i = 0; i < FXV_TITLE_NBTN; i++)
        sdl_button(sdl.title.btn[i], sdl_title_label[i],
                   i == FXV_TITLE_PLAY, sdl.title.hot == i,
                   sdl.title.focus == i,
                   sdl.title.down == i && sdl.title.hot == i, TRUE, s);
    sdl_sound_icon(sdl.title.snd, sdl.title.hot == SDL_TITLE_SOUND, s);

    /* the versions, bottom left; the copyright, bottom right */
    m = (int) (24 * s);
    f = sdl_face(SDL_FACE_UI, max(10, (int) (14 * s)));
    y = h - (int) (18 * s) - TTF_GetFontHeight(f);
    Sprintf(buf, "NetHack %d.%d.%d", VERSION_MAJOR, VERSION_MINOR,
            PATCHLEVEL);
    (void) sdl_face_text(f, buf, m, y, 0, dim, 1);
    (void) sdl_face_text(f, COPYRIGHT_BANNER_D, w - m, y, 2, dim, 1);
    (void) sdl_face_text(f, COPYRIGHT_BANNER_A, w - m, y - (int) (21 * s), 2,
                         dim, 1);
    f = sdl_face(SDL_FACE_UI_BOLD, max(10, (int) (15 * s)));
    Sprintf(buf, "NetHack Modern v%s", MODERN_VERSION_STRING);
    (void) sdl_face_text(f, buf, m, y - (int) (21 * s), 0, accent, 1);
}

/* About: the versions and the copyright, in a text window over it */
static void
sdl_title_about(void)
{
    winid w = sdl_create_nhwindow(NHW_TEXT);
    char buf[BUFSZ];

    Sprintf(buf, "NetHack Modern v%s", MODERN_VERSION_STRING);
    sdl_putstr(w, ATR_BOLD, buf);
    sdl_putstr(w, 0, getversionstring(buf, sizeof buf));
    sdl_putstr(w, 0, "");
    sdl_putstr(w, 0, COPYRIGHT_BANNER_A);
    sdl_putstr(w, 0, COPYRIGHT_BANNER_B);
    sdl_putstr(w, 0, COPYRIGHT_BANNER_C);
    sdl_putstr(w, 0, COPYRIGHT_BANNER_D);
    sdl_putstr(w, 0, "");
    sdl_putstr(w, 0, "NetHack Modern adds presentation only: effects,");
    sdl_putstr(w, 0, "sound and this window. The game is NetHack's own.");
    (void) sdl_run_overlay(sdl_getwin(w), PICK_NONE, (menu_item **) 0);
    sdl_destroy_nhwindow(w);
}

/* show the title screen and wait; returns only when Play is chosen
   (Exit, or closing the window, ends the program) */
static void
sdl_title_screen(void)
{
    int c;

    sdl.title.up = TRUE;
    sdl.title.focus = FXV_TITLE_PLAY, sdl.title.hot = sdl.title.down = -1;
    sdl_music_sync();
    for (;;) {
        sdl.dirty = TRUE;
        c = sdl_wait_key();
        if (c == '\n' || c == '\r' || c == ' ')
            c = (sdl.title.focus == FXV_TITLE_ABOUT) ? 'a'
                : (sdl.title.focus == FXV_TITLE_EXIT) ? 'x' : 'p';
        c = lowc(c);
        if (c == 'p') {
            sdl_play_again = TRUE; /* a finished game comes back here */
            break;
        }
        if (c == 'a') {
            sdl_title_about();
        } else if (c == 'm') {
            sdl_music_toggle();
        } else if (c == 'x' || c == 'q') {
            /* nothing has started: nothing to save */
            sdl_play_again = FALSE;
            sdl_exit_nhwindows((char *) 0);
            nh_terminate(EXIT_SUCCESS);
        }
    }
    sdl.title.up = FALSE;
    sdl.dirty = TRUE;
}


/* ---- the name and character screens (as NetHackW's) --------------- */

/* Like the title screen they fill the window, over the same art, and
   work with the mouse or the keyboard alone. The rules, the words and
   where things go are shared with NetHackW (fxview.c). */


static const char *const sdl_cs_btn_label[2][FXV_CSB_N] = {
    { "Back", "", "Continue" },
    { "Back", "Random", "Play" },
};


static boolean
sdl_cs_btn_used(int b)
{
    return sdl.cs.mode == SDL_CS_CHAR || b != FXV_CSB_RANDOM;
}

/* the name, with spaces trimmed off both ends */
static void
sdl_cs_trimmed(char *out)
{
    const char *p = sdl.cs.name;
    size_t n;

    while (*p == ' ')
        p++;
    Strcpy(out, p);
    n = strlen(out);
    while (n > 0 && out[n - 1] == ' ')
        out[--n] = '\0';
}

/* what's at (x, y): sets *g to a group, SDL_CS_BTN, _BOX or _SND (or
   -1); returns the row or button, or -1 */
static int
sdl_cs_hit(float x, float y, int *g)
{
    struct fxv_cs_layout *L = &sdl.cs.L;
    int i, k;

    *g = -1;
    if (CS_IN(L->snd, x, y)) {
        *g = SDL_CS_SND;
        return 0;
    }
    for (i = 0; i < FXV_CSB_N; i++)
        if (sdl_cs_btn_used(i) && CS_IN(L->btn[i], x, y)) {
            *g = SDL_CS_BTN;
            return i;
        }
    if (sdl.cs.mode == SDL_CS_NAME) {
        if (CS_IN(L->box, x, y)) {
            *g = SDL_CS_BOX;
            return 0;
        }
    } else {
        for (k = 0; k < FXV_CS_NGRP; k++)
            for (i = 0; i < fxv_cs_n[k] && i < FXV_CS_MAXROWS; i++)
                if (CS_IN(L->row[k][i], x, y)) {
                    *g = k;
                    return i;
                }
    }
    return -1;
}

/* a wrapped paragraph from r[1] down, between r[0] and r[2]; moves r[1]
   past it */
static void
sdl_cs_para(TTF_Font *f, const char *t, int r[4], fxv_color c)
{
    static const SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *surf;
    SDL_Texture *tex;
    SDL_FRect d;

    if (!t || !*t)
        return;
    TTF_SetFontWrapAlignment(f, TTF_HORIZONTAL_ALIGN_LEFT);
    if (!(surf = TTF_RenderText_Blended_Wrapped(f, t, 0, white,
                                                max(20, r[2] - r[0]))))
        return;
    tex = SDL_CreateTextureFromSurface(sdl.ren, surf);
    d.x = (float) r[0], d.y = (float) r[1];
    d.w = (float) surf->w, d.h = (float) surf->h;
    r[1] += surf->h;
    SDL_DestroySurface(surf);
    if (tex) {
        (void) SDL_SetTextureColorMod(tex, (Uint8) FXV_R(c),
                                      (Uint8) FXV_G(c), (Uint8) FXV_B(c));
        SDL_RenderTexture(sdl.ren, tex, NULL, &d);
        SDL_DestroyTexture(tex);
    }
}

/* a line of text centred up and down in r, from its left (or centred
   across it with how 1, or up to its right with how 2) */
static void
sdl_cs_line(TTF_Font *f, const char *t, const int r[4], int how,
            fxv_color c, int shadow)
{
    int x = how == 1 ? (r[0] + r[2]) / 2 : how == 2 ? r[2] : r[0];

    (void) sdl_face_text(f, t, x, (r[1] + r[3]) / 2 - TTF_GetFontHeight(f) / 2,
                         how, c, shadow);
}

#define S(v) ((int) ((v) * s + 0.5))
#define CS_ACCENT FXV_RGB(229, 186, 96)
#define CS_TEXT FXV_RGB(222, 226, 233)
#define CS_DIM FXV_RGB(138, 146, 160)
#define CS_PANEL FXV_RGB(24, 27, 33)
#define CS_HEADER FXV_RGB(33, 37, 46)
#define CS_SEP FXV_RGB(52, 58, 70)
#define CS_BG FXV_RGB(14, 16, 20)

static void
sdl_cs_buttons(double s)
{
    int b;

    for (b = 0; b < FXV_CSB_N; b++) {
        boolean ok = TRUE;

        if (!sdl_cs_btn_used(b))
            continue;
        if (b == FXV_CSB_PLAY && sdl.cs.mode == SDL_CS_NAME) {
            char t[PL_NSIZ];

            sdl_cs_trimmed(t);
            ok = (*t != '\0');
        }
        sdl_button(sdl.cs.L.btn[b], sdl_cs_btn_label[sdl.cs.mode][b],
                   b == FXV_CSB_PLAY, sdl.cs.hg == SDL_CS_BTN && sdl.cs.hi == b,
                   sdl.cs.fg == SDL_CS_BTN && sdl.cs.fb == b,
                   sdl.cs.dg == SDL_CS_BTN && sdl.cs.di == b
                       && sdl.cs.hi == b,
                   ok, s);
    }
}

static void
sdl_cs_hints(int w, int h, double s, const char *t)
{
    int r[4];

    r[0] = 0, r[1] = h - S(34), r[2] = w, r[3] = h - S(10);
    sdl_cs_line(sdl_face(SDL_FACE_UI, max(10, S(14))), t, r, 1, CS_DIM, 1);
}

static void
sdl_cs_draw_name(int w, int h, double s)
{
    struct fxv_cs_layout *L = &sdl.cs.L;
    boolean focus = (sdl.cs.fg == SDL_CS_BOX);
    TTF_Font *f;
    int r[4], x;

    f = sdl_face(SDL_FACE_TITLE, S(60));
    (void) sdl_face_text(f, "Who are you?", w / 2,
                         L->top + S(76) - TTF_GetFontHeight(f), 1, CS_ACCENT,
                         max(2, S(3)));

    /* the box */
    sdl_edged(L->box, CS_PANEL, focus ? CS_ACCENT : CS_SEP,
              focus ? max(2, S(2)) : 1);
    f = sdl_face(SDL_FACE_UI_BOLD, max(11, S(26)));
    r[0] = L->box[0] + S(18), r[1] = L->box[1], r[2] = L->box[2] - S(18);
    r[3] = L->box[3];
    x = r[0];
    if (sdl.cs.name[0]) {
        sdl_cs_line(f, sdl.cs.name, r, 0, CS_TEXT, 0);
        x += sdl_text_w_in(f, sdl.cs.name) + S(2);
    } else {
        sdl_cs_line(f, "Enter a name", r, 0, CS_SEP, 0);
    }
    if (focus && (SDL_GetTicks() / 530) % 2 == 0) /* the caret, blinking */
        sdl_fill(x, L->box[1] + S(14), max(2, S(2)),
                 L->box[3] - L->box[1] - 2 * S(14), FXV_R(CS_ACCENT),
                 FXV_G(CS_ACCENT), FXV_B(CS_ACCENT), 255);

    f = sdl_face(SDL_FACE_UI, max(10, S(17)));
    (void) sdl_face_text(f, "If this hero has a saved game, it will carry "
                            "on from there.",
                         w / 2, L->box[3] + S(10), 1, CS_DIM, 1);
    sdl_cs_buttons(s);
    sdl_cs_hints(w, h, s, "Type a name        Enter: continue        "
                          "Esc: back        Ctrl+M: music");
}

static void
sdl_cs_draw_group(int g, double s)
{
    static const char *const title[FXV_CS_NGRP] = {
        "Role", "Race", "Gender", "Alignment"
    };
    const int *p = sdl.cs.L.panel[g];
    boolean focus = (sdl.cs.fg == g);
    int i, sel = fxv_cs_value(g), r[4];
    char buf[BUFSZ];

    sdl_edged(p, CS_PANEL, focus ? CS_ACCENT : CS_SEP,
              focus ? max(2, S(2)) : 1);
    r[0] = p[0] + S(16), r[1] = p[1] + S(6), r[2] = p[2] - S(12);
    r[3] = p[1] + S(36);
    sdl_cs_line(sdl_face(SDL_FACE_UI_BOLD, max(10, S(16))), title[g], r, 0,
                CS_ACCENT, 0);
    for (i = 0; i < fxv_cs_n[g] && i < FXV_CS_MAXROWS; i++) {
        boolean ok = fxv_cs_ok(g, i),
                hot = (sdl.cs.hg == g && sdl.cs.hi == i);
        fxv_color ink;

        (void) memcpy(r, sdl.cs.L.row[g][i], sizeof r);
        if (i == sel) {
            sdl_edged(r, CS_ACCENT, focus ? FXV_RGB(255, 236, 190) : CS_ACCENT,
                      focus ? max(2, S(2)) : 1);
            ink = CS_BG;
        } else if (hot && ok) {
            sdl_edged(r, CS_HEADER, CS_HEADER, 1);
            ink = CS_TEXT;
        } else {
            ink = ok ? CS_TEXT : FXV_RGB(76, 82, 94);
        }
        r[0] += S(12);
        sdl_cs_line(sdl_face(i == sel ? SDL_FACE_UI_BOLD : SDL_FACE_UI,
                             max(10, S(17))),
                    fxv_cs_label(g, i, buf), r, 0, ink, 0);
        if (!ok && hot) { /* a small cross: "not with these choices" */
            r[2] -= S(10);
            sdl_cs_line(sdl_face(SDL_FACE_UI, max(10, S(17))), "x", r, 2,
                        FXV_RGB(200, 96, 80), 0);
        }
    }
}

/* the right-hand panel: about the role pointed at, or the one chosen */
static void
sdl_cs_draw_info(double s)
{
    const int *box = sdl.cs.L.info;
    int rl = (sdl.cs.hg == FXV_CS_ROLE && sdl.cs.hi >= 0) ? sdl.cs.hi
                                                         : flags.initrole;
    TTF_Font *head = sdl_face(SDL_FACE_UI_BOLD, max(10, S(16)));
    int r[4], line[4];
    char buf[BUFSZ];

    sdl_edged(box, CS_PANEL, CS_SEP, 1);
    r[0] = box[0] + S(22), r[1] = box[1] + S(16), r[2] = box[2] - S(22);
    r[3] = box[3];
    line[0] = r[0], line[1] = r[1], line[2] = r[2], line[3] = r[1] + S(42);
    sdl_cs_line(sdl_face(SDL_FACE_TITLE, S(32)), fxv_cs_role_name(rl), line,
                0, CS_ACCENT, 0);
    r[1] = line[3];

    fxv_cs_info(rl, FXV_CSI_STARTS, buf);
    sdl_cs_para(sdl_face(SDL_FACE_UI, max(10, S(15))), buf, r, CS_DIM);
    r[1] += S(12);
    fxv_cs_info(rl, FXV_CSI_BLURB, buf);
    if (*buf) {
        sdl_cs_para(sdl_face(SDL_FACE_UI, max(10, S(15))), buf, r, CS_TEXT);
        r[1] += S(16);
    }
    sdl_cs_para(head, "Races", r, CS_ACCENT);
    fxv_cs_info(rl, FXV_CSI_RACES, buf);
    sdl_cs_para(sdl_face(SDL_FACE_UI, max(10, S(15))), buf, r, CS_TEXT);
    r[1] += S(8);
    sdl_cs_para(sdl_face(SDL_FACE_UI_BOLD, max(10, S(16))), "Alignments", r,
                CS_ACCENT);
    fxv_cs_info(rl, FXV_CSI_ALIGNS, buf);
    sdl_cs_para(sdl_face(SDL_FACE_UI, max(10, S(15))), buf, r, CS_TEXT);
    fxv_cs_info(rl, FXV_CSI_GENDER, buf);
    if (*buf)
        sdl_cs_para(sdl_face(SDL_FACE_UI, max(10, S(15))), buf, r, CS_DIM);
    r[1] += S(8);
    sdl_cs_para(sdl_face(SDL_FACE_UI_BOLD, max(10, S(16))), "Gods", r,
                CS_ACCENT);
    fxv_cs_info(rl, FXV_CSI_GODS, buf);
    sdl_cs_para(sdl_face(SDL_FACE_UI, max(10, S(15))), buf, r, CS_TEXT);

    /* pointing at a choice that doesn't fit: say why */
    if (sdl.cs.hg >= FXV_CS_RACE && sdl.cs.hg < FXV_CS_NGRP
        && sdl.cs.hi >= 0 && !fxv_cs_ok(sdl.cs.hg, sdl.cs.hi)) {
        fxv_cs_why_not(sdl.cs.hg, sdl.cs.hi, buf);
        line[0] = box[0] + S(22), line[2] = box[2] - S(22);
        line[3] = box[3] - S(16), line[1] = line[3] - S(26);
        sdl_cs_line(sdl_face(SDL_FACE_UI_BOLD, max(10, S(17))), buf, line, 0,
                    FXV_RGB(232, 128, 106), 0);
    }
}

static void
sdl_cs_draw_char(int w, int h, double s)
{
    int r[4], g;
    char buf[BUFSZ];

    r[0] = 0, r[1] = sdl.cs.L.top, r[2] = w, r[3] = sdl.cs.L.top + S(62);
    sdl_cs_line(sdl_face(SDL_FACE_TITLE, S(48)), "Create your hero", r, 1,
                CS_ACCENT, max(2, S(3)));
    fxv_cs_hero_line(buf); /* the hero as chosen so far */
    r[1] = sdl.cs.L.top + S(64), r[3] = sdl.cs.L.top + S(94);
    sdl_cs_line(sdl_face(SDL_FACE_UI, max(10, S(19))), buf, r, 1, CS_TEXT,
                1);
    for (g = 0; g < FXV_CS_NGRP; g++)
        sdl_cs_draw_group(g, s);
    sdl_cs_draw_info(s);
    sdl_cs_buttons(s);
    sdl_cs_hints(w, h, s, "Arrows or mouse: choose      Tab: next      "
                          "Enter: play      *: random      Esc: back      "
                          "Ctrl+M: music");
}

static void
sdl_draw_cs(void)
{
    int w, h;

    SDL_GetRenderOutputSize(sdl.ren, &w, &h);
    fxv_cs_layout(&sdl.cs.L, w, h, sdl.cs.mode == SDL_CS_NAME);
    sdl_backdrop(w, h, sdl.cs.mode == SDL_CS_CHAR ? 150 : 40);
    if (sdl.cs.mode == SDL_CS_NAME)
        sdl_cs_draw_name(w, h, sdl.cs.L.s);
    else
        sdl_cs_draw_char(w, h, sdl.cs.L.s);
    sdl_sound_icon(sdl.cs.L.snd, sdl.cs.hg == SDL_CS_SND, sdl.cs.L.s);
}

#undef S

/* moving about */
static void
sdl_cs_focus_group(int g)
{
    sdl.cs.fg = sdl.cs.lastgrp = g;
    if (g != FXV_CS_ROLE)
        sdl.cs.lastmid = g;
}

static void
sdl_cs_focus_btn(int b)
{
    sdl.cs.fg = SDL_CS_BTN, sdl.cs.fb = b;
}

static int
sdl_cs_next_btn(int dir)
{
    int b = sdl.cs.fb;

    do
        b = (b + FXV_CSB_N + dir) % FXV_CSB_N;
    while (!sdl_cs_btn_used(b));
    return b;
}

static boolean
sdl_cs_move(int g, int dir)
{
    int i = fxv_cs_step(g, fxv_cs_value(g), dir);

    return i >= 0 && fxv_cs_pick(g, i);
}

/* Tab and Shift+Tab: through the groups, then the buttons */
static void
sdl_cs_tab(int dir)
{
    static const int stops[2][7] = {
        { SDL_CS_BOX, 10 + FXV_CSB_PLAY, 10 + FXV_CSB_BACK, -1 },
        { FXV_CS_ROLE, FXV_CS_RACE, FXV_CS_GEND, FXV_CS_ALIGN,
          10 + FXV_CSB_PLAY, 10 + FXV_CSB_RANDOM, 10 + FXV_CSB_BACK },
    };
    const int *st = stops[sdl.cs.mode];
    int n = (sdl.cs.mode == SDL_CS_NAME) ? 3 : 7, cur = 0, k;

    for (k = 0; k < n; k++)
        if ((sdl.cs.fg == SDL_CS_BTN) ? st[k] == 10 + sdl.cs.fb
                                      : st[k] == sdl.cs.fg)
            cur = k;
    k = st[(cur + n + dir) % n];
    if (k >= 10)
        sdl_cs_focus_btn(k - 10);
    else if (k == SDL_CS_BOX)
        sdl.cs.fg = SDL_CS_BOX;
    else
        sdl_cs_focus_group(k);
}

/* a button: 1 go on, 0 back, -1 carry on here */
static int
sdl_cs_activate(int b)
{
    if (b == FXV_CSB_BACK)
        return 0;
    if (b == FXV_CSB_RANDOM) {
        if (sdl.cs.mode == SDL_CS_CHAR)
            (void) fxv_cs_randomize();
        return -1;
    }
    if (sdl.cs.mode == SDL_CS_NAME) {
        char t[PL_NSIZ];

        sdl_cs_trimmed(t);
        return *t ? 1 : -1;
    }
    return 1;
}

/* show a screen and wait: 1 to go on, 0 to go back */
static int
sdl_cs_run(int mode)
{
    int c, res = -1;

    sdl.cs.mode = mode;
    sdl.cs.hg = sdl.cs.hi = sdl.cs.dg = sdl.cs.di = -1;
    if (mode == SDL_CS_NAME) {
        sdl.cs.fg = SDL_CS_BOX;
    } else {
        sdl.cs.fg = sdl.cs.lastgrp = FXV_CS_ROLE;
        sdl.cs.lastmid = FXV_CS_RACE;
    }
    sdl.cs.fb = FXV_CSB_PLAY;
    while (res < 0) {
        int g = sdl.cs.fg;

        sdl.dirty = TRUE;
        c = sdl_wait_key();
        if (c >= SDL_CSK_BTN && c < SDL_CSK_BTN + FXV_CSB_N) {
            res = sdl_cs_activate(c - SDL_CSK_BTN);
            continue;
        }
        switch (c) {
        case '\033':
            res = sdl_cs_activate(FXV_CSB_BACK);
            continue;
        case '\n':
        case '\r':
            res = sdl_cs_activate(g == SDL_CS_BTN ? sdl.cs.fb : FXV_CSB_PLAY);
            continue;
        case SDL_CSK_MUSIC:
            sdl_music_toggle();
            continue;
        case SDL_CSK_CLEAR:
            if (mode == SDL_CS_NAME)
                sdl.cs.name[0] = '\0', sdl.cs.fg = SDL_CS_BOX;
            continue;
        case SDL_CSK_TAB:
        case SDL_CSK_BACKTAB:
            sdl_cs_tab(c == SDL_CSK_TAB ? 1 : -1);
            continue;
        case SDL_CSK_UP:
            if (g == SDL_CS_BTN)
                sdl.cs.fg = (mode == SDL_CS_NAME) ? SDL_CS_BOX
                                                  : sdl.cs.lastgrp;
            else if (g < FXV_CS_NGRP && !sdl_cs_move(g, -1)) {
                if (g == FXV_CS_GEND)
                    sdl_cs_focus_group(FXV_CS_RACE);
                else if (g == FXV_CS_ALIGN)
                    sdl_cs_focus_group(FXV_CS_GEND);
            }
            continue;
        case SDL_CSK_DOWN:
            if (g == SDL_CS_BOX) {
                sdl_cs_focus_btn(FXV_CSB_PLAY);
            } else if (g < FXV_CS_NGRP && !sdl_cs_move(g, 1)) {
                if (g == FXV_CS_RACE)
                    sdl_cs_focus_group(FXV_CS_GEND);
                else if (g == FXV_CS_GEND)
                    sdl_cs_focus_group(FXV_CS_ALIGN);
                else
                    sdl_cs_focus_btn(FXV_CSB_PLAY);
            }
            continue;
        case SDL_CSK_LEFT:
            if (g == SDL_CS_BTN)
                sdl_cs_focus_btn(sdl_cs_next_btn(-1));
            else if (g > FXV_CS_ROLE && g < FXV_CS_NGRP)
                sdl_cs_focus_group(FXV_CS_ROLE);
            continue;
        case SDL_CSK_RIGHT:
            if (g == SDL_CS_BTN)
                sdl_cs_focus_btn(sdl_cs_next_btn(1));
            else if (g == FXV_CS_ROLE)
                sdl_cs_focus_group(sdl.cs.lastmid);
            continue;
        case SDL_CSK_HOME:
        case SDL_CSK_END:
            if (g < FXV_CS_NGRP) {
                int i = (c == SDL_CSK_HOME) ? fxv_cs_step(g, -1, 1)
                                            : fxv_cs_step(g, fxv_cs_n[g], -1);

                if (i >= 0)
                    (void) fxv_cs_pick(g, i);
            }
            continue;
        default:
            break;
        }
        if (c >= 0x100)
            continue;
        if (mode == SDL_CS_NAME) {
            size_t n = strlen(sdl.cs.name);

            if (c == ' ' && g == SDL_CS_BTN) {
                res = sdl_cs_activate(sdl.cs.fb); /* Space pressed it */
                continue;
            }
            if (c == '\b') {
                if (n)
                    sdl.cs.name[n - 1] = '\0';
            } else if (c >= ' ' && c <= '~') {
                if (n == 0 && c == ' ')
                    continue;
                if (n < sizeof sdl.cs.name - 1)
                    sdl.cs.name[n] = (char) c, sdl.cs.name[n + 1] = '\0';
            } else {
                continue;
            }
            sdl.cs.fg = SDL_CS_BOX;
        } else if (c == ' ' && g == SDL_CS_BTN) {
            res = sdl_cs_activate(sdl.cs.fb);
        } else if (c == '*') {
            res = sdl_cs_activate(FXV_CSB_RANDOM);
        } else if (c < 128 && isalpha(c)) {
            /* the next choice in the focused group (the roles, from the
               buttons) that starts with it */
            int grp = (g < FXV_CS_NGRP) ? g : FXV_CS_ROLE,
                i = fxv_cs_letter(grp, c);

            if (i >= 0 && fxv_cs_pick(grp, i))
                sdl_cs_focus_group(grp);
        }
    }
    sdl.cs.mode = -1;
    sdl.dirty = TRUE;
    return res;
}

/* the name screen; FALSE for Back */
static boolean
sdl_name_screen(void)
{
    char t[PL_NSIZ];

    (void) strncpy(sdl.cs.name, svp.plname, sizeof sdl.cs.name - 1);
    sdl.cs.name[sizeof sdl.cs.name - 1] = '\0';
    if (!sdl_cs_run(SDL_CS_NAME))
        return FALSE;
    sdl_cs_trimmed(t);
    (void) strncpy(svp.plname, t, sizeof svp.plname - 1);
    svp.plname[sizeof svp.plname - 1] = '\0';
    return TRUE;
}


/* ---- the death screen and the score list (as NetHackW's) ----------- */

/* They take the place of the tombstone and the plain text score list,
   fill the window, and work with the mouse or the keyboard alone. What
   they show is gathered by the shared code (fxview.c). */

#define ES_BONE FXV_RGB(232, 226, 212)  /* the death screen's pale text */
#define ES_FAINT FXV_RGB(104, 110, 122) /* dimmer than CS_DIM */
#define S(v) ((int) ((v) * s + 0.5))

/* a line of text cut short with "..." to fit in px, in font f */
static void
sdl_fit_in(TTF_Font *f, char *buf, size_t sz, const char *t, int px)
{
    size_t n;

    Snprintf(buf, sz, "%s", t);
    if (sdl_text_w_in(f, buf) <= px)
        return;
    for (n = strlen(buf); n > 1; n--) {
        Snprintf(buf + n - 1, sz - (n - 1), "...");
        if (sdl_text_w_in(f, buf) <= px)
            return;
    }
}

/* the painting drained of its colour, cold and dark at the edges, for
   the death screen; made once from the art's pixels */
static SDL_Texture *
sdl_art_grey(void)
{
    SDL_Surface *g;
    int x, y, w, h;

    if (sdl.art_grey || !sdl.art_px)
        return sdl.art_grey;
    w = sdl.art_px->w, h = sdl.art_px->h;
    if (!(g = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_XRGB8888)))
        return 0;
    for (y = 0; y < h; y++) {
        const Uint32 *src = (const Uint32 *) ((const Uint8 *) sdl.art_px->pixels
                                              + (size_t) y * sdl.art_px->pitch);
        Uint32 *dst = (Uint32 *) ((Uint8 *) g->pixels + (size_t) y * g->pitch);
        double dy = (y - h * 0.45) / (h * 0.70);

        for (x = 0; x < w; x++) {
            Uint32 v = src[x];
            int r = (v >> 16) & 255, green = (v >> 8) & 255, b = v & 255;
            double dx = (x - w / 2.0) / (w * 0.62), d = dx * dx + dy * dy,
                   f = 0.40 * (1.0 - 0.75 * min(d, 1.0)),
                   l = (0.30 * r + 0.59 * green + 0.11 * b) * f;

            r = (int) (l * 0.90), green = (int) (l * 0.96);
            b = (int) min(l * 1.10 + 4, 255.0);
            dst[x] = ((Uint32) r << 16) | ((Uint32) green << 8) | (Uint32) b;
        }
    }
    sdl.art_grey = SDL_CreateTextureFromSurface(sdl.ren, g);
    SDL_DestroySurface(g);
    if (sdl.art_grey)
        (void) SDL_SetTextureScaleMode(sdl.art_grey, SDL_SCALEMODE_LINEAR);
    return sdl.art_grey;
}

/* the backdrop: the painting; for the death screen drained of colour,
   for the score list darkened */
static void
sdl_es_backdrop(int w, int h)
{
    SDL_Texture *g;

    if (sdl.es.mode != SDL_ES_DEATH || !(g = sdl_art_grey())) {
        sdl_backdrop(w, h, sdl.es.mode == SDL_ES_DEATH ? 170 : 195);
        return;
    }
    sdl_fill(0, 0, w, h, 14, 16, 20, 255);
    {
        float sc = max((float) w / sdl.art_w, (float) h / sdl.art_h);
        SDL_FRect d;

        d.w = sdl.art_w * sc, d.h = sdl.art_h * sc;
        d.x = (w - d.w) / 2, d.y = (h - d.h) / 3;
        SDL_RenderTexture(sdl.ren, g, NULL, &d);
    }
}

/* is (x, y) on a tombstone w x h: a rounded head on a block? */
static boolean
sdl_on_stone(int x, int y, int w, int h)
{
    if (x < 0 || x >= w || y < 0 || y >= h)
        return FALSE;
    if (y >= w / 2)
        return TRUE;
    {
        double dx = (x + 0.5 - w / 2.0) / (w / 2.0),
               dy = (y + 0.5 - w / 2.0) / (w / 2.0);

        return dx * dx + dy * dy <= 1.0;
    }
}

/* the tombstone's face, w x h: lit from the upper left, weathered, a
   crack, an edge; made once for its size */
static SDL_Texture *
sdl_stone(int w, int h, double s)
{
    SDL_Surface *g;
    Uint32 *px;
    unsigned long seed = 0x2545F491UL;
    int x, y, i, pitch;

    if (sdl.stone && sdl.stone_w == w && sdl.stone_h == h)
        return sdl.stone;
    if (sdl.stone)
        SDL_DestroyTexture(sdl.stone), sdl.stone = 0;
    if (w < 8 || h < 8
        || !(g = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888)))
        return 0;
    px = (Uint32 *) g->pixels, pitch = g->pitch / 4;
#define PUT(xx, yy, r, grey, b) \
    do { if (sdl_on_stone((xx), (yy), w, h)) \
             px[(yy) * pitch + (xx)] = 0xFF000000U | ((Uint32) (r) << 16) \
                                       | ((Uint32) (grey) << 8) | (Uint32) (b); \
    } while (0)
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            double t = (double) x / max(1, w - 1); /* a sideways gradient */

            px[y * pitch + x] = 0;
            PUT(x, y, (int) (0xA4 + (0x5C - 0xA4) * t),
                (int) (0xA4 + (0x5E - 0xA4) * t),
                (int) (0xA0 + (0x62 - 0xA0) * t));
        }
    /* weathering: specks, and moss creeping up from the ground */
    for (i = 0; i < 520; i++) {
        int sz, grey, r, green, b, k2, j;

        seed = seed * 1103515245UL + 12345UL;
        x = (int) ((seed >> 8) % (unsigned long) w);
        seed = seed * 1103515245UL + 12345UL;
        y = (int) ((seed >> 8) % (unsigned long) h);
        sz = max(1, S((seed >> 4) % 3 ? 1 : 2));
        if (i < 380) { /* just darker or lighter than the stone */
            grey = (int) ((seed >> 12) % 24) + 8;
            if ((seed >> 20) & 1)
                r = 88 - grey, green = 90 - grey, b = 94 - grey;
            else
                r = 124 + grey, green = 124 + grey, b = 121 + grey;
        } else { /* moss: low down, thicker toward the ground */
            unsigned long k = (seed >> 9) % (unsigned long) max(1, S(70));

            y = h - 1 - (int) (k * k / (unsigned long) max(1, S(70)));
            r = 70 + (int) (seed % 16), green = 88 + (int) (seed % 18), b = 60;
            sz = max(2, S(3));
        }
        for (k2 = 0; k2 < sz; k2++)
            for (j = 0; j < sz; j++)
                PUT(x + j, y + k2, r, green, b);
    }
    /* a crack */
    {
        int pts[4][2], seg;

        pts[0][0] = w - w / 5, pts[0][1] = w / 3;
        pts[1][0] = pts[0][0] - S(9), pts[1][1] = pts[0][1] + S(26);
        pts[2][0] = pts[0][0] - S(4), pts[2][1] = pts[0][1] + S(44);
        pts[3][0] = pts[0][0] - S(16), pts[3][1] = pts[0][1] + S(70);
        for (seg = 0; seg < 3; seg++) {
            int n = max(abs(pts[seg + 1][0] - pts[seg][0]),
                        abs(pts[seg + 1][1] - pts[seg][1])), k;

            for (k = 0; k <= n; k++) {
                int cx = pts[seg][0] + (pts[seg + 1][0] - pts[seg][0]) * k
                                           / max(1, n),
                    cy = pts[seg][1] + (pts[seg + 1][1] - pts[seg][1]) * k
                                           / max(1, n), q;

                for (q = 0; q < max(1, S(1)); q++)
                    PUT(cx + q, cy, 58, 59, 62);
            }
        }
    }
    /* the edge, a little in from the outline */
    {
        int e = max(2, S(2)), dx, dy;

        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                boolean edge = FALSE;

                if (!sdl_on_stone(x, y, w, h))
                    continue;
                for (dy = -e; dy <= e && !edge; dy += e)
                    for (dx = -e; dx <= e && !edge; dx += e)
                        if (!sdl_on_stone(x + dx, y + dy, w, h + e + 1))
                            edge = TRUE;
                if (edge)
                    px[y * pitch + x] = 0xFF000000U | (40U << 16) | (42U << 8)
                                        | 45U;
            }
    }
#undef PUT
    sdl.stone = SDL_CreateTextureFromSurface(sdl.ren, g);
    sdl.stone_w = w, sdl.stone_h = h;
    SDL_DestroySurface(g);
    return sdl.stone;
}

/* text cut into the stone: dark, with a lit lower edge; wrapped to
   the width of r, centred; returns its height */
static int
sdl_carve(TTF_Font *f, const char *t, const int r[4], double s)
{
    static const SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *surf;
    SDL_Texture *tex;
    SDL_FRect d;
    int k = max(1, S(1)), hh;

    TTF_SetFontWrapAlignment(f, TTF_HORIZONTAL_ALIGN_CENTER);
    if (!t || !*t
        || !(surf = TTF_RenderText_Blended_Wrapped(f, t, 0, white,
                                                   max(20, r[2] - r[0]))))
        return 0;
    tex = SDL_CreateTextureFromSurface(sdl.ren, surf);
    d.w = (float) surf->w, d.h = (float) surf->h;
    d.x = (float) ((r[0] + r[2]) / 2 - surf->w / 2), d.y = (float) r[1];
    hh = surf->h;
    SDL_DestroySurface(surf);
    if (!tex)
        return hh;
    d.x += k, d.y += k;
    (void) SDL_SetTextureColorMod(tex, 178, 178, 174);
    SDL_RenderTexture(sdl.ren, tex, NULL, &d);
    d.x -= k, d.y -= k;
    (void) SDL_SetTextureColorMod(tex, 46, 47, 50);
    SDL_RenderTexture(sdl.ren, tex, NULL, &d);
    SDL_DestroyTexture(tex);
    return hh;
}

/* the tombstone, with the game's four lines carved in it */
static void
sdl_es_stone(const int box[4], double s)
{
    int w = box[2] - box[0], h = box[3] - box[1], i, r[4], y;
    SDL_Texture *t;

    /* the earth it stands in: a dark low mound */
    {
        int cx = (box[0] + box[2]) / 2, rx = w / 2 + S(50), ry = S(26);

        for (y = -ry; y <= ry; y++) {
            double f = 1.0 - (double) (y * y) / (ry * ry);
            int half = (int) (rx * sqrt(max(0.0, f)));

            sdl_fill(cx - half, box[3] + y, 2 * half, 1, 14, 16, 15, 255);
        }
    }
    if ((t = sdl_stone(w, h, s)) != 0) {
        SDL_FRect d;

        d.x = (float) box[0], d.y = (float) box[1];
        d.w = (float) w, d.h = (float) h;
        SDL_RenderTexture(sdl.ren, t, NULL, &d);
    }
    /* the words */
    r[0] = box[0], r[2] = box[2];
    r[1] = box[1] + S(34) + (S(76) - TTF_GetFontHeight(sdl_face(SDL_FACE_TITLE,
                                                                S(66)))) / 2;
    (void) sdl_carve(sdl_face(SDL_FACE_TITLE, S(66)), "RIP", r, s);
    sdl_fill(box[0] + S(46), box[1] + S(122) - max(1, S(2)) / 2,
             w - 2 * S(46), max(1, S(2)), 70, 71, 74, 255);
    r[0] = box[0] + S(22), r[2] = box[2] - S(22), r[1] = box[1] + S(138);
    for (i = 0; i < fxv_end.nstone; i++)
        r[1] += sdl_carve(sdl_face(SDL_FACE_TITLE,
                                   i == 0 ? S(27) : max(10, S(19))),
                          fxv_end.stone[i], r, s)
                + S(i == 0 ? 10 : 6);
}

/* a wrapped paragraph's height at width px */
static int
sdl_para_h(TTF_Font *f, const char *t, int px)
{
    int w = 0, h = 0;

    if (!TTF_GetStringSizeWrapped(f, t, 0, max(20, px), &w, &h))
        return 0;
    return h;
}

static void
sdl_es_draw_death(int w, int h, double s)
{
    int x0 = (w - S(900)) / 2, top = max(S(14), (h - S(640)) / 2);
    int stone[4], panel[4], r[4], i, y;
    char buf[BUFSZ];
    TTF_Font *f;

    r[0] = 0, r[1] = top, r[2] = w, r[3] = top + S(22);
    sdl_cs_line(sdl_face(SDL_FACE_UI_BOLD, max(10, S(15))),
                "R E S T    I N    P E A C E", r, 1, FXV_RGB(150, 60, 52), 1);
    f = sdl_face(SDL_FACE_TITLE, S(44));
    sdl_fit_in(f, buf, sizeof buf, fxv_end.title, w - S(40));
    r[1] = top + S(26), r[3] = top + S(86);
    sdl_cs_line(f, buf, r, 1, ES_BONE, max(2, S(3)));
    f = sdl_face(SDL_FACE_TITLE, max(10, S(20)));
    sdl_fit_in(f, buf, sizeof buf, fxv_end.cause, w - S(40));
    r[1] = top + S(88), r[3] = top + S(118);
    sdl_cs_line(f, buf, r, 1, FXV_RGB(190, 192, 198), 1);

    /* the stone on the left, the last words and the tally on the right */
    stone[0] = x0 + S(40), stone[1] = top + S(156);
    stone[2] = x0 + S(310), stone[3] = top + S(510);
    sdl_es_stone(stone, s);

    panel[0] = x0 + S(370), panel[1] = top + S(156);
    panel[2] = x0 + S(900), panel[3] = top + S(510);
    sdl_rbox(panel[0], panel[1], panel[2] - panel[0], panel[3] - panel[1],
             FXV_CORNER, FXV_RGB(64, 68, 78));
    sdl_fill(panel[0] + 1, panel[1] + 1, panel[2] - panel[0] - 2,
             panel[3] - panel[1] - 2, 8, 9, 12, 190);

    r[0] = panel[0] + S(26), r[1] = panel[1] + S(16);
    r[2] = panel[2] - S(20), r[3] = panel[1] + S(40);
    sdl_cs_line(sdl_face(SDL_FACE_UI_BOLD, max(10, S(15))), "FINAL WORDS", r,
                0, CS_ACCENT, 0);
    /* the words themselves, under a large opening quote */
    (void) sdl_face_text(sdl_face(SDL_FACE_TITLE, S(72)), "\xE2\x80\x9C",
                         panel[0] + S(20), panel[1] + S(32), 0,
                         FXV_RGB(120, 50, 44), 0);
    {
        int area[4], n0 = 0, hsum;

        f = sdl_face(SDL_FACE_TITLE, max(11, S(23)));
        TTF_SetFontStyle(f, TTF_STYLE_ITALIC | TTF_STYLE_BOLD);
        area[0] = panel[0] + S(70), area[1] = panel[1] + S(52);
        area[2] = panel[2] - S(28), area[3] = panel[1] + S(196);
        if (!fxv_end.nlast) {
            r[0] = area[0], r[1] = area[1], r[2] = area[2], r[3] = area[3];
            sdl_cs_para(f, "(nothing more was said)", r, ES_FAINT);
        } else {
            /* drop the oldest until the rest fit */
            for (;;) {
                hsum = 0;
                for (i = n0; i < fxv_end.nlast; i++)
                    hsum += sdl_para_h(f, fxv_end.last[i], area[2] - area[0])
                            + S(6);
                if (hsum <= area[3] - area[1] || n0 >= fxv_end.nlast - 1)
                    break;
                n0++;
            }
            r[0] = area[0], r[1] = area[1], r[2] = area[2], r[3] = area[3];
            for (i = n0; i < fxv_end.nlast; i++) {
                sdl_cs_para(f, fxv_end.last[i], r,
                            i == fxv_end.nlast - 1 ? ES_BONE : CS_DIM);
                r[1] += S(6);
            }
        }
        TTF_SetFontStyle(f, TTF_STYLE_BOLD);
    }

    /* the tally */
    y = panel[1] + S(212);
    sdl_fill(panel[0] + S(26), y, panel[2] - panel[0] - S(52), 1, 60, 64, 74,
             255);
    for (i = 0; i < 6; i++) {
        int cw = (panel[2] - panel[0] - S(52)) / 3;
        int cx = panel[0] + S(26) + (i % 3) * cw;
        int cy = panel[1] + S(226) + (i / 3) * S(62);

        r[0] = cx, r[1] = cy, r[2] = cx + cw, r[3] = cy + S(36);
        sdl_cs_line(sdl_face(SDL_FACE_TITLE, S(30)), fxv_end.stat[i].value, r,
                    0, ES_BONE, 0);
        r[1] = cy + S(36), r[3] = cy + S(54);
        sdl_cs_line(sdl_face(SDL_FACE_UI_BOLD, max(9, S(12))),
                    fxv_end.stat[i].label, r, 0, CS_DIM, 0);
    }

    sdl.es.btn[0] = (w - S(240)) / 2, sdl.es.btn[1] = top + S(548);
    sdl.es.btn[2] = (w + S(240)) / 2, sdl.es.btn[3] = top + S(594);
    sdl_button(sdl.es.btn, "Continue", TRUE, sdl.es.hot, TRUE,
               sdl.es.down && sdl.es.hot, TRUE, s);
    sdl_cs_hints(w, h, s, "Enter, Space or Esc: continue");
}

/* a rank in a circle: gold, silver and bronze for the first three */
static void
sdl_es_badge(int cx, int cy, int rank, double s)
{
    static const fxv_color medal[3] = {
        FXV_RGB(229, 186, 96), FXV_RGB(196, 202, 212), FXV_RGB(196, 132, 84)
    };
    int rad = S(17), r[4];
    char buf[16];

    if (rank >= 1 && rank <= 3) {
        sdl_dot((float) cx, (float) cy, (float) rad, medal[rank - 1], FALSE);
    } else {
        sdl_dot((float) cx, (float) cy, (float) rad, CS_SEP, FALSE);
        sdl_dot((float) cx, (float) cy, (float) (rad - max(1, S(1))),
                FXV_RGB(10, 11, 14), FALSE);
    }
    if (rank > 0)
        Sprintf(buf, "%d", rank);
    else
        Strcpy(buf, "-");
    r[0] = cx - rad, r[1] = cy - rad, r[2] = cx + rad, r[3] = cy + rad;
    sdl_cs_line(sdl_face(SDL_FACE_UI_BOLD, max(10, S(16))), buf, r, 1,
                (rank >= 1 && rank <= 3) ? CS_BG : CS_DIM, 0);
}

static void
sdl_es_draw_scores(int w, int h, double s)
{
    int pw = min(w - S(48), S(1040)), pl = (w - pw) / 2, pr = pl + pw;
    int top = max(S(14), (h - S(700)) / 2), rh = S(64), hh = S(36);
    int ptop = top + (fxv_sl_note[0] ? S(118) : S(90)),
        pbot = h - S(34) - S(46) - S(34);
    int xpts = pl + S(196), xwho = pl + S(222), xfate = pl + S(222) + S(260),
        xhp = pr - S(110);
    int r[4], i, y;
    char buf[BUFSZ];
    TTF_Font *f;

    r[0] = 0, r[1] = top, r[2] = w, r[3] = top + S(62);
    sdl_cs_line(sdl_face(SDL_FACE_TITLE, S(48)), "High Scores", r, 1,
                CS_ACCENT, max(2, S(3)));
    f = sdl_face(SDL_FACE_UI, max(10, S(19)));
    sdl_fit_in(f, buf, sizeof buf, fxv_sl_note, w - S(40));
    r[1] = top + S(66), r[3] = top + S(96);
    sdl_cs_line(f, buf, r, 1, CS_TEXT, 1);

    /* the table */
    if (fxv_sl_nent) {
        int need = hh + fxv_sl_nent * rh + S(10);

        if (ptop + need < pbot)
            pbot = ptop + need;
    } else {
        pbot = min(pbot, ptop + S(120));
    }
    sdl_rbox(pl, ptop, pr - pl, pbot - ptop, FXV_CORNER, CS_SEP);
    sdl_fill(pl + 1, ptop + 1, pr - pl - 2, pbot - ptop - 2, 10, 11, 14, 205);

    if (!fxv_sl_nent) {
        r[0] = pl, r[1] = ptop, r[2] = pr, r[3] = pbot;
        sdl_cs_line(sdl_face(SDL_FACE_UI, max(10, S(16))),
                    "No scores to show.", r, 1, CS_DIM, 0);
    } else {
        TTF_Font *head = sdl_face(SDL_FACE_UI_BOLD, max(9, S(13)));

        y = ptop + S(10);
        (void) sdl_face_text(head, "RANK", pl + S(18), y, 0, CS_DIM, 0);
        (void) sdl_face_text(head, "POINTS", xpts, y, 2, CS_DIM, 0);
        (void) sdl_face_text(head, "HERO", xwho, y, 0, CS_DIM, 0);
        (void) sdl_face_text(head, "FATE", xfate, y, 0, CS_DIM, 0);
        (void) sdl_face_text(head, "HP", pr - S(20), y, 2, CS_DIM, 0);

        sdl.es.fit = max(1, (pbot - ptop - hh - S(6)) / rh);
        if (sdl.es.top > fxv_sl_nent - sdl.es.fit)
            sdl.es.top = max(0, fxv_sl_nent - sdl.es.fit);
        for (i = sdl.es.top; i < fxv_sl_nent && i < sdl.es.top + sdl.es.fit;
             i++) {
            struct fxv_sl_entry *e = &fxv_sl_ent[i];
            int row[4];

            y = ptop + hh + (i - sdl.es.top) * rh;
            row[0] = pl + S(6), row[1] = y, row[2] = pr - S(6);
            row[3] = y + rh - S(4);
            if (e->hero) {
                sdl_edged(row, FXV_RGB(52, 44, 26), FXV_RGB(120, 96, 50), 1);
                sdl_fill(row[0], row[1] + S(8), S(4),
                         row[3] - row[1] - 2 * S(8), FXV_R(CS_ACCENT),
                         FXV_G(CS_ACCENT), FXV_B(CS_ACCENT), 255);
            } else if ((i - sdl.es.top) % 2) {
                sdl_edged(row, FXV_RGB(27, 30, 36), FXV_RGB(27, 30, 36), 1);
            }
            sdl_es_badge(pl + S(36), (row[1] + row[3]) / 2, e->rank, s);

            Sprintf(buf, "%ld", e->points);
            r[0] = pl + S(60), r[1] = row[1], r[2] = xpts, r[3] = row[3];
            sdl_cs_line(sdl_face(SDL_FACE_TITLE, max(11, S(22))), buf, r, 2,
                        e->hero ? CS_ACCENT : CS_TEXT, 0);

            f = sdl_face(SDL_FACE_UI_BOLD, max(10, S(18)));
            sdl_fit_in(f, buf, sizeof buf, e->name, xfate - S(16) - xwho);
            r[0] = xwho, r[1] = row[1] + S(9), r[2] = xfate - S(16);
            r[3] = row[1] + S(33);
            sdl_cs_line(f, buf, r, 0, e->hero ? CS_ACCENT : CS_TEXT, 0);
            f = sdl_face(SDL_FACE_UI, max(9, S(14)));
            sdl_fit_in(f, buf, sizeof buf, e->who, xfate - S(16) - xwho);
            r[1] = row[1] + S(33), r[3] = row[1] + S(54);
            sdl_cs_line(f, buf, r, 0, CS_DIM, 0);

            f = sdl_face(SDL_FACE_UI, max(10, S(16)));
            sdl_fit_in(f, buf, sizeof buf, e->fate, xhp - S(16) - xfate);
            r[0] = xfate, r[2] = xhp - S(16);
            r[1] = row[1] + (e->place[0] ? S(9) : 0);
            r[3] = e->place[0] ? row[1] + S(33) : row[3];
            sdl_cs_line(f, buf, r, 0, CS_TEXT, 0);
            if (e->place[0]) {
                f = sdl_face(SDL_FACE_UI, max(9, S(14)));
                sdl_fit_in(f, buf, sizeof buf, e->place, xhp - S(16) - xfate);
                r[1] = row[1] + S(33), r[3] = row[1] + S(54);
                sdl_cs_line(f, buf, r, 0, CS_DIM, 0);
            }
            if (e->maxhp)
                Sprintf(buf, "%s / %d", !strcmp(e->hp, "-") ? "0" : e->hp,
                        e->maxhp);
            else
                buf[0] = '\0';
            r[0] = xhp, r[1] = row[1], r[2] = pr - S(20), r[3] = row[3];
            sdl_cs_line(sdl_face(SDL_FACE_UI, max(10, S(16))), buf, r, 2,
                        CS_DIM, 0);
        }
        /* more above or below */
        f = sdl_face(SDL_FACE_UI, max(9, S(14)));
        if (sdl.es.top > 0)
            (void) sdl_face_text(f, "more above  (scroll or Up)", w / 2,
                                 ptop - S(22), 1, CS_DIM, 0);
        if (sdl.es.top + sdl.es.fit < fxv_sl_nent)
            (void) sdl_face_text(f, "more below  (scroll or Down)", w / 2,
                                 pbot + S(2), 1, CS_DIM, 0);
    }

    y = max(pbot + S(30), min(h - S(34) - S(56), pbot + S(40)));
    sdl.es.btn[0] = (w - S(240)) / 2, sdl.es.btn[1] = y;
    sdl.es.btn[2] = (w + S(240)) / 2, sdl.es.btn[3] = y + S(46);
    sdl_button(sdl.es.btn, "Done", TRUE, sdl.es.hot, TRUE,
               sdl.es.down && sdl.es.hot, TRUE, s);
    sdl_cs_hints(w, h, s,
                 fxv_sl_nent > sdl.es.fit
                     ? "Up, Down or wheel: scroll      Enter or Esc: done"
                     : "Enter or Esc: done");
}

static void
sdl_draw_es(void)
{
    int w, h;
    double s;

    SDL_GetRenderOutputSize(sdl.ren, &w, &h);
    s = fxv_cs_scale(w, h);
    sdl_es_backdrop(w, h);
    if (sdl.es.mode == SDL_ES_DEATH)
        sdl_es_draw_death(w, h, s);
    else
        sdl_es_draw_scores(w, h, s);
}

#undef S

/* show an end screen until it's dismissed */
static void
sdl_es_run(int mode)
{
    int c;

    sdl.es.mode = mode;
    sdl.es.hot = sdl.es.down = FALSE;
    sdl.es.btn[0] = sdl.es.btn[1] = sdl.es.btn[2] = sdl.es.btn[3] = 0;
    sdl.bar_open = -1;
    for (;;) {
        sdl.dirty = TRUE;
        c = sdl_wait_key();
        if (c == '\n' || c == '\r' || c == ' ' || c == '\033')
            break;
        if (mode != SDL_ES_SCORES)
            continue;
        switch (c) {
        case SDL_CSK_UP: sdl.es.top--; break;
        case SDL_CSK_DOWN: sdl.es.top++; break;
        case SDL_CSK_PGUP: sdl.es.top -= sdl.es.fit; break;
        case SDL_CSK_PGDN: sdl.es.top += sdl.es.fit; break;
        case SDL_CSK_HOME: sdl.es.top = 0; break;
        case SDL_CSK_END: sdl.es.top = fxv_sl_nent; break;
        default: break;
        }
        sdl.es.top = max(0, sdl.es.top); /* (and kept short when drawn) */
    }
    sdl.es.mode = -1;
    sdl.dirty = TRUE;
}

/* the tombstone: its four lines go first in the end-of-game window,
   whose farewell follows; the death screen shows them both */
static void
sdl_outrip(winid w, int how, time_t when)
{
    struct sdl_win *sw = sdl_getwin(w);
    char buf[BUFSZ];

    if (!sw)
        return;
    sdl_free_items(sw);
    sw->rip = TRUE;
    Snprintf(buf, 17, "%s", svp.plname); /* what fits on the stone */
    sdl_putstr(w, 0, buf);
    Sprintf(buf, "%ld Au", gd.done_money);
    buf[16] = '\0';
    sdl_putstr(w, 0, buf);
    formatkiller(buf, sizeof buf, how, FALSE);
    sdl_putstr(w, 0, buf);
    Sprintf(buf, "%4ld", yyyymmdd(when) / 10000L);
    sdl_putstr(w, 0, buf);
}

/* show the death screen (from the tombstone's window) or the score list
   (from topten()'s) */
static void
sdl_end_screens(struct sdl_win *sw, boolean scores)
{
    int i;

    if (scores) {
        fxv_scores_clear();
        for (i = 0; i < sw->nitems; i++)
            fxv_scores_add(sw->items[i].attr, sw->items[i].text);
        sdl.es.top = fxv_scores_parse(); /* the hero's entry in view */
        sdl.es.fit = 6;
        sdl_es_run(SDL_ES_SCORES);
        fxv_scores_clear();
    } else {
        char rip[BUFSZ * 4 + 8], *summary;
        size_t n = 0;

        rip[0] = '\0';
        for (i = 0; i < sw->nitems && i < 4; i++)
            Snprintf(eos(rip), sizeof rip - strlen(rip), "%s\n",
                     sw->items[i].text);
        for (i = 4; i < sw->nitems; i++)
            n += strlen(sw->items[i].text) + 1;
        summary = (char *) alloc(n + 1);
        summary[0] = '\0';
        for (i = 4; i < sw->nitems; i++)
            Strcat(strcat(summary, sw->items[i].text), "\n");
        fxv_end_gather(rip, summary);
        free(summary);
        sdl_es_run(SDL_ES_DEATH);
    }
}

/* ---- back to the title screen after a game (as NetHackW) ----------- */

/* Death, quit and save all lead back to the title screen, by starting
   the program afresh (the core keeps its state in globals that aren't
   made to be reset); closing the window doesn't. */
/* (sdl_play_again: declared at the top) */
static char **sdl_argv; /* this program's command line, as it started */

static void
sdl_relaunch(void)
{
#ifdef __linux__
    if (sdl_play_again && sdl_argv && !sdl.closing
        && !getenv("NETHACK_SDL_KEYS")) /* (not when testing) */
        (void) execv("/proc/self/exe", sdl_argv);
#endif
}

/* keep the command line, to start the same way again */
static void
sdl_keep_cmdline(void)
{
#ifdef __linux__
    FILE *f = fopen("/proc/self/cmdline", "rb");
    static char buf[4096];
    size_t n, i;
    int argc = 0, k;

    if (!f)
        return;
    n = fread(buf, 1, sizeof buf - 1, f);
    (void) fclose(f);
    if (n == 0 || n >= sizeof buf - 1)
        return; /* nothing, or too long to trust */
    buf[n] = '\0';
    for (i = 0; i < n; i++)
        if (buf[i] == '\0')
            argc++;
    sdl_argv = (char **) alloc((argc + 1) * sizeof (char *));
    for (i = 0, k = 0; i < n && k < argc; k++) {
        sdl_argv[k] = &buf[i];
        i += strlen(&buf[i]) + 1;
    }
    sdl_argv[k] = 0;
    (void) atexit(sdl_relaunch);
#endif
}

/* ---- what the shared effects need from this port (fxview.h) ------- */

static fxv_ms
sdl_fxv_now(void)
{
    return (fxv_ms) SDL_GetTicks();
}

static void
sdl_fxv_start_timer(void)
{
    sdl.dirty = TRUE; /* sdl_wait_key() animates while anything moves */
}

static const glyph_info *
sdl_fxv_glyph_at(int x, int y)
{
    return &sdl.map[x][y];
}

static boolean
sdl_fxv_tiles_shown(void)
{
    return !Is_rogue_level(&u.uz);
}

/* the core's fx events (fx.h) */
/* the back-th newest message, and its turn (for the last words) */
static const char *
sdl_msg_back(int back, long *turn)
{
    int i = sdl.nmsgs - 1 - back;

    if (back < 0 || i < 0 || i < sdl.nmsgs - SDL_MSGKEEP)
        return (const char *) 0;
    *turn = sdl.msg_turn[i % SDL_MSGKEEP];
    return sdl.msgs[i % SDL_MSGKEEP];
}

static void
sdl_fx_event(const struct fx_event *ev)
{
    if (ev->type == FXE_UI && ev->text && !strcmp(ev->text, "death"))
        fxv_end_snapshot(sdl_msg_back); /* the death screen's last words */
    fxv_fx_event(ev);
    sdl.dirty = TRUE;
}

/* ---- the window procedures ----------------------------------------- */

static void
sdl_init_nhwindows(int *argc, char **argv)
{
    SDL_Surface *s;
    static const struct fxv_hooks hooks = {
        sdl_fxv_now, sdl_fxv_start_timer, sdl_fxv_glyph_at,
        sdl_fxv_tiles_shown, (fxv_color (*)(int)) 0
    };

    nhUse(argc), nhUse(argv);
    sdl_keep_cmdline(); /* to come back to the title screen after a game */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        raw_printf("SDL: %s", SDL_GetError());
        nh_terminate(EXIT_FAILURE);
    }
    if (!TTF_Init()) {
        raw_printf("SDL_ttf: %s", SDL_GetError());
        nh_terminate(EXIT_FAILURE);
    }
    {
        /* the window: most of the screen, centred */
        SDL_Rect usable;
        int ww = 1280, wh = 800;

        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)
            && usable.w > 0 && usable.h > 0) {
            /* no wider than 16:10, so an ultrawide screen doesn't get
               a window stretched across all of it */
            wh = max(600, usable.h * 85 / 100);
            ww = max(800, min(usable.w * 85 / 100, wh * 16 / 10));
        } else {
            usable.h = 1080;
        }
        if (iflags.fx_window_w && iflags.fx_window_h) /* fx_window_size */
            ww = iflags.fx_window_w, wh = iflags.fx_window_h;
        if (!SDL_CreateWindowAndRenderer("NetHack Modern", ww, wh,
                                         SDL_WINDOW_RESIZABLE
                                             | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                         &sdl.window, &sdl.ren)) {
            raw_printf("SDL: %s", SDL_GetError());
            nh_terminate(EXIT_FAILURE);
        }
        (void) SDL_SetWindowPosition(sdl.window, SDL_WINDOWPOS_CENTERED,
                                     SDL_WINDOWPOS_CENTERED);
        /* text sized to the screen (in its pixels: a high-density
           display gets proportionally more), about 1/64 of its height */
        {
            float scale = SDL_GetWindowDisplayScale(sdl.window);

            if (scale <= 0.0f)
                scale = 1.0f;
            sdl.auto_text_px = max(14.0f,
                                   min(64.0f, (float) usable.h / 64.0f
                                                  * scale));
        }
    }
    SDL_SetRenderVSync(sdl.ren, 1);
    /* fx_text_size, or the size that suits the screen */
    sdl.text_set = iflags.fx_text_size;
    sdl.text_px = sdl.text_set ? (float) sdl.text_set : sdl.auto_text_px;
    sdl.font = sdl_open_font(sdl.text_px);
    if (!sdl.font) {
        raw_print("SDL: no TrueType font found; set NETHACK_FONT to one");
        nh_terminate(EXIT_FAILURE);
    }
    sdl_text_size(sdl.text_px);
    /* messages on the map are bold, as NetHackW's (semibold) are */
    if ((sdl.msgfont = TTF_OpenFont(sdl.fontpath, sdl.text_px)) != 0)
        TTF_SetFontStyle(sdl.msgfont, TTF_STYLE_BOLD);
    s = SDL_LoadBMP("tiles.bmp");
    if (s) {
        SDL_Surface *argb;

        /* solid, for floors and backgrounds */
        if ((argb = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888)) != 0) {
            sdl.tiles_solid = SDL_CreateTextureFromSurface(sdl.ren, argb);
            /* kept: its pixels are measured for turning missiles */
            sdl.tiles_px = argb;
            if (sdl.tiles_solid)
                (void) SDL_SetTextureScaleMode(sdl.tiles_solid,
                                               SDL_SCALEMODE_NEAREST);
        }
        (void) SDL_SetSurfaceColorKey(s, true,
                                      SDL_MapSurfaceRGB(s, 71, 108, 108));
        /* full colour with the key turned into alpha: an 8-bit sheet
           would otherwise become a palette texture, which ignores the
           alpha that see-through creatures and fades are drawn with */
        argb = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);
        sdl.tiles_across = max(1, s->w / SDL_TILE);
        sdl.tiles = SDL_CreateTextureFromSurface(sdl.ren, argb ? argb : s);
        if (argb)
            SDL_DestroySurface(argb);
        SDL_DestroySurface(s);
        if (sdl.tiles) {
            (void) SDL_SetTextureBlendMode(sdl.tiles, SDL_BLENDMODE_BLEND);
            (void) SDL_SetTextureScaleMode(sdl.tiles, SDL_SCALEMODE_NEAREST);
        }
    } else {
        raw_printf("SDL: can't load tiles.bmp: %s", SDL_GetError());
    }
    sdl.zoom_set = iflags.fx_map_zoom; /* fx_map_zoom, or automatic */
    sdl.zoom = sdl_zoom_for(sdl.zoom_set), sdl.zoom_used = -1;
    sdl.bar_open = sdl.bar_hot = sdl.bar_item = -1; /* menu bar closed */
    sdl.cs.mode = sdl.es.mode = -1;
    /* the score list goes in a window, to be shown as a table (as
       NetHackW does) */
    iflags.toptenwin = 1;
    sdl.herox = COLNO / 2, sdl.heroy = ROWNO / 2, sdl.recentre = TRUE;
    {
        int x, y;

        for (x = 0; x < COLNO; x++)
            for (y = 0; y < ROWNO; y++)
                sdl.map[x][y] = sdl.bkmap[x][y] = nul_glyphinfo;
    }
    (void) memset(&fxv, 0, sizeof fxv);
    fxv_init(&hooks);
    fx_set_handler(sdl_fx_event);
    SDL_StartTextInput(sdl.window);
    {
        /* for testing: NETHACK_SDL_KEYS="..." is typed in at the start
           (\n written as ^J, Escape as ^[) */
        const char *k = getenv("NETHACK_SDL_KEYS");

        for (; k && *k; k++)
            if (k[0] == '^' && k[1])
                sdl_push_key(C(k[1] == '[' ? '[' : k[1])), k++;
            else
                sdl_push_key(*k);
    }
    sdl.dirty = TRUE;
    sdl_render();
    /* NetHack Modern: the title screen; it returns when Play is chosen
       (Exit ends the program) */
    sdl_title_screen();
    {
        /* on Unix the game takes the login name when it isn't given
           one; ask, as NetHackW does, with that name filled in (a name
           given with -u or the name option is used as it is) */
        const char *who = getenv("USER");

        if (!who || !*who)
            who = getenv("LOGNAME");
        if (!*svp.plname || (who && !strcmp(who, svp.plname)))
            while (!sdl_name_screen())
                sdl_title_screen();
    }
    iflags.window_inited = TRUE;
    sdl.dirty = TRUE;
    sdl_render();
}

static void
sdl_askname(void)
{
    /* NetHack Modern: asked over the title art; Back returns to the
       title screen, and from there to here */
    while (!sdl_name_screen())
        sdl_title_screen();
}

static void
sdl_get_nh_event(void)
{
    SDL_Event e;

    while (SDL_PollEvent(&e))
        sdl_handle_event(&e);
}

static void
sdl_exit_nhwindows(const char *str)
{
    int i;

    fx_set_handler((fx_handler_fn) 0);
    if (getenv("NETHACK_SDL_KEYS")) {
        /* testing: what was said, for the person running the test */
        for (i = max(0, sdl.nmsgs - SDL_MSGKEEP); i < sdl.nmsgs; i++)
            (void) fprintf(stderr, "msg: %s\n", sdl.msgs[i % SDL_MSGKEEP]);
    }
    for (i = 0; i < 3; i++)
        if (sdl.fxfont[i])
            TTF_CloseFont(sdl.fxfont[i]), sdl.fxfont[i] = 0;
    if (sdl.disc)
        SDL_DestroyTexture(sdl.disc), sdl.disc = 0;
    if (sdl.light)
        SDL_DestroyTexture(sdl.light), sdl.light = 0;
    if (sdl.font)
        TTF_CloseFont(sdl.font), sdl.font = 0;
    for (i = 0; i < SDL_NFACE; i++)
        if (sdl.face[i])
            TTF_CloseFont(sdl.face[i]), sdl.face[i] = 0;
    if (sdl.msgfont)
        TTF_CloseFont(sdl.msgfont), sdl.msgfont = 0;
    if (sdl.art)
        SDL_DestroyTexture(sdl.art), sdl.art = 0;
    if (sdl.art_grey)
        SDL_DestroyTexture(sdl.art_grey), sdl.art_grey = 0;
    if (sdl.art_px)
        SDL_DestroySurface(sdl.art_px), sdl.art_px = 0;
    if (sdl.stone)
        SDL_DestroyTexture(sdl.stone), sdl.stone = 0;
    if (sdl.tiles)
        SDL_DestroyTexture(sdl.tiles), sdl.tiles = 0;
    if (sdl.tiles_solid)
        SDL_DestroyTexture(sdl.tiles_solid), sdl.tiles_solid = 0;
    if (sdl.tiles_px)
        SDL_DestroySurface(sdl.tiles_px), sdl.tiles_px = 0;
    if (sdl.ren)
        SDL_DestroyRenderer(sdl.ren), sdl.ren = 0;
    if (sdl.window)
        SDL_DestroyWindow(sdl.window), sdl.window = 0;
    TTF_Quit();
    SDL_Quit();
    iflags.window_inited = FALSE;
    if (str && *str)
        raw_print(str);
}

static void
sdl_suspend_nhwindows(const char *str)
{
    nhUse(str);
}

static void
sdl_resume_nhwindows(void)
{
    sdl.dirty = TRUE;
}

static winid
sdl_create_nhwindow(int type)
{
    int i;

    for (i = 0; i < SDL_MAXWIN; i++)
        if (!sdl.win[i].type) {
            (void) memset(&sdl.win[i], 0, sizeof sdl.win[i]);
            sdl.win[i].type = type;
            return i;
        }
    panic("SDL: too many windows");
    return WIN_ERR;
}

static struct sdl_win *
sdl_getwin(winid w)
{
    return (w >= 0 && w < SDL_MAXWIN && sdl.win[w].type) ? &sdl.win[w] : 0;
}

static void
sdl_clear_nhwindow(winid w)
{
    struct sdl_win *sw = sdl_getwin(w);
    int x, y;

    if (!sw)
        return;
    switch (sw->type) {
    case NHW_MESSAGE:
        sdl.msg_seen = sdl.nmsgs; /* what's up there now is old news */
        break;
    case NHW_MAP:
        for (x = 0; x < COLNO; x++)
            for (y = 0; y < ROWNO; y++)
                sdl.map[x][y] = sdl.bkmap[x][y] = nul_glyphinfo;
        fxv.nfloaters = 0;
        sdl.recentre = TRUE; /* a new level: start with the hero mid-view */
        break;
    case NHW_STATUS:
        sw->status[0][0] = sw->status[1][0] = '\0';
        break;
    default:
        sdl_free_items(sw);
        break;
    }
    sdl.dirty = TRUE;
}

static void
sdl_display_nhwindow(winid w, boolean blocking)
{
    struct sdl_win *sw = sdl_getwin(w);

    if (!sw)
        return;
    if (sw->rip || (w == gt.toptenwin && sw->nitems)) {
        sdl_end_screens(sw, w == gt.toptenwin);
        return;
    }
    if (sw->type == NHW_MENU || sw->type == NHW_TEXT) {
        if (sw->nitems)
            (void) sdl_run_overlay(sw, PICK_NONE, (menu_item **) 0);
        return;
    }
    sdl_render();
    if (blocking && sw->type == NHW_MESSAGE && sdl.nmsgs > sdl.msg_seen)
        sdl_more();
}

static void
sdl_destroy_nhwindow(winid w)
{
    struct sdl_win *sw = sdl_getwin(w);

    if (!sw)
        return;
    sdl_free_items(sw);
    if (sw->items)
        free(sw->items);
    (void) memset(sw, 0, sizeof *sw);
    sdl.dirty = TRUE;
}

static void
sdl_curs(winid w, int x, int y)
{
    struct sdl_win *sw = sdl_getwin(w);

    if (!sw)
        return;
    if (sw->type == NHW_MAP)
        sdl.curx = x, sdl.cury = y;
    else
        sw->cury = y;
}

static void
sdl_putstr(winid w, int attr, const char *str)
{
    struct sdl_win *sw = sdl_getwin(w);
    struct sdl_item *it;

    if (!sw || !str)
        return;
    switch (sw->type) {
    case NHW_MESSAGE:
        sdl_add_msg(str);
        break;
    case NHW_STATUS:
        Snprintf(sw->status[sw->cury & 1], sizeof sw->status[0], "%s", str);
        sdl.dirty = TRUE;
        break;
    case NHW_MAP:
        sdl_add_msg(str);
        break;
    default:
        if (sw->nitems < SDL_TEXTMAX) {
            it = sdl_new_item(sw);
            it->attr = attr, it->color = NO_COLOR;
            it->text = dupstr(str);
        }
        break;
    }
}

static void
sdl_start_menu(winid w, unsigned long mbehavior)
{
    struct sdl_win *sw = sdl_getwin(w);

    if (sw) {
        sdl_free_items(sw);
        sw->perminv = (mbehavior & MENU_BEHAVE_PERMINV) != 0;
    }
}

static void
sdl_add_menu(winid w, const glyph_info *glyphinfo, const anything *id,
             char ch, char gch, int attr, int clr, const char *str,
             unsigned int itemflags)
{
    struct sdl_win *sw = sdl_getwin(w);
    struct sdl_item *it;

    if (!sw || sw->nitems >= SDL_TEXTMAX)
        return;
    it = sdl_new_item(sw);
    if (glyphinfo && glyphinfo->glyph != NO_GLYPH)
        it->tile = glyphinfo->gm.tileidx;
    it->id = *id;
    it->acc = ch, it->gacc = gch;
    it->attr = attr, it->color = clr;
    it->selected = (itemflags & MENU_ITEMFLAGS_SELECTED) != 0;
    it->text = dupstr(str ? str : "");
}

static void
sdl_end_menu(winid w, const char *prompt)
{
    struct sdl_win *sw = sdl_getwin(w);

    if (sw && prompt && *prompt)
        sw->prompt = dupstr(prompt);
}

static int
sdl_select_menu(winid w, int how, menu_item **menu_list)
{
    struct sdl_win *sw = sdl_getwin(w);

    *menu_list = 0;
    if (!sw)
        return -1;
    /* the persistent inventory stays in its panel, as NetHackW's does;
       'i' with the panel hidden shows it as a menu instead */
    if (sw->perminv && how == PICK_NONE
        && (sdl.inv.updating || sdl_inv_w() > 0)) {
        sdl.dirty = TRUE;
        return 0;
    }
    return sdl_run_overlay(sw, how, menu_list);
}

static void
sdl_mark_synch(void)
{
    sdl_render();
}

static void
sdl_wait_synch(void)
{
    sdl_render();
}

#ifdef CLIPPING
static void
sdl_cliparound(int x, int y)
{
    sdl.herox = x, sdl.heroy = y;
    sdl.dirty = TRUE;
}
#endif

static void
sdl_print_glyph(winid w, coordxy x, coordxy y, const glyph_info *glyphinfo,
                const glyph_info *bkglyphinfo)
{
    nhUse(w);
    if (!isok(x, y))
        return;
    sdl.map[x][y] = *glyphinfo;
    sdl.bkmap[x][y] = *bkglyphinfo;
    fxv_glyph_shown(x, y, glyphinfo); /* rays and digging leave sparks */
    sdl.dirty = TRUE;
}

static void
sdl_raw_print(const char *str)
{
    if (str && *str) {
        (void) fprintf(stderr, "%s\n", str);
        if (iflags.window_inited)
            sdl_add_msg(str);
    }
}

static void
sdl_player_selection(void)
{
    /* NetHack Modern: the character screen, unless the options already
       said who the hero is; Back returns to the title screen, and from
       there to here */
    fxv_cs_begin();
    if (!fxv_cs_randomize())
        while (!sdl_cs_run(SDL_CS_CHAR))
            sdl_title_screen();
    sdl_music_end(); /* the hero is chosen: the game begins */
}

static int
sdl_nhgetch(void)
{
    if (program_state.in_moveloop)
        sdl_music_end(); /* (a restored game) */
    int c = sdl_wait_key();

    if (sdl.nmsgs > sdl.msg_seen)
        sdl.msg_seen = sdl.nmsgs; /* the player has seen them */
    return c;
}

static int
sdl_nh_poskey(coordxy *x, coordxy *y, int *mod)
{
    nhUse(x), nhUse(y), nhUse(mod);
    return sdl_nhgetch();
}

static void
sdl_nhbell(void)
{
}

/* the message history, in a text window */
static int
sdl_doprev_message(void)
{
    winid w = create_nhwindow(NHW_TEXT);
    int i;

    for (i = max(0, sdl.nmsgs - SDL_MSGKEEP); i < sdl.nmsgs; i++)
        putstr(w, 0, sdl.msgs[i % SDL_MSGKEEP]);
    display_nhwindow(w, TRUE);
    destroy_nhwindow(w);
    return 0;
}

static char
sdl_yn_function(const char *query, const char *resp, char def)
{
    char prompt[BUFSZ + 40], choices[BUFSZ];
    int c;

    if (resp && *resp) {
        /* show the choices up to an ESC, like tty: "[ynq] (n)" */
        const char *e = strchr(resp, '\033');
        size_t n = e ? (size_t) (e - resp) : strlen(resp);

        Snprintf(choices, sizeof choices, "%.*s", (int) n, resp);
        if (def)
            Snprintf(prompt, sizeof prompt, "%s [%s] (%c)", query, choices,
                     def);
        else
            Snprintf(prompt, sizeof prompt, "%s [%s]", query, choices);
    } else {
        Snprintf(prompt, sizeof prompt, "%s", query);
    }
    sdl.prompt = prompt;
    sdl.dirty = TRUE;
    for (;;) {
        c = sdl_wait_key();
        if (!resp || !*resp)
            break; /* any key */
        if (c == '\033') {
            c = strchr(resp, 'q') ? 'q' : strchr(resp, 'n') ? 'n' : def;
            break;
        }
        if (c == '\n' || c == '\r' || c == ' ') {
            if (def) {
                c = def;
                break;
            }
            continue;
        }
        if (c <= 0 || c >= 0x80)
            continue; /* (strchr() would match the terminating 0) */
        if (strchr(resp, c))
            break;
        if (strchr(resp, lowc((char) c))) {
            c = lowc((char) c);
            break;
        }
    }
    sdl.prompt = 0;
    {
        char line[BUFSZ + 48];

        Snprintf(line, sizeof line, "%s %c", prompt,
                 (c > ' ' && c < 127) ? (char) c : ' ');
        sdl_add_msg(line);
        sdl.msg_seen = sdl.nmsgs;
    }
    return (char) c;
}

static void
sdl_getlin(const char *query, char *bufp)
{
    char prompt[2 * BUFSZ + 8];
    int len = 0, c;

    bufp[0] = '\0';
    for (;;) {
        Snprintf(prompt, sizeof prompt, "%s %s_", query, bufp);
        sdl.prompt = prompt;
        sdl.dirty = TRUE;
        c = sdl_wait_key();
        if (c == '\033') {
            Strcpy(bufp, "\033");
            break;
        }
        if (c == '\n' || c == '\r')
            break;
        if (c == '\b' || c == 127) {
            if (len > 0)
                bufp[--len] = '\0';
            continue;
        }
        if (c >= ' ' && c < 127 && len < BUFSZ - 1) {
            bufp[len++] = (char) c;
            bufp[len] = '\0';
        }
    }
    sdl.prompt = 0;
    sdl.dirty = TRUE;
}

/* "#" commands: type the name, or enough of it to tell it apart */
static int
sdl_get_ext_cmd(void)
{
    char buf[BUFSZ];
    int i, found = -1, nfound = 0;
    size_t len;

    getlin("#", buf);
    (void) mungspaces(buf);
    if (buf[0] == '\033' || !buf[0])
        return -1;
    len = strlen(buf);
    for (i = 0; extcmdlist[i].ef_txt; i++) {
        unsigned fl = extcmdlist[i].flags;

        if ((fl & (CMD_NOT_AVAILABLE | INTERNALCMD))
            || ((fl & WIZMODECMD) && !wizard))
            continue;
        if (!strcmpi(buf, extcmdlist[i].ef_txt))
            return i;
        if (!strncmpi(buf, extcmdlist[i].ef_txt, len))
            found = i, nfound++;
    }
    if (nfound == 1)
        return found;
    pline("%s: unknown extended command.", buf);
    return -1;
}

static void
sdl_number_pad(int state)
{
    nhUse(state);
}

/* the core pausing (missiles, explosions): keep drawing meanwhile */
static void
sdl_delay_output(void)
{
    Uint64 until = SDL_GetTicks() + 50;
    SDL_Event e;

    sdl_render();
    while (SDL_GetTicks() < until) {
        while (SDL_PollEvent(&e))
            sdl_handle_event(&e);
        if (sdl_effects_moving())
            sdl_render();
        SDL_Delay(8);
    }
}

#ifdef CHANGE_COLOR
static void
sdl_change_color(int color, long rgb, int reverse)
{
    nhUse(color), nhUse(rgb), nhUse(reverse);
}

static char *
sdl_get_color_string(void)
{
    return (char *) "";
}
#endif

static void
sdl_update_inventory(int arg)
{
    if (!iflags.perm_invent || !program_state.something_worth_saving
        || !iflags.window_inited || WIN_INVEN == WIN_ERR)
        return;
    if (!arg) {
        /* the inventory changed: fill the panel again */
        sdl.inv.updating = TRUE;
        repopulate_perminvent();
        sdl.inv.updating = FALSE;
    } else if (sdl_inv_w() > 0) {
        /* #perminv: scroll the panel with the keys until done */
        int c, page;

        do {
            page = max(1, sdl_inv_lines() - 1);
            sdl.dirty = TRUE;
            c = sdl_wait_key();
            if (c == '>' || c == ' ' || c == MENU_NEXT_PAGE)
                sdl.inv.first += page;
            else if (c == '<' || c == MENU_PREVIOUS_PAGE)
                sdl.inv.first -= page;
            else if (c == '^' || c == MENU_FIRST_PAGE)
                sdl.inv.first = 0;
            else if (c == '|' || c == MENU_LAST_PAGE)
                sdl.inv.first = 10000; /* kept in range when drawn */
            sdl.inv.first = max(0, sdl.inv.first);
        } while (c != '\033' && c != '\n' && c != '\r');
    }
    sdl.dirty = TRUE;
}

static win_request_info *
sdl_ctrl_nhwindow(winid w, int request, win_request_info *wri)
{
    nhUse(w), nhUse(request);
    return wri;
}

struct window_procs sdl_procs = {
    WPID(sdl),
    (WC_COLOR | WC_HILITE_PET | WC_TILED_MAP | WC_PERM_INVENT),
    (WC2_DARKGRAY | WC2_FLUSH_STATUS),
    { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    sdl_init_nhwindows,
    sdl_player_selection,
    sdl_askname,
    sdl_get_nh_event,
    sdl_exit_nhwindows,
    sdl_suspend_nhwindows,
    sdl_resume_nhwindows,
    sdl_create_nhwindow,
    sdl_clear_nhwindow,
    sdl_display_nhwindow,
    sdl_destroy_nhwindow,
    sdl_curs,
    sdl_putstr,
    genl_putmixed,
    genl_display_file,
    sdl_start_menu,
    sdl_add_menu,
    sdl_end_menu,
    sdl_select_menu,
    genl_message_menu,
    sdl_mark_synch,
    sdl_wait_synch,
#ifdef CLIPPING
    sdl_cliparound,
#endif
#ifdef POSITIONBAR
    (void (*)(char *)) 0,
#endif
    sdl_print_glyph,
    sdl_raw_print,
    sdl_raw_print,
    sdl_nhgetch,
    sdl_nh_poskey,
    sdl_nhbell,
    sdl_doprev_message,
    sdl_yn_function,
    sdl_getlin,
    sdl_get_ext_cmd,
    sdl_number_pad,
    sdl_delay_output,
#ifdef CHANGE_COLOR
    sdl_change_color,
#ifdef MAC68K
    (void (*)(int)) 0,
    (short (*)(winid, char *)) 0,
#endif
    sdl_get_color_string,
#endif
    sdl_outrip,
    genl_preference_update,
    genl_getmsghistory,
    genl_putmsghistory,
    genl_status_init,
    genl_status_finish,
    genl_status_enablefield,
    sdl_status_update,
    genl_can_suspend_no,
    sdl_update_inventory,
    sdl_ctrl_nhwindow,
};

#endif /* SDL_GRAPHICS */
