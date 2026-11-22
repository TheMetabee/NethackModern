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

/* drop the particles and floaters whose life is over */
void
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
        fxv_color c = nhcolor_to_RGB(ev->amount);

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
