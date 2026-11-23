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

/* motion: creatures drawn off their square for a moment (the map state
   itself never moves), and the whole view shaking */
#define FXV_MAXANIM 96
#define FXM_LUNGE 1         /* a melee attacker pushes toward its target */
#define FXM_SHAKE 2         /* a creature that's hit shakes */
#define FXM_DEATH 3         /* shake, then fade out */
#define FXM_SLIDE 4         /* a step or knockback: glide in from before */
#define FXM_GHOST 5         /* speed afterimage left behind by a step */
#define FXV_GHOST_MS 460
#define FXV_INVIS_ALPHA 110 /* how solid an invisible-but-seen creature is */
#define FXV_SLIDE_MS 70     /* one hurtled step */
#define FXV_STEP_MS 110     /* an ordinary step */
#define FXV_DEATH_MS 360    /* shake ~35%, then fade out */
#define FXV_LUNGE_MS 150    /* out ~40%, back ~60% */
#define FXV_LUNGE_GAP 110   /* successive lunges in one turn queue up */
#define FXV_SHAKE_MS 220

struct fxv_anim {
    int kind;               /* FXM_* */
    int x, y;               /* square whose creature moves */
    int dx, dy;             /* direction */
    short tile;             /* creature tile when the event happened */
    boolean hit;            /* lunge connected; slide was a knockback */
    int strength;           /* shake: 1..100; ghost: which one */
    fxv_ms start, dur;
};

struct fxv_state {
    struct fxv_part parts[FXV_MAXPART];
    int nparts;
    struct fxv_floater floaters[FXV_MAXFLOAT];
    int nfloaters;
    struct fxv_anim anims[FXV_MAXANIM];
    int nanims;
    fxv_ms anim_seq;        /* when the queued lunges finish */
    fxv_ms shake_start, shake_dur; /* screen shake (0 dur = none) */
    int shake_strength;     /* 1..100 */
};
extern struct fxv_state fxv;

/* what the front end provides */
struct fxv_hooks {
    fxv_ms (*now)(void);        /* a millisecond clock */
    void (*start_timer)(void);  /* something is moving: keep repainting */
    /* what the front end has drawn on a map square (its own copy of
       the map, as print_glyph() gave it) */
    const glyph_info *(*glyph_at)(int x, int y);
    boolean (*tiles_shown)(void); /* tiles, not ASCII (motion needs them) */
    fxv_color (*nh_color)(int);   /* a NetHack colour (CLR_*) as RGB */
};
void fxv_init(const struct fxv_hooks *);
fxv_ms fxv_now(void);
void fxv_start_timer(void);
const glyph_info *fxv_glyph_at(int x, int y);
boolean fxv_tiles_shown(void);
fxv_color fxv_nh_color(int);

/* auras: lasting effects on creatures (sleeping z's, stars round a
   stunned head, a slowed creature's drag, the hero's Conflict, and the
   rest of FXV_AURA_MASK) and drifting gas clouds. The shared code says
   what to draw; the front end draws it through these, as light added
   onto the map (a dot or letter of colour c brightens what's under it
   by c; black adds nothing). begin and end, if set, bracket each
   square's effects with the box they stay inside (NetHackW clears and
   then ORs a box of its black glow layer); a square whose box starts
   past layer_w x layer_h (when set) is skipped. */
#define FXV_AURA_MASK (FXA_ASLEEP | FXA_CONFUSED | FXA_STUNNED | FXA_SLOW \
                       | FXA_LEVITATE | FXA_REFLECT | FXA_PROTECT          \
                       | FXA_STONING | FXA_SLIMED | FXA_SICK | FXA_HALLU   \
                       | FXA_INVIS | FXA_CONFLICT)
struct fxv_glow {
    void *ctx;
    int layer_w, layer_h;
    void (*begin)(void *ctx, int l, int t, int r, int b);
    void (*dot)(void *ctx, int x, int y, int r, fxv_color c);
    void (*text)(void *ctx, int x, int y, const char *s, fxv_color c);
    void (*end)(void *ctx, int l, int t, int r, int b);
};
int fxv_paint_auras(const struct fxv_glow *, fxv_ms now, int ox, int oy,
                    int tw, int th);

