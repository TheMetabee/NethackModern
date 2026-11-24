/* NetHack Modern fx.h */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * Presentation event stream ("fx").
 *
 * The game core reports things worth animating (damage, heals, attacks,
 * misses, deaths) and persistent per-creature status ("auras", health)
 * to whichever window port wants them.  Everything here is pure output:
 * it never changes game state, never touches the main RNG and is never
 * saved.  A port that registers no handler gets nothing and pays almost
 * nothing.  See docs/modern/ROADMAP.md.
 */

#ifndef FX_H
#define FX_H

/* the NetHack Modern release, shown on the title screen and in About */
#define MODERN_VERSION_STRING "0.3.1"

/* one-shot events */
enum fx_event_type {
    FXE_DAMAGE = 1, /* target lost 'amount' HP */
    FXE_HEAL,       /* target gained 'amount' HP */
    FXE_ATTACK,     /* source attacked target (hit or miss) */
    FXE_MISS,       /* source's attack on target missed */
    FXE_DEATH,      /* target died (monsters only) */
    FXE_HEAR,       /* a "You hear ..." message (x,y = hero) */
    FXE_SWAP,       /* hero swapped places with target (x,y = target's
                     * new square, sx,sy = hero's new square) */
    FXE_BUMP,       /* hero was stopped by friendly target at x,y */
    FXE_PICKUP,     /* hero picked up an item of class 'objclass' */
    FXE_TRAP,       /* target set off a trap of type 'ttyp' at x,y */
    FXE_EAT,        /* hero starts eating food 'otyp' ('texture' for
                     * corpses) */
    FXE_LAUNCH,     /* source fires/throws missile 'wep_otyp' (from
                     * launcher 'launcher_otyp', if any) */
    FXE_WEPFX,      /* a weapon's special effect of kind 'adtyp' landed on
                     * the target (not resisted): Fire Brand burning, etc */
    FXE_APPLY,      /* hero applies tool 'otyp'; amount = 1 if it was lit */
    FXE_READ,       /* hero reads scroll 'otyp'; amount = 1 if confused */
    FXE_APPEAR,     /* a notable monster ('mndx') just came into view */
    FXE_STATUS,     /* target gained ('st_gain') and/or lost ('st_lose')
                     * FXA_* statuses */
    FXE_LEVEL,      /* hero's experience level is now 'amount'; FXF_HIT
                     * set if it went up, clear if drained */
    FXE_MESSAGE,    /* a message is being shown: 'text' (valid only during
                     * the handler call) */
    FXE_EXP,        /* hero gained 'amount' experience points */
    FXE_SHAKE,      /* shake the view; 'amount' = strength 1..100 */
    FXE_EXPLODE,    /* explosion at x,y: 'adtyp' element, 'amount' damage;
                     * FXF_UNSEEN if only heard */
    FXE_KNOCKBACK,  /* a creature was hurtled one step, sx,sy -> x,y */
    FXE_MOVE,       /* a seen creature moved sx,sy -> x,y (1-2 squares) */
    FXE_IMPACT,     /* a force (striking, force bolt) hit x,y;
                     * FXF_RESISTED if it bounced off */
    FXE_WAND,       /* a wand/spell effect: 'otyp' is the look (0 = an
                     * unidentified wand, shown plainly), 'adtyp' one of
                     * FXW_*, 'amount' the beam step (for timing) */
    FXE_TELEPORT,   /* a creature teleported sx,sy -> x,y; 'amount' bit 0:
                     * departure seen, bit 1: arrival seen */
    FXE_QUAFF,      /* the hero drinks a potion: 'adtyp' 0 as it's drunk,
                     * 1 once its effect is known; 'otyp' the look (0 =
                     * unidentified), 'amount' the potion's colour,
                     * 'mndx' 1 blessed / -1 cursed / 0 */
    FXE_UNIHORN,    /* unicorn horn applied: 'amount' 1 cured something,
                     * 0 nothing happened, -1 cursed horn */
    FXE_ARRIVE,     /* the hero is on a different level (or the game
                     * just started or was restored) */
    FXE_UI,         /* an interface action: 'text' names it ("inventory");
                     * valid only during the handler call */
    FXE_DETECT,     /* detection revealed things: 'adtyp' FXD_*; with
                     * FXF_HERO_TARGET once at the hero ('amount' = how
                     * many), otherwise one per square found ('amount' =
                     * its distance, for a ripple outward) */
    FXE_SUMMON,     /* a summoned or created monster appeared in sight at
                     * <x,y>: 'adtyp' FXS_*, 'amount' its place in the
                     * group (0 first), for a staggered entrance */
    FXE_ENGRAVE,    /* engraving at the hero's square: 'adtyp' FXG_*;
                     * for FXG_STROKE 'amount' is the engraving type (DUST,
                     * ENGRAVE, BURN, ...) and 'otyp' 1 for the first
                     * action; 'ttyp' the surface (FXG_SURF_*); FXF_UNSEEN
                     * if the hero can't see it (sound only) */
    FXE_CAST,       /* the hero casts spell 'otyp': 'adtyp' 0 as it's
                     * cast ('amount' its school, P_*_SPELL), 1 once it
                     * has taken effect (sx,sy where the hero cast it);
                     * FXF_UNSEEN if the hero is blind (sound only) */
    FXE_SEARCH      /* the hero searches by command ('s'; never the
                     * automatic kind); FXF_BULK for the turns after the
                     * first of a counted search, FXF_UNSEEN if blind */
};

