/* NetHack Modern fx.c */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * Presentation event stream.  See include/fx.h and docs/modern/ROADMAP.md.
 *
 * Damage and healing are found by comparing each creature's HP with the
 * value seen at the previous sync, rather than by hooking every place in
 * the core that changes HP.  fx_sync() runs from flush_screen(), so a
 * number appears together with the message or screen update it belongs
 * to.  Explicit hooks supply what a diff can't know: who attacked whom,
 * misses, deaths, and the damage kind (fire, cold, ...).
 *
 * Nothing here alters game state, uses the main RNG, or is saved.
 *
 * Setting the environment variable NETHACK_FXLOG to a file name logs
 * every event and status change, with or without a window port handler.
 */

#include "hack.h"

#define FX_MAXTRACK 400

struct fx_track {
    unsigned id;          /* m_id */
    int hp, hpmax;
    short mndx;           /* species, to spot polymorph */
    int pendkind;         /* AD_* to attach to the next damage, or -1 */
    long pendmoves;       /* turn pendkind was noted */
    unsigned long auras;  /* last FXA_* mask sent to the display */
    uchar health;         /* last fxhealth sent to the display */
    boolean alive;        /* mark for mark-and-sweep in fx_sync() */
    boolean inview;       /* hero could see it at the last sync */
};

static struct {
    fx_handler_fn handler;       /* display (window port) */
    fx_handler_fn audio_handler; /* sound (soundlib) */
    FILE *logf;
    boolean log_checked;
    boolean busy;         /* re-entrancy guard */
    /* level the table belongs to */
    d_level lev;
    boolean have_lev;
    /* the hero */
    boolean hero_init, hero_poly;
    int hero_hp, hero_hpmax, hero_pendkind;
    long hero_pendmoves;
    unsigned long hero_auras;
    uchar hero_health;
    /* monsters on the current level */
    struct fx_track tab[FX_MAXTRACK];
    int ntab;
} fx;

staticfn void fx_log_open(void);
staticfn void fx_emit(struct fx_event *);
staticfn struct fx_track *fx_find(struct monst *, boolean);
staticfn void fx_reset_level(void);
staticfn unsigned long fx_hero_auras(void);
staticfn unsigned long fx_mon_auras(struct monst *);
staticfn uchar fx_health(int, int);
staticfn unsigned fx_bulk_flag(void);
staticfn void fx_hpchange(struct monst *, int, int, int *);
staticfn const char *fx_typename(int);
staticfn void fx_appear(struct monst *);

void
fx_set_handler(fx_handler_fn fn)
{
    fx.handler = fn;
}

void
fx_set_audio_handler(fx_handler_fn fn)
{
    fx.audio_handler = fn;
}

staticfn void
fx_log_open(void)
{
    const char *path;

    if (fx.log_checked)
        return;
    fx.log_checked = TRUE;
    path = nh_getenv("NETHACK_FXLOG");
    if (path && *path) {
        fx.logf = fopen(path, "a");
        if (fx.logf)
            fprintf(fx.logf, "=== fx log start ===\n");
    }
}

/* free-form diagnostic line in the NETHACK_FXLOG file, if logging */
void
fx_debug_log(const char *msg)
{
    fx_log_open();
    if (fx.logf && msg) {
        fprintf(fx.logf, "T%ld NOTE   %s\n", svm.moves, msg);
        fflush(fx.logf);
    }
}

boolean
fx_active(void)
{
    fx_log_open();
    return (fx.handler || fx.audio_handler || fx.logf) ? TRUE : FALSE;
}