/* the hero frame (fx_hero_frame): small corner marks round the hero's
   square, gold over a dark outline, or thinner and half see-through
   (fx_hero_frame_light). fxv_hero_frame() fills r[8] with the corner
   arms as left, top, right, bottom for a square at x,y of tw x th,
   grown by g pixels on every side (the outline is the arms grown by 1);
   it returns FALSE when the frame isn't shown. Colours: */
#define FXV_FRAME_GOLD FXV_RGB(229, 186, 96)
#define FXV_FRAME_EDGE FXV_RGB(10, 10, 14)
#define FXV_FRAME_LIGHT FXV_RGB(255, 226, 150) /* drawn at 150/255 */
boolean fxv_hero_frame(int x, int y, int tw, int th, int g, int r[8][4]);

/* missiles pointing where they fly (fx_turn_missiles, and fx_flight_at()
   in the core for the heading): fxv_missile_shape() measures a tile's
   picture once; fxv_missile_turn() says how to turn it for a heading.
   The front end turns the picture about (cx, cy) by rot and scales it
   by scale, so that middle lands on the middle of the square. */
struct fxv_missile {
    double points;     /* the way the picture points, radians, y down */
    double cx, cy;     /* its middle, in the tile's pixels */
    double reach;      /* how far it reaches from there */
    int tw, th;        /* the tile's size */
};
boolean fxv_missile_shape(const uint32_t *px, const uint32_t *floor, int tw,
                          int th, boolean builtin, unsigned char *mask,
                          struct fxv_missile *);
boolean fxv_missile_turn(const struct fxv_missile *, int dx, int dy,
                         double *rot, double *scale);

/* health bars (fx_healthbars): see fxv_health_bar() */
boolean fxv_health_bar(int pct, int x, int y, int tw, int th, int bar[4],
                       int fill[4], fxv_color *);

/* the status panel's game logic (the drawing is each front end's) */
struct fxv_chip {
    const char *name;
    fxv_color c;
};
int fxv_status_chips(struct fxv_chip *, int max);
fxv_color fxv_hp_color(int pct);
void fxv_stat_arrows(int dir[6]);
fxv_color fxv_bar_row(fxv_color c, double k, int row, int rows);
fxv_color fxv_bar_stripe(fxv_color c);
/* the twelve equipment slots, and a slot's blessed/cursed edge */
struct obj;
void fxv_equip_slots(struct obj *slot[12]);
fxv_color fxv_equip_edge(struct obj *);
/* the context actions (fxv_actions(); 'moves' adds stairs) */
struct fxv_action {
    const char *label, *hint;
    int key;
};
int fxv_actions(struct fxv_action *, int max, boolean moves);

/* lighting: a shade layer over the map, FXV_LIGHT_S pixels per square,
   smoothed between squares so light fades off instead of stopping at
   tile edges. fxv_light_layer() builds it and points *pixels at it:
   FXV_LIGHT_W x FXV_LIGHT_H, row by row, premultiplied 0xAARRGGBB (a
   Win32 32-bit DIB and SDL's ARGB8888 alike); *changed says whether it
   differs from last time. It returns FALSE when lighting isn't shown
   (then *pixels is null), and sets *flicker while a light flickers
   (keep repainting). Column 0 is never part of the level. */
#define FXV_LIGHT_S 8
#define FXV_LIGHT_W (COLNO * FXV_LIGHT_S)
#define FXV_LIGHT_H (ROWNO * FXV_LIGHT_S)
boolean fxv_light_layer(fxv_ms now, const uint32_t **pixels,
                        boolean *changed, boolean *flicker);

/* motion, made from fx events. fxv_anim_pose() says where to draw an
   animating creature this frame, as an offset from its square in tiles
   (x in tile widths, y in tile heights), and how solid (0..255); FALSE
   if it isn't showing. fxv_shake_at() is the whole view's offset, in
   tile widths. fxv_snap() stops it all at once (TRUE if anything was
   moving). */
