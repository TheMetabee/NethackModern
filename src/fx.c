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
#include <math.h>

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
    boolean sinit;        /* sbase is valid */
    unsigned long sbase;  /* statuses last announced (FXE_STATUS) */
    coordxy mx, my;       /* where it was at the last sync */
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
    boolean hero_sinit;
    unsigned long hero_sbase;
    coordxy hero_x, hero_y; /* hero's square at the last sync (0: unknown) */
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
staticfn void fx_step(coordxy, coordxy, coordxy, coordxy, boolean);
staticfn void fx_status(struct monst *, unsigned long, unsigned long);

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
    case FXE_STATUS:
        return "STATUS";
    case FXE_LEVEL:
        return "LEVEL";
    case FXE_MESSAGE:
        return "MESSAGE";
    case FXE_EXP:
        return "EXP";
    case FXE_SHAKE:
        return "SHAKE";
    case FXE_EXPLODE:
        return "EXPLODE";
    case FXE_KNOCKBACK:
        return "KNOCKBACK";
    case FXE_MOVE:
        return "MOVE";
    case FXE_IMPACT:
        return "IMPACT";
    case FXE_WAND:
        return "WAND";
    case FXE_TELEPORT:
        return "TELEPORT";
    case FXE_QUAFF:
        return "QUAFF";
    case FXE_UNIHORN:
        return "UNIHORN";
    case FXE_DETECT:
        return "DETECT";
    case FXE_ARRIVE:
        return "ARRIVE";
    case FXE_UI:
        return "UI";
    case FXE_SUMMON:
        return "SUMMON";
    case FXE_ENGRAVE:
        return "ENGRAVE";
    case FXE_CAST:
        return "CAST";
    case FXE_SEARCH:
        return "SEARCH";
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
        if (ev->type != FXE_ENGRAVE) /* its 'adtyp' isn't a damage kind */
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
    fx.hero_x = fx.hero_y = 0; /* arriving isn't a step */
    {
        /* tell the ports which level we're on now (ambience) */
        struct fx_event ev;

        (void) memset((genericptr_t) &ev, 0, sizeof ev);
        ev.type = FXE_ARRIVE;
        ev.flags = FXF_HERO_TARGET;
        ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
        ev.adtyp = -1;
        fx_emit(&ev);
    }
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
    t->sinit = FALSE;
    t->sbase = 0UL;
    t->health = 0;
    t->alive = TRUE;
    t->mx = mtmp->mx, t->my = mtmp->my;
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
    if (Conflict)
        a |= FXA_CONFLICT;
    if (gm.multi < 0)
        a |= FXA_HELPLESS;
    return a;
}

/* monsters put to sleep by magic (sleep rays, spells, gas): the core
   holds them with the same "can't move for N turns" as paralysis, so
   the monster carries a flag saying it's really asleep (mfxslept, which
   is saved with it) */
void
fx_mon_slept(struct monst *mtmp)
{
    if (mtmp && mtmp != &gy.youmonst)
        mtmp->mfxslept = 1;
}

staticfn boolean
fx_is_slept(struct monst *mtmp)
{
    return mtmp->mfxslept ? TRUE : FALSE;
}

