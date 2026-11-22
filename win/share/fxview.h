/* NetHack Modern fxview.h */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * The effects every tiles front end shows (NetHackW now, SDL next),
 * kept apart from how any one of them draws. So far: particles (sparks,
 * rays, bursts, swirls) and floating text (damage numbers, status
 * names), made from the fx event stream (fx.h) and moving by
 * themselves. A front end draws fxv.parts[] and fxv.floaters[] where
 * fxv_part_at() and fxv_floater_at() say, keeps a frame timer running
 * while there are any, and calls fxv_expire() on each frame.
 *
 * Positions are in map squares (a square's centre is +0.5), times in
 * milliseconds, colours 0x00BBGGRR (the same layout as a Win32
 * COLORREF, so NetHackW can hand them straight to GDI).
 *
 * Include after hack.h.
 */

#ifndef FXVIEW_H
#define FXVIEW_H

typedef unsigned long fxv_ms;
typedef unsigned long fxv_color;

#define FXV_RGB(r, g, b) \
    ((fxv_color) (((unsigned long) (unsigned char) (r))              \
                  | ((unsigned long) (unsigned char) (g) << 8)       \
                  | ((unsigned long) (unsigned char) (b) << 16)))
#define FXV_R(c) ((int) ((c) & 0xff))
#define FXV_G(c) ((int) (((c) >> 8) & 0xff))
#define FXV_B(c) ((int) (((c) >> 16) & 0xff))

#define FXV_MAXPART 1000 /* particles alive at once */

struct fxv_part {
    float x, y;             /* position, squares (centre = +0.5) */
    float vx, vy;           /* squares per second */
    float grav;             /* added to vy per second (< 0 rises) */
    float drag;             /* fraction of speed kept per second */
    float size;             /* radius, fraction of a tile */
    fxv_color c0, c1;       /* colour when born, when it dies */
    fxv_ms start, life;
};

/* floating text: numbers, "Miss", "Killed", status names, "Level 5!" */
#define FXV_MAXFLOAT 64
#define FXV_FLOAT_MS 900     /* lifetime of one number */
#define FXV_TEXT_MS 1400     /* lifetime of a status name */
#define FXV_LEVEL_MS 2600    /* lifetime of "Level N!" */
#define FXV_KILL_MS 1100     /* lifetime of "Killed" */
#define FXV_STACK_MS 400     /* a new number on a square this young stacks */

struct fxv_floater {
    int x, y;               /* map square */
    unsigned target;        /* m_id, 0 = hero */
    int type;               /* FXE_DAMAGE, FXE_HEAL or FXE_STATUS */
    int amount;
    char text[24];          /* status name; empty for numbers */
    fxv_ms life;            /* how long it floats, ms */
    int stack;              /* raised this many slots to avoid overlap */
    int xoff;               /* sideways jitter, percent of a tile */
    boolean low;            /* starts at the health bar, rises less */
    boolean big;            /* large hit: drawn bigger */
    fxv_color color;
    fxv_ms start;           /* when (re)started */
};

struct fxv_state {
    struct fxv_part parts[FXV_MAXPART];
    int nparts;
    struct fxv_floater floaters[FXV_MAXFLOAT];
    int nfloaters;
};
extern struct fxv_state fxv;

/* what the front end provides */
struct fxv_hooks {
    fxv_ms (*now)(void);        /* a millisecond clock */
    void (*start_timer)(void);  /* something is moving: keep repainting */
};
void fxv_init(const struct fxv_hooks *);
fxv_ms fxv_now(void);
void fxv_start_timer(void);

/* drop the particles and floaters that are done */
void fxv_expire(fxv_ms now);

/* floating text: made from fx events; drawn by the front end at
   fxv_floater_at() (centre, squares; brightness 0..1), saying
   fxv_floater_text(), big if ->big, smaller if it's text */
void fxv_floater_event(const struct fx_event *);
boolean fxv_floater_at(const struct fxv_floater *, fxv_ms now, float *cx,
                       float *cy, float *bright);
void fxv_floater_text(const struct fxv_floater *, char *buf, size_t bufsz);

/* the particles' life */
/* where a particle is at 'now', in squares, and how far through its
   life (0..1; outside that range it isn't showing) */
void fxv_part_at(const struct fxv_part *, fxv_ms now, float *x, float *y,
                 float *k);
fxv_color fxv_lerp_color(fxv_color, fxv_color, float);

/* spawned from fx events (and from the map's own zap and dig glyphs) */
struct fx_event;
extern fxv_ms fxv_chain_until; /* chain lightning is being cast */
void fxv_ray_particles(int x, int y, int zt, int dir);
void fxv_chain_sparks(float fx, float fy, int n, fxv_ms delay);
void fxv_burst_particles(int x, int y, int kind, int dam);
void fxv_impact_particles(int x, int y, boolean resisted);
void fxv_dig_particles(int x, int y);
void fxv_wand_particles(const struct fx_event *);
void fxv_teleport_particles(const struct fx_event *);
void fxv_engrave_particles(const struct fx_event *);
void fxv_summon_particles(const struct fx_event *);
void fxv_quaff_particles(const struct fx_event *);
void fxv_detect_particles(const struct fx_event *);
void fxv_scroll_particles(const struct fx_event *);
void fxv_cast_particles(const struct fx_event *);

#endif /* FXVIEW_H */
