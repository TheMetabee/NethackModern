/* NetHack Modern fxview.c */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * The effects shared by the tiles front ends; see fxview.h. Particles
 * so far: what spawns them for each fx event, and how they move.
 * Drawing them is left to the front end.
 */

#include "hack.h"
#include "fxview.h"

#include <math.h>

struct fxv_state fxv;
fxv_ms fxv_chain_until;
static struct fxv_hooks hooks;

struct fxv_pstyle;
static struct fxv_part *fxv_spawn_at(const struct fxv_pstyle *, boolean,
                                     float, float, float, float, fxv_ms);

void
fxv_init(const struct fxv_hooks *h)
{
    hooks = *h;
}

fxv_ms
fxv_now(void)
{
    return hooks.now ? (*hooks.now)() : 0;
}

void
fxv_start_timer(void)
{
    if (hooks.start_timer)
        (*hooks.start_timer)();
}

const glyph_info *
fxv_glyph_at(int x, int y)
{
    if (!hooks.glyph_at || !isok(x, y))
        return &nul_glyphinfo;
    return (*hooks.glyph_at)(x, y);
}

boolean
fxv_tiles_shown(void)
{
    return hooks.tiles_shown ? (*hooks.tiles_shown)() : TRUE;
}

/* a NetHack colour as RGB, in the front end's palette if it has one */
fxv_color
fxv_nh_color(int c)
{
    static const fxv_color dflt[16] = {
        FXV_RGB(0x55, 0x55, 0x55), FXV_RGB(0xb0, 0x00, 0x00),
        FXV_RGB(0x00, 0x9b, 0x00), FXV_RGB(0x8b, 0x5a, 0x2b),
        FXV_RGB(0x00, 0x00, 0xc8), FXV_RGB(0xb0, 0x00, 0xb0),
        FXV_RGB(0x00, 0xa0, 0xa0), FXV_RGB(0xc0, 0xc0, 0xc0),
        FXV_RGB(0x80, 0x80, 0x80), FXV_RGB(0xff, 0x80, 0x00),
        FXV_RGB(0x00, 0xff, 0x00), FXV_RGB(0xff, 0xff, 0x00),
        FXV_RGB(0x40, 0x40, 0xff), FXV_RGB(0xff, 0x40, 0xff),
        FXV_RGB(0x00, 0xff, 0xff), FXV_RGB(0xff, 0xff, 0xff),
    };

    if (hooks.nh_color)
        return (*hooks.nh_color)(c);
    return (c >= 0 && c < 16) ? dflt[c] : FXV_RGB(0, 0, 0);
}

/* drop the particles, floaters and motion whose time is over */
boolean
fxv_expire(fxv_ms now)
{
    int i, j;

    for (i = j = 0; i < fxv.nparts; i++)
        if ((long) (now - fxv.parts[i].start) < (long) fxv.parts[i].life)
            fxv.parts[j++] = fxv.parts[i];
    fxv.nparts = j;
    for (i = j = 0; i < fxv.nfloaters; i++)
        if (now - fxv.floaters[i].start < fxv.floaters[i].life)
            fxv.floaters[j++] = fxv.floaters[i];
    fxv.nfloaters = j;
    for (i = j = 0; i < fxv.nanims; i++)
        if ((long) (now - fxv.anims[i].start) < (long) fxv.anims[i].dur)
            fxv.anims[j++] = fxv.anims[i];
    fxv.nanims = j;
    if (fxv.shake_dur
        && (long) (now - fxv.shake_start) >= (long) fxv.shake_dur)
        fxv.shake_dur = 0;
    return (fxv.nparts || fxv.nfloaters || fxv.nanims || fxv.shake_dur);
}

/* where a particle is now: it moves at v slowed by drag (distance =
   v*(drag^t - 1)/ln(drag), integrating v*drag^t) and falls or rises
   with grav */
void
fxv_part_at(const struct fxv_part *p, fxv_ms now, float *x, float *y,
            float *k)
{
    float s = (float) (long) (now - p->start) / 1000.0f;
    float dist;

    *k = (float) (long) (now - p->start) / (float) p->life;
    if (s < 0)
        s = 0;
    dist = (p->drag > 0.0f && p->drag < 1.0f)
               ? (float) (((float) pow(p->drag, s) - 1.0f) / log(p->drag))
               : s;
    *x = p->x + p->vx * dist;
    *y = p->y + p->vy * dist + 0.5f * p->grav * s * s;
}

/* ---- particles ---------------------------------------------------- */

/* private generator: the game's RNG is never touched */
static float
fxv_prand(void)
{
    static unsigned long seed = 0;

    if (!seed)
        seed = (unsigned long) fxv_now() | 1UL;
    seed = seed * 1103515245UL + 12345UL;
    return (float) ((seed >> 8) & 0xFFFF) / 65535.0f;
}

static float
fxv_prange(float lo, float hi)
{
    return lo + (hi - lo) * fxv_prand();
}

/* the look of one element's particles */
struct fxv_pstyle {
    fxv_color c0, c1, c2;   /* two birth colours to pick between, death */
    float speed;           /* squares/second */
    float grav;            /* < 0 rises (embers), > 0 falls (frost, acid) */
    float drag;
    float size;
    int life_lo, life_hi;  /* ms */
};