staticfn const char *
fx_typename(int type)
{
    switch (type) {
    case FXE_DAMAGE:
        return "DAMAGE";
    case FXE_HEAL:
        return "HEAL";
    case FXE_ATTACK:
        return "ATTACK";
    case FXE_MISS:
        return "MISS";
    case FXE_DEATH:
        return "DEATH";
    case FXE_HEAR:
        return "HEAR";
    case FXE_SWAP:
        return "SWAP";
    case FXE_BUMP:
        return "BUMP";
    case FXE_PICKUP:
        return "PICKUP";
    case FXE_TRAP:
        return "TRAP";
    case FXE_EAT:
        return "EAT";
    case FXE_LAUNCH:
        return "LAUNCH";
    case FXE_WEPFX:
        return "WEPFX";
    case FXE_APPLY:
        return "APPLY";
    case FXE_READ:
        return "READ";
    case FXE_APPEAR:
        return "APPEAR";
    }
    return "?";
}

staticfn void
fx_emit(struct fx_event *ev)
{
    if (Hallucination) {
        ev->flags |= FXF_HALLU;
        if (ev->type == FXE_DAMAGE || ev->type == FXE_HEAL)
            ev->amount = 1 + rn2_on_display_rng(2 * ev->amount + 10);
        ev->adtyp = rn2_on_display_rng(AD_ACID + 1);
    }
    if (fx.logf) {
        fprintf(fx.logf,
                "T%ld %-6s tgt=%s%u@%d,%d src=%s%u@%d,%d amt=%d ad=%d "
                "hp=%d/%d flags=0x%x ttyp=%d otyp=%d tex=%d mndx=%d "
                "oclass=%d wep=%d art=%d launcher=%d\n",
                svm.moves, fx_typename(ev->type),
                (ev->flags & FXF_HERO_TARGET) ? "hero" : "m", ev->target_id,
                ev->x, ev->y,
                (ev->flags & FXF_HERO_SOURCE) ? "hero" : "m", ev->source_id,
                ev->sx, ev->sy, ev->amount, ev->adtyp, ev->hp, ev->hpmax,
                ev->flags, ev->ttyp, ev->otyp, ev->texture, ev->mndx,
                ev->objclass, ev->wep_otyp, ev->wep_art, ev->launcher_otyp);
        fflush(fx.logf);
    }
    if (fx.audio_handler)
        (*fx.audio_handler)(ev);
    if (fx.handler)
        (*fx.handler)(ev);
}

staticfn unsigned
fx_bulk_flag(void)
{
    return (gm.multi > 0 || go.occupation || svc.context.run
            || svc.context.travel) ? FXF_BULK : 0U;
}

staticfn void
fx_reset_level(void)
{
    fx.ntab = 0;
    fx.lev = u.uz;
    fx.have_lev = TRUE;
}

/* find (or optionally start tracking) a monster */
staticfn struct fx_track *
fx_find(struct monst *mtmp, boolean create)
{
    int i;
    struct fx_track *t;

    for (i = 0; i < fx.ntab; i++)
        if (fx.tab[i].id == mtmp->m_id)
            return &fx.tab[i];
    if (!create || fx.ntab >= FX_MAXTRACK)
        return (struct fx_track *) 0;
    t = &fx.tab[fx.ntab++];
    t->id = mtmp->m_id;
    t->hp = mtmp->mhp;
    t->hpmax = mtmp->mhpmax;
    t->mndx = monsndx(mtmp->data);
    t->pendkind = -1;
    t->pendmoves = 0L;
    t->auras = 0UL;
    t->health = 0;
    t->alive = TRUE;
    return t;
}

staticfn uchar
fx_health(int hp, int hpmax)
{
    int pct;

    if (hpmax <= 0)
        return 0;
    pct = (hp * 100) / hpmax;
    if (pct < 1)
        pct = 1;
    if (pct > 100)
        pct = 100;
    return (uchar) pct;
}