/* it can move again: forget it */
staticfn void
fx_wake(struct monst *mtmp)
{
    mtmp->mfxslept = 0;
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
    if (mtmp->msleeping || (!mtmp->mcanmove && fx_is_slept(mtmp)))
        a |= FXA_ASLEEP;
    else if (!mtmp->mcanmove)
        a |= FXA_HELPLESS;
    /* within the hero's Conflict: the same test movemon() makes before
       each monster rolls to resist it (the Wizard is immune) */
    if (Conflict && !mtmp->iswiz && m_canseeu(mtmp)
        && cansee(mtmp->mx, mtmp->my) && mdistu(mtmp) <= BOLT_LIM * BOLT_LIM)
        a |= FXA_CONFLICT;
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

    fx_options_remember(); /* NetHack Modern: keep changed fx_ settings */
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
    if (fx.hero_x && (u.ux != fx.hero_x || u.uy != fx.hero_y))
        fx_step(fx.hero_x, fx.hero_y, u.ux, u.uy, TRUE);
    fx.hero_x = u.ux, fx.hero_y = u.uy;
    auras = fx_hero_auras();
    health = fx_health(cur, max);
    {
        unsigned long st = auras & ~FXA_HELPLESS;

        if (!fx.hero_sinit) {
            fx.hero_sinit = TRUE; /* baseline: no "Fast!" at game start */
        } else if (st != fx.hero_sbase) {
            fx_status(&gy.youmonst, st & ~fx.hero_sbase,
                      fx.hero_sbase & ~st);
        }
        fx.hero_sbase = st;
    }
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
        if (mtmp->mcanmove || DEADMONSTER(mtmp))
            fx_wake(mtmp);
        if (!DEADMONSTER(mtmp) && t->inview && canseemon(mtmp)
            && (mtmp->mx != t->mx || mtmp->my != t->my))
            fx_step(t->mx, t->my, mtmp->mx, mtmp->my, FALSE);
        t->mx = mtmp->mx, t->my = mtmp->my;
        if (!DEADMONSTER(mtmp)) {
            boolean inview = canseemon(mtmp) ? TRUE : FALSE;

            if (inview && !t->inview)
                fx_appear(mtmp); /* just came into view */
            t->inview = inview;
        }
        if (t->mndx != monsndx(mtmp->data)) {
            /* polymorphed: HP was re-rolled, not damaged; its species
               statuses (flying etc.) are new, not gained */
            t->mndx = monsndx(mtmp->data);
            t->sinit = FALSE;
        } else if (canseemon(mtmp)) {
            fx_hpchange(mtmp, t->hp, mtmp->mhp, &t->pendkind);
        }
        t->hp = mtmp->mhp, t->hpmax = mtmp->mhpmax;
        if (DEADMONSTER(mtmp))
            continue;

        if (canseemon(mtmp)) {
            /* status floaters: only for monsters actually seen, and only
               changes after the first look (no "Flying" for every bat) */
            unsigned long st = fx_mon_auras(mtmp);

            if (!t->sinit) {
                t->sinit = TRUE;
            } else if (st != t->sbase) {
                fx_status(mtmp, st & ~t->sbase, t->sbase & ~st);
            }
            t->sbase = st;
        }
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

/* mtmp (or the hero) gained and/or lost FXA_* statuses */
staticfn void
fx_status(struct monst *mtmp, unsigned long gain, unsigned long lose)
{
    struct fx_event ev;
    boolean hero = (mtmp == &gy.youmonst);

    if (!gain && !lose)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_STATUS;
    ev.flags = fx_bulk_flag() | (hero ? FXF_HERO_TARGET : 0U)
               | (!hero && mtmp->mtame ? FXF_TARGET_PET : 0U);
    ev.x = ev.sx = hero ? u.ux : mtmp->mx;
    ev.y = ev.sy = hero ? u.uy : mtmp->my;
    ev.target_id = hero ? 0 : mtmp->m_id;
    ev.adtyp = -1;
    ev.st_gain = gain;
    ev.st_lose = lose;
    if (fx.logf) {
        fprintf(fx.logf, "T%ld STATUS %s%u +0x%05lx -0x%05lx\n", svm.moves,
                hero ? "hero" : "m", ev.target_id, gain, lose);
        fflush(fx.logf);
    }
    fx_emit(&ev);
}

/* the hero gained (up) or lost an experience level; now at newlevel */
void
fx_level_change(int newlevel, boolean up)
{
    struct fx_event ev;

    if (!fx_active())
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_LEVEL;
    ev.flags = fx_bulk_flag() | FXF_HERO_TARGET | (up ? FXF_HIT : 0U);
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = -1;
    ev.amount = newlevel;
    fx_emit(&ev);
}

/* a message is being shown (called from vpline()); sent to the sound
   handler only, and not logged (the message window already shows it) */
void
fx_message(const char *text)
{
    struct fx_event ev;

    if (!fx.audio_handler || !text || !*text)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_MESSAGE;
    ev.flags = fx_bulk_flag();
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = -1;
    ev.text = text;
    (*fx.audio_handler)(&ev);
}

/* the hero gained 'amount' experience points */
void
fx_exp_gain(int amount)
{
    struct fx_event ev;

    if (!fx_active() || amount <= 0)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_EXP;
    ev.flags = fx_bulk_flag() | FXF_HERO_TARGET;
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = -1;
    ev.amount = amount;
    fx_emit(&ev);
}

/* something big happened: shake the view (strength 1..100) */
void
fx_screen_shake(int strength)
{
    struct fx_event ev;

    if (!fx_active() || strength <= 0)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_SHAKE;
    ev.flags = fx_bulk_flag();
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = -1;
    ev.amount = min(strength, 100);
    fx_emit(&ev);
}

/* an explosion of element 'adtyp' doing 'dam' at <x,y>; 'seen' if the
   hero can see it (otherwise it's only heard) */
void
fx_explosion(coordxy x, coordxy y, int adtyp, int dam, boolean seen)
{
    struct fx_event ev;

    if (!fx_active())
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_EXPLODE;
    ev.flags = fx_bulk_flag() | (seen ? 0U : FXF_UNSEEN);
    ev.x = ev.sx = x, ev.y = ev.sy = y;
    ev.adtyp = adtyp;
    ev.amount = max(dam, 0);
    fx_emit(&ev);
}

/* a seen creature (or the hero) moved from <fx,fy> to <tx,ty> since the
   last sync; only plain steps (one or two squares) are reported, not
   teleports or level changes */
staticfn void
fx_step(coordxy fx, coordxy fy, coordxy tx, coordxy ty, boolean hero)
{
    struct fx_event ev;
    int d = max(abs(tx - fx), abs(ty - fy));

    if (d < 1 || d > 2 || !isok(fx, fy) || !isok(tx, ty))
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_MOVE;
    ev.flags = fx_bulk_flag() | (hero ? FXF_HERO_TARGET : 0U);
    ev.sx = fx, ev.sy = fy;
    ev.x = tx, ev.y = ty;
    ev.adtyp = -1;
    fx_emit(&ev);
}

/* a force (wand of striking, force bolt) struck <x,y>, which the hero
   can see; 'resisted' if it bounced off ("Boing!") */
void
fx_impact(coordxy x, coordxy y, boolean resisted)
{
    static coordxy lastx, lasty;
    static long lastmove = -1L;
    struct fx_event ev;

    if (!fx_active() || !isok(x, y) || !cansee(x, y))
        return;
    /* a pile struck at once is one impact, not one per object */
    if (x == lastx && y == lasty && svm.moves == lastmove)
        return;
    lastx = x, lasty = y, lastmove = svm.moves;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_IMPACT;
    ev.flags = fx_bulk_flag() | (resisted ? FXF_RESISTED : 0U);
    if (x == u.ux && y == u.uy)
        ev.flags |= FXF_HERO_TARGET;
    ev.x = ev.sx = x, ev.y = ev.sy = y;
    ev.adtyp = -1;
    fx_emit(&ev);
}

/* what a wand/spell effect should look like: spells look like the wand
   that does the same thing (the caster knows what they cast); a wand
   whose type isn't identified yet looks plain, so its colours never
   give away what it is */
staticfn int
fx_wand_look(int otyp)
{
    switch (otyp) {
    case SPE_FORCE_BOLT:
        return WAN_STRIKING;
    case SPE_SLOW_MONSTER:
        return WAN_SLOW_MONSTER;
    case SPE_TURN_UNDEAD:
        return WAN_UNDEAD_TURNING;
    case SPE_POLYMORPH:
        return WAN_POLYMORPH;
    case SPE_CANCELLATION:
        return WAN_CANCELLATION;
    case SPE_TELEPORT_AWAY:
        return WAN_TELEPORTATION;
    case SPE_KNOCK:
        return WAN_OPENING;
    case SPE_WIZARD_LOCK:
        return WAN_LOCKING;
    case SPE_LIGHT:
        return WAN_LIGHT;
    case SPE_DETECT_UNSEEN:
        return WAN_SECRET_DOOR_DETECTION;
    case SPE_DIG:
    case WAN_DIGGING: /* the digging beam is drawn anyway */
        return WAN_DIGGING;
    case SPE_DRAIN_LIFE:
    case SPE_HEALING:
    case SPE_EXTRA_HEALING:
    case SPE_STONE_TO_FLESH:
        return otyp; /* no wand does these: they have looks of their own */
    }
    if (otyp < FIRST_OBJECT || otyp >= NUM_OBJECTS
        || objects[otyp].oc_class != WAND_CLASS)
        return 0;
    return objects[otyp].oc_name_known ? otyp : 0;
}

/* a wand or spell effect at <x,y> the hero can see; 'kind' is FXW_*,
   'step' how far along the beam (so it can be drawn travelling) */
void
fx_wand(int otyp, coordxy x, coordxy y, int kind, int step)
{
    static coordxy lastx, lasty;
    static long lastmove = -1L;
    static int lastotyp;
    struct fx_event ev;

    if (!fx_active() || Blind || !isok(x, y) || !cansee(x, y))
        return;
    if (kind == FXW_HIT) {
        /* a pile struck at once is one hit, not one per object */
        if (x == lastx && y == lasty && svm.moves == lastmove
            && otyp == lastotyp)
            return;
        lastx = x, lasty = y, lastmove = svm.moves, lastotyp = otyp;
    }
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_WAND;
    ev.flags = fx_bulk_flag();
    if (x == u.ux && y == u.uy)
        ev.flags |= FXF_HERO_TARGET;
    ev.x = ev.sx = x, ev.y = ev.sy = y;
    ev.adtyp = kind;
    ev.otyp = fx_wand_look(otyp);
    ev.amount = max(step, 0);
    fx_emit(&ev);
}

/* a beam struck a monster: only if the hero can tell something is there
   (never for an unseen one or a mimic still in disguise) */
void
fx_wand_mon(int otyp, struct monst *mtmp)
{
    if (!mtmp)
        return;
    if (mtmp == &gy.youmonst) {
        fx_wand(otyp, u.ux, u.uy, FXW_HIT, 0);
        return;
    }
    if (!canspotmon(mtmp) || M_AP_TYPE(mtmp) != M_AP_NOTHING)
        return;
    fx_wand(otyp, mtmp->mx, mtmp->my, FXW_HIT, 0);
}

/* a non-directional wand (or spell) went off and did something the
   hero noticed; plays around the hero */
void
fx_wand_self(int otyp)
{
    struct fx_event ev;

    if (!fx_active() || Blind)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_WAND;
    ev.flags = fx_bulk_flag() | FXF_HERO_TARGET;
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = FXW_SELF;
    ev.otyp = fx_wand_look(otyp);
    fx_emit(&ev);
}

/* the hero or a monster teleported from <fx,fy> to <tx,ty>; 'from_seen'
   and 'to_seen' say which end the hero saw */
void
fx_teleport(coordxy fx, coordxy fy, coordxy tx, coordxy ty, boolean hero,
            boolean from_seen, boolean to_seen)
{
    struct fx_event ev;

    if (!fx_active() || (!hero && !from_seen && !to_seen))
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_TELEPORT;
    ev.flags = fx_bulk_flag() | (hero ? FXF_HERO_TARGET : 0U);
    ev.sx = fx, ev.sy = fy;
    ev.x = tx, ev.y = ty;
    ev.adtyp = -1;
    ev.amount = (from_seen && !Blind ? 1 : 0) | (to_seen && !Blind ? 2 : 0);
    fx_emit(&ev);
}

/* the hero drinks potion 'otmp'; 'stage' 0 as it goes down (a swirl of
   its colour and a gulp), 1 after its effect (only shown once the type is
   known, so it never gives an unidentified potion away) */
void
fx_quaff(struct obj *otmp, int stage)
{
    struct fx_event ev;

    if (!fx_active() || !otmp || otmp->oclass != POTION_CLASS)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_QUAFF;
    ev.flags = FXF_HERO_TARGET | (Blind ? FXF_UNSEEN : 0U);
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = stage;
    ev.otyp = objects[otmp->otyp].oc_name_known ? otmp->otyp : 0;
    ev.amount = objects[otmp->otyp].oc_color; /* its appearance */
    ev.mndx = !otmp->bknown ? 0 : otmp->blessed ? 1 : otmp->cursed ? -1 : 0;
    if (otmp->otyp == POT_WATER && otmp->bknown)
        ev.mndx = otmp->blessed ? 1 : otmp->cursed ? -1 : 0;
    fx_emit(&ev);
}

/* the unicorn horn's result: 1 cured something, 0 nothing, -1 cursed */
void
fx_unihorn(int result)
{
    struct fx_event ev;

    if (!fx_active())
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_UNIHORN;
    ev.flags = FXF_HERO_TARGET | (Blind ? FXF_UNSEEN : 0U);
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = -1;
    ev.amount = result;
    fx_emit(&ev);
}

/* engraving at the hero's square: 'kind' is FXG_*; for FXG_STROKE
   'etype' is the engraving type and 'first' says it's the first action */
void
fx_engrave(int kind, int etype, boolean first)
{
    struct fx_event ev;
    struct rm *lev = &levl[u.ux][u.uy];

    if (!fx_active())
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_ENGRAVE;
    ev.flags = FXF_HERO_SOURCE | FXF_HERO_TARGET | (Blind ? FXF_UNSEEN : 0U);
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = kind;
    ev.amount = etype;
    ev.otyp = first ? 1 : 0;
    ev.ttyp = IS_GRAVE(lev->typ) ? FXG_SURF_GRAVE
              : is_ice(u.ux, u.uy) ? FXG_SURF_ICE
              : (lev->typ == DRAWBRIDGE_DOWN) ? FXG_SURF_WOOD
              : FXG_SURF_FLOOR;
    fx_emit(&ev);
}

/* the hero searches by command (dosearch(); intrinsic and ring searching
   call dosearch0() directly and never get here). A counted search
   ("20s") is bulk after its first turn, so ports show only the start. */
void
fx_search(void)
{
    static long last_moves = -10L;
    static boolean last_counted = FALSE;
    struct fx_event ev;
    boolean counted = (fx_bulk_flag() != 0U),
            going_on = (counted && last_counted
                        && svm.moves - last_moves <= 2L);

    last_moves = svm.moves, last_counted = counted;
    if (!fx_active() || u.uswallow)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_SEARCH;
    ev.flags = FXF_HERO_SOURCE | FXF_HERO_TARGET | (Blind ? FXF_UNSEEN : 0U)
               | (going_on ? FXF_BULK : 0U);
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = -1;
    fx_emit(&ev);
}

/* detection has just put things on the map (before the hero is asked to
   look them over): one event per square showing such a thing, and one
   at the hero for the sound; 'kind' is FXD_* */
void
fx_detected(int kind)
{
    struct fx_event ev;
    coordxy x, y;
    int n = 0;

    if (!fx_active())
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_DETECT;
    ev.adtyp = kind;
    for (x = 1; x < COLNO && n < 80; x++)
        for (y = 0; y < ROWNO && n < 80; y++) {
            int glyph = glyph_at(x, y);

            if (u_at(x, y))
                continue;
            if (kind == FXD_MONSTER ? !glyph_is_monster(glyph)
                                    : !glyph_is_object(glyph))
                continue;
            ev.flags = 0U;
            ev.x = ev.sx = x, ev.y = ev.sy = y;
            ev.amount = distmin(u.ux, u.uy, x, y);
            fx_emit(&ev);
            n++;
        }
    ev.flags = FXF_HERO_TARGET;
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.amount = n;
    fx_emit(&ev);
}

/* the hero casts spell 'otyp': 'stage' 0 as it's cast (a flourish in the
   colour of its school), 1 once it has taken effect, for the spells that
   act like a scroll or potion (or have no other event) to show their
   look; a familiar is shown arriving like any summoned monster */
void
fx_cast(int otyp, int stage)
{
    static coordxy cast_x, cast_y;
    static unsigned cast_mark;
    struct fx_event ev;

    if (!fx_active())
        return;
    if (stage == 0) {
        cast_x = u.ux, cast_y = u.uy;
        cast_mark = fx_summon_begin();
    } else if (otyp == SPE_CREATE_FAMILIAR) {
        fx_summon_end(cast_mark, FXS_ARCANE);
    }
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_CAST;
    ev.flags = fx_bulk_flag() | FXF_HERO_SOURCE | FXF_HERO_TARGET
               | (Blind ? FXF_UNSEEN : 0U);
    ev.x = u.ux, ev.y = u.uy;
    ev.sx = stage ? cast_x : u.ux, ev.sy = stage ? cast_y : u.uy;
    ev.adtyp = stage;
    ev.otyp = otyp;
    ev.amount = spell_skilltype(otyp);
    fx_emit(&ev);
}

/* NetHack Modern: summoning.  fx_summon_begin() notes where monster
   numbering stands; fx_summon_end() then shows each monster made since
   (groups included) that the hero can see, one after another.  Hidden
   or disguised ones are left alone so nothing is given away. */
unsigned
fx_summon_begin(void)
{
    return svc.context.ident;
}

void
fx_summon_end(unsigned mark, int kind)
{
    struct monst *mtmp;
    struct fx_event ev;
    int n = 0;

    if (!fx_active())
        return;
    for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
        if (DEADMONSTER(mtmp) || mtmp->m_id < mark
            || !isok(mtmp->mx, mtmp->my) || !canspotmon(mtmp)
            || mtmp->mundetected
            || (M_AP_TYPE(mtmp) != M_AP_NOTHING
                && M_AP_TYPE(mtmp) != M_AP_MONSTER))
            continue;
        (void) memset((genericptr_t) &ev, 0, sizeof ev);
        ev.type = FXE_SUMMON;
        ev.target_id = mtmp->m_id;
        ev.x = ev.sx = mtmp->mx, ev.y = ev.sy = mtmp->my;
        ev.adtyp = kind;
        ev.amount = n++;
        ev.mndx = monsndx(mtmp->data);
        fx_emit(&ev);
    }
}