static const struct fxv_pstyle *
fxv_part_style(int kind)
{
    static const struct fxv_pstyle
        fire = { FXV_RGB(255, 200, 70), FXV_RGB(255, 120, 20), FXV_RGB(90, 10, 0),
                 1.3f, -1.6f, 0.25f, 0.075f, 380, 800 },
        cold = { FXV_RGB(235, 250, 255), FXV_RGB(130, 200, 255), FXV_RGB(0, 30, 80),
                 1.0f, 0.7f, 0.3f, 0.06f, 420, 850 },
        elec = { FXV_RGB(255, 255, 210), FXV_RGB(255, 235, 60), FXV_RGB(60, 50, 0),
                 3.2f, 0.0f, 0.05f, 0.045f, 160, 340 },
        sleep = { FXV_RGB(210, 190, 255), FXV_RGB(150, 120, 255), FXV_RGB(30, 10, 70),
                  0.6f, -0.3f, 0.4f, 0.065f, 500, 900 },
        death = { FXV_RGB(150, 60, 170), FXV_RGB(90, 90, 90), FXV_RGB(10, 0, 15),
                  0.9f, 0.0f, 0.3f, 0.07f, 400, 800 },
        poison = { FXV_RGB(160, 230, 70), FXV_RGB(90, 170, 40), FXV_RGB(10, 35, 0),
                   0.5f, -0.25f, 0.45f, 0.11f, 600, 1100 },
        acid = { FXV_RGB(220, 255, 70), FXV_RGB(170, 230, 30), FXV_RGB(30, 45, 0),
                 0.9f, 1.8f, 0.4f, 0.06f, 400, 800 },
        magic = { FXV_RGB(255, 170, 255), FXV_RGB(190, 110, 255), FXV_RGB(40, 0, 60),
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

static struct fxv_part *
fxv_new_part(void)
{
    if (fxv.nparts >= FXV_MAXPART) {
        /* full: recycle the oldest */
        memmove(&fxv.parts[0], &fxv.parts[1],
                (FXV_MAXPART - 1) * sizeof fxv.parts[0]);
        fxv.nparts--;
    }
    return &fxv.parts[fxv.nparts++];
}

static void
fxv_spawn(const struct fxv_pstyle *st, float x, float y,
         float speed, float sizemul, float lifemul)
{
    struct fxv_part *p = fxv_new_part();
    float ang = fxv_prange(0.0f, 6.2831853f), v = speed * fxv_prange(0.25f, 1.0f);

    p->x = x, p->y = y;
    p->vx = v * (float) cos(ang);
    p->vy = v * (float) sin(ang);
    p->grav = st->grav;
    p->drag = st->drag;
    p->size = st->size * sizemul * fxv_prange(0.7f, 1.3f);
    p->c0 = (fxv_prand() < 0.5f) ? st->c0 : st->c1;
    p->c1 = st->c2;
    p->start = fxv_now();
    p->life = (fxv_ms) (lifemul * fxv_prange((float) st->life_lo,
                                           (float) st->life_hi));
}

/* zap glyph type -> damage kind (ZT_* order in zap.c) */
static int
fxv_zap_kind(int zt)
{
    static const int kinds[NUM_ZAP] = { AD_MAGM, AD_FIRE, AD_COLD, AD_SLEE,
                                        AD_DISN, AD_ELEC, AD_DRST, AD_ACID };

    return (zt >= 0 && zt < NUM_ZAP) ? kinds[zt] : AD_MAGM;
}

/* a ray (wand, spell or breath) is being drawn through <x,y>; 'dir' is
   the beam's shape: 0 |, 1 -, 2 \, 3 / */
void
fxv_ray_particles(int x, int y, int zt, int dir)
{
    /* unit vector across the beam, for each shape */
    static const float across[4][2] = { { 1.0f, 0.0f }, { 0.0f, 1.0f },
                                        { 0.7071f, -0.7071f },
                                        { 0.7071f, 0.7071f } };
    const struct fxv_pstyle *st;
    int i, n;

    if (!iflags.fx_particles || !isok(x, y))
        return;
    st = fxv_part_style(fxv_zap_kind(zt));
    n = (st == fxv_part_style(AD_ELEC)) ? 8 : 6;
    for (i = 0; i < n; i++) {
        float along = fxv_prange(-0.5f, 0.5f);
        float side = (fxv_prand() < 0.5f) ? -1.0f : 1.0f;
        float sp = fxv_prange(1.1f, 2.4f) * (0.6f + 0.4f * st->speed);
        struct fxv_part *p;

        /* born on the beam's line, anywhere along this square */
        fxv_spawn(st,
                 x + 0.5f + along * across[dir & 3][1],
                 y + 0.5f - along * across[dir & 3][0], 0.0f, 1.0f, 1.2f);
        p = &fxv.parts[fxv.nparts - 1];
        /* thrown out to one side, with a little scatter */
        p->vx = side * across[dir & 3][0] * sp + fxv_prange(-0.3f, 0.3f);
        p->vy = side * across[dir & 3][1] * sp + fxv_prange(-0.3f, 0.3f);
    }
    if (st == fxv_part_style(AD_ELEC) && fxv_now() < fxv_chain_until) {
        /* chain lightning: the bolt keeps crackling while it's drawn */
        fxv_ms at;

        fxv_chain_sparks(x + 0.5f, y + 0.5f, 12, 0);
        for (at = 90; at <= 630; at += 90)
            fxv_chain_sparks(x + 0.5f + fxv_prange(-0.4f, 0.4f),
                            y + 0.5f + fxv_prange(-0.4f, 0.4f), 3, at);
    }
    fxv_start_timer();
}

/* chain lightning sparks: 'n' white-hot sparks flying off <fx,fy> in
   every direction, 'delay' ms from now */
void
fxv_chain_sparks(float fx, float fy, int n, fxv_ms delay)
{
    const struct fxv_pstyle *st = fxv_part_style(AD_ELEC);
    int i;

    for (i = 0; i < n; i++) {
        float a = fxv_prange(0.0f, 6.2831853f), sp = fxv_prange(2.5f, 5.5f);
        struct fxv_part *p = fxv_spawn_at(st, FALSE, fx, fy, 0.0f, 0.7f,
                                        delay);

        p->c0 = (i % 3 == 0) ? FXV_RGB(255, 255, 255)
                : (i % 3 == 1) ? FXV_RGB(150, 215, 255) : p->c0;
        p->size = fxv_prange(0.07f, 0.12f);
        p->vx = sp * (float) cos(a);
        p->vy = sp * (float) sin(a);
        p->drag = 0.02f;
        p->grav = 1.5f;
        p->life = (fxv_ms) fxv_prange(140, 300);
    }
}

/* an explosion centred on <x,y>: a burst over its 3x3 area, more for
   bigger blasts */
void
fxv_burst_particles(int x, int y, int kind, int dam)
{
    const struct fxv_pstyle *st;
    int i, n;

    if (!iflags.fx_particles || !isok(x, y))
        return;
    st = fxv_part_style(kind);
    n = 70 + min(dam, 60) * 2;
    for (i = 0; i < n; i++) {
        /* most from the middle flying outward, some from the edges */
        float r = fxv_prange(0.0f, 0.6f), a = fxv_prange(0.0f, 6.2831853f);

        fxv_spawn(st, x + 0.5f + r * (float) cos(a),
                 y + 0.5f + r * (float) sin(a), st->speed * 1.9f, 1.15f,
                 1.1f);
    }
    fxv_start_timer();
}

/* a force strike (wand of striking, force bolt): a quick puff of pale
   specks and a ring snapping outward; smaller and greyer when it bounced
   off ("Boing!") */
void
fxv_impact_particles(int x, int y, boolean resisted)
{
    static const struct fxv_pstyle
        force = { FXV_RGB(245, 250, 255), FXV_RGB(170, 210, 255), FXV_RGB(10, 25, 60),
                  5.5f, 0.0f, 0.02f, 0.075f, 260, 480 },
        bounce = { FXV_RGB(210, 215, 225), FXV_RGB(150, 165, 190), FXV_RGB(15, 18, 25),
                   3.2f, 0.0f, 0.02f, 0.06f, 200, 340 };
    const struct fxv_pstyle *st = resisted ? &bounce : &force;
    int i, nspeck = resisted ? 18 : 48, nring = resisted ? 24 : 40;
    float ringv = resisted ? 4.0f : 7.0f; /* reaches ~0.7 / ~1.3 squares */

    if (!iflags.fx_particles || !isok(x, y))
        return;
    /* a bright flash at the point of impact */
    for (i = 0; i < (resisted ? 2 : 4); i++) {
        struct fxv_part *p;

        fxv_spawn(st, x + 0.5f, y + 0.5f, 0.0f, 1.0f, 1.0f);
        p = &fxv.parts[fxv.nparts - 1];
        p->vx = p->vy = 0.0f;
        p->size = resisted ? 0.2f : 0.32f;
        p->c0 = FXV_RGB(255, 255, 255);
        p->life = resisted ? 110 : 150;
    }
    /* scattered specks */
    for (i = 0; i < nspeck; i++)
        fxv_spawn(st, x + 0.5f + fxv_prange(-0.15f, 0.15f),
                 y + 0.5f + fxv_prange(-0.15f, 0.15f), st->speed, 1.0f, 1.0f);
    /* the ring: evenly spaced, all at the same speed, stopping short */
    for (i = 0; i < nring; i++) {
        struct fxv_part *p;
        float a = (6.2831853f * i) / nring + fxv_prange(-0.05f, 0.05f);

        fxv_spawn(st, x + 0.5f, y + 0.5f, 0.0f, 0.8f, 1.0f);
        p = &fxv.parts[fxv.nparts - 1];
        p->vx = ringv * (float) cos(a);
        p->vy = ringv * (float) sin(a);
        p->drag = 0.004f; /* snaps out, then hangs */
        p->c0 = st->c0;
        p->size = st->size * 1.1f;
        p->life = resisted ? 300 : 420;
    }
    fxv_start_timer();
}

/* ---- wand effects ----------------------------------------------- */

/* a bright colour of random hue (polymorph, wishing) */
static fxv_color
fxv_rainbow(void)
{
    float h = fxv_prange(0.0f, 6.0f), f = h - (float) (int) h;
    int q = (int) (255 * (1.0f - f)), t = (int) (255 * f);

    switch ((int) h) {
    case 0:
        return FXV_RGB(255, t, 60);
    case 1:
        return FXV_RGB(q, 255, 60);
    case 2:
        return FXV_RGB(60, 255, t);
    case 3:
        return FXV_RGB(60, q, 255);
    case 4:
        return FXV_RGB(t, 60, 255);
    default:
        return FXV_RGB(255, 60, q);
    }
}

/* the look of each wand; 0 is an unidentified wand (plain) */
static const struct fxv_pstyle *
fxv_wand_style(int look, boolean *rainbow)
{
    static const struct fxv_pstyle
        plain = { FXV_RGB(225, 228, 238), FXV_RGB(175, 185, 205), FXV_RGB(20, 22, 30),
                  0.8f, 0.0f, 0.2f, 0.05f, 250, 450 },
        striking = { FXV_RGB(245, 250, 255), FXV_RGB(170, 210, 255), FXV_RGB(10, 25, 60),
                     1.0f, 0.0f, 0.2f, 0.05f, 250, 450 },
        locking = { FXV_RGB(215, 185, 120), FXV_RGB(160, 160, 178), FXV_RGB(30, 25, 15),
                    1.0f, 0.8f, 0.25f, 0.055f, 300, 550 },
        opening = { FXV_RGB(255, 225, 130), FXV_RGB(255, 190, 70), FXV_RGB(60, 35, 0),
                    1.0f, -0.8f, 0.25f, 0.055f, 300, 600 },
        invis = { FXV_RGB(240, 248, 255), FXV_RGB(190, 212, 238), FXV_RGB(0, 0, 0),
                  0.7f, 0.0f, 0.1f, 0.045f, 200, 400 },
        probing = { FXV_RGB(140, 255, 240), FXV_RGB(70, 200, 255), FXV_RGB(0, 30, 40),
                    1.6f, 0.0f, 0.15f, 0.045f, 220, 420 },
        slow = { FXV_RGB(110, 140, 255), FXV_RGB(70, 90, 210), FXV_RGB(5, 10, 40),
                 0.5f, 0.4f, 0.4f, 0.06f, 600, 1000 },
        speed = { FXV_RGB(230, 255, 130), FXV_RGB(255, 255, 185), FXV_RGB(40, 50, 0),
                  2.8f, 0.0f, 0.05f, 0.045f, 180, 340 },
        undead = { FXV_RGB(255, 250, 225), FXV_RGB(255, 230, 160), FXV_RGB(60, 50, 20),
                   0.9f, -1.2f, 0.3f, 0.06f, 400, 750 },
        cancel = { FXV_RGB(185, 105, 235), FXV_RGB(115, 75, 155), FXV_RGB(0, 0, 0),
                   0.9f, 0.0f, 0.25f, 0.06f, 350, 650 },
        poly = { FXV_RGB(255, 120, 120), FXV_RGB(120, 255, 120), FXV_RGB(20, 20, 20),
                 1.2f, 0.0f, 0.2f, 0.055f, 350, 650 },
        tele = { FXV_RGB(255, 120, 235), FXV_RGB(180, 100, 255), FXV_RGB(30, 0, 50),
                 1.4f, 0.0f, 0.2f, 0.055f, 300, 600 },
        dig = { FXV_RGB(195, 165, 125), FXV_RGB(145, 125, 100), FXV_RGB(30, 25, 20),
                1.0f, 1.2f, 0.3f, 0.06f, 350, 700 },
        light = { FXV_RGB(255, 248, 210), FXV_RGB(255, 222, 145), FXV_RGB(70, 50, 10),
                  0.4f, -0.4f, 0.3f, 0.06f, 600, 1100 },
        secret = { FXV_RGB(160, 225, 255), FXV_RGB(95, 175, 255), FXV_RGB(0, 20, 50),
                   0.5f, 0.0f, 0.3f, 0.055f, 500, 900 },
        create = { FXV_RGB(175, 145, 195), FXV_RGB(115, 105, 135), FXV_RGB(15, 10, 20),
                   1.2f, -0.2f, 0.3f, 0.12f, 600, 1000 },
        enlight = { FXV_RGB(255, 235, 150), FXV_RGB(255, 255, 215), FXV_RGB(60, 40, 0),
                    0.4f, -1.6f, 0.5f, 0.055f, 700, 1300 },
        wish = { FXV_RGB(255, 225, 120), FXV_RGB(255, 255, 255), FXV_RGB(40, 30, 0),
                 1.0f, 3.0f, 0.5f, 0.06f, 700, 1300 },
        stasis = { FXV_RGB(205, 238, 255), FXV_RGB(155, 195, 225), FXV_RGB(20, 30, 40),
                   0.15f, 0.0f, 0.5f, 0.055f, 900, 1500 },
        /* spells no wand does */
        drain = { FXV_RGB(150, 60, 200), FXV_RGB(70, 20, 110), FXV_RGB(0, 0, 0),
                  0.9f, 0.6f, 0.3f, 0.06f, 400, 750 },
        heal = { FXV_RGB(170, 255, 170), FXV_RGB(235, 255, 225), FXV_RGB(0, 40, 10),
                 0.8f, -1.2f, 0.3f, 0.055f, 450, 800 },
        flesh = { FXV_RGB(175, 170, 165), FXV_RGB(255, 175, 150), FXV_RGB(60, 20, 15),
                  0.9f, -0.3f, 0.3f, 0.06f, 400, 750 };

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
    case SPE_DRAIN_LIFE:
        return &drain;
    case SPE_HEALING:
    case SPE_EXTRA_HEALING:
        return &heal;
    case SPE_STONE_TO_FLESH:
        return &flesh;
    default:
        return &plain;
    }
}

/* spawn one particle of style 'st', 'delay' ms from now */
static struct fxv_part *
fxv_spawn_at(const struct fxv_pstyle *st, boolean rainbow,
            float x, float y, float speed, float sizemul, fxv_ms delay)
{
    struct fxv_part *p;

    fxv_spawn(st, x, y, speed, sizemul, 1.0f);
    p = &fxv.parts[fxv.nparts - 1];
    if (rainbow)
        p->c0 = fxv_rainbow();
    p->start += delay;
    return p;
}

/* a ring of 'n' particles centred on square <x,y>, reaching 'radius'
   squares; 'drag' sets how sharply it stops (small = snaps out) */
static void
fxv_ring(const struct fxv_pstyle *st, boolean rainbow,
        int x, int y, int n, float radius, float drag, fxv_ms life,
        fxv_ms delay)
{
    int i;
    float v = radius * (float) -log(drag);

    for (i = 0; i < n; i++) {
        float a = (6.2831853f * i) / n + fxv_prange(-0.04f, 0.04f);
        struct fxv_part *p = fxv_spawn_at(st, rainbow, x + 0.5f,
                                        y + 0.5f, 0.0f, 1.0f, delay);

        p->vx = v * (float) cos(a);
        p->vy = v * (float) sin(a);
        p->drag = drag;
        p->grav = 0.0f;
        p->life = life;
    }
}

/* the digging beam: rock dust kicked up along its path */
void
fxv_dig_particles(int x, int y)
{
    boolean rb;
    const struct fxv_pstyle *st = fxv_wand_style(WAN_DIGGING, &rb);
    int i;

    if (!iflags.fx_particles || !isok(x, y))
        return;
    for (i = 0; i < 7; i++)
        (void) fxv_spawn_at(st, FALSE, x + fxv_prange(0.2f, 0.8f),
                           y + fxv_prange(0.2f, 0.8f), st->speed, 1.0f, 0);
    fxv_start_timer();
}

void
fxv_wand_particles(const struct fx_event *ev)
{
    boolean rb;
    const struct fxv_pstyle *st;
    int i, x = ev->x, y = ev->y, look = ev->otyp;

    if (!iflags.fx_particles || !isok(x, y))
        return;
    if (Hallucination && look)
        look = WAN_POLYMORPH; /* everything's a rainbow */
    st = fxv_wand_style(look, &rb);

    switch (ev->adtyp) {
    case FXW_BEAM: {
        /* a short streak on each square, timed so the beam travels */
        fxv_ms delay = (fxv_ms) (ev->amount * 22);

        for (i = 0; i < (look ? 5 : 3); i++)
            (void) fxv_spawn_at(st, rb, x + fxv_prange(0.25f, 0.75f),
                               y + fxv_prange(0.25f, 0.75f),
                               st->speed * 0.35f, 0.9f, delay);
        break;
    }
    case FXW_HIT:
        /* a burst where it strikes, and a small ring once identified */
        for (i = 0; i < (look ? 28 : 14); i++)
            (void) fxv_spawn_at(st, rb, x + 0.5f, y + 0.5f,
                               st->speed * 1.4f + 0.6f, 1.0f, 0);
        if (look)
            fxv_ring(st, rb, x, y, 18, 0.8f, 0.01f, 380, 0);
        break;
    case FXW_SELF:
        switch (look) {
        case WAN_LIGHT:
            /* a wave of light rolling out, then warm motes hanging */
            fxv_ring(st, FALSE, x, y, 90, 5.0f, 0.02f, 750, 0);
            for (i = 0; i < 70; i++) {
                float r = fxv_prange(0.3f, 4.5f), a = fxv_prange(0, 6.283f);

                (void) fxv_spawn_at(st, FALSE,
                                   x + 0.5f + r * (float) cos(a),
                                   y + 0.5f + r * (float) sin(a) * 0.8f,
                                   st->speed, 1.0f, (fxv_ms) (r * 70));
            }
            break;
        case WAN_SECRET_DOOR_DETECTION:
            /* two searching pulses */
            fxv_ring(st, FALSE, x, y, 90, 7.0f, 0.03f, 850, 0);
            fxv_ring(st, FALSE, x, y, 70, 5.0f, 0.03f, 750, 180);
            break;
        case WAN_CREATE_MONSTER:
            /* dark smoke swirling out from you */
            for (i = 0; i < 55; i++)
                (void) fxv_spawn_at(st, FALSE,
                                   x + 0.5f + fxv_prange(-0.3f, 0.3f),
                                   y + 0.5f + fxv_prange(-0.3f, 0.3f),
                                   st->speed * 1.6f, 1.0f,
                                   (fxv_ms) fxv_prange(0, 160));
            break;
        case WAN_ENLIGHTENMENT:
            /* golden motes rising all around you */
            for (i = 0; i < 70; i++)
                (void) fxv_spawn_at(st, FALSE,
                                   x + 0.5f + fxv_prange(-1.2f, 1.2f),
                                   y + 0.6f + fxv_prange(-0.4f, 0.8f),
                                   st->speed, 1.0f,
                                   (fxv_ms) fxv_prange(0, 350));
            fxv_ring(st, FALSE, x, y, 40, 1.6f, 0.02f, 600, 0);
            break;
        case WAN_WISHING: {
            /* a fountain of coloured sparks and a golden ring */
            for (i = 0; i < 110; i++) {
                struct fxv_part *p = fxv_spawn_at(
                    st, TRUE, x + 0.5f + fxv_prange(-0.15f, 0.15f),
                    y + 0.5f, 0.0f, 1.0f, (fxv_ms) fxv_prange(0, 300));

                p->vx = fxv_prange(-1.4f, 1.4f);
                p->vy = fxv_prange(-5.5f, -3.0f);
            }
            fxv_ring(st, FALSE, x, y, 50, 2.0f, 0.02f, 700, 0);
            break;
        }
        case WAN_DIGGING:
            break; /* its dust is drawn along the beam */
        case WAN_STASIS:
            /* time stills: a slow ring, and motes that barely move */
            fxv_ring(st, FALSE, x, y, 60, 3.0f, 0.15f, 1300, 0);
            for (i = 0; i < 50; i++) {
                float r = fxv_prange(0.2f, 2.6f), a = fxv_prange(0, 6.283f);

                (void) fxv_spawn_at(st, FALSE,
                                   x + 0.5f + r * (float) cos(a),
                                   y + 0.5f + r * (float) sin(a),
                                   st->speed, 1.0f, 0);
            }
            break;
        default:
            /* unidentified: a plain shimmer */
            fxv_ring(st, FALSE, x, y, 36, 1.5f, 0.02f, 500, 0);
            break;
        }
        break;
    }
    fxv_start_timer();
}

/* teleporting: sparks rush in and the creature is gone; at the other
   end a flash and a burst as it appears */
void
fxv_teleport_particles(const struct fx_event *ev)
{
    static const struct fxv_pstyle
        tele = { FXV_RGB(255, 140, 240), FXV_RGB(190, 120, 255), FXV_RGB(40, 0, 60),
                 1.6f, 0.0f, 0.2f, 0.06f, 300, 500 };
    int i;

    if (!iflags.fx_particles)
        return;
    if ((ev->amount & 1) && isok(ev->sx, ev->sy)) {
        /* departure: a ring collapsing inward, and a column rising */
        for (i = 0; i < 36; i++) {
            float a = (6.2831853f * i) / 36, r = fxv_prange(0.75f, 1.0f);
            struct fxv_part *p = fxv_spawn_at(
                &tele, FALSE, ev->sx + 0.5f + r * (float) cos(a),
                ev->sy + 0.5f + r * (float) sin(a), 0.0f, 1.0f, 0);

            p->drag = 0.01f; /* ends just at the middle */
            p->vx = -r * 4.6f * (float) cos(a);
            p->vy = -r * 4.6f * (float) sin(a);
            p->life = 380;
        }
        for (i = 0; i < 18; i++) {
            struct fxv_part *p = fxv_spawn_at(
                &tele, FALSE, ev->sx + 0.5f + fxv_prange(-0.2f, 0.2f),
                ev->sy + fxv_prange(0.4f, 0.9f), 0.0f, 0.8f,
                (fxv_ms) fxv_prange(80, 260));

            p->vx = 0.0f;
            p->vy = fxv_prange(-2.6f, -1.4f);
            p->life = 420;
        }
    }
    if ((ev->amount & 2) && isok(ev->x, ev->y)) {
        /* arrival, a moment later: a flash, a burst and a ring */
        fxv_ms at = (ev->amount & 1) ? 260 : 0;

        for (i = 0; i < 3; i++) {
            struct fxv_part *p = fxv_spawn_at(&tele, FALSE,
                                            ev->x + 0.5f, ev->y + 0.5f, 0.0f,
                                            1.0f, at);

            p->vx = p->vy = 0.0f;
            p->size = 0.3f;
            p->c0 = FXV_RGB(255, 235, 255);
            p->life = 160;
        }
        for (i = 0; i < 30; i++)
            (void) fxv_spawn_at(&tele, FALSE, ev->x + 0.5f, ev->y + 0.5f,
                               tele.speed * 1.6f, 1.0f, at);
        fxv_ring(&tele, FALSE, ev->x, ev->y, 26, 0.9f, 0.01f, 400, at);
    }
    fxv_start_timer();
}

/* 'n' particles of style 'st' born around the hero's feet on <x,y>,
   thrown at 'speed' (upward if 'up'), 'delay' ms from now */
static void
fxv_engr_spray(const struct fxv_pstyle *st, boolean rainbow,
              int x, int y, int n, float speed, boolean up, fxv_ms delay)
{
    int i;

    for (i = 0; i < n; i++) {
        struct fxv_part *p = fxv_spawn_at(
            st, rainbow, x + 0.5f + fxv_prange(-0.3f, 0.3f),
            y + fxv_prange(0.6f, 0.85f), speed, 1.0f,
            delay + (fxv_ms) fxv_prange(0, 120));

        if (up) {
            p->vx = fxv_prange(-0.9f, 0.9f) * speed;
            p->vy = -fxv_prange(0.6f, 1.3f) * speed;
        }
    }
}

/* engraving: motes of whatever is being written in, and the wand clues */
void
fxv_engrave_particles(const struct fx_event *ev)
{
    static const struct fxv_pstyle
        dust = { FXV_RGB(200, 185, 160), FXV_RGB(160, 150, 130), FXV_RGB(30, 28, 24),
                 0.5f, 0.5f, 0.3f, 0.04f, 350, 650 },
        frost = { FXV_RGB(240, 250, 255), FXV_RGB(190, 220, 245), FXV_RGB(20, 40, 60),
                  0.5f, 0.4f, 0.3f, 0.04f, 350, 650 },
        chips = { FXV_RGB(185, 185, 190), FXV_RGB(130, 130, 138), FXV_RGB(25, 25, 28),
                  1.0f, 4.0f, 0.6f, 0.045f, 300, 550 },
        splinters = { FXV_RGB(190, 140, 90), FXV_RGB(140, 100, 60), FXV_RGB(30, 20, 10),
                      1.0f, 4.0f, 0.6f, 0.045f, 300, 550 },
        smoke = { FXV_RGB(120, 115, 110), FXV_RGB(90, 88, 85), FXV_RGB(15, 15, 15),
                  0.25f, -0.9f, 0.5f, 0.07f, 600, 1000 },
        ink = { FXV_RGB(60, 70, 120), FXV_RGB(40, 45, 80), FXV_RGB(10, 10, 20),
                0.3f, 0.3f, 0.3f, 0.03f, 300, 500 },
        blood = { FXV_RGB(170, 25, 25), FXV_RGB(120, 15, 15), FXV_RGB(30, 0, 0),
                  0.3f, 0.6f, 0.3f, 0.035f, 300, 550 },
        fade = { FXV_RGB(175, 175, 185), FXV_RGB(140, 140, 150), FXV_RGB(0, 0, 0),
                 0.5f, -0.7f, 0.4f, 0.045f, 600, 1000 },
        ice = { FXV_RGB(240, 250, 255), FXV_RGB(175, 215, 255), FXV_RGB(40, 70, 110),
                0.2f, 7.0f, 0.9f, 0.06f, 380, 480 },
        bugs = { FXV_RGB(70, 62, 52), FXV_RGB(45, 40, 35), FXV_RGB(10, 10, 10),
                 1.2f, 0.0f, 0.004f, 0.03f, 900, 1200 },
        flash = { FXV_RGB(255, 255, 255), FXV_RGB(255, 255, 220), FXV_RGB(120, 120, 60),
                  0.0f, 0.0f, 0.5f, 0.9f, 140, 200 };
    const struct fxv_pstyle *st;
    boolean rb;
    int i, x = ev->x, y = ev->y;

    if (!iflags.fx_particles || !isok(x, y) || (ev->flags & FXF_UNSEEN))
        return;
    switch (ev->adtyp) {
    case FXG_STROKE:
        switch (ev->amount) {
        case ENGRAVE:
        case HEADSTONE:
            st = (ev->ttyp == FXG_SURF_WOOD) ? &splinters
                 : (ev->ttyp == FXG_SURF_ICE) ? &frost : &chips;
            fxv_engr_spray(st, FALSE, x, y, 6, 1.6f, TRUE, 0);
            break;
        case BURN:
            fxv_engr_spray(fxv_part_style(AD_FIRE), FALSE, x, y, 8, 0.8f,
                          TRUE, 0);
            fxv_engr_spray(&smoke, FALSE, x, y, 5, 0.3f, FALSE, 120);
            break;
        case MARK:
            fxv_engr_spray(&ink, FALSE, x, y, 3, 0.3f, FALSE, 0);
            break;
        case ENGR_BLOOD:
            fxv_engr_spray(&blood, FALSE, x, y, 4, 0.3f, FALSE, 0);
            break;
        default: /* DUST */
            fxv_engr_spray((ev->ttyp == FXG_SURF_ICE) ? &frost : &dust,
                          FALSE, x, y, 6, 0.5f, FALSE, 0);
            break;
        }
        break;
    case FXG_VANISH:
        /* the old writing lifts off as grey motes and is gone */
        for (i = 0; i < 26; i++)
            (void) fxv_spawn_at(&fade, FALSE,
                               x + 0.5f + fxv_prange(-0.4f, 0.4f),
                               y + fxv_prange(0.55f, 0.9f), fade.speed, 1.0f,
                               (fxv_ms) fxv_prange(0, 250));
        break;
    case FXG_ICE:
        /* a few ice cubes tumbling from the wand to the floor */
        for (i = 0; i < 5; i++) {
            struct fxv_part *p = fxv_spawn_at(
                &ice, FALSE, x + 0.5f + fxv_prange(-0.25f, 0.25f),
                y + fxv_prange(0.1f, 0.3f), 0.0f, fxv_prange(0.8f, 1.3f),
                (fxv_ms) (i * 70));

            p->vx = fxv_prange(-0.4f, 0.4f);
            p->vy = fxv_prange(-0.5f, 0.0f);
        }
        break;
    case FXG_BUGS_STOP:
        /* tiny specks scurry, then stop dead */
        for (i = 0; i < 12; i++)
            (void) fxv_spawn_at(&bugs, FALSE,
                               x + 0.5f + fxv_prange(-0.35f, 0.35f),
                               y + fxv_prange(0.55f, 0.9f), bugs.speed, 1.0f,
                               0);
        break;
    case FXG_GRAVEL:
        st = (ev->ttyp == FXG_SURF_WOOD) ? &splinters
             : (ev->ttyp == FXG_SURF_ICE) ? &frost : &chips;
        fxv_engr_spray(st, FALSE, x, y, 22, 3.0f, TRUE, 0);
        fxv_dig_particles(x, y);
        break;
    case FXG_FLAMES:
        fxv_engr_spray(fxv_part_style(AD_FIRE), FALSE, x, y, 30, 1.6f,
                      TRUE, 0);
        fxv_engr_spray(&smoke, FALSE, x, y, 8, 0.3f, FALSE, 200);
        break;
    case FXG_LIGHTNING:
        /* a blinding flash and sparks crackling out */
        for (i = 0; i < 3; i++) {
            struct fxv_part *p = fxv_spawn_at(&flash, FALSE, x + 0.5f,
                                            y + 0.5f, 0.0f, 1.0f + i * 0.6f,
                                            0);

            p->vx = p->vy = 0.0f;
        }
        st = fxv_part_style(AD_ELEC);
        for (i = 0; i < 40; i++)
            (void) fxv_spawn_at(st, FALSE, x + 0.5f, y + 0.6f,
                               st->speed * 1.3f, 1.0f, 0);
        break;
    case FXG_BULLETS: {
        /* small puffs popping up across the square, timed with the shots */
        static const fxv_ms at[6] = { 0, 75, 140, 225, 285, 350 };

        st = fxv_part_style(AD_MAGM);
        for (i = 0; i < 6; i++) {
            float px = x + fxv_prange(0.15f, 0.85f),
                  py = y + fxv_prange(0.4f, 0.9f);
            int k;

            for (k = 0; k < 6; k++)
                (void) fxv_spawn_at(st, FALSE, px, py, 1.2f, 0.8f,
                                   at[i]);
        }
        break;
    }
    case FXG_POLY:
        st = fxv_wand_style(WAN_POLYMORPH, &rb);
        fxv_engr_spray(st, TRUE, x, y, 24, 0.9f, FALSE, 0);
        fxv_ring(st, TRUE, x, y, 16, 0.6f, 0.01f, 380, 0);
        break;
    case FXG_SLOW:
        st = fxv_wand_style(WAN_SLOW_MONSTER, &rb);
        fxv_engr_spray(st, FALSE, x, y, 14, 0.3f, FALSE, 0);
        break;
    case FXG_SPEED:
        st = fxv_wand_style(WAN_SPEED_MONSTER, &rb);
        fxv_engr_spray(st, FALSE, x, y, 16, 2.2f, FALSE, 0);
        break;
    case FXG_STRUGGLE:
        fxv_impact_particles(x, y, TRUE);
        return; /* (starts its own timer) */
    case FXG_CRUMBLE:
        for (i = 0; i < 30; i++)
            (void) fxv_spawn_at(&dust, FALSE,
                               x + 0.5f + fxv_prange(-0.15f, 0.15f),
                               y + 0.5f + fxv_prange(-0.15f, 0.15f), 1.0f,
                               1.4f, (fxv_ms) fxv_prange(0, 150));
        break;
    case FXG_WIPE: {
        /* the writing swept aside */
        float dir = (fxv_prand() < 0.5f) ? -1.0f : 1.0f;

        st = (ev->ttyp == FXG_SURF_ICE) ? &frost
             : (ev->amount == ENGR_BLOOD) ? &blood
             : (ev->amount == MARK) ? &ink : &dust;
        for (i = 0; i < 16; i++) {
            struct fxv_part *p = fxv_spawn_at(
                st, FALSE, x + 0.5f + fxv_prange(-0.35f, 0.35f),
                y + fxv_prange(0.6f, 0.85f), 0.0f, 1.0f,
                (fxv_ms) fxv_prange(0, 180));

            p->vx = dir * fxv_prange(0.8f, 1.8f);
            p->vy = fxv_prange(-0.2f, 0.1f);
        }
        break;
    }
    case FXG_WORN:
    default:
        return;
    }
    fxv_start_timer();
}

/* a summoned monster appearing: a spiral drawing in to its square, a
   flash and a ring; each of a group a little after the one before */
void
fxv_summon_particles(const struct fx_event *ev)
{
    static const struct fxv_pstyle styles[4] = {
        /* arcane: violet */
        { FXV_RGB(235, 170, 255), FXV_RGB(160, 110, 255), FXV_RGB(30, 0, 60),
          1.4f, 0.0f, 0.22f, 0.06f, 350, 600 },
        /* demonic: fire and embers */
        { FXV_RGB(255, 190, 80), FXV_RGB(240, 70, 30), FXV_RGB(50, 0, 0),
          1.4f, -0.5f, 0.22f, 0.07f, 400, 700 },
        /* holy: gold-white */
        { FXV_RGB(255, 250, 215), FXV_RGB(255, 215, 110), FXV_RGB(60, 40, 0),
          1.3f, -0.3f, 0.2f, 0.06f, 400, 700 },
        /* swarm: dusty green-brown */
        { FXV_RGB(200, 220, 120), FXV_RGB(150, 130, 70), FXV_RGB(20, 25, 0),
          1.8f, 0.4f, 0.25f, 0.05f, 250, 450 },
    };
    const struct fxv_pstyle *st;
    fxv_ms at;
    float cx, cy;
    int i, k = ev->adtyp;

    if (!iflags.fx_particles || !isok(ev->x, ev->y))
        return;
    k = (k < 0 || k > 3) ? 0 : k;
    st = &styles[k];
    at = (fxv_ms) min(ev->amount, 12) * 120; /* one after another */
    cx = ev->x + 0.5f, cy = ev->y + 0.5f;
    if (k != FXS_SWARM) {
        /* a spiral of motes drawing in to the middle */
        for (i = 0; i < 28; i++) {
            float a = (6.2831853f * i) / 14, r = 0.55f + 0.5f * (i / 28.0f);
            struct fxv_part *p = fxv_spawn_at(st, FALSE,
                                            cx + r * (float) cos(a),
                                            cy + r * (float) sin(a), 0.0f,
                                            0.9f, at + (fxv_ms) (i * 9));

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
        struct fxv_part *p = fxv_spawn_at(st, FALSE, cx, cy, 0.0f, 1.0f,
                                        at);

        p->vx = p->vy = 0.0f;
        p->size = 0.32f;
        p->c0 = (k == FXS_DEMONIC) ? FXV_RGB(255, 230, 160) : FXV_RGB(255, 250, 255);
        p->life = 170;
    }
    for (i = 0; i < 24; i++)
        (void) fxv_spawn_at(st, FALSE, cx, cy, st->speed * 1.5f, 1.0f,
                           at);
    fxv_ring(st, FALSE, ev->x, ev->y, 22, 0.8f, 0.01f, 380, at);
    if (k == FXS_DEMONIC) {
        /* a curl of smoke rising after */
        static const struct fxv_pstyle smoke = {
            FXV_RGB(90, 80, 80), FXV_RGB(60, 55, 55), FXV_RGB(10, 10, 10),
            0.3f, -1.2f, 0.35f, 0.03f, 700, 1100 };

        for (i = 0; i < 10; i++)
            (void) fxv_spawn_at(&smoke, FALSE,
                               cx + fxv_prange(-0.3f, 0.3f),
                               cy + fxv_prange(-0.1f, 0.3f), 0.3f, 1.0f,
                               at + 60);
    }
    fxv_start_timer();
}

/* drinking a potion: first a swirl of its own colour rising around the
   hero (what anyone could see); then, if its type is known, its effect */
void
fxv_quaff_particles(const struct fx_event *ev)
{
    static const struct fxv_pstyle
        swirl = { FXV_RGB(255, 255, 255), FXV_RGB(255, 255, 255), FXV_RGB(10, 10, 10),
                  0.0f, -0.6f, 0.4f, 0.06f, 500, 850 },
        heal = { FXV_RGB(200, 255, 200), FXV_RGB(110, 240, 120), FXV_RGB(0, 40, 0),
                 0.5f, -1.3f, 0.45f, 0.06f, 600, 1100 },
        energy = { FXV_RGB(190, 215, 255), FXV_RGB(110, 150, 255), FXV_RGB(0, 10, 50),
                   0.5f, -1.3f, 0.45f, 0.06f, 600, 1100 },
        gold = { FXV_RGB(255, 235, 150), FXV_RGB(255, 200, 80), FXV_RGB(50, 30, 0),
                 0.4f, -1.8f, 0.45f, 0.06f, 700, 1300 },
        acid = { FXV_RGB(220, 255, 70), FXV_RGB(170, 230, 30), FXV_RGB(30, 45, 0),
                 1.4f, -0.4f, 0.3f, 0.05f, 300, 600 },
        sick = { FXV_RGB(170, 210, 60), FXV_RGB(120, 160, 40), FXV_RGB(15, 25, 0),
                 0.5f, -0.6f, 0.4f, 0.08f, 600, 1000 },
        booze = { FXV_RGB(255, 200, 110), FXV_RGB(230, 160, 60), FXV_RGB(40, 20, 0),
                  0.4f, -0.9f, 0.4f, 0.06f, 600, 1000 },
        holy = { FXV_RGB(255, 252, 230), FXV_RGB(255, 235, 170), FXV_RGB(60, 50, 20),
                 0.5f, -1.4f, 0.4f, 0.06f, 600, 1100 },
        unholy = { FXV_RGB(200, 40, 40), FXV_RGB(120, 20, 40), FXV_RGB(20, 0, 5),
                   0.5f, -0.9f, 0.4f, 0.06f, 600, 1000 },
        detect = { FXV_RGB(220, 170, 255), FXV_RGB(160, 110, 255), FXV_RGB(25, 0, 50),
                   0.5f, 0.0f, 0.3f, 0.055f, 500, 900 };
    const struct fxv_pstyle *st = 0;
    boolean rb = FALSE;
    int i, x = ev->x, y = ev->y, n = 50;

    if (!iflags.fx_particles || !isok(x, y))
        return;

    if (ev->adtyp == 0) {
        /* a spiral of the potion's colour rising round the hero */
        fxv_color c = fxv_nh_color(ev->amount);

        if (FXV_R(c) + FXV_G(c) + FXV_B(c) < 200)
            c = FXV_RGB(110, 110, 135); /* inky or black: still visible */
        for (i = 0; i < 26; i++) {
            float a = (6.2831853f * i) / 13.0f;
            struct fxv_part *p = fxv_spawn_at(
                &swirl, Hallucination, x + 0.5f + 0.42f * (float) cos(a),
                y + 0.85f, 0.0f, 1.0f, (fxv_ms) (i * 22));

            if (!Hallucination)
                p->c0 = c;
            p->vx = -0.9f * (float) sin(a);
            p->vy = -1.1f;
        }
        fxv_start_timer();
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
        fxv_ring(&detect, FALSE, x, y, 80, 6.0f, 0.03f, 800, 0);
        fxv_start_timer();
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
        (void) fxv_spawn_at(st, rb, x + 0.5f + fxv_prange(-0.9f, 0.9f),
                           y + 0.7f + fxv_prange(-0.3f, 0.5f), st->speed,
                           1.0f, (fxv_ms) fxv_prange(0, 300));
    fxv_ring(st, rb, x, y, 32, 1.3f, 0.02f, 550, 0);
    fxv_start_timer();
}

/* detection: each thing found flashes briefly where it appears, in a
   ripple spreading out from the hero; a unicorn horn that cured
   something sparkles clean around the hero */
void
fxv_detect_particles(const struct fx_event *ev)
{
    static const struct fxv_pstyle
        mon = { FXV_RGB(255, 140, 170), FXV_RGB(220, 90, 255), FXV_RGB(40, 0, 30),
                1.4f, -0.4f, 0.2f, 0.055f, 350, 650 },
        obj = { FXV_RGB(255, 235, 140), FXV_RGB(140, 230, 255), FXV_RGB(30, 30, 10),
                1.4f, -0.4f, 0.2f, 0.055f, 350, 650 },
        cure = { FXV_RGB(235, 250, 255), FXV_RGB(150, 230, 255), FXV_RGB(0, 30, 40),
                 0.6f, -1.6f, 0.4f, 0.06f, 500, 950 };
    int i, x = ev->x, y = ev->y;

    if (!iflags.fx_particles || !isok(x, y))
        return;
    if (ev->type == FXE_UNIHORN) {
        if (ev->amount <= 0)
            return;
        for (i = 0; i < 45; i++)
            (void) fxv_spawn_at(&cure, FALSE,
                               x + 0.5f + fxv_prange(-0.7f, 0.7f),
                               y + 0.6f + fxv_prange(-0.3f, 0.5f), cure.speed,
                               1.0f, (fxv_ms) fxv_prange(0, 250));
        fxv_ring(&cure, FALSE, x, y, 30, 1.2f, 0.02f, 500, 0);
        fxv_start_timer();
        return;
    }
    if (ev->flags & FXF_HERO_TARGET)
        return; /* the summary is for the sound */
    {
        const struct fxv_pstyle *st = (ev->adtyp == FXD_MONSTER) ? &mon : &obj;
        fxv_ms at = (fxv_ms) (ev->amount * 45); /* the ripple spreading out */

        for (i = 0; i < 3; i++) {
            struct fxv_part *p = fxv_spawn_at(st, FALSE, x + 0.5f,
                                            y + 0.5f, 0.0f, 1.0f, at);

            p->vx = p->vy = 0.0f;
            p->size = 0.22f;
            p->c0 = FXV_RGB(255, 255, 255);
            p->life = 160;
        }
        for (i = 0; i < 12; i++)
            (void) fxv_spawn_at(st, FALSE, x + 0.5f, y + 0.5f,
                               st->speed, 1.0f, at);
        fxv_ring(st, FALSE, x, y, 14, 0.7f, 0.01f, 420, at);
    }
    fxv_start_timer();
}

/* a scroll whose type is known has taken effect: its look around the hero
   (teleportation, fire and the detection scrolls have their own) */
void
fxv_scroll_particles(const struct fx_event *ev)
{
    static const struct fxv_pstyle
        wpn = { FXV_RGB(200, 230, 255), FXV_RGB(110, 170, 255), FXV_RGB(0, 15, 50),
                0.4f, -2.0f, 0.4f, 0.055f, 600, 1000 },
        armor = { FXV_RGB(255, 235, 150), FXV_RGB(255, 200, 80), FXV_RGB(50, 30, 0),
                  0.6f, -0.9f, 0.4f, 0.06f, 600, 1100 },
        holy = { FXV_RGB(255, 252, 230), FXV_RGB(255, 235, 170), FXV_RGB(60, 50, 20),
                 0.5f, -1.4f, 0.4f, 0.06f, 700, 1200 },
        ident = { FXV_RGB(170, 255, 250), FXV_RGB(90, 210, 255), FXV_RGB(0, 30, 40),
                  1.4f, -0.3f, 0.25f, 0.05f, 400, 700 },
        mapping = { FXV_RGB(200, 240, 255), FXV_RGB(120, 200, 255), FXV_RGB(0, 20, 40),
                    0.0f, 0.0f, 0.1f, 0.055f, 900, 900 },
        earth = { FXV_RGB(195, 165, 125), FXV_RGB(145, 125, 100), FXV_RGB(30, 25, 20),
                  2.2f, 1.4f, 0.2f, 0.07f, 450, 850 },
        taming = { FXV_RGB(255, 170, 210), FXV_RGB(255, 120, 180), FXV_RGB(60, 0, 25),
                   0.5f, -1.0f, 0.4f, 0.065f, 700, 1200 },
        genocide = { FXV_RGB(210, 30, 40), FXV_RGB(120, 0, 30), FXV_RGB(15, 0, 5),
                     0.0f, 0.0f, 0.1f, 0.07f, 1000, 1000 },
        charging = { FXV_RGB(255, 255, 200), FXV_RGB(140, 200, 255), FXV_RGB(20, 20, 50),
                     3.0f, 0.0f, 0.05f, 0.045f, 160, 360 },
        scare = { FXV_RGB(170, 90, 230), FXV_RGB(90, 40, 150), FXV_RGB(10, 0, 20),
                  0.0f, 0.0f, 0.1f, 0.07f, 800, 800 },
        shards = { FXV_RGB(200, 200, 205), FXV_RGB(140, 140, 150), FXV_RGB(20, 20, 25),
                   1.6f, 2.4f, 0.3f, 0.05f, 450, 800 },
        fog = { FXV_RGB(150, 150, 165), FXV_RGB(110, 110, 125), FXV_RGB(10, 10, 15),
                0.4f, 0.0f, 0.4f, 0.12f, 900, 1500 },
        confuse = { FXV_RGB(255, 110, 120), FXV_RGB(200, 90, 230), FXV_RGB(40, 0, 30),
                    0.4f, -0.6f, 0.4f, 0.055f, 600, 1000 },
        punish = { FXV_RGB(160, 160, 170), FXV_RGB(110, 110, 120), FXV_RGB(15, 15, 20),
                   1.4f, 0.8f, 0.2f, 0.06f, 400, 700 };
    boolean rb;
    int i, x = ev->x, y = ev->y;

    if (!iflags.fx_particles || !isok(x, y))
        return;
    switch (ev->otyp) {
    case SCR_ENCHANT_WEAPON:
        /* blue sparks climbing as the weapon glows */
        for (i = 0; i < 45; i++)
            (void) fxv_spawn_at(&wpn, FALSE,
                               x + 0.5f + fxv_prange(-0.35f, 0.35f),
                               y + 0.8f, wpn.speed, 1.0f,
                               (fxv_ms) fxv_prange(0, 350));
        fxv_ring(&wpn, FALSE, x, y, 24, 0.9f, 0.02f, 450, 0);
        break;
    case SCR_ENCHANT_ARMOR:
        /* a golden shimmer all over */
        for (i = 0; i < 60; i++)
            (void) fxv_spawn_at(&armor, FALSE,
                               x + 0.5f + fxv_prange(-0.55f, 0.55f),
                               y + 0.5f + fxv_prange(-0.5f, 0.5f),
                               armor.speed, 1.0f, (fxv_ms) fxv_prange(0, 300));
        fxv_ring(&armor, FALSE, x, y, 32, 1.1f, 0.02f, 550, 0);
        break;
    case SCR_REMOVE_CURSE:
        for (i = 0; i < 60; i++)
            (void) fxv_spawn_at(&holy, FALSE,
                               x + 0.5f + fxv_prange(-1.0f, 1.0f),
                               y + 0.7f + fxv_prange(-0.3f, 0.5f), holy.speed,
                               1.0f, (fxv_ms) fxv_prange(0, 400));
        fxv_ring(&holy, FALSE, x, y, 40, 1.6f, 0.02f, 650, 0);
        break;
    case SCR_IDENTIFY:
        for (i = 0; i < 30; i++)
            (void) fxv_spawn_at(&ident, FALSE, x + 0.5f, y + 0.4f,
                               ident.speed, 1.0f, (fxv_ms) fxv_prange(0, 150));
        fxv_ring(&ident, FALSE, x, y, 24, 0.9f, 0.01f, 400, 0);
        break;
    case SCR_MAGIC_MAPPING:
        /* the level drawing itself out from you */
        fxv_ring(&mapping, FALSE, x, y, 120, 12.0f, 0.15f, 1300, 0);
        fxv_ring(&mapping, FALSE, x, y, 90, 8.0f, 0.15f, 1100, 200);
        break;
    case SCR_LIGHT:
        /* the same wave of light as the wand */
        {
            struct fx_event le = *ev;

            le.type = FXE_WAND;
            le.adtyp = FXW_SELF;
            le.otyp = WAN_LIGHT;
            fxv_wand_particles(&le);
        }
        return;
    case SCR_CREATE_MONSTER:
    case SCR_STINKING_CLOUD:
        {
            struct fx_event le = *ev;

            le.type = FXE_WAND;
            le.adtyp = FXW_SELF;
            le.otyp = WAN_CREATE_MONSTER; /* a puff of smoke */
            fxv_wand_particles(&le);
        }
        return;
    case SCR_EARTH:
        /* dust thrown up all around as the boulders land */
        for (i = 0; i < 70; i++) {
            float r = fxv_prange(0.6f, 1.6f), a = fxv_prange(0, 6.283f);

            (void) fxv_spawn_at(&earth, FALSE,
                               x + 0.5f + r * (float) cos(a),
                               y + 0.5f + r * (float) sin(a), earth.speed,
                               1.0f, (fxv_ms) fxv_prange(0, 200));
        }
        break;
    case SCR_TAMING:
        for (i = 0; i < 40; i++)
            (void) fxv_spawn_at(&taming, FALSE,
                               x + 0.5f + fxv_prange(-1.2f, 1.2f),
                               y + 0.6f + fxv_prange(-0.6f, 0.6f),
                               taming.speed, 1.0f, (fxv_ms) fxv_prange(0, 400));
        fxv_ring(&taming, FALSE, x, y, 40, 1.8f, 0.03f, 700, 0);
        break;
    case SCR_GENOCIDE:
        /* a dark red pulse rolling out, twice */
        fxv_ring(&genocide, FALSE, x, y, 90, 7.0f, 0.1f, 1200, 0);
        fxv_ring(&genocide, FALSE, x, y, 70, 4.5f, 0.1f, 1000, 250);
        break;
    case SCR_CHARGING:
        for (i = 0; i < 50; i++)
            (void) fxv_spawn_at(&charging, FALSE,
                               x + 0.5f + fxv_prange(-0.3f, 0.3f),
                               y + 0.5f + fxv_prange(-0.3f, 0.3f),
                               charging.speed, 0.9f,
                               (fxv_ms) fxv_prange(0, 400));
        break;
    case SCR_SCARE_MONSTER:
        fxv_ring(&scare, FALSE, x, y, 80, 5.0f, 0.08f, 1000, 0);
        break;
    case SCR_DESTROY_ARMOR:
        for (i = 0; i < 40; i++)
            (void) fxv_spawn_at(&shards, FALSE,
                               x + 0.5f + fxv_prange(-0.4f, 0.4f),
                               y + 0.4f + fxv_prange(-0.3f, 0.3f),
                               shards.speed, 1.0f, (fxv_ms) fxv_prange(0, 150));
        break;
    case SCR_AMNESIA:
        for (i = 0; i < 40; i++)
            (void) fxv_spawn_at(&fog, FALSE,
                               x + 0.5f + fxv_prange(-1.0f, 1.0f),
                               y + 0.5f + fxv_prange(-0.8f, 0.8f), fog.speed,
                               1.0f, (fxv_ms) fxv_prange(0, 500));
        break;
    case SCR_CONFUSE_MONSTER:
        /* the hands glowing */
        for (i = 0; i < 30; i++)
            (void) fxv_spawn_at(&confuse, FALSE,
                               x + 0.5f + ((i & 1) ? 0.35f : -0.35f)
                                   + fxv_prange(-0.1f, 0.1f),
                               y + 0.65f, confuse.speed, 1.0f,
                               (fxv_ms) fxv_prange(0, 400));
        break;
    case SCR_PUNISHMENT:
        for (i = 0; i < 26; i++)
            (void) fxv_spawn_at(&punish, FALSE, x + 0.5f, y + 0.95f,
                               punish.speed, 1.0f, (fxv_ms) fxv_prange(0, 100));
        break;
    default:
        return;
    }
    (void) rb;
    fxv_start_timer();
}

/* casting a spell: motes of its school's colour drawn in to the hero's
   hands and a small ring as it's released; then, for the spells that act
   like a scroll or potion (or have nothing else to show), its look */
void
fxv_cast_particles(const struct fx_event *ev)
{
    static const struct fxv_pstyle school[7] = {
        /* attack */
        { FXV_RGB(255, 140, 80), FXV_RGB(255, 70, 60), FXV_RGB(50, 0, 0),
          0.0f, 0.0f, 0.2f, 0.05f, 380, 380 },
        /* healing */
        { FXV_RGB(160, 255, 160), FXV_RGB(225, 255, 210), FXV_RGB(0, 40, 10),
          0.0f, 0.0f, 0.2f, 0.05f, 380, 380 },
        /* divination */
        { FXV_RGB(140, 240, 255), FXV_RGB(90, 180, 255), FXV_RGB(0, 20, 50),
          0.0f, 0.0f, 0.2f, 0.05f, 380, 380 },
        /* enchantment */
        { FXV_RGB(255, 140, 220), FXV_RGB(200, 110, 255), FXV_RGB(40, 0, 40),
          0.0f, 0.0f, 0.2f, 0.05f, 380, 380 },
        /* clerical */
        { FXV_RGB(255, 245, 200), FXV_RGB(255, 220, 130), FXV_RGB(60, 45, 10),
          0.0f, 0.0f, 0.2f, 0.05f, 380, 380 },
        /* escape */
        { FXV_RGB(220, 240, 255), FXV_RGB(170, 200, 255), FXV_RGB(20, 30, 50),
          0.0f, 0.0f, 0.2f, 0.05f, 380, 380 },
        /* matter */
        { FXV_RGB(230, 190, 120), FXV_RGB(190, 150, 100), FXV_RGB(40, 25, 10),
          0.0f, 0.0f, 0.2f, 0.05f, 380, 380 },
    };
    static const struct fxv_pstyle
        haste = { FXV_RGB(230, 255, 130), FXV_RGB(255, 255, 200), FXV_RGB(40, 50, 0),
                  0.0f, 0.0f, 0.1f, 0.07f, 380, 600 },
        airy = { FXV_RGB(225, 240, 255), FXV_RGB(180, 215, 255), FXV_RGB(10, 20, 40),
                 0.3f, -0.9f, 0.4f, 0.055f, 700, 1100 },
        shield = { FXV_RGB(255, 230, 140), FXV_RGB(255, 210, 90), FXV_RGB(60, 40, 0),
                   0.0f, 0.0f, 0.1f, 0.06f, 900, 900 },
        dust = { FXV_RGB(195, 175, 145), FXV_RGB(150, 135, 115), FXV_RGB(25, 20, 15),
                 1.4f, -0.2f, 0.25f, 0.075f, 450, 800 },
        vision = { FXV_RGB(170, 240, 255), FXV_RGB(120, 190, 255), FXV_RGB(0, 20, 45),
                   0.0f, 0.0f, 0.1f, 0.055f, 1100, 1100 };
    struct fx_event le = *ev;
    boolean rb;
    int i, x = ev->x, y = ev->y;

    if (!iflags.fx_particles || !isok(x, y))
        return;
    if (ev->adtyp == 0) {
        int k = ev->amount - P_ATTACK_SPELL;
        const struct fxv_pstyle *st = &school[(k < 0 || k > 6) ? 0 : k];

        for (i = 0; i < 26; i++) {
            float a = (6.2831853f * i) / 26, r = fxv_prange(0.8f, 1.1f);
            struct fxv_part *p = fxv_spawn_at(
                st, Hallucination, x + 0.5f + r * (float) cos(a),
                y + 0.5f + r * (float) sin(a), 0.0f, 0.9f,
                (fxv_ms) fxv_prange(0, 90));

            p->drag = 0.01f; /* comes to rest at the hands */
            p->vx = -r * 4.4f * (float) cos(a);
            p->vy = -r * 4.4f * (float) sin(a);
            p->life = 360;
        }
        fxv_ring(st, Hallucination, x, y, 22, 0.9f, 0.02f, 380, 300);
        fxv_start_timer();
        return;
    }

    switch (ev->otyp) {
    /* like scrolls */
    case SPE_REMOVE_CURSE:
        le.otyp = SCR_REMOVE_CURSE;
        break;
    case SPE_IDENTIFY:
        le.otyp = SCR_IDENTIFY;
        break;
    case SPE_MAGIC_MAPPING:
        le.otyp = SCR_MAGIC_MAPPING;
        break;
    case SPE_CONFUSE_MONSTER:
        le.otyp = SCR_CONFUSE_MONSTER;
        break;
    case SPE_CHARM_MONSTER:
        le.otyp = SCR_TAMING;
        break;
    case SPE_CAUSE_FEAR:
        le.otyp = SCR_SCARE_MONSTER;
        break;
    /* like potions */
    case SPE_DETECT_MONSTERS:
        le.type = FXE_QUAFF, le.otyp = POT_MONSTER_DETECTION;
        break;
    case SPE_DETECT_TREASURE:
    case SPE_DETECT_FOOD:
        le.type = FXE_QUAFF, le.otyp = POT_OBJECT_DETECTION;
        break;
    case SPE_RESTORE_ABILITY:
        le.type = FXE_QUAFF, le.otyp = POT_RESTORE_ABILITY;
        break;
    /* like a unicorn horn that cured something */
    case SPE_CURE_BLINDNESS:
    case SPE_CURE_SICKNESS:
        le.type = FXE_UNIHORN, le.amount = 1;
        fxv_detect_particles(&le);
        return;
    case SPE_HASTE_SELF:
        /* streaks whipping past on both sides */
        for (i = 0; i < 64; i++) {
            float side = (i & 1) ? 1.0f : -1.0f;
            struct fxv_part *p = fxv_spawn_at(
                &haste, FALSE, x + 0.5f + side * fxv_prange(0.3f, 0.6f),
                y + fxv_prange(0.1f, 0.9f), 0.0f, 0.8f,
                (fxv_ms) fxv_prange(0, 300));

            p->vx = side * fxv_prange(-0.5f, 0.5f);
            p->vy = fxv_prange(-5.5f, -3.5f);
        }
        fxv_ring(&haste, FALSE, x, y, 24, 1.0f, 0.01f, 350, 0);
        break;
    case SPE_INVISIBILITY: {
        /* a pale shimmer gathering and fading out */
        const struct fxv_pstyle *st = fxv_wand_style(WAN_MAKE_INVISIBLE, &rb);

        for (i = 0; i < 50; i++)
            (void) fxv_spawn_at(st, FALSE,
                               x + 0.5f + fxv_prange(-0.45f, 0.45f),
                               y + 0.5f + fxv_prange(-0.5f, 0.5f), 0.3f, 1.0f,
                               (fxv_ms) fxv_prange(0, 450));
        fxv_ring(st, FALSE, x, y, 30, 1.2f, 0.02f, 550, 0);
        break;
    }
    case SPE_LEVITATION:
        /* air swirling up from under the hero's feet */
        for (i = 0; i < 40; i++) {
            float a = (6.2831853f * i) / 14.0f;
            struct fxv_part *p = fxv_spawn_at(
                &airy, FALSE, x + 0.5f + 0.5f * (float) cos(a),
                y + 0.95f, 0.0f, 1.0f, (fxv_ms) (i * 18));

            p->vx = -1.2f * (float) sin(a);
            p->vy = -0.9f;
        }
        break;
    case SPE_PROTECTION:
        /* a golden haze closing round the hero, then hanging there */
        for (i = 0; i < 44; i++) {
            float a = (6.2831853f * i) / 44, r = fxv_prange(1.3f, 1.6f);
            struct fxv_part *p = fxv_spawn_at(
                &shield, FALSE, x + 0.5f + r * (float) cos(a),
                y + 0.5f + r * (float) sin(a), 0.0f, 1.0f, 0);

            p->drag = 0.02f;
            p->vx = -(r - 0.75f) * 3.9f * (float) cos(a);
            p->vy = -(r - 0.75f) * 3.9f * (float) sin(a);
        }
        for (i = 0; i < 40; i++) {
            float a = fxv_prange(0, 6.283f), r = fxv_prange(0.4f, 0.8f);

            (void) fxv_spawn_at(&shield, FALSE,
                               x + 0.5f + r * (float) cos(a),
                               y + 0.5f + r * (float) sin(a), 0.1f, 0.8f,
                               (fxv_ms) fxv_prange(250, 650));
        }
        break;
    case SPE_JUMPING:
        /* dust kicked up where the hero left and where they land */
        if (ev->sx == x && ev->sy == y)
            return;
        for (i = 0; i < 26; i++) {
            if (isok(ev->sx, ev->sy))
                (void) fxv_spawn_at(&dust, FALSE,
                                   ev->sx + 0.5f + fxv_prange(-0.4f, 0.4f),
                                   ev->sy + 0.9f, dust.speed, 1.0f, 0);
            (void) fxv_spawn_at(&dust, FALSE,
                               x + 0.5f + fxv_prange(-0.4f, 0.4f), y + 0.9f,
                               dust.speed * 1.3f, 1.0f, 0);
        }
        break;
    case SPE_CLAIRVOYANCE:
        /* sight reaching out over the surroundings */
        fxv_ring(&vision, FALSE, x, y, 110, 8.0f, 0.12f, 1200, 0);
        for (i = 0; i < 60; i++) {
            float r = fxv_prange(1.0f, 8.0f), a = fxv_prange(0, 6.283f);

            (void) fxv_spawn_at(&vision, FALSE,
                               x + 0.5f + r * (float) cos(a),
                               y + 0.5f + r * (float) sin(a) * 0.6f, 0.2f,
                               0.8f, (fxv_ms) (r * 90));
        }
        break;
    default:
        return;
    }
    if (le.type == FXE_QUAFF) {
        le.adtyp = 1;
        fxv_quaff_particles(&le);
        return;
    }
    if (le.otyp != ev->otyp) {
        le.type = FXE_READ;
        le.adtyp = 1;
        fxv_scroll_particles(&le);
        return;
    }
    fxv_start_timer();
}

fxv_color
fxv_lerp_color(fxv_color a, fxv_color b, float t)
{
    return FXV_RGB((int) (FXV_R(a) + (FXV_R(b) - FXV_R(a)) * t),
               (int) (FXV_G(a) + (FXV_G(b) - FXV_G(a)) * t),
               (int) (FXV_B(a) + (FXV_B(b) - FXV_B(a)) * t));
}

/* ---- floating text ------------------------------------------------ */

/* damage and heal numbers, "Miss", "Killed", status names, "+20 XP",
   "Level 5!": each floats up off its square and fades */

static struct fxv_floater *fxv_add_floater(const struct fx_event *, int,
                                           const char *, fxv_color);
static void fxv_status_floaters(const struct fx_event *);

static fxv_color
fxv_kind_color(const struct fx_event *ev)
{
    if (ev->type == FXE_HEAL)
        return FXV_RGB(90, 255, 90);
    switch (ev->adtyp) {
    case AD_FIRE:
        return FXV_RGB(255, 150, 40);
    case AD_COLD:
        return FXV_RGB(120, 200, 255);
    case AD_ELEC:
        return FXV_RGB(255, 240, 80);
    case AD_DRST:
    case AD_DRDX:
    case AD_DRCO:
        return FXV_RGB(140, 230, 60);
    case AD_ACID:
        return FXV_RGB(200, 255, 60);
    case AD_MAGM:
        return FXV_RGB(230, 120, 255);
    case AD_SLEE:
        return FXV_RGB(170, 140, 255);
    case AD_DISN:
        return FXV_RGB(190, 190, 190);
    case AD_DRLI:
        return FXV_RGB(190, 80, 210);
    }
    /* physical or unknown: red when it's you getting hurt */
    return (ev->flags & FXF_HERO_TARGET) ? FXV_RGB(255, 70, 70)
                                         : FXV_RGB(255, 255, 255);
}

/* start a floater over ev's square: a number, or text if not null */
static struct fxv_floater *
fxv_add_floater(const struct fx_event *ev, int amount,
               const char *text, fxv_color color)
{
    struct fxv_floater *f;
    fxv_ms now = fxv_now();
    int i, stack = 0;

    /* raise above floaters still young on the same square */
    for (i = 0; i < fxv.nfloaters; i++) {
        f = &fxv.floaters[i];
        if (f->x == ev->x && f->y == ev->y && now - f->start < FXV_STACK_MS
            && f->stack >= stack)
            stack = f->stack + 1;
    }

    if (fxv.nfloaters >= FXV_MAXFLOAT) {
        /* drop the oldest */
        memmove(&fxv.floaters[0], &fxv.floaters[1],
                (FXV_MAXFLOAT - 1) * sizeof fxv.floaters[0]);
        fxv.nfloaters--;
    }
    f = &fxv.floaters[fxv.nfloaters++];
    f->x = ev->x, f->y = ev->y;
    f->target = ev->target_id;
    f->type = ev->type;
    f->amount = amount;
    f->text[0] = '\0';
    if (text)
        Strcpy(f->text, text);
    f->life = text ? FXV_TEXT_MS : FXV_FLOAT_MS;
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

    fxv_start_timer();
    return f;
}

/* status names, ToME style: "Confused" floats up when it lands */
#define FXS_GOOD FXV_RGB(255, 215, 90)
#define FXS_BAD FXV_RGB(255, 95, 85)
#define FXS_MIND FXV_RGB(205, 130, 255)
#define FXS_BODY FXV_RGB(160, 225, 80)
#define FXS_LOST FXV_RGB(175, 175, 175)
static const struct fxv_status_name {
    unsigned long bit;
    const char *name;
    fxv_color color;
} fxv_status_names[] = {
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
    { FXA_CONFLICT, "Conflict", FXS_BAD },
};

static void
fxv_status_floaters(const struct fx_event *ev)
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
    /* monsters drift in and out of Conflict's reach as they move; their
       red aura shows it, so only the hero's own gets a name */
    if (!(ev->flags & FXF_HERO_TARGET)) {
        gain &= ~FXA_CONFLICT;
        lose &= ~FXA_CONFLICT;
    }
    for (i = 0; i < SIZE(fxv_status_names); i++) {
        const struct fxv_status_name *s = &fxv_status_names[i];

        if (gain & s->bit)
            (void) fxv_add_floater(ev, 0, s->name, s->color);
        if (lose & s->bit) {
            Snprintf(buf, sizeof buf, "-%s", s->name);
            (void) fxv_add_floater(ev, 0, buf, FXS_LOST);
        }
    }
}

/* the floating text an fx event makes, if any */
void
fxv_floater_event(const struct fx_event *ev)
{
    struct fxv_floater *f;
    fxv_ms now = fxv_now();
    int i;

    if (ev->type == FXE_STATUS) {
        if (iflags.fx_status_text)
            fxv_status_floaters(ev);
        return;
    }
    if (ev->type == FXE_EXP) {
        /* "+20 XP" in light blue over the hero */
        if (iflags.fx_exp_text) {
            char buf[32];

            Snprintf(buf, sizeof buf, "+%d XP", ev->amount);
            f = fxv_add_floater(ev, 0, buf, FXV_RGB(120, 200, 255));
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
            f = fxv_add_floater(ev, 0, buf,
                               up ? FXV_RGB(255, 205, 60) : FXV_RGB(170, 120, 210));
            f->big = TRUE;
            f->life = FXV_LEVEL_MS;
            f->xoff = 0; /* centred over the hero */
        }
        return;
    }
    if (!iflags.fx_damage_numbers)
        return;
    if (ev->type == FXE_DEATH) {
        /* "Killed" where the health bar was (pets: a grey "Died") */
        boolean pet = (ev->flags & FXF_TARGET_PET) != 0;

        f = fxv_add_floater(ev, 0, pet ? "Died" : "Killed",
                           pet ? FXV_RGB(170, 170, 170) : FXV_RGB(235, 60, 50));
        f->life = FXV_KILL_MS;
        f->low = TRUE;
        f->xoff = 0;
        f->stack = 0;
        return;
    }
    if (ev->type == FXE_MISS) {
        /* "Miss" floats up off whoever dodged */
        (void) fxv_add_floater(ev, 0, "Miss", FXV_RGB(190, 190, 190));
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
        for (i = 0; i < fxv.nfloaters; i++) {
            f = &fxv.floaters[i];
            if (f->target == ev->target_id && f->type == ev->type) {
                f->amount += ev->amount;
                f->x = ev->x, f->y = ev->y;
                f->start = now;
                f->color = fxv_kind_color(ev);
                return;
            }
        }
    }

    f = fxv_add_floater(ev, ev->amount, (const char *) 0,
                       fxv_kind_color(ev));
    f->big = (ev->type == FXE_DAMAGE
              && (ev->hp <= 0 || ev->amount * 4 >= max(ev->hpmax, 1)));
}

/* where a floater is at 'now': its centre in squares, and how bright
   (1 = full, falling to 0 as it fades out); FALSE once it's gone */
boolean
fxv_floater_at(const struct fxv_floater *f, fxv_ms now, float *cx,
               float *cy, float *bright)
{
    fxv_ms age = now - f->start;
    double t, rise, fadefrom;

    if (age >= f->life)
        return FALSE;
    t = (double) age / f->life; /* 0..1 */
    if (f->low) {
        /* "Killed": starts at the health bar, drifts up a little */
        rise = 0.35 * (1.0 - (1.0 - t) * (1.0 - t)) - 0.6;
        fadefrom = 0.6;
    } else if (f->life == FXV_LEVEL_MS) {
        /* level messages rise in the first third, hang above the hero,
           then fade only over the last quarter */
        double tr = min(1.0, t / 0.33);

        rise = 1.3 * (1.0 - (1.0 - tr) * (1.0 - tr));
        fadefrom = 0.75;
    } else {
        /* rise fast, then ease out */
        rise = 0.9 * (1.0 - (1.0 - t) * (1.0 - t));
        fadefrom = 0.66;
    }
    *cx = (float) f->x + 0.5f + (float) f->xoff / 100.0f;
    *cy = (float) f->y + 1.0f / 3.0f - (float) rise - (float) f->stack * 0.45f;
    *bright = (t > fadefrom)
                  ? (float) (1.0 - (t - fadefrom) / (1.0 - fadefrom)) : 1.0f;
    return TRUE;
}

/* what a floater says: its text, or its number ("-5" when the hero is
   hurt, "+12" for a heal) */
void
fxv_floater_text(const struct fxv_floater *f, char *buf, size_t bufsz)
{
    if (f->text[0])
        Snprintf(buf, bufsz, "%s", f->text);
    else
        Snprintf(buf, bufsz, "%s%d",
                 f->type == FXE_HEAL ? "+" : f->target == 0 ? "-" : "",
                 f->amount);
}

/* ---- motion ------------------------------------------------------- */

static fxv_ms fxv_step_pace; /* a step following quickly on the last */

static struct fxv_anim *
fxv_new_anim(void)
{
    if (fxv.nanims >= FXV_MAXANIM) {
        memmove(&fxv.anims[0], &fxv.anims[1],
                (FXV_MAXANIM - 1) * sizeof fxv.anims[0]);
        fxv.nanims--;
    }
    return &fxv.anims[fxv.nanims++];
}

/* melee attacks lunge the attacker toward its target; damage shakes the
   victim. Only in tiles mode, never while running, and only for a
   creature actually drawn on that square. */
void
fxv_motion_event(const struct fx_event *ev)
{
    fxv_ms now = fxv_now();
    struct fxv_anim *a;
    int dx, dy, i;

    if (!iflags.fx_motion || (ev->flags & FXF_BULK)
        || !fxv_tiles_shown() || Is_rogue_level(&u.uz))
        return;

    if (ev->type == FXE_ATTACK && !(ev->flags & FXF_RANGED)) {
        dx = ev->x - ev->sx, dy = ev->y - ev->sy;
        if ((!dx && !dy) || dx < -1 || dx > 1 || dy < -1 || dy > 1)
            return; /* not adjacent */
        if (!isok(ev->sx, ev->sy)
            || !glyph_is_monster(fxv_glyph_at(ev->sx, ev->sy)->glyph))
            return; /* attacker not visibly there */
        a = fxv_new_anim();
        a->kind = FXM_LUNGE;
        a->x = ev->sx, a->y = ev->sy;
        a->dx = dx, a->dy = dy;
        a->tile = fxv_glyph_at(ev->sx, ev->sy)->gm.tileidx;
        a->hit = (ev->flags & FXF_HIT) != 0;
        a->strength = 0;
        /* several attacks in one turn play one after another */
        a->start = ((long) (fxv.anim_seq - now) > 0) ? fxv.anim_seq : now;
        a->dur = FXV_LUNGE_MS;
        fxv.anim_seq = a->start + FXV_LUNGE_GAP;
        fxv_start_timer();
    } else if (ev->type == FXE_DAMAGE && ev->amount > 0) {
        fxv_ms start = now;
        int pct;

        if (!isok(ev->x, ev->y)
            || !glyph_is_monster(fxv_glyph_at(ev->x, ev->y)->glyph))
            return;
        /* start at the peak of a lunge that just went for this square */
        for (i = fxv.nanims - 1; i >= 0; i--) {
            struct fxv_anim *l = &fxv.anims[i];

            if (l->kind == FXM_LUNGE && l->x + l->dx == ev->x
                && l->y + l->dy == ev->y
                && (long) (l->start + l->dur - now) > 0) {
                start = l->start + (l->dur * 2) / 5;
                break;
            }
        }
        pct = (ev->amount * 100) / max(ev->hpmax, 1);
        a = fxv_new_anim();
        a->kind = FXM_SHAKE;
        a->x = ev->x, a->y = ev->y;
        /* recoil away from the attacker that lunged in */
        if (i >= 0)
            a->dx = fxv.anims[i].dx, a->dy = fxv.anims[i].dy;
        else
            a->dx = a->dy = 0;
        a->tile = fxv_glyph_at(ev->x, ev->y)->gm.tileidx;
        a->hit = TRUE;
        a->strength = max(15, min(100, pct * 3));
        a->start = start;
        a->dur = FXV_SHAKE_MS;
        fxv_start_timer();
    } else if (ev->type == FXE_KNOCKBACK || ev->type == FXE_MOVE) {
        boolean step = (ev->type == FXE_MOVE);

        int lim = step ? 2 : 1;

        dx = ev->x - ev->sx, dy = ev->y - ev->sy;
        if (!isok(ev->x, ev->y) || dx < -lim || dx > lim || dy < -lim
            || dy > lim)
            return;
        /* (a step is reported before the map is redrawn, so whether
           something is drawn there is checked when painting) */
        if (!step && !glyph_is_monster(fxv_glyph_at(ev->x, ev->y)->glyph))
            return;
        /* the previous step's slide is over: this one takes over.  When
           steps come faster than a slide lasts (a held key, quick taps,
           running), the slides keep to that pace instead, or are skipped
           when very quick, so the picture never trails behind the
           creature */
        {
            /* where recent steps ended, and when */
            static struct {
                coordxy x, y;
                fxv_ms at;
            } last[16];
            static int nlast;
            long pace = -1;
            int k;

            for (i = 0; i < fxv.nanims; i++)
                if (fxv.anims[i].kind == FXM_SLIDE
                    && ((fxv.anims[i].x == ev->sx
                         && fxv.anims[i].y == ev->sy)
                        || (fxv.anims[i].x == ev->x
                            && fxv.anims[i].y == ev->y)))
                    fxv.anims[i].dur = 0;
            if (step) {
                for (k = 0; k < nlast; k++)
                    if (last[k].x == ev->sx && last[k].y == ev->sy) {
                        long since = (long) (now - last[k].at);

                        if (since >= 0 && since < FXV_STEP_MS)
                            pace = since;
                        break;
                    }
                if (k == nlast) { /* a walker not seen lately */
                    if (nlast < SIZE(last)) {
                        k = nlast++;
                    } else { /* reuse the slot stepped longest ago */
                        for (k = 0, i = 1; i < nlast; i++)
                            if ((long) (last[i].at - last[k].at) < 0)
                                k = i;
                    }
                }
                last[k].x = ev->x, last[k].y = ev->y, last[k].at = now;
            }
            if (step && pace >= 0) {
                /* afterimages of earlier steps go too */
                for (i = 0; i < fxv.nanims; i++)
                    if (fxv.anims[i].kind == FXM_GHOST
                        && fxv.anims[i].x == ev->sx
                        && fxv.anims[i].y == ev->sy)
                        fxv.anims[i].dur = 0;
                if (pace < 35)
                    return; /* too quick to show: just be there */
                fxv_step_pace = (fxv_ms) pace;
            }
        }
        /* shakes and recoils of the knocked creature are left behind */
        for (i = 0; i < fxv.nanims; i++)
            if (fxv.anims[i].kind == FXM_SHAKE
                && fxv.anims[i].x == ev->sx && fxv.anims[i].y == ev->sy)
                fxv.anims[i].dur = 0;
        a = fxv_new_anim();
        a->kind = FXM_SLIDE;
        a->x = ev->x, a->y = ev->y;
        a->dx = dx, a->dy = dy;
        a->tile = fxv_glyph_at(ev->x, ev->y)->gm.tileidx; /* refreshed in paint */
        a->hit = !step;
        a->strength = 0;
        a->start = now;
        a->dur = step ? (max(abs(dx), abs(dy)) > 1 ? FXV_STEP_MS + 40
                                                   : FXV_STEP_MS)
                      : FXV_SLIDE_MS;
        if (step && fxv_step_pace) /* keeping up with quick steps */
            a->dur = min(a->dur, fxv_step_pace);
        if (step && iflags.fx_afterimage && !fxv_step_pace) {
            int k;
            short tile = a->tile;

            for (k = 1; k <= 1; k++) { /* one afterimage */
                a = fxv_new_anim();
                a->kind = FXM_GHOST;
                a->x = ev->x, a->y = ev->y;
                a->dx = dx, a->dy = dy;
                a->tile = tile;
                a->hit = FALSE;
                a->strength = k; /* 1 = nearest the creature */
                a->start = now;
                a->dur = FXV_GHOST_MS;
            }
        }
        fxv_step_pace = 0;
        fxv_start_timer();
    } else if (ev->type == FXE_DEATH) {
        fxv_ms start = now;

        if (!isok(ev->x, ev->y)
            || !glyph_is_monster(fxv_glyph_at(ev->x, ev->y)->glyph))
            return; /* nothing of it drawn there to fade */
        /* follow the killing blow's shake on this square */
        for (i = fxv.nanims - 1; i >= 0; i--) {
            struct fxv_anim *l = &fxv.anims[i];

            if (l->kind == FXM_SHAKE && l->x == ev->x && l->y == ev->y
                && (long) (l->start + l->dur - now) > 0) {
                start = l->start + l->dur / 2;
                l->dur = l->dur / 2; /* the death shake takes over */
                break;
            }
        }
        a = fxv_new_anim();
        a->kind = FXM_DEATH;
        a->x = ev->x, a->y = ev->y;
        a->dx = a->dy = 0;
        a->tile = fxv_glyph_at(ev->x, ev->y)->gm.tileidx;
        a->hit = TRUE;
        a->strength = 60;
        a->start = start;
        a->dur = FXV_DEATH_MS;
        fxv_start_timer();
    }
}

/* explosions, earthquakes and very big hits on the hero shake the whole
   map view (tiles and ASCII alike); never while running */
void
fxv_shake_event(const struct fx_event *ev)
{
    fxv_ms now = fxv_now();
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
    if (fxv.shake_dur && (long) (fxv.shake_start + fxv.shake_dur - now) > 0
        && fxv.shake_strength >= s)
        return;
    fxv.shake_start = now;
    fxv.shake_strength = s;
    fxv.shake_dur = 280 + 4 * s;
    fxv_start_timer();
}

/* where an animating creature is drawn this frame: offset from its
   square in tiles, and how solid; FALSE if it isn't showing. Slides
   and afterimages take the tile drawn there now (a step is reported
   before the map catches up), so a->tile is refreshed for them. */
boolean
fxv_anim_pose(struct fxv_anim *a, fxv_ms now, float *offx, float *offy,
              int *alpha)
{
    long age = (long) (now - a->start);
    const glyph_info *here = fxv_glyph_at(a->x, a->y);
    double p, e;

    *offx = *offy = 0.0f;
    *alpha = 255;
    if (age < 0 || age >= (long) a->dur)
        return FALSE;
    p = (double) age / a->dur;
    switch (a->kind) {
    case FXM_GHOST: {
        unsigned long au = here->fxauras;
        int nghost = (au & (FXA_VERYFAST | FXA_FAST)) ? 1 : 0;
        double back, lag;

        /* the hero's intrinsic speed is too common to trail every step:
           only extra speed (boots, potion, spell) shows */
        if (a->x == u.ux && a->y == u.uy && !(au & FXA_VERYFAST))
            nghost = 0;
        if (!iflags.fx_afterimage)
            return FALSE; /* turned off since the step */
        if (a->strength > nghost || !glyph_is_monster(here->glyph))
            return FALSE;
        /* where the creature is on its slide right now (the slide runs
           FXV_STEP_MS); a single afterimage hangs back along the way it
           came (further when very fast), then slowly catches up as it
           fades */
        lag = (au & FXA_VERYFAST) ? 0.95 : 0.75;
        back = 1.0 - min(1.0, (double) age / FXV_STEP_MS);
        back = back * back * (3 - 2 * back);
        back = min(1.0, back + lag * (1.0 - p) * (1.0 - p));
        *offx = (float) (-a->dx * back);
        *offy = (float) (-a->dy * back);
        *alpha = (int) (125 * (1.0 - p));
        a->tile = here->gm.tileidx;
        return TRUE;
    }
    case FXM_DEATH: {
        /* a small shake, then fade out over what's left there (its
           corpse, items or bare floor) */
        double fadefrom = 0.35;
        double amp = 0.06 * (p < fadefrom ? 1.0 - p / fadefrom : 0.0);

        *alpha = (p < fadefrom)
                     ? 255
                     : (int) (255 * (1.0 - (p - fadefrom) / (1 - fadefrom)));
        *offx = (float) (amp * sin(p * 2 * 3.14159265358979 * 9.0));
        return TRUE;
    }
    case FXM_LUNGE: {
        /* quick push out, softer ease back */
        double reach = a->hit ? 0.28 : 0.2;

        e = (p < 0.4) ? (p / 0.4) : (1.0 - (p - 0.4) / 0.6);
        e = e * e * (3 - 2 * e); /* smoothstep */
        *offx = (float) (a->dx * reach * e);
        *offy = (float) (a->dy * reach * e);
        return TRUE;
    }
    case FXM_SLIDE:
        /* whatever is drawn there now; nothing to slide if it's gone */
        if (!glyph_is_monster(here->glyph))
            return FALSE;
        a->tile = here->gm.tileidx;
        /* from the square it left, easing in (steps a bit softer) */
        e = 1.0 - p;
        e = a->hit ? e * e : e * e * (3 - 2 * e);
        *offx = (float) (-a->dx * e);
        *offy = (float) (-a->dy * e);
        return TRUE;
    default: { /* FXM_SHAKE: decaying side-to-side jitter */
        double amp = 0.04 + 0.10 * a->strength / 100.0;
        /* plus a push away from the blow, springing back */
        double push = (0.06 + 0.10 * a->strength / 100.0) * (1.0 - p)
                      * (1.0 - p);

        *offx = (float) (amp * sin(p * 2 * 3.14159265358979 * 3.0) * (1.0 - p)
                         + a->dx * push);
        *offy = (float) (amp * 0.3 * sin(p * 2 * 3.14159265358979 * 5.0)
                             * (1.0 - p)
                         + a->dy * push);
        return TRUE;
    }
    }
}

/* where the whole view sits this frame, in tile widths; FALSE when it
   isn't shaking */
boolean
fxv_shake_at(fxv_ms now, float *dx, float *dy)
{
    long age;
    double p, amp, decay;

    *dx = *dy = 0.0f;
    if (!fxv.shake_dur)
        return FALSE;
    age = (long) (now - fxv.shake_start);
    if (age < 0 || age >= (long) fxv.shake_dur) {
        fxv.shake_dur = 0;
        return FALSE;
    }
    p = (double) age / fxv.shake_dur;
    decay = (1.0 - p) * (1.0 - p);
    amp = 0.06 + 0.22 * fxv.shake_strength / 100.0;
    /* two unrelated frequencies per axis so it rattles, not sways */
    *dx = (float) (amp * decay
                   * (0.7 * sin(age * 0.105) + 0.3 * sin(age * 0.297)));
    *dy = (float) (amp * 0.8 * decay
                   * (0.6 * cos(age * 0.083) + 0.4 * sin(age * 0.241)));
    return TRUE;
}

/* how far the hero is drawn off its square while sliding in from the
   last one, in tiles (things that follow the hero move with it) */
void
fxv_hero_slide(fxv_ms now, float *offx, float *offy)
{
    int i;

    *offx = *offy = 0.0f;
    for (i = 0; i < fxv.nanims; i++) {
        struct fxv_anim *a = &fxv.anims[i];
        long age = (long) (now - a->start);
        double e;

        if (a->kind != FXM_SLIDE || a->x != u.ux || a->y != u.uy
            || age < 0 || age >= (long) a->dur)
            continue;
        /* the same easing fxv_anim_pose() uses */
        e = 1.0 - (double) age / a->dur;
        e = a->hit ? e * e : e * e * (3 - 2 * e);
        *offx = (float) (-a->dx * e);
        *offy = (float) (-a->dy * e);
        return;
    }
}

/* is the creature on <x,y> in the middle of an animation (other than
   leaving an afterimage)? */
boolean
fxv_square_busy(int x, int y, fxv_ms now)
{
    int i;

    for (i = 0; i < fxv.nanims; i++) {
        struct fxv_anim *a = &fxv.anims[i];
        long age = (long) (now - a->start);

        if (a->x == x && a->y == y && a->kind != FXM_GHOST && age >= 0
            && age < (long) a->dur)
            return TRUE;
    }
    return FALSE;
}

/* stop all motion at once; TRUE if anything was moving */
boolean
fxv_snap(void)
{
    if (!fxv.nanims && !fxv.shake_dur)
        return FALSE;
    fxv.nanims = 0;
    fxv.anim_seq = 0;
    fxv.shake_dur = 0;
    return TRUE;
}

/* ---- lighting ----------------------------------------------------- */

/* A shade layer over the map, made from how lit each square looks
   (fx_light_map() in the core) and smoothed between squares. Rebuilt
   only when the light changes. */
boolean
fxv_light_layer(fxv_ms now, const uint32_t **pixels, boolean *changed,
                boolean *flicker)
{
    static unsigned char bright[COLNO][ROWNO], warm[COLNO][ROWNO];
    static unsigned char pbright[COLNO][ROWNO], pwarm[COLNO][ROWNO];
    static unsigned char rock[COLNO][ROWNO], prock[COLNO][ROWNO];
    static uint32_t bits[FXV_LIGHT_W * FXV_LIGHT_H];
    static boolean built = FALSE;
    const int S = FXV_LIGHT_S, W = FXV_LIGHT_W, H = FXV_LIGHT_H;
    int x, y;

    *pixels = (const uint32_t *) 0;
    *changed = *flicker = FALSE;
    if (!iflags.fx_lighting || !fxv_tiles_shown() || u.uswallow
        || !program_state.in_moveloop)
        return FALSE;
    *flicker = fx_light_map(bright, warm, (double) now / 1000.0) != 0;
    /* squares never seen take their neighbours' shade, so the dark of
       the unknown doesn't make a halo round what you remember */
    for (x = 0; x < COLNO; x++)
        for (y = 0; y < ROWNO; y++) {
            int g = fxv_glyph_at(x, y)->glyph;

            rock[x][y] = 0;
            if ((g == NO_GLYPH || g == GLYPH_UNEXPLORED) && !cansee(x, y))
                bright[x][y] = fx_shade_level(FALSE), warm[x][y] = 0;
            /* solid rock is only backdrop: keep it well down so the
               rooms and corridors stand out */
            else if (glyph_is_cmap(g) && glyph_to_cmap(g) == S_stone)
                bright[x][y] = fx_shade_level(TRUE), warm[x][y] = 0,
                rock[x][y] = 1;
        }
    *pixels = bits;
    if (built && !memcmp(bright, pbright, sizeof bright)
        && !memcmp(warm, pwarm, sizeof warm)
        && !memcmp(rock, prock, sizeof rock))
        return TRUE;
    memcpy(pbright, bright, sizeof bright);
    memcpy(pwarm, warm, sizeof warm);
    memcpy(prock, rock, sizeof rock);
    built = TRUE;
    *changed = TRUE;
    {
        int px, py;

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
                int A, R, G, B;
                int cx = max(1, min(COLNO - 1, px / S)),
                    cy = max(0, min(ROWNO - 1, py / S));
                double bs[4], ws[4];
                int sx[4], sy[4], k;

                fx = fx * fx * (3 - 2 * fx);
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
                bits[py * W + px] = ((uint32_t) A << 24)
                                    | ((uint32_t) R << 16)
                                    | ((uint32_t) G << 8) | (uint32_t) B;
            }
        }
    }
    return TRUE;
}