staticfn unsigned long
fx_hero_auras(void)
{
    unsigned long a = 0UL;

    if (Very_fast)
        a |= FXA_VERYFAST;
    else if (Fast)
        a |= FXA_FAST;
    if (Invis)
        a |= FXA_INVIS;
    if (Levitation)
        a |= FXA_LEVITATE;
    else if (Flying)
        a |= FXA_FLYING;
    if (Reflecting)
        a |= FXA_REFLECT;
    if (u.uspellprot)
        a |= FXA_PROTECT;
    if (Confusion)
        a |= FXA_CONFUSED;
    if (Stunned)
        a |= FXA_STUNNED;
    if (Stoned)
        a |= FXA_STONING;
    if (Slimed)
        a |= FXA_SLIMED;
    if (Sick)
        a |= FXA_SICK;
    if (Hallucination)
        a |= FXA_HALLU;
    if (Blind)
        a |= FXA_BLIND;
    if (gm.multi < 0)
        a |= FXA_HELPLESS;
    return a;
}

staticfn unsigned long
fx_mon_auras(struct monst *mtmp)
{
    unsigned long a = 0UL;

    if (mtmp->mspeed == MFAST)
        a |= FXA_FAST;
    else if (mtmp->mspeed == MSLOW)
        a |= FXA_SLOW;
    if (mtmp->minvis)
        a |= FXA_INVIS;
    if (is_flyer(mtmp->data) || is_floater(mtmp->data))
        a |= FXA_FLYING;
    if (mtmp->mconf)
        a |= FXA_CONFUSED;
    if (mtmp->mstun)
        a |= FXA_STUNNED;
    if (mtmp->mflee)
        a |= FXA_FLEEING;
    if (mtmp->mcan)
        a |= FXA_CANCELLED;
    if (!mtmp->mcansee)
        a |= FXA_BLIND;
    if (mtmp->msleeping || !mtmp->mcanmove)
        a |= FXA_HELPLESS;
    return a;
}

/* fill in glyph_info's fx fields; called from map_glyphinfo() */
void
fx_glyphinfo(coordxy x, coordxy y, int glyph, glyph_info *gi)
{
    struct monst *mtmp;

    gi->fxauras = 0UL;
    gi->fxhealth = 0;
    if (!fx_active() || !isok(x, y) || !glyph_is_monster(glyph))
        return;
    if (u_at(x, y)) {
        gi->fxauras = fx_hero_auras();
        gi->fxhealth = Upolyd ? fx_health(u.mh, u.mhmax)
                              : fx_health(u.uhp, u.uhpmax);
    } else if ((mtmp = m_at(x, y)) != 0 && canspotmon(mtmp)) {
        gi->fxauras = fx_mon_auras(mtmp);
        gi->fxhealth = fx_health(mtmp->mhp, mtmp->mhpmax);
    }
}

/* report an HP change; *pendkind is consumed by damage */
staticfn void
fx_hpchange(struct monst *mtmp, int oldhp, int newhp, int *pendkind)
{
    struct fx_event ev;
    boolean hero = (mtmp == &gy.youmonst);

    if (newhp == oldhp)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = (newhp < oldhp) ? FXE_DAMAGE : FXE_HEAL;
    ev.amount = (newhp < oldhp) ? oldhp - newhp : newhp - oldhp;
    ev.adtyp = -1;
    if (ev.type == FXE_DAMAGE) {
        ev.adtyp = *pendkind;
        *pendkind = -1;
    }
    ev.flags = fx_bulk_flag();
    if (hero) {
        ev.flags |= FXF_HERO_TARGET;
        ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
        ev.hp = newhp;
        ev.hpmax = Upolyd ? u.mhmax : u.uhpmax;
        ev.target_id = 0;
    } else {
        if (mtmp->mtame)
            ev.flags |= FXF_TARGET_PET;
        ev.x = ev.sx = mtmp->mx, ev.y = ev.sy = mtmp->my;
        ev.hp = newhp;
        ev.hpmax = mtmp->mhpmax;
        ev.target_id = mtmp->m_id;
    }
    fx_emit(&ev);
}

