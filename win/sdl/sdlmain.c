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

#include <math.h>

#ifdef SDL_GRAPHICS

/* ---- the port's state --------------------------------------------- */

#define SDL_TILE 16            /* tile sheet: pixels per tile */
#define SDL_MAXWIN 24
#define SDL_MSGKEEP 200        /* message history */
#define SDL_MSGSHOW 3          /* message lines on screen */
#define SDL_TEXTMAX 1000       /* lines in a text or menu window */
#define SDL_KEYQ 64

struct sdl_item {              /* a line of a menu or text window */
    anything id;               /* a_void == 0: not selectable */
    char acc;                  /* accelerator */
    char gacc;                 /* group accelerator */
    int attr, color;
    boolean selected;
    char *text;
};

struct sdl_win {
    int type;                  /* NHW_*, 0 = free slot */
    struct sdl_item *items;
    int nitems, maxitems;
    char *prompt;              /* end_menu() prompt */
    char status[2][BUFSZ];     /* NHW_STATUS: its two lines */
    int cury;
};

static struct {
    SDL_Window *window;
    SDL_Renderer *ren;
    TTF_Font *font;
    SDL_Texture *tiles;        /* the tile sheet, key colour cut out */
    SDL_Texture *tiles_solid;  /* the same, solid: floors, backgrounds */
    int tiles_across;
    int cw, lh;                /* character width, line height */
    int zoom;                  /* map tile = SDL_TILE * zoom pixels */
    int camx, camy;            /* map square at the centre of the view */
    int curx, cury;            /* map cursor (getpos) */
    glyph_info map[COLNO][ROWNO], bkmap[COLNO][ROWNO];
    struct sdl_win win[SDL_MAXWIN];
    char *msgs[SDL_MSGKEEP];
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
    } ovl;
} sdl;

static void sdl_render(void);
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