/* ---- auras -------------------------------------------------------- */

static fxv_color
fxv_dim(fxv_color c, double k)
{
    if (k < 0)
        k = 0;
    if (k > 1)
        k = 1;
    return FXV_RGB((int) (FXV_R(c) * k), (int) (FXV_G(c) * k),
               (int) (FXV_B(c) * k));
}

/* a phase 0..1 that loops every 'period' ms, offset per creature so a
   room of them doesn't move in step */
static double
fxv_phase(fxv_ms now, int x, int y, int salt, fxv_ms period)
{
    fxv_ms beat = (fxv_ms) (x * 389 + y * 157 + salt * 613);

    return (double) ((now + beat) % period) / period;
}

/* lasting status effects on creatures, for as long as the core says so
   (the square's fxauras), and drifting gas clouds: drawn as light
   through the front end's glow primitives (fxview.h); returns how many
   squares had one. ox, oy is where map square 0,0 is drawn, tw x th the
   size of a square, all in the front end's pixels. */
int
fxv_paint_auras(const struct fxv_glow *glow, fxv_ms now, int ox, int oy,
                int tw, int th)
{
#define FXV_Z_PERIOD 2100   /* ms for one z to rise and fade */
#define FXV_Z_COUNT 3       /* z's in the air at once */
#define TAU 6.2831853
    int x, y, n = 0, i;

    if (!iflags.fx_auras)
        return 0;
    for (x = 1; x < COLNO; x++)
        for (y = 0; y < ROWNO; y++) {
            unsigned long au = fxv_glyph_at(x, y)->fxauras & FXV_AURA_MASK;
            int glyph = fxv_glyph_at(x, y)->glyph;
            /* gas clouds (stinking cloud, breath, a hezrou's trail) and
               plain clouds drift where they're drawn */
            int gas = (glyph == cmap_to_glyph(S_poisoncloud)) ? 1
                      : (glyph == cmap_to_glyph(S_cloud)) ? 2 : 0;
            int box_l, box_t, box_r, box_b;
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
            box_l = bx - tw / 2, box_t = by - th;
            box_r = bx + tw + tw / 2, box_b = by + th + th / 2;
            if (box_r < 0 || box_b < 0
                || (glow->layer_w && box_l > glow->layer_w)
                || (glow->layer_h && box_t > glow->layer_h))
                continue;
            if (glow->begin)
                (*glow->begin)(glow->ctx, box_l, box_t, box_r, box_b);

            if (gas) {
                /* billowing gas: big soft puffs rolling round the square
                   and spilling past its edges, each swelling and thinning
                   on its own beat, so a cloud seems to churn */
                fxv_color gasc = (gas == 1) ? FXV_RGB(130, 215, 60)
                                         : FXV_RGB(160, 165, 175);
                fxv_color hc = (gas == 1) ? FXV_RGB(200, 255, 120)
                                         : FXV_RGB(215, 220, 228);

                for (i = 0; i < 5; i++) {
                    double t = fmod(fxv_phase(now, x, y, 29 + i * 5, 2300)
                                    + i / 5.0, 1.0);
                    double k = sin(t * 3.14159);
                    /* each puff circles the middle, slowly, its own way */
                    double a = (x * 1.7 + y * 2.3 + i * 1.26)
                               + (now % 9000) / 9000.0 * 6.2831853
                                     * ((i & 1) ? 1 : -1);
                    int px = cx + (int) (tw * 0.30 * cos(a));
                    int py = cy + (int) (th * (0.26 * sin(a) - 0.12 * t));
                    int rr = (int) (tw * (0.22 + 0.2 * t));

                    glow->dot(glow->ctx, px, py, rr, fxv_dim(gasc, k * 0.38));
                    glow->dot(glow->ctx, px, py, (rr * 2) / 3,
                           fxv_dim(gasc, k * 0.55));
                    glow->dot(glow->ctx, px - rr / 5, py - rr / 5,
                           max(1, rr / 3), fxv_dim(hc, k * 0.6));
                }
            }
            if (au & FXA_ASLEEP) {
                /* z's rising, drifting, growing and fading */
                for (i = 0; i < FXV_Z_COUNT; i++) {
                    double t = fxv_phase(now, x, y,
                                        i * 7, FXV_Z_PERIOD);
                    double fade;
                    int zx, zy, c;
                    const char *z;

                    t = fmod(t + (double) i / FXV_Z_COUNT, 1.0);
                    fade = (t < 0.15) ? t / 0.15 : 1.0 - (t - 0.15) / 0.85;
                    zx = bx + (int) (tw * (0.55 + 0.35 * t
                                           + 0.06 * sin(t * TAU * 1.5)));
                    zy = by + (int) (th * (0.30 - 0.95 * t));
                    c = (int) (255 * fade * fade);
                    z = (t < 0.45) ? "z" : "Z";
                    glow->text(glow->ctx, zx, zy - th / 3, z,
                            FXV_RGB((c * 190) / 255, (c * 215) / 255, c));
                }
            }
            if (au & (FXA_STUNNED | FXA_CONFUSED | FXA_HALLU)) {
                /* things circling the head: stars when stunned, swirling
                   motes when confused, every colour when hallucinating */
                boolean stun = (au & FXA_STUNNED) != 0;
                boolean hallu = (au & FXA_HALLU) != 0;
                int k = stun ? 3 : 4;
                double spin = fxv_phase(now, x, y, 3, stun ? 1100 : 1700);

                for (i = 0; i < k; i++) {
                    double a = (spin + (double) i / k) * TAU;
                    double depth = 0.55 + 0.45 * sin(a); /* front brighter */
                    int px = hx + (int) (tw * 0.38 * cos(a));
                    int py = hy - th / 8 + (int) (th * 0.13 * sin(a));
                    fxv_color c;

                    if (hallu)
                        c = fxv_lerp_color(FXV_RGB(255, 80, 80), FXV_RGB(80, 160, 255),
                                          (float) fmod(spin * 3 + i * 0.3, 1.0));
                    else if (stun)
                        c = FXV_RGB(255, 230, 90);
                    else
                        c = FXV_RGB(220, 140, 255);
                    if (stun) {
                        /* a glowing four-point star */
                        int sr = max(4, tw / 6), j;

                        glow->dot(glow->ctx, px, py, sr,
                               fxv_dim(c, depth * 0.25));
                        for (j = 1; j <= sr; j++) {
                            int ar = max(1, (sr - j) / 3);

                            glow->dot(glow->ctx, px - j, py, ar,
                                   fxv_dim(c, depth));
                            glow->dot(glow->ctx, px + j, py, ar,
                                   fxv_dim(c, depth));
                            glow->dot(glow->ctx, px, py - j, ar,
                                   fxv_dim(c, depth));
                            glow->dot(glow->ctx, px, py + j, ar,
                                   fxv_dim(c, depth));
                        }
                        glow->dot(glow->ctx, px, py, max(2, sr / 3),
                               fxv_dim(FXV_RGB(255, 255, 230), depth));
                    } else {
                        glow->dot(glow->ctx, px, py, r1 * 2 + 1,
                               fxv_dim(c, depth * 0.3));
                        glow->dot(glow->ctx, px, py, r1, fxv_dim(c, depth));
                    }
                }
            }
            if (au & FXA_SLOW) {
                /* heavy blue motes sinking slowly at the feet */
                /* (drops sinking down the body, each with a soft glow,
                   like something weighing it down) */
                for (i = 0; i < 4; i++) {
                    double t = fmod(fxv_phase(now, x, y, 11, 2600)
                                    + i / 4.0, 1.0);
                    double k = sin(t * 3.14159);
                    int px = bx + (int) (tw * (0.18 + 0.21 * i));
                    int py = by + (int) (th * (0.15 + 0.85 * t));
                    int rr = max(3, tw / 8);

                    glow->dot(glow->ctx, px, py, rr * 2,
                           fxv_dim(FXV_RGB(70, 110, 255), k * 0.3));
                    glow->dot(glow->ctx, px, py, rr,
                           fxv_dim(FXV_RGB(110, 150, 255), k));
                }
            }
            if (au & FXA_LEVITATE) {
                /* soft air stirring under the feet */
                for (i = 0; i < 4; i++) {
                    double t = fmod(fxv_phase(now, x, y, 5, 1500)
                                    + i / 4.0, 1.0);
                    int side = (i & 1) ? 1 : -1;
                    int px = cx + side * (int) (tw * (0.08 + 0.32 * t));
                    int py = by + th - th / 10 + (int) (th * 0.15 * t);

                    glow->dot(glow->ctx, px, py, r1,
                           fxv_dim(FXV_RGB(190, 225, 255), (1.0 - t) * 0.7));
                }
            }
            if (au & FXA_REFLECT) {
                /* every few seconds a glint sweeps across */
                double t = fxv_phase(now, x, y, 2, 2600);

                if (t < 0.14) {
                    double g = t / 0.14;
                    int glx = bx + (int) (tw * (0.1 + 0.8 * g));
                    int gly = by + (int) (th * (0.85 - 0.7 * g));

                    for (i = -2; i <= 2; i++)
                        glow->dot(glow->ctx, glx + i * tw / 14,
                               gly - i * th / 14, max(1, r1 - abs(i) / 2),
                               fxv_dim(FXV_RGB(255, 255, 255),
                                      (1.0 - abs(i) * 0.3)
                                          * sin(g * 3.14159)));
                }
            }
            if ((au & FXA_CONFLICT) && x == u.ux && y == u.uy) {
                /* the hero radiating Conflict: dark red rings pulsing
                   outward, one after another */
                for (i = 0; i < 2; i++) {
                    double t = fmod(fxv_phase(now, x, y, 21, 1800)
                                    + i / 2.0, 1.0);
                    double r = 0.3 + 0.55 * t, k = sin(t * 3.14159) * 0.75;
                    int j;

                    for (j = 0; j < 20; j++) {
                        double a = j / 20.0 * TAU;

                        glow->dot(glow->ctx, cx + (int) (tw * r * cos(a)),
                               cy + (int) (th * r * sin(a)), r1,
                               fxv_dim(FXV_RGB(200, 30, 40), k));
                    }
                }
            } else if (au & FXA_CONFLICT) {
                /* a creature in its grip: a throbbing red glow, and red
                   embers flickering up off it like rage */
                double beat = fxv_phase(now, x, y, 23, 700);
                double k = 0.25 + 0.2 * sin(beat * TAU);

                glow->dot(glow->ctx, cx, cy + th / 6, tw / 2,
                       fxv_dim(FXV_RGB(170, 20, 25), k));
                for (i = 0; i < 5; i++) {
                    double t = fmod(fxv_phase(now, x, y, 27 + i, 1100)
                                    + i / 5.0, 1.0);
                    int px = bx + (int) (tw * (0.15 + 0.17 * i
                                               + 0.05 * sin(t * TAU * 2)));
                    int py = by + (int) (th * (0.85 - 0.9 * t));

                    glow->dot(glow->ctx, px, py, r1,
                           fxv_dim(FXV_RGB(255, 70, 50), sin(t * 3.14159)));
                }
            }
            if (au & FXA_PROTECT) {
                /* a slow golden ring of light around it, breathing */
                double t = fxv_phase(now, x, y, 9, 3000);
                double br = 0.35 + 0.25 * sin(t * TAU);

                for (i = 0; i < 16; i++) {
                    double a = (i / 16.0 + t * 0.25) * TAU;

                    glow->dot(glow->ctx,
                           cx + (int) (tw * 0.52 * cos(a)),
                           cy + (int) (th * 0.52 * sin(a)), r1,
                           fxv_dim(FXV_RGB(255, 210, 110), br));
                }
            }
            if (au & FXA_STONING) {
                /* grey grit creeping inward */
                for (i = 0; i < 5; i++) {
                    double t = fmod(fxv_phase(now, x, y, 13, 1900)
                                    + i / 5.0, 1.0);
                    double a = (i * 0.21 + 0.1) * TAU;
                    double r = 0.6 * (1.0 - t);

                    glow->dot(glow->ctx, cx + (int) (tw * r * cos(a)),
                           cy + (int) (th * r * sin(a)), r1,
                           fxv_dim(FXV_RGB(200, 200, 190), t * 0.8));
                }
            }
            if (au & FXA_SLIMED) {
                /* green slime dripping off */
                for (i = 0; i < 3; i++) {
                    double t = fmod(fxv_phase(now, x, y, 17, 1600)
                                    + i / 3.0, 1.0);
                    int px = bx + (int) (tw * (0.22 + 0.28 * i));
                    int py = by + (int) (th * (0.45 + 0.6 * t * t));

                    glow->dot(glow->ctx, px, py, r1 + (t > 0.6),
                           fxv_dim(FXV_RGB(120, 230, 60), 1.0 - t * 0.7));
                }
            }
            if (au & FXA_SICK) {
                /* sickly bubbles rising and popping */
                for (i = 0; i < 3; i++) {
                    double t = fmod(fxv_phase(now, x, y, 19, 2000)
                                    + i / 3.0, 1.0);
                    int px = bx + (int) (tw * (0.3 + 0.2 * i)
                                         + tw * 0.05 * sin(t * TAU * 2));
                    int py = by + (int) (th * (0.85 - 0.8 * t));

                    glow->dot(glow->ctx, px, py, r1 + (int) (t * 1.5),
                           fxv_dim(FXV_RGB(190, 220, 70), sin(t * 3.14159) * 0.8));
                }
            }
            if (au & FXA_INVIS) {
                /* seen though invisible (it is also drawn see-through):
                   just two faint specks drifting slowly down it */
                for (i = 0; i < 2; i++) {
                    double t = fmod(fxv_phase(now, x, y, 23, 3400)
                                    + i / 2.0, 1.0);
                    int px = bx + (int) (tw * (0.32 + 0.36 * i));
                    int py = by + (int) (th * (0.2 + 0.7 * t));

                    glow->dot(glow->ctx, px, py, max(1, r1 - 1),
                           fxv_dim(FXV_RGB(150, 190, 230),
                                  sin(t * 3.14159) * 0.4));
                }
            }
            if (glow->end)
                (*glow->end)(glow->ctx, box_l, box_t, box_r, box_b);
        }
    return n;
#undef TAU
}