/* compare current state with what we last saw; called by flush_screen() */
void
fx_sync(void)
{
    struct monst *mtmp;
    struct fx_track *t;
    unsigned long auras;
    uchar health;
    int i, j, cur, max;

    if (fx.busy || !fx_active() || program_state.in_getlev
        || program_state.restoring || !u.uz.dlevel)
        return;
    fx.busy = TRUE;

    if (!fx.have_lev || !on_level(&fx.lev, &u.uz))
        fx_reset_level();
    /* a kind noted for an attack that did no damage expires; it gets one
       extra turn because the HP loss is often first seen after the turn
       counter has advanced */
    if (svm.moves - fx.hero_pendmoves > 1L)
        fx.hero_pendkind = -1;
    for (i = 0; i < fx.ntab; i++)
        if (svm.moves - fx.tab[i].pendmoves > 1L)
            fx.tab[i].pendkind = -1;

    /* the hero */
    cur = Upolyd ? u.mh : u.uhp;
    max = Upolyd ? u.mhmax : u.uhpmax;
    if (!fx.hero_init || fx.hero_poly != (Upolyd ? TRUE : FALSE)) {
        fx.hero_init = TRUE;
        fx.hero_poly = Upolyd ? TRUE : FALSE;
        fx.hero_pendkind = -1;
    } else {
        fx_hpchange(&gy.youmonst, fx.hero_hp, cur, &fx.hero_pendkind);
    }
    fx.hero_hp = cur, fx.hero_hpmax = max;
    auras = fx_hero_auras();
    health = fx_health(cur, max);
    if (auras != fx.hero_auras || health != fx.hero_health) {
        if (fx.logf && auras != fx.hero_auras) {
            fprintf(fx.logf, "T%ld AURA   hero 0x%05lx -> 0x%05lx\n",
                    svm.moves, fx.hero_auras, auras);
            fflush(fx.logf);
        }
        fx.hero_auras = auras, fx.hero_health = health;
        if (isok(u.ux, u.uy))
            newsym(u.ux, u.uy);
    }

    /* monsters on this level */
    for (i = 0; i < fx.ntab; i++)
        fx.tab[i].alive = FALSE;
    for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
        /* a monster at 0 HP may still be here, waiting for mondead();
           report its final damage now since mondead() may come after
           the "You kill it!" message has already flushed the screen */
        if ((t = fx_find(mtmp, FALSE)) == 0) {
            if (DEADMONSTER(mtmp))
                continue;
            /* first sighting: just remember it */
            if ((t = fx_find(mtmp, TRUE)) != 0) {
                t->inview = canseemon(mtmp) ? TRUE : FALSE;
                if (t->inview)
                    fx_appear(mtmp);
            }
            continue;
        }
        t->alive = TRUE;
        if (!DEADMONSTER(mtmp)) {
            boolean inview = canseemon(mtmp) ? TRUE : FALSE;

            if (inview && !t->inview)
                fx_appear(mtmp); /* just came into view */
            t->inview = inview;
        }
        if (t->mndx != monsndx(mtmp->data)) {
            /* polymorphed: HP was re-rolled, not damaged */
            t->mndx = monsndx(mtmp->data);
        } else if (canseemon(mtmp)) {
            fx_hpchange(mtmp, t->hp, mtmp->mhp, &t->pendkind);
        }
        t->hp = mtmp->mhp, t->hpmax = mtmp->mhpmax;
        if (DEADMONSTER(mtmp))
            continue;

        if (canspotmon(mtmp)) {
            auras = fx_mon_auras(mtmp);
            health = fx_health(mtmp->mhp, mtmp->mhpmax);
        } else {
            auras = 0UL, health = 0;
        }
        if (auras != t->auras || health != t->health) {
            if (fx.logf && auras != t->auras) {
                fprintf(fx.logf, "T%ld AURA   m%u@%d,%d 0x%05lx -> 0x%05lx\n",
                        svm.moves, t->id, mtmp->mx, mtmp->my, t->auras,
                        auras);
                fflush(fx.logf);
            }
            t->auras = auras, t->health = health;
            if (isok(mtmp->mx, mtmp->my))
                newsym(mtmp->mx, mtmp->my);
        }
    }
    /* forget monsters that are gone (left the level, died, ...) */
    for (i = j = 0; i < fx.ntab; i++) {
        if (fx.tab[i].alive && fx.tab[i].id != 0)
            fx.tab[j++] = fx.tab[i];
    }
    /* entries created during this pass were marked alive by fx_find() */
    fx.ntab = j;

    fx.busy = FALSE;
}