static void
sdl_handle_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_QUIT:
        /* closing the window: save, as a hangup would */
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
        break;
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
        if ((m & SDL_KMOD_CTRL) && k >= 'a' && k <= 'z') {
            sdl_push_key(C((int) k));
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

static void
sdl_layout(int *top, int *bottom, int *ts, int *ox, int *oy)
{
    int w, h;

    SDL_GetRenderOutputSize(sdl.ren, &w, &h);
    *top = SDL_MSGSHOW * sdl.lh + 8;
    *bottom = h - (2 * sdl.lh + 8);
    *ts = SDL_TILE * sdl.zoom;
    /* the camera square sits in the middle of the map area */
    *ox = w / 2 - sdl.camx * *ts - *ts / 2;
    *oy = (*top + *bottom) / 2 - sdl.camy * *ts - *ts / 2;
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
            if (bg->glyph != NO_GLYPH && bg->glyph != GLYPH_UNEXPLORED) {
                sdl_tile_solid(bg->gm.tileidx, px, py, (float) ts);
                if (fg->glyph != NO_GLYPH)
                    sdl_tile(fg->gm.tileidx, px, py, (float) ts);
            } else if (fg->glyph != NO_GLYPH) {
                sdl_tile_solid(fg->gm.tileidx, px, py, (float) ts);
            }
        }
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

static void
sdl_draw_messages(void)
{
    int i, first = max(0, sdl.nmsgs - SDL_MSGSHOW), row = 0;

    sdl_fill(0, 0, 4000, SDL_MSGSHOW * sdl.lh + 6, 8, 8, 12, 230);
    if (sdl.prompt) /* a question: it takes the bottom line */
        first = max(0, sdl.nmsgs - (SDL_MSGSHOW - 1));
    for (i = first; i < sdl.nmsgs; i++, row++)
        sdl_text(8, 3 + row * sdl.lh, sdl.msgs[i % SDL_MSGKEEP],
                 i >= sdl.msg_seen ? sdl_white : sdl_dim);
    if (sdl.prompt)
        sdl_text(8, 3 + row * sdl.lh, sdl.prompt, sdl_gold);
}

static void
sdl_draw_status(void)
{
    int w, h, i;
    struct sdl_win *st = (WIN_STATUS >= 0 && WIN_STATUS < SDL_MAXWIN)
                             ? &sdl.win[WIN_STATUS] : 0;

    SDL_GetRenderOutputSize(sdl.ren, &w, &h);
    sdl_fill(0, h - (2 * sdl.lh + 8), w, 2 * sdl.lh + 8, 8, 8, 12, 230);
    if (!st || st->type != NHW_STATUS)
        return;
    for (i = 0; i < 2; i++)
        sdl_text(8, h - (2 * sdl.lh + 4) + i * sdl.lh, st->status[i],
                 sdl_white);
}

/* the whole screen, without any menu over it */
static void
sdl_draw_screen(void)
{
    SDL_SetRenderDrawColor(sdl.ren, 0, 0, 0, 255);
    SDL_RenderClear(sdl.ren);
    sdl_draw_map();
    sdl_draw_status();
    sdl_draw_messages();
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
    sdl_draw_screen();
    sdl_draw_overlay();
    sdl_present();
    sdl.dirty = FALSE;
}

/* ---- messages ------------------------------------------------------ */

static void
sdl_add_msg(const char *s)
{
    int slot = sdl.nmsgs % SDL_MSGKEEP;

    if (sdl.nmsgs >= SDL_MSGKEEP)
        free(sdl.msgs[slot]);
    sdl.msgs[slot] = dupstr(s);
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
    sdl_fill(x, y, bw, bh, 14, 16, 24, 240);
    sdl_fill(x, y, bw, 2, 90, 110, 150, 255);
    y += 8;
    if (w->prompt) {
        sdl_text(x + 12, y, w->prompt, sdl_gold);
        y += sdl.lh;
    }
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
    sdl.ovl.w = w, sdl.ovl.how = how;
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
static void
sdl_fx_event(const struct fx_event *ev)
{
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
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        raw_printf("SDL: %s", SDL_GetError());
        nh_terminate(EXIT_FAILURE);
    }
    if (!TTF_Init()) {
        raw_printf("SDL_ttf: %s", SDL_GetError());
        nh_terminate(EXIT_FAILURE);
    }
    if (!SDL_CreateWindowAndRenderer("NetHack Modern", 1280, 800,
                                     SDL_WINDOW_RESIZABLE
                                         | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &sdl.window, &sdl.ren)) {
        raw_printf("SDL: %s", SDL_GetError());
        nh_terminate(EXIT_FAILURE);
    }
    SDL_SetRenderVSync(sdl.ren, 1);
    sdl.font = sdl_open_font(15.0f);
    if (!sdl.font) {
        raw_print("SDL: no TrueType font found; set NETHACK_FONT to one");
        nh_terminate(EXIT_FAILURE);
    }
    {
        int w = 8, h = 16;

        (void) TTF_GetStringSize(sdl.font, "M", 1, &w, &h);
        sdl.cw = w;
        sdl.lh = TTF_GetFontLineSkip(sdl.font);
    }
    s = SDL_LoadBMP("tiles.bmp");
    if (s) {
        SDL_Surface *argb;

        /* solid, for floors and backgrounds */
        if ((argb = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888)) != 0) {
            sdl.tiles_solid = SDL_CreateTextureFromSurface(sdl.ren, argb);
            SDL_DestroySurface(argb);
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
    sdl.zoom = 2;
    sdl.camx = COLNO / 2, sdl.camy = ROWNO / 2;
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
    iflags.window_inited = TRUE;
    sdl.dirty = TRUE;
    sdl_render();
}

static void
sdl_askname(void)
{
    char buf[BUFSZ];

    do {
        getlin("Who are you?", buf);
        if (buf[0] == '\033')
            Strcpy(buf, "player");
        (void) mungspaces(buf);
    } while (!buf[0]);
    (void) strncpy(svp.plname, buf, sizeof svp.plname - 1);
    svp.plname[sizeof svp.plname - 1] = '\0';
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
    if (sdl.tiles)
        SDL_DestroyTexture(sdl.tiles), sdl.tiles = 0;
    if (sdl.tiles_solid)
        SDL_DestroyTexture(sdl.tiles_solid), sdl.tiles_solid = 0;
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
    if (sw->type == NHW_MENU || sw->type == NHW_TEXT) {
        if (sw->nitems)
            (void) sdl_run_overlay(sw, PICK_NONE, (menu_item **) 0);
        return;
    }
    sdl_render();
    if (blocking && sw->type == NHW_MESSAGE && sdl.nmsgs > sdl.msg_seen) {
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

    nhUse(mbehavior);
    if (sw)
        sdl_free_items(sw);
}

static void
sdl_add_menu(winid w, const glyph_info *glyphinfo, const anything *id,
             char ch, char gch, int attr, int clr, const char *str,
             unsigned int itemflags)
{
    struct sdl_win *sw = sdl_getwin(w);
    struct sdl_item *it;

    nhUse(glyphinfo);
    if (!sw || sw->nitems >= SDL_TEXTMAX)
        return;
    it = sdl_new_item(sw);
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
    sdl.camx = x, sdl.camy = y;
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

static int
sdl_nhgetch(void)
{
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
    nhUse(arg);
}

static win_request_info *
sdl_ctrl_nhwindow(winid w, int request, win_request_info *wri)
{
    nhUse(w), nhUse(request);
    return wri;
}

struct window_procs sdl_procs = {
    WPID(sdl),
    (WC_COLOR | WC_HILITE_PET | WC_TILED_MAP),
    (WC2_DARKGRAY | WC2_FLUSH_STATUS),
    { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    sdl_init_nhwindows,
    genl_player_selection,
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
    genl_outrip,
    genl_preference_update,
    genl_getmsghistory,
    genl_putmsghistory,
    genl_status_init,
    genl_status_finish,
    genl_status_enablefield,
    genl_status_update,
    genl_can_suspend_no,
    sdl_update_inventory,
    sdl_ctrl_nhwindow,
};

#endif /* SDL_GRAPHICS */