/* ---- events ------------------------------------------------------- */

/* everything the shared effects do with one fx event: motion, the
   view's shake, particles and floating text. A front end's fx handler
   calls this (after anything of its own). */
void
fxv_fx_event(const struct fx_event *ev)
{
    fxv_ms now = fxv_now();

    fxv_motion_event(ev); /* lunges and shakes */
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
        fxv_start_timer();
    }
    if ((ev->type == FXE_DETECT || ev->type == FXE_UNIHORN)
        && !(ev->flags & FXF_UNSEEN))
        fxv_detect_particles(ev);
    if (ev->type == FXE_IMPACT && !(ev->flags & FXF_BULK))
        fxv_impact_particles(ev->x, ev->y,
                            (ev->flags & FXF_RESISTED) != 0);
    fxv_floater_event(ev); /* numbers, status names, Killed, Level N */
}

/* a glyph the front end has just put on the map: digging and rays
   leave particles where they're drawn */
void
fxv_glyph_shown(int x, int y, const glyph_info *shown)
{
    if (shown->glyph == cmap_to_glyph(S_digbeam))
        fxv_dig_particles(x, y);
    if (glyph_is_cmap_zap(shown->glyph))
        fxv_ray_particles(x, y, (shown->glyph - GLYPH_ZAP_OFF) >> 2,
                          (shown->glyph - GLYPH_ZAP_OFF) & 3);
}