/* magr attacked mdef (either may be &gy.youmonst) with damage type adtyp */
void
fx_attack(struct monst *magr, struct monst *mdef, int adtyp, boolean hit)
{
    fx_attack_wep(magr, mdef, adtyp, hit, (struct obj *) 0, FALSE);
}

/* magr (hero, a monster, or null if unknown) attacked mdef with weapon or
   missile wep (null for a natural attack); ranged if wep was thrown/fired */
void
fx_attack_wep(
    struct monst *magr, struct monst *mdef, int adtyp, boolean hit,
    struct obj *wep, boolean ranged)
{
    struct fx_event ev;
    boolean hsrc = (magr == &gy.youmonst), htgt = (mdef == &gy.youmonst);

    if (!fx_active() || !mdef)
        return;
    if (!hsrc && !htgt && !canseemon(mdef) && !(magr && canseemon(magr)))
        return;

    if (hit)
        fx_note_kind(mdef, adtyp);

    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_ATTACK;
    ev.flags = fx_bulk_flag() | (hit ? FXF_HIT : 0U)
               | (hsrc ? FXF_HERO_SOURCE : 0U)
               | (htgt ? FXF_HERO_TARGET : 0U)
               | (ranged ? FXF_RANGED : 0U);
    ev.x = htgt ? u.ux : mdef->mx, ev.y = htgt ? u.uy : mdef->my;
    if (hsrc)
        ev.sx = u.ux, ev.sy = u.uy;
    else if (magr)
        ev.sx = magr->mx, ev.sy = magr->my;
    else
        ev.sx = ev.x, ev.sy = ev.y;
    ev.source_id = (hsrc || !magr) ? 0 : magr->m_id;
    ev.target_id = htgt ? 0 : mdef->m_id;
    ev.adtyp = adtyp;
    if (wep) {
        ev.wep_otyp = wep->otyp;
        ev.wep_art = wep->oartifact;
    }
    if (htgt) {
        ev.hp = Upolyd ? u.mh : u.uhp;
        ev.hpmax = Upolyd ? u.mhmax : u.uhpmax;
    } else {
        ev.hp = mdef->mhp, ev.hpmax = mdef->mhpmax;
        if (mdef->mtame)
            ev.flags |= FXF_TARGET_PET;
    }
    fx_emit(&ev);
    if (!hit) {
        ev.type = FXE_MISS;
        ev.flags &= ~FXF_HIT;
        fx_emit(&ev);
    }
}

/* a weapon's special effect of kind adtyp hit mdef (or the hero) */
void
fx_weapon_effect(struct monst *mdef, int adtyp)
{
    struct fx_event ev;
    boolean htgt = (mdef == &gy.youmonst);

    if (!fx_active() || !mdef)
        return;
    if (!htgt && !canseemon(mdef))
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_WEPFX;
    ev.flags = fx_bulk_flag() | (htgt ? FXF_HERO_TARGET : 0U);
    ev.x = ev.sx = htgt ? u.ux : mdef->mx;
    ev.y = ev.sy = htgt ? u.uy : mdef->my;
    ev.target_id = htgt ? 0 : mdef->m_id;
    ev.adtyp = adtyp;
    fx_emit(&ev);
}

