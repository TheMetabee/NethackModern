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
    FXE_DEATH       /* target died (monsters only) */
};

/* fx_event.flags */
#define FXF_HERO_TARGET 0x0001U /* target is the hero */
#define FXF_HERO_SOURCE 0x0002U /* source is the hero */
#define FXF_HIT         0x0004U /* FXE_ATTACK connected */
#define FXF_BULK        0x0008U /* hero is running/travelling/occupied:
                                 * ports should summarise, not animate */
#define FXF_HALLU       0x0010U /* hero hallucinating; values scrambled */
#define FXF_TARGET_PET  0x0020U /* target is tame */

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
#define FXA_HELPLESS   0x00020000UL /* asleep or paralyzed */

/* glyph_info.fxhealth: 0 = no bar, else 1..100 percent of max HP */

/* fx.c */
extern void fx_set_handler(fx_handler_fn);
extern boolean fx_active(void);
extern void fx_sync(void);
extern void fx_attack(struct monst *, struct monst *, int, boolean);
extern void fx_note_kind(struct monst *, int);
extern void fx_mon_dying(struct monst *);
extern void fx_mon_died(struct monst *);
extern void fx_glyphinfo(coordxy, coordxy, int, glyph_info *);

#endif /* FX_H */