/* ---- the hero frame ----------------------------------------------- */

/* the corner arms round the hero's square (see fxview.h) */
boolean
fxv_hero_frame(int x, int y, int tw, int th, int g, int r[8][4])
{
    int len, t, j;
    /* top-left, top-right, bottom-left, bottom-right: across, then down;
       each as offsets from the square's own corners */
    int arms[8][4];

    if (!iflags.fx_hero_frame || u.uswallow || !isok(u.ux, u.uy))
        return FALSE;
    if (iflags.fx_hero_frame_light)
        len = max(3, min(tw, th) / 5), t = 1, g = 0;
    else
        len = max(3, min(tw, th) / 4), t = max(1, min(tw, th) / 16);
    arms[0][0] = x, arms[0][1] = y, arms[0][2] = x + len,
        arms[0][3] = y + t;
    arms[1][0] = x, arms[1][1] = y, arms[1][2] = x + t,
        arms[1][3] = y + len;
    arms[2][0] = x + tw - len, arms[2][1] = y, arms[2][2] = x + tw,
        arms[2][3] = y + t;
    arms[3][0] = x + tw - t, arms[3][1] = y, arms[3][2] = x + tw,
        arms[3][3] = y + len;
    arms[4][0] = x, arms[4][1] = y + th - t, arms[4][2] = x + len,
        arms[4][3] = y + th;
    arms[5][0] = x, arms[5][1] = y + th - len, arms[5][2] = x + t,
        arms[5][3] = y + th;
    arms[6][0] = x + tw - len, arms[6][1] = y + th - t, arms[6][2] = x + tw,
        arms[6][3] = y + th;
    arms[7][0] = x + tw - t, arms[7][1] = y + th - len, arms[7][2] = x + tw,
        arms[7][3] = y + th;
    if (iflags.fx_hero_frame_light) {
        /* the light version's down arms start below the across ones, so
           the corners aren't drawn twice at half opacity */
        arms[1][1] += t, arms[3][1] += t;
        arms[5][3] -= t, arms[7][3] -= t;
    }
    for (j = 0; j < 8; j++) {
        r[j][0] = arms[j][0] - g, r[j][1] = arms[j][1] - g;
        r[j][2] = arms[j][2] + g, r[j][3] = arms[j][3] + g;
    }
    return TRUE;
}