/* hero applies obj (#apply) */
void
fx_apply(struct obj *obj)
{
    struct fx_event ev;

    if (!fx_active() || !obj)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_APPLY;
    ev.flags = fx_bulk_flag() | FXF_HERO_SOURCE;
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = -1;
    ev.otyp = obj->otyp;
    ev.objclass = obj->oclass;
    ev.amount = obj->lamplit ? 1 : 0;
    fx_emit(&ev);
}

/* mtmp just came into the hero's view; only notable monsters are
   reported (the Wizard of Yendor for now) */
staticfn void
fx_appear(struct monst *mtmp)
{
    struct fx_event ev;

    if (!mtmp->iswiz)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_APPEAR;
    ev.flags = fx_bulk_flag();
    ev.x = ev.sx = mtmp->mx, ev.y = ev.sy = mtmp->my;
    ev.adtyp = -1;
    ev.target_id = mtmp->m_id;
    ev.mndx = monsndx(mtmp->data);
    fx_emit(&ev);
}

/* the Wizard of Yendor laughs (returning, cloning, taunting) */
void
fx_wizard_laugh(struct monst *mtmp)
{
    if (fx_active() && mtmp && (canseemon(mtmp) || !Deaf))
        fx_appear(mtmp);
}

/* hero reads a scroll of type otyp */
void
fx_read_scroll(int otyp, boolean confused)
{
    struct fx_event ev;

    if (!fx_active())
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_READ;
    ev.flags = fx_bulk_flag() | FXF_HERO_SOURCE;
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = -1;
    ev.otyp = otyp;
    ev.amount = confused ? 1 : 0;
    fx_emit(&ev);
}

/* mon (or the hero) throws or fires obj, from launcher if not null */
void
fx_launch(struct monst *mon, struct obj *obj, struct obj *launcher)
{
    struct fx_event ev;
    boolean hero = (mon == &gy.youmonst);

    if (!fx_active() || !mon || !obj)
        return;
    if (!hero && !canseemon(mon))
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_LAUNCH;
    ev.flags = fx_bulk_flag() | (hero ? FXF_HERO_SOURCE : 0U) | FXF_RANGED;
    ev.x = ev.sx = hero ? u.ux : mon->mx;
    ev.y = ev.sy = hero ? u.uy : mon->my;
    ev.source_id = hero ? 0 : mon->m_id;
    ev.adtyp = -1;
    ev.wep_otyp = obj->otyp;
    ev.wep_art = obj->oartifact;
    ev.launcher_otyp = launcher ? launcher->otyp : 0;
    fx_emit(&ev);
}

/* the next HP loss of mtmp (or the hero) is of kind adtyp */
void
fx_note_kind(struct monst *mtmp, int adtyp)
{
    struct fx_track *t;

    if (!fx_active() || !mtmp)
        return;
    if (mtmp == &gy.youmonst) {
        fx.hero_pendkind = adtyp;
        fx.hero_pendmoves = svm.moves;
    } else if ((t = fx_find(mtmp, TRUE)) != 0) {
        t->pendkind = adtyp;
        t->pendmoves = svm.moves;
    }
}

/* mondead() is about to set mhp to 0; report the killing blow first */
void
fx_mon_dying(struct monst *mtmp)
{
    struct fx_track *t;

    if (!fx_active() || fx.busy || (t = fx_find(mtmp, FALSE)) == 0)
        return;
    if (canseemon(mtmp) && t->mndx == monsndx(mtmp->data))
        fx_hpchange(mtmp, t->hp, mtmp->mhp, &t->pendkind);
    t->hp = 0;
}

/* the hero is told "You hear ..." (called from You_hear()) */
void
fx_hear(void)
{
    struct fx_event ev;

    if (!fx_active())
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_HEAR;
    ev.flags = fx_bulk_flag() | FXF_HERO_TARGET;
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = -1;
    fx_emit(&ev);
}