/* fx_event.adtyp for FXE_ENGRAVE. Wands that give the same message give
   the same kind, so neither sound nor look tells them apart. */
#define FXG_STROKE    0  /* one action of writing (each turn) */
#define FXG_VANISH    1  /* "The engraving ... vanishes!" */
#define FXG_ICE       2  /* "A few ice cubes drop from the wand." */
#define FXG_BUGS_STOP 3  /* "The bugs ... stop moving!" */
#define FXG_GRAVEL    4  /* digging: gravel, ice chips, splinters */
#define FXG_FLAMES    5  /* "Flames fly from the wand." */
#define FXG_LIGHTNING 6  /* "Lightning arcs from the wand." */
#define FXG_BULLETS   7  /* "... riddled by bullet holes!" */
#define FXG_POLY      8  /* "The engraving now reads: ..." */
#define FXG_SLOW      9  /* "The bugs ... slow down!" */
#define FXG_SPEED     10 /* "The bugs ... speed up!" */
#define FXG_STRUGGLE  11 /* striking: "The wand unsuccessfully fights ..." */
#define FXG_WORN      12 /* "The wand is too worn out to engrave." */
#define FXG_CRUMBLE   13 /* a cancelled wand "turns to dust" */
#define FXG_WIPE      14 /* the hero wipes out a message (towel, rewriting) */

/* fx_event.ttyp for FXE_ENGRAVE: what's being written on */
#define FXG_SURF_FLOOR 0
#define FXG_SURF_ICE   1
#define FXG_SURF_WOOD  2 /* a lowered drawbridge */
#define FXG_SURF_GRAVE 3 /* a headstone */

/* fx_event.adtyp for FXE_SUMMON: what kind of summoning */
#define FXS_ARCANE  0 /* create monster: a violet swirl */
#define FXS_DEMONIC 1 /* demons gated in: red fire and smoke */
#define FXS_HOLY    2 /* angels and lawful minions: gold light */
#define FXS_SWARM   3 /* insects or snakes: a dusty green burst */

/* fx_event.texture: how a corpse turned out to taste (see eatcorpse()) */
enum fx_texture {
    FXT_NONE = 0, /* no taste message (or not a corpse) */
    FXT_OKAY,     /* vegetarian corpses: "tastes okay" */
    FXT_STRINGY,
    FXT_GAMEY,
    FXT_FATTY,
    FXT_TOUGH,
    FXT_DELICIOUS,
    FXT_TERRIBLE
};