/* an interface action with a sound of its own ("inventory") */
void
fx_ui(const char *what)
{
    struct fx_event ev;

    if (!fx_active() || !what)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_UI;
    ev.flags = FXF_HERO_SOURCE;
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = -1;
    ev.text = what;
    fx_emit(&ev);
}

/* a creature was knocked (hurtled) one step from <fx,fy> to <tx,ty> */
void
fx_knockback(coordxy fx, coordxy fy, coordxy tx, coordxy ty)
{
    struct fx_event ev;

    if (!fx_active() || (fx == tx && fy == ty))
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_KNOCKBACK;
    ev.flags = fx_bulk_flag();
    if (tx == u.ux && ty == u.uy)
        ev.flags |= FXF_HERO_TARGET;
    ev.sx = fx, ev.sy = fy;
    ev.x = tx, ev.y = ty;
    ev.adtyp = -1;
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
    ev.adtyp = 0; /* reading it; 1 = its effect (fx_scroll_effect) */
    ev.otyp = otyp;
    ev.amount = confused ? 1 : 0;
    fx_emit(&ev);
}

/* a scroll of type otyp has taken effect; its look plays around the hero
   only if the type is known by now (FXE_READ with 'adtyp' 1) */
void
fx_scroll_effect(int otyp)
{
    struct fx_event ev;

    if (!fx_active() || Blind || !objects[otyp].oc_name_known)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_READ;
    ev.flags = fx_bulk_flag() | FXF_HERO_SOURCE | FXF_HERO_TARGET;
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = 1;
    ev.otyp = otyp;
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

/* hero put 'obj' down on the floor */
void
fx_drop(struct obj *obj)
{
    struct fx_event ev;

    if (!fx_active() || !obj)
        return;
    (void) memset((genericptr_t) &ev, 0, sizeof ev);
    ev.type = FXE_PICKUP;
    ev.flags = fx_bulk_flag() | FXF_HERO_TARGET | FXF_DROP;
    ev.x = ev.sx = u.ux, ev.y = ev.sy = u.uy;
    ev.adtyp = -1;
    ev.objclass = obj->oclass;
    ev.otyp = obj->otyp;
    ev.amount = (int) obj->owt;
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

/* NetHack Modern: the brightness (0..255) of remembered squares, or of
   solid rock, for the fx_shade the player picked */
int
fx_shade_level(boolean rock)
{
    static const unsigned char remembered[4] = { 160, 105, 70, 42 },
                               stone[4] = { 85, 55, 36, 20 };
    int i = iflags.fx_shade;

    i = (i < 0) ? 0 : (i > 3) ? 3 : i;
    return rock ? stone[i] : remembered[i];
}

/* NetHack Modern: how lit each square looks, for NetHackW's lighting.
   Only what the hero can tell is used: squares in sight are bright
   (lit rooms fully, squares lit by a lamp or seen in the dark less so);
   remembered squares are dim.  'warm' is the glow of a nearby lamp,
   candle or other light source on a square in sight.  Both 0..255. */
int
fx_light_map(unsigned char bright[COLNO][ROWNO],
             unsigned char warm[COLNO][ROWNO], double t)
{
#define FX_MAXLIGHTS 64
    coordxy lx[FX_MAXLIGHTS], ly[FX_MAXLIGHTS];
    short lr[FX_MAXLIGHTS], lf[FX_MAXLIGHTS];
    double gain[FX_MAXLIGHTS], dim[FX_MAXLIGHTS], heat[FX_MAXLIGHTS];
    int x, y, i, n = fx_light_sources(lx, ly, lr, lf, FX_MAXLIGHTS);
    int flickering = 0;

    /* a flame breathes: the whole pool of light dims and brightens, its
       reach and warmth swelling with it.  A few slow, unrelated waves
       (about one breath a second), each flame out of step with the
       others */
    for (i = 0; i < n; i++) {
        double ph = lx[i] * 1.37 + ly[i] * 2.91 + i * 0.73,
               k = lf[i] / 100.0,
               wob = 0.6 * sin(t * 3.4 + ph) + 0.28 * sin(t * 5.9 + 2.1 * ph)
                     + 0.12 * sin(t * 9.7 + 0.7 * ph);

        gain[i] = 1.0 + k * 0.2 * wob;               /* reach */
        dim[i] = 1.0 - k * 0.2 * (0.5 - 0.5 * wob);  /* whole pool */
        heat[i] = 1.0 + k * 0.45 * wob;              /* warmth */
    }

    for (x = 0; x < COLNO; x++)
        for (y = 0; y < ROWNO; y++) {
            double b, w = 0.0;

            if (!cansee(x, y) && !u_at(x, y)) {
                bright[x][y] = fx_shade_level(FALSE); /* remembered */
                warm[x][y] = 0;
                continue;
            }
            if (levl[x][y].lit && !templit(x, y))
                b = 1.0;           /* a lit room */
            else if (levl[x][y].lit)
                b = 1.0;           /* lit room with a lamp in it too */
            else if (templit(x, y))
                b = 0.0;           /* lit by a light source: below */
            else
                b = 0.62;          /* seen in the dark (next to you) */
            for (i = 0; i < n; i++) {
                double dx = x - lx[i], dy = y - ly[i];
                double d = sqrt(dx * dx + dy * dy), r = lr[i] + 0.5,
                       rg = r * gain[i];

                if (d > r)
                    continue;
                if (lf[i])
                    flickering = 1;
                /* a lamp's own light: full at the lamp, softer at the
                   edge of its reach (which a flame makes waver) */
                if (templit(x, y) && !levl[x][y].lit)
                    b = max(b, min(1.0, 1.0 - 0.42 * (d / rg) * (d / rg))
                                   * dim[i]);
                w = max(w, min(1.0, (1.0 - d / r) * gain[i] * heat[i]));
            }
            if (b <= 0.0)
                b = 0.62;
            if (u_at(x, y))
                b = max(b, 0.8);
            bright[x][y] = (unsigned char) (b * 255.0);
            warm[x][y] = (unsigned char) (w * 255.0);
        }
#undef FX_MAXLIGHTS
    return flickering;
}

/* NetHack Modern: a thrown or fired object in flight (shown by tmp_at),
   so the window port can turn it to point the way it's going.  The
   direction comes from its last step, or before the first step from a
   hint given by whoever launched it. */
static struct {
    boolean on, placed;
    coordxy x, y;
    schar dx, dy;
} fx_fly;

void
fx_flight_begin(int glyph)
{
    fx_fly.on = glyph_is_object(glyph);
    fx_fly.placed = FALSE;
    fx_fly.dx = fx_fly.dy = 0;
}

void
fx_flight_hint(int dx, int dy)
{
    if (fx_fly.on && !fx_fly.placed)
        fx_fly.dx = (schar) sgn(dx), fx_fly.dy = (schar) sgn(dy);
}

void
fx_flight_step(coordxy x, coordxy y)
{
    if (!fx_fly.on)
        return;
    if (fx_fly.placed && (x != fx_fly.x || y != fx_fly.y))
        fx_fly.dx = (schar) sgn(x - fx_fly.x),
        fx_fly.dy = (schar) sgn(y - fx_fly.y);
    fx_fly.x = x, fx_fly.y = y;
    fx_fly.placed = TRUE;
}

void
fx_flight_end(void)
{
    fx_fly.on = FALSE;
}

/* is the object shown at x,y in flight?  if so, which way */
boolean
fx_flight_at(coordxy x, coordxy y, int *dx, int *dy)
{
    if (!fx_fly.on || !fx_fly.placed || x != fx_fly.x || y != fx_fly.y
        || (!fx_fly.dx && !fx_fly.dy))
        return FALSE;
    *dx = fx_fly.dx, *dy = fx_fly.dy;
    return TRUE;
}