/* hero/monster movement interplay: FXE_SWAP or FXE_BUMP */
staticfn void
fx_mover(int type, struct monst *mtmp, coordxy x, coordxy y)
{
    struct fx_event ev;

    if (!fx_active() || !mtmp)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = type;
    ev.flags = fx_bulk_flag() | FXF_HERO_SOURCE
               | (mtmp->mtame ? FXF_TARGET_PET : 0U);
    ev.x = x, ev.y = y;
    ev.sx = u.ux, ev.sy = u.uy;
    ev.adtyp = -1;
    ev.hp = mtmp->mhp, ev.hpmax = mtmp->mhpmax;
    ev.target_id = mtmp->m_id;
    fx_emit(&ev);
}

/* hero swapped places with mtmp, which is now at <x,y> */
void
fx_swap(struct monst *mtmp, coordxy x, coordxy y)
{
    fx_mover(FXE_SWAP, mtmp, x, y);
}

/* hero tried to move onto friendly mtmp at <x,y> and stopped */
void
fx_bump(struct monst *mtmp, coordxy x, coordxy y)
{
    fx_mover(FXE_BUMP, mtmp, x, y);
}

/* hero picked up something of class oclass */
void
fx_pickup(int oclass, int otyp)
{
    struct fx_event ev;

    if (!fx_active())
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_PICKUP;
    ev.flags = fx_bulk_flag() | FXF_HERO_TARGET;
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = -1;
    ev.objclass = oclass;
    ev.otyp = otyp;
    fx_emit(&ev);
}

/* mtmp (or the hero) set off a trap of type ttyp at <x,y> */
void
fx_trap(struct monst *mtmp, int ttyp, coordxy x, coordxy y)
{
    struct fx_event ev;
    boolean hero = (mtmp == &gy.youmonst);

    if (!fx_active() || !mtmp)
        return;
    if (!hero && !canseemon(mtmp))
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_TRAP;
    ev.flags = fx_bulk_flag() | (hero ? FXF_HERO_TARGET : 0U)
               | (!hero && mtmp->mtame ? FXF_TARGET_PET : 0U);
    ev.x = ev.sx = x, ev.y = ev.sy = y;
    ev.adtyp = -1;
    ev.target_id = hero ? 0 : mtmp->m_id;
    ev.ttyp = ttyp;
    fx_emit(&ev);
}

/* eatcorpse() reports how the corpse tastes, just before eating starts */
static int fx_pending_texture = FXT_NONE;

void
fx_eat_texture(int texture)
{
    fx_pending_texture = texture;
}

/* hero starts (or resumes) eating something of type otyp */
void
fx_eat(int otyp)
{
    struct fx_event ev;
    int texture = fx_pending_texture;

    fx_pending_texture = FXT_NONE;
    if (!fx_active())
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_EAT;
    ev.flags = fx_bulk_flag() | FXF_HERO_TARGET;
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = -1;
    ev.otyp = otyp;
    ev.texture = texture;
    fx_emit(&ev);
}

/* mtmp has died (it is still on the map) */
void
fx_mon_died(struct monst *mtmp)
{
    struct fx_event ev;
    struct fx_track *t;

    if (!fx_active())
        return;
    if ((t = fx_find(mtmp, FALSE)) != 0)
        t->id = 0, t->alive = FALSE; /* dropped at next sync */
    if (!canseemon(mtmp))
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_DEATH;
    ev.flags = fx_bulk_flag() | (mtmp->mtame ? FXF_TARGET_PET : 0U);
    ev.x = ev.sx = mtmp->mx, ev.y = ev.sy = mtmp->my;
    ev.adtyp = -1;
    ev.hp = 0, ev.hpmax = mtmp->mhpmax;
    ev.target_id = mtmp->m_id;
    ev.mndx = monsndx(mtmp->data);
    fx_emit(&ev);
}

/*fx.c*/