void fxv_motion_event(const struct fx_event *);
void fxv_shake_event(const struct fx_event *);
boolean fxv_anim_pose(struct fxv_anim *, fxv_ms now, float *offx,
                      float *offy, int *alpha);
boolean fxv_shake_at(fxv_ms now, float *dx, float *dy);
void fxv_hero_slide(fxv_ms now, float *offx, float *offy);
boolean fxv_square_busy(int x, int y, fxv_ms now);
boolean fxv_snap(void);

/* a front end's fx handler (fx_set_handler()) passes each event here;
   its print_glyph passes each glyph it puts on the map to
   fxv_glyph_shown() (rays and digging leave particles) */
void fxv_fx_event(const struct fx_event *);
void fxv_glyph_shown(int x, int y, const glyph_info *);

/* drop the particles, floaters and motion that are done; TRUE while
   anything is left (keep the frame timer going) */
boolean fxv_expire(fxv_ms now);

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
void fxv_search_particles(const struct fx_event *);
void fxv_scroll_particles(const struct fx_event *);
void fxv_cast_particles(const struct fx_event *);

/* messages on the map (fx_msg_overlay): newest at the bottom of the
   block, at most FXV_MSG_SHOW lines, older turns fading and dropping
   away after FXV_MSG_KEEP turns */
#define FXV_MSG_SHOW 6
#define FXV_MSG_KEEP 6
#define FXV_MSG_SHADE FXV_RGB(4, 5, 8)  /* outline, and shade behind */
boolean fxv_msg_highlight(const char *, fxv_color *);
double fxv_msg_fade(long age, boolean recent);
boolean fxv_msg_place(int *col);
int fxv_msg_shade(int dx, int dy, int fade);

/* the full-window screens (title, character, death, scores): their
   sizes scale with the window, 1.0 for one 720 pixels high */
#define FXV_TITLE_PLAY 0
#define FXV_TITLE_ABOUT 1
#define FXV_TITLE_EXIT 2
#define FXV_TITLE_NBTN 3
/* every rounded box (panels, bars, chips, buttons, the screens' panels)
   has the same small corners: this radius in pixels, not scaled; for
   GDI's RoundRect, which takes the corner's diameter, FXV_CORNER_GDI */
#define FXV_CORNER 4
#define FXV_CORNER_GDI (2 * FXV_CORNER - 1)
double fxv_scr_scale(int w, int h);
void fxv_title_buttons(int w, int h, int r[FXV_TITLE_NBTN][4]);
void fxv_title_sound_rect(int w, double s, int r[4]);
int fxv_title_shade(int y, int h);

/* the name and character screens: the game's rules on which choices go
   together, the words they show, and where things go */
#define FXV_CS_ROLE 0
#define FXV_CS_RACE 1
#define FXV_CS_GEND 2
#define FXV_CS_ALIGN 3
#define FXV_CS_NGRP 4
#define FXV_CS_MAXROWS 32
#define FXV_CSB_BACK 0   /* the buttons along the bottom */
#define FXV_CSB_RANDOM 1 /* (the character screen only) */
#define FXV_CSB_PLAY 2
#define FXV_CSB_N 3
#define FXV_CSI_STARTS 0 /* the info panel's lines, for fxv_cs_info() */
#define FXV_CSI_BLURB 1
#define FXV_CSI_RACES 2
#define FXV_CSI_ALIGNS 3
#define FXV_CSI_GENDER 4
#define FXV_CSI_GODS 5
struct fxv_cs_layout {
    double s;              /* the scale (1.0 for 740 pixels high) */
    int top;               /* where the heading goes */
    int panel[FXV_CS_NGRP][4], row[FXV_CS_NGRP][FXV_CS_MAXROWS][4];
    int info[4], btn[FXV_CSB_N][4], box[4], snd[4]; /* l, t, r, b */
};
extern int fxv_cs_n[FXV_CS_NGRP];
void fxv_cs_count(void);
void fxv_cs_begin(void);
boolean fxv_cs_randomize(void);
int fxv_cs_value(int g);
boolean fxv_cs_ok(int g, int i);
void fxv_cs_fix(void);
boolean fxv_cs_pick(int g, int i);
int fxv_cs_step(int g, int from, int dir);
int fxv_cs_letter(int g, int c);
const char *fxv_cs_role_name(int r);
const char *fxv_cs_label(int g, int i, char *buf);
void fxv_cs_why_not(int g, int i, char *buf);
void fxv_cs_hero_line(char *buf);
void fxv_cs_info(int role, int what, char *buf);
double fxv_cs_scale(int w, int h);
void fxv_cs_layout(struct fxv_cs_layout *, int w, int h, boolean name);

