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
    FXE_LEVEL       /* hero's experience level is now 'amount'; FXF_HIT
                     * set if it went up, clear if drained */
};

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

/* glyph_info.fxhealth: 0 = no bar, else 1..100 percent of max HP */

/* fx.c */
extern void fx_set_handler(fx_handler_fn);
extern void fx_set_audio_handler(fx_handler_fn);
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
extern void fx_note_kind(struct monst *, int);
extern void fx_mon_dying(struct monst *);
extern void fx_mon_died(struct monst *);
extern void fx_hear(void);
extern void fx_swap(struct monst *, coordxy, coordxy);
extern void fx_bump(struct monst *, coordxy, coordxy);
extern void fx_pickup(int, int);
extern void fx_trap(struct monst *, int, coordxy, coordxy);
extern void fx_eat_texture(int);
extern void fx_eat(int);
extern void fx_glyphinfo(coordxy, coordxy, int, glyph_info *);

#endif /* FX_H */