/* ---- missiles pointing where they fly ----------------------------- */

/* Measure a missile's picture (fx_turn_missiles): which way it points
   and where its middle is, so it can be turned to its heading. px is
   the tile, tw x th pixels as 0x..RRGGBB (any alpha byte is ignored),
   with the key colour (71,108,108) see-through. For a tile set that
   draws everything on a floor (builtin FALSE), floor is that tile's
   floor, and what matches it, and a frame round the edge, are left
   out. mask (tw*th bytes) gets 1 where the picture is. FALSE if the
   picture isn't long and thin (round things aren't turned). */
boolean
fxv_missile_shape(const uint32_t *px, const uint32_t *floor, int tw, int th,
                  boolean builtin, unsigned char *mask,
                  struct fxv_missile *m)
{
    int x, y, n = 0, margin;
    double sx = 0, sy = 0, cxx = 0, cyy = 0, cxy = 0, cx, cy, a, l1, l2,
           tr, det, ux, uy, rad = 0;

    if (tw < 4 || th < 4)
        return FALSE;
    margin = builtin ? 0 : tw / 12;
    for (y = 0; y < th; y++)
        for (x = 0; x < tw; x++) {
            uint32_t p = px[y * tw + x];
            int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            boolean on = !(r == 71 && g == 108 && b == 108);

            if (on && !builtin) {
                if (x < margin || y < margin || x >= tw - margin
                    || y >= th - margin) {
                    on = FALSE;
                } else if (floor) {
                    uint32_t q = floor[y * tw + x];
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
                    uint32_t p = px[y * tw + x];
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
            a += 3.14159265358979;
    }
    /* how far it reaches from its middle, for shrinking it to fit */
    for (y = 0; y < th; y++)
        for (x = 0; x < tw; x++)
            if (mask[y * tw + x])
                rad = max(rad, sqrt((x + 0.5 - cx) * (x + 0.5 - cx)
                                    + (y + 0.5 - cy) * (y + 0.5 - cy)));
    m->points = a, m->cx = cx, m->cy = cy, m->reach = rad;
    m->tw = tw, m->th = th;
    return TRUE;
}

/* how to turn a measured missile to head dx,dy: *rot radians (clockwise
   on screen, y down) and *scale (shrunk so it stays in its square);
   FALSE if it already points that way */
boolean
fxv_missile_turn(const struct fxv_missile *m, int dx, int dy, double *rot,
                 double *scale)
{
    double r = atan2((double) dy, (double) dx) - m->points;

    r = fmod(r + 3 * 3.14159265358979, 2 * 3.14159265358979)
        - 3.14159265358979;
    if (fabs(r) < 0.05)
        return FALSE;
    *rot = r;
    *scale = min(1.0, (min(m->tw, m->th) * 0.5) / max(m->reach, 1.0));
    return TRUE;
}

/* ---- health bars -------------------------------------------------- */

/* the thin bar along the bottom of a hurt creature's square, for a
   square at x,y of tw x th: the whole bar (black) and its filled part,
   as left, top, right, bottom, and the fill's colour; FALSE if none
   (fx_healthbars off, or the creature is unhurt or not reported) */
boolean
fxv_health_bar(int pct, int x, int y, int tw, int th, int bar[4],
               int fill[4], fxv_color *c)
{
    int h = max(2, th / 10), w = tw - 4;

    if (!iflags.fx_healthbars || pct <= 0 || pct >= 100 || w < 4)
        return FALSE;
    bar[0] = x + 2, bar[1] = y + th - h - 1;
    bar[2] = x + tw - 2, bar[3] = y + th - 1;
    fill[0] = bar[0], fill[1] = bar[1], fill[3] = bar[3];
    fill[2] = bar[0] + max(1, (w * pct) / 100);
    *c = pct > 50 ? FXV_RGB(60, 220, 60)
         : pct > 25 ? FXV_RGB(240, 200, 40) : FXV_RGB(230, 50, 40);
    return TRUE;
}