/* fx_event.flags */
#define FXF_HERO_TARGET 0x0001U /* target is the hero */
#define FXF_HERO_SOURCE 0x0002U /* source is the hero */
#define FXF_HIT         0x0004U /* FXE_ATTACK connected */
#define FXF_BULK        0x0008U /* hero is running/travelling/occupied:
                                 * ports should summarise, not animate */
#define FXF_HALLU       0x0010U /* hero hallucinating; values scrambled */
#define FXF_TARGET_PET  0x0020U /* target is tame */
#define FXF_RANGED      0x0040U /* ATTACK/MISS by a thrown/fired missile */
#define FXF_UNSEEN      0x0080U /* EXPLODE: heard, not seen */
#define FXF_RESISTED    0x0100U /* IMPACT: it bounced off ("Boing!") */
#define FXF_DROP        0x0200U /* PICKUP: it was put down, not picked up
                                 * ('amount' = its weight) */

/* FXE_DETECT kinds */
#define FXD_MONSTER 1
#define FXD_OBJECT  2

/* FXE_WAND kinds */
#define FXW_BEAM 1 /* the beam passing through x,y */
#define FXW_HIT  2 /* the beam struck a creature or object at x,y */
#define FXW_SELF 3 /* a non-directional wand going off around the hero */

struct fx_event {
    int type;            /* enum fx_event_type */
    unsigned flags;      /* FXF_* */
    coordxy x, y;        /* target square */
    coordxy sx, sy;      /* source square (ATTACK/MISS), else same as x,y */
    int amount;          /* DAMAGE/HEAL amount */
    int adtyp;           /* damage kind: AD_* from monattk.h, -1 unknown */
    int hp, hpmax;       /* target HP after the event */
    unsigned target_id;  /* m_id of target, 0 for hero */
    unsigned source_id;  /* m_id of source, 0 for hero or none */
    int objclass;        /* PICKUP: object class (POTION_CLASS etc.) */
    int mndx;            /* DEATH: monster species (mons[] index) */
    int ttyp;            /* TRAP: trap type (ARROW_TRAP etc.) */
    int otyp;            /* EAT: object type being eaten */
    int texture;         /* EAT: enum fx_texture */
    int wep_otyp;        /* ATTACK/MISS/LAUNCH: weapon or missile otyp,
                          * 0 (STRANGE_OBJECT) for none/natural attack */
    int wep_art;         /* ...and its artifact index, 0 if ordinary */
    int launcher_otyp;   /* LAUNCH: bow/crossbow/sling used, else 0 */
    unsigned long st_gain; /* STATUS: FXA_* bits newly set */
    unsigned long st_lose; /* STATUS: FXA_* bits that ended */
    const char *text;    /* MESSAGE: the message */
};

typedef void (*fx_handler_fn)(const struct fx_event *);

/*
 * Persistent status bits, delivered in glyph_info.fxauras for the hero's
 * square and for squares showing a monster the hero can see.
 */
#define FXA_FAST       0x00000001UL /* intrinsic/monster speed */
#define FXA_VERYFAST   0x00000002UL /* extrinsic or potion speed */
#define FXA_SLOW       0x00000004UL
#define FXA_INVIS      0x00000008UL /* invisible but displayed anyway */
#define FXA_LEVITATE   0x00000010UL
#define FXA_FLYING     0x00000020UL
#define FXA_REFLECT    0x00000040UL
#define FXA_PROTECT    0x00000080UL /* protection spell */
#define FXA_CONFUSED   0x00000100UL
#define FXA_STUNNED    0x00000200UL
#define FXA_STONING    0x00000400UL
#define FXA_SLIMED     0x00000800UL
#define FXA_SICK       0x00001000UL
#define FXA_HALLU      0x00002000UL
#define FXA_BLIND      0x00004000UL
#define FXA_FLEEING    0x00008000UL
#define FXA_CANCELLED  0x00010000UL
#define FXA_HELPLESS   0x00020000UL /* paralyzed (hero: any helplessness) */
#define FXA_ASLEEP     0x00040000UL /* monster asleep */
#define FXA_CONFLICT   0x00080000UL /* hero: radiating Conflict; monster:
                                     * within its reach this turn */