/* the saved heroes screen: this player's saved games as cards, to go
   on with one, start a new hero, or delete one (fxv_saves_*) */
#define FXV_SV_MAX 500 /* more than anyone keeps; past it, not listed */
#define FXV_SVB_BACK 0 /* the buttons along the bottom */
#define FXV_SVB_DELETE 1
#define FXV_SVB_NEW 2
#define FXV_SVB_PLAY 3
#define FXV_SVB_N 4
#define FXV_SVC_CANCEL 0 /* the delete confirmation's buttons */
#define FXV_SVC_DELETE 1
struct fxv_save {
    char tag[PL_NSIZ_PLUS]; /* "name-Val-Hum-Fem-Law", for plname[] */
    char name[PL_NSIZ];
    char who[BUFSZ];        /* "Lawful female human Valkyrie" */
    char mode;              /* '-' normal, 'X' explore, 'D' wizard */
    int tile;               /* its portrait: the tile it has on the map,
                               the role's creature or, with showrace,
                               the race's; -1 if not known */
};
struct fxv_sv_layout {
    double s;
    int top;                    /* where the heading goes */
    int cols, rows;             /* cards across, and rows shown */
    int first;                  /* the first card shown */
    int card[FXV_SV_MAX][4];    /* l, t, r, b; all zero if not shown */
    int btn[FXV_SVB_N][4], snd[4];
    int confirm[4], cbtn[2][4]; /* the "Delete ...?" panel */
    int track[4], thumb[4];     /* the scroll bar, when not all fit;
                                   all zero when they do */
};
extern struct fxv_save fxv_sv[FXV_SV_MAX];
extern int fxv_sv_n, fxv_sv_total; /* listed; found (more if past MAX) */
void fxv_saves_more(char *buf);
int fxv_saves_load(void);
void fxv_saves_choose(int i);
void fxv_saves_delete(int i);
const char *fxv_saves_mode_name(char mode);
void fxv_saves_layout(struct fxv_sv_layout *, int w, int h, int sel);

/* the death screen and the score list: what they show, gathered from
   the game as it ends and from the lines topten() writes */
#define FXV_END_LASTN 4
struct fxv_end {
    char title[BUFSZ], cause[BUFSZ], stone[4][BUFSZ];
    int nstone;
    char last[FXV_END_LASTN][BUFSZ]; /* the last turn's messages */
    int nlast;
    struct {
        const char *label;
        char value[32];
    } stat[6];
};
extern struct fxv_end fxv_end;
void fxv_end_snapshot(const char *(*line)(int back, long *turn));
void fxv_end_gather(const char *rip, const char *summary);
#define FXV_SL_MAXLINES 400
#define FXV_SL_MAXENTRIES 120
struct fxv_sl_entry {
    int rank;           /* 0: the hero's, when not on the list */
    long points;
    char name[PL_NSIZ];
    char who[BUFSZ];    /* "chaotic female human Wizard" */
    char fate[BUFSZ];   /* "Killed by a jackal" */
    char place[BUFSZ];  /* "Died in The Dungeons of Doom on level 2" */
    char hp[16];
    int maxhp;
    boolean hero;
};
extern struct fxv_sl_entry fxv_sl_ent[FXV_SL_MAXENTRIES];
extern int fxv_sl_nent;
extern char fxv_sl_note[BUFSZ];
void fxv_scores_add(int attr, const char *text);
void fxv_scores_clear(void);
int fxv_scores_parse(void);

#endif /* FXVIEW_H */