/* glyph_info.fxhealth: 0 = no bar, else 1..100 percent of max HP */

/* fx.c */
extern void fx_set_handler(fx_handler_fn);
extern void fx_set_audio_handler(fx_handler_fn);
/* the font names a window port can offer for fx_font_* number 'which'
   (FX_FONT_*; the SDL one sets it, offering only monospace fonts for
   the text): points *names at them, sorted, and returns how many */
typedef int (*fx_font_lister_fn)(int which, const char *const **names);
extern fx_font_lister_fn fx_font_lister;
/* a window port's own window for choosing those fonts (#fonts), if it
   has one; else #fonts uses menus */
extern void (*fx_font_dialog)(void);
extern boolean fx_active(void);
extern void fx_debug_log(const char *);
extern void fx_sync(void);
extern void fx_attack(struct monst *, struct monst *, int, boolean);
extern void fx_attack_wep(struct monst *, struct monst *, int, boolean,
                          struct obj *, boolean);
extern void fx_launch(struct monst *, struct obj *, struct obj *);
extern void fx_weapon_effect(struct monst *, int);
extern void fx_apply(struct obj *);
extern void fx_read_scroll(int, boolean);
extern void fx_wizard_laugh(struct monst *);
extern void fx_level_change(int, boolean);
extern void fx_message(const char *);
extern void fx_exp_gain(int);
extern void fx_screen_shake(int);
extern void fx_explosion(coordxy, coordxy, int, int, boolean);
extern void fx_knockback(coordxy, coordxy, coordxy, coordxy);
extern void fx_impact(coordxy, coordxy, boolean);
extern void fx_wand(int, coordxy, coordxy, int, int);
extern void fx_wand_mon(int, struct monst *);
extern void fx_wand_self(int);
extern void fx_mon_slept(struct monst *);
extern void fx_teleport(coordxy, coordxy, coordxy, coordxy, boolean,
                        boolean, boolean);
extern void fx_quaff(struct obj *, int);
extern void fx_scroll_effect(int);
extern void fx_unihorn(int);
extern void fx_engrave(int, int, boolean);
extern void fx_detected(int);
extern void fx_search(void);
extern void fx_cast(int, int);
extern void fx_ui(const char *);
extern unsigned fx_summon_begin(void);
extern void fx_summon_end(unsigned, int);
extern void fx_flight_begin(int);
extern void fx_flight_hint(int, int);
extern void fx_flight_step(coordxy, coordxy);
extern void fx_flight_end(void);
extern boolean fx_flight_at(coordxy, coordxy, int *, int *);
extern void fx_note_kind(struct monst *, int);
extern void fx_mon_dying(struct monst *);
extern void fx_mon_died(struct monst *);
extern void fx_hear(void);
extern void fx_swap(struct monst *, coordxy, coordxy);
extern void fx_bump(struct monst *, coordxy, coordxy);
extern void fx_pickup(int, int);
extern void fx_drop(struct obj *);
extern void fx_trap(struct monst *, int, coordxy, coordxy);
extern void fx_eat_texture(int);
extern void fx_eat(int);
extern void fx_glyphinfo(coordxy, coordxy, int, glyph_info *);

/* NetHack Modern: how lit each square looks (for the window port) */
int fx_shade_level(boolean rock);
int fx_light_map(unsigned char bright[COLNO][ROWNO],
                 unsigned char warm[COLNO][ROWNO], double t);

#endif /* FX_H */
