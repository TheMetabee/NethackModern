/* NetHack Modern masound.c */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * "miniaudio" soundlib (SND_LIB_MINIAUDIO).
 *
 * Plays combat sounds driven by the presentation event stream (fx.h):
 * hits, misses and deaths, with random pitch variation, quieter with
 * distance from the hero and panned left/right. A few of the core's
 * own sound effects (doors) are mapped to our samples too. Everything
 * else (instruments, achievements, user sounds) is handed to the stock
 * Windows soundlib when it is built in, so nothing that worked before
 * stops working.
 *
 * Samples live in a "sounds" folder next to the executable; see
 * sound/modern/attributions.txt. Missing files are simply silent.
 */

#include "win32api.h"
#include "hack.h"
#include "nhminiaudio.h"

#ifdef SND_LIB_MINIAUDIO

#ifdef SND_LIB_WINDSOUND
extern struct sound_procs windsound_procs; /* fallback for the rest */
#endif

static void ma_init_nhsound(void);
static void ma_exit_nhsound(const char *);
static void ma_achievement(schar, schar, int32_t);
static void ma_soundeffect(char *, int32_t, int32_t);
static void ma_hero_playnotes(int32_t, const char *, int32_t);
static void ma_play_usersound(char *, int32_t, int32_t);
static void ma_ambience(int32_t, int32_t, int32_t);
static void ma_verbal(char *, int32_t, int32_t, int32_t, int32_t);
static void ma_fx_event(const struct fx_event *);

struct sound_procs miniaudio_procs = {
    SOUNDID(miniaudio),
    SOUND_TRIGGER_USERSOUNDS | SOUND_TRIGGER_SOUNDEFFECTS
        | SOUND_TRIGGER_HEROMUSIC | SOUND_TRIGGER_AMBIENCE
        | SOUND_TRIGGER_ACHIEVEMENTS | SOUND_TRIGGER_VERBAL,
    ma_init_nhsound,
    ma_exit_nhsound,
    ma_achievement,
    ma_soundeffect,
    ma_hero_playnotes,
    ma_play_usersound,
    ma_ambience,
    ma_verbal,
};

/* sample groups: files <name>_1.wav .. <name>_N.wav */
enum ma_group {
    MAG_HIT, MAG_HURT, MAG_MISS, MAG_DEATH, MAG_HIT_COLD, MAG_HIT_MAGIC,
    MAG_HIT_FIRE, MAG_DOOR_OPEN, MAG_DOOR_CLOSE, MAG_HEAR, MAG_SWAP,
    MAG_BUMP, MAG_COUNT
};
static const char *const ma_group_names[MAG_COUNT] = {
    "hit", "hurt", "miss", "death", "hit_cold", "hit_magic", "hit_fire",
    "door_open", "door_close", "hear", "swap", "bump"
};
#define MA_MAXVAR 9

/* core sound effect names (seffects.h), so any se_<name>_N.wav dropped
   into the sounds folder plays for that effect */
struct ma_semap {
    enum sound_effect_entries seid;
    const char *name;
};
#define SEFFECTS_AUTOMAP
static const struct ma_semap ma_se_names[] = {
    { se_zero_invalid, "" },
#include "seffects.h"
};
#undef SEFFECTS_AUTOMAP

static struct {
    boolean ok;
    char dir[MAX_PATH];             /* ends with a backslash */
    int nvar[MAG_COUNT];            /* variations found per group */
    DWORD last_play[MAG_COUNT];     /* GetTickCount() of last play */
    signed char se_nvar[number_of_se_entries]; /* -1 = not looked yet */
    DWORD last_se;                  /* last time a core effect played */
    unsigned long seed;             /* private RNG, never the game's */
} ma;

static unsigned
ma_rand(unsigned n)
{
    ma.seed = ma.seed * 1103515245UL + 12345UL;
    return n ? (unsigned) ((ma.seed >> 16) % n) : 0;
}

/* random float in [lo, hi] */
static float
ma_frand(float lo, float hi)
{
    return lo + (hi - lo) * (float) ma_rand(1001) / 1000.0f;
}

static boolean
ma_dir_has_samples(const char *dir)
{
    char path[MAX_PATH + 32];

    Snprintf(path, sizeof path, "%s%s_1.wav", dir, ma_group_names[MAG_HIT]);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* find the samples folder: <exedir>\sounds\, or, when running straight
   out of the build tree (binary\<cfg>\<platform>), the source copy */
static boolean
ma_find_dir(void)
{
    char exe[MAX_PATH], *p;
    static const char *const tries[] = {
        "sounds\\", "..\\..\\..\\sound\\modern\\"
    };
    int i;

    if (!GetModuleFileNameA(NULL, exe, sizeof exe))
        return FALSE;
    if ((p = strrchr(exe, '\\')) != 0)
        p[1] = '\0';
    for (i = 0; i < SIZE(tries); i++) {
        Snprintf(ma.dir, sizeof ma.dir, "%s%s", exe, tries[i]);
        if (ma_dir_has_samples(ma.dir))
            return TRUE;
    }
    ma.dir[0] = '\0';
    return FALSE;
}

static void
ma_init_nhsound(void)
{
    int g, v;
    char path[MAX_PATH + 32];

    ma.seed = (unsigned long) GetTickCount() | 1UL;
    (void) memset(ma.se_nvar, -1, sizeof ma.se_nvar);
#ifdef SND_LIB_WINDSOUND
    if (windsound_procs.sound_init_nhsound)
        (*windsound_procs.sound_init_nhsound)();
#endif
    if (!nhma_init()) {
        fx_debug_log("miniaudio: engine init FAILED, staying silent");
        return; /* no audio device; stay silent */
    }
    nhma_set_master_volume(0.8f);
    if (ma_find_dir()) {
        for (g = 0; g < MAG_COUNT; g++) {
            for (v = 1; v <= MA_MAXVAR; v++) {
                Snprintf(path, sizeof path, "%s%s_%d.wav", ma.dir,
                         ma_group_names[g], v);
                if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
                    break;
            }
            ma.nvar[g] = v - 1;
        }
    }
    ma.ok = TRUE;
    fx_set_audio_handler(ma_fx_event);
    Snprintf(path, sizeof path,
             "miniaudio: init ok, dir=\"%s\" hit=%d hurt=%d miss=%d death=%d",
             ma.dir, ma.nvar[MAG_HIT], ma.nvar[MAG_HURT], ma.nvar[MAG_MISS],
             ma.nvar[MAG_DEATH]);
    fx_debug_log(path);
}

static void
ma_exit_nhsound(const char *reason)
{
    fx_set_audio_handler((fx_handler_fn) 0);
    nhma_shutdown();
    ma.ok = FALSE;
#ifdef SND_LIB_WINDSOUND
    if (windsound_procs.sound_exit_nhsound)
        (*windsound_procs.sound_exit_nhsound)(reason);
#else
    nhUse(reason);
#endif
}

/* play one variation of a group; volume 0..1 */
static void
ma_play_group(enum ma_group g, float volume, float pitch, float pan)
{
    char path[MAX_PATH + 32], note[MAX_PATH + 96];
    DWORD now = GetTickCount();
    int started;

    if (!ma.ok || !ma.nvar[g] || !iflags.sounds || Deaf) {
        Snprintf(note, sizeof note,
                 "miniaudio: skip %s (ok=%d nvar=%d sounds=%d deaf=%d)",
                 ma_group_names[g], (int) ma.ok, ma.nvar[g],
                 (int) iflags.sounds, Deaf ? 1 : 0);
        fx_debug_log(note);
        return;
    }
    /* the same kind of sound twice within a few ms (an explosion
       hitting a crowd) just sounds like one louder sound */
    if (now - ma.last_play[g] < 35)
        return;
    ma.last_play[g] = now;
    Snprintf(path, sizeof path, "%s%s_%u.wav", ma.dir, ma_group_names[g],
             1 + ma_rand((unsigned) ma.nvar[g]));
    started = nhma_play(path, volume, pitch, pan);
    Snprintf(note, sizeof note, "miniaudio: play %s vol=%.2f -> %s", path,
             (double) volume, started ? "started" : "FAILED");
    fx_debug_log(note);
}

/* named sample sets looked up on first use: <name>_1.wav .. <name>_N.wav */
#define MA_MAXNAMED 256
static struct {
    const char *name;
    int nvar;
    DWORD last;
} ma_named[MA_MAXNAMED];
static int ma_nnamed = 0;

/* index of a named sample set, looking its files up on first use;
   -1 if the table is full */
static int
ma_named_find(const char *name)
{
    char path[MAX_PATH + 64];
    int i, n;

    for (i = 0; i < ma_nnamed; i++)
        if (!strcmp(ma_named[i].name, name))
            return i;
    if (ma_nnamed >= MA_MAXNAMED || !ma.ok)
        return -1;
    for (n = 1; n <= MA_MAXVAR; n++) {
        Snprintf(path, sizeof path, "%s%s_%d.wav", ma.dir, name, n);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            break;
    }
    ma_named[i].name = name; /* callers pass string literals */
    ma_named[i].nvar = n - 1;
    ma_named[i].last = 0;
    ma_nnamed++;
    return i;
}

/* how many variations a named set has (0 if none) */
static int
ma_named_count(const char *name)
{
    int i = ma_named_find(name);

    return (i < 0) ? 0 : ma_named[i].nvar;
}

static void
ma_play_named(const char *name, float volume, float pitch, float pan)
{
    char path[MAX_PATH + 64];
    int i;
    DWORD now = GetTickCount();

    if (!ma.ok || !iflags.sounds || Deaf)
        return;
    if ((i = ma_named_find(name)) < 0)
        return;
    if (!ma_named[i].nvar || now - ma_named[i].last < 35)
        return;
    ma_named[i].last = now;
    Snprintf(path, sizeof path, "%s%s_%u.wav", ma.dir, name,
             1 + ma_rand((unsigned) ma_named[i].nvar));
    {
        char note[MAX_PATH + 96];
        int started = nhma_play(path, volume, pitch, pan);

        Snprintf(note, sizeof note, "miniaudio: play %s vol=%.2f -> %s",
                 path, (double) volume, started ? "started" : "FAILED");
        fx_debug_log(note);
    }
}

/* applying a tool. Tools with their own core sound (instruments,
   whistles, the stethoscope) or with an open sound (containers) are not
   listed; pick-axes and axes sound per turn while digging (se_dig_*). */
static void
ma_apply(int otyp, boolean was_lit)
{
    const char *name = 0;
    float pitch = ma_frand(0.95f, 1.05f), vol = 0.7f;

    switch (otyp) {
    case EXPENSIVE_CAMERA:
        name = "use_camera";
        break;
    case WAX_CANDLE:
    case TALLOW_CANDLE:
    case OIL_LAMP:
    case MAGIC_LAMP:
    case CANDELABRUM_OF_INVOCATION:
    case POT_OIL:
        name = was_lit ? "use_snuff" : "use_light";
        break;
    case BRASS_LANTERN:
        name = "use_lantern";
        break;
    case TINNING_KIT:
        name = "use_tinning";
        break;
    case CAN_OF_GREASE:
    case CREAM_PIE:
    case LUMP_OF_ROYAL_JELLY:
        name = "use_grease";
        break;
    case LOCK_PICK:
    case CREDIT_CARD:
    case SKELETON_KEY:
    case TIN_OPENER:
        name = "use_lockpick";
        break;
    case LEASH:
    case SADDLE:
        name = "pickup_food"; /* leather */
        pitch *= 0.9f;
        break;
    case TOWEL:
    case BLINDFOLD:
        name = "pickup_bag"; /* cloth */
        break;
    case LENSES:
        name = "use_lockpick";
        pitch *= 1.3f;
        break;
    case MIRROR:
        name = "use_mirror";
        break;
    case CRYSTAL_BALL:
        name = "use_crystal";
        break;
    case UNICORN_HORN:
        return; /* its result has a sound of its own (FXE_UNIHORN) */
    case BULLWHIP:
        name = "wpn_whip_hit";
        break;
    case GRAPPLING_HOOK:
        name = "wpn_launch_throw";
        pitch *= 0.8f;
        break;
    case FIGURINE:
        name = "use_figurine";
        break;
    case HORN_OF_PLENTY:
        name = "use_plenty";
        break;
    case LAND_MINE:
    case BEARTRAP:
        name = "trap_bear"; /* setting the jaws */
        pitch *= 1.15f, vol = 0.55f;
        break;
    case FLINT:
    case LUCKSTONE:
    case LOADSTONE:
    case TOUCHSTONE:
        name = "use_rubstone";
        break;
    default:
        return;
    }
    ma_play_named(name, vol, pitch, 0.0f);
}

/* reading a scroll: the paper, then (baked-in ~0.25 s later) an effect
   for the scroll type. Scare monster is left to the core's laughter. */
static void
ma_read(int otyp, boolean confused)
{
    const char *name = 0;
    float pitch = confused ? ma_frand(0.8f, 1.25f) : ma_frand(0.97f, 1.03f);

    ma_play_named("read_paper", 0.6f, ma_frand(0.95f, 1.08f), 0.0f);
    switch (otyp) {
    case SCR_ENCHANT_WEAPON:
    case SCR_ENCHANT_ARMOR:
        name = "scroll_enchant";
        break;
    case SCR_REMOVE_CURSE:
        name = "scroll_holy";
        break;
    case SCR_TELEPORTATION:
        name = "scroll_teleport";
        break;
    case SCR_GOLD_DETECTION:
        name = "scroll_gold";
        break;
    case SCR_FOOD_DETECTION:
        name = "scroll_detect";
        break;
    case SCR_IDENTIFY:
        name = "scroll_identify";
        break;
    case SCR_MAGIC_MAPPING:
        name = "scroll_mapping";
        break;
    case SCR_LIGHT:
        name = "scroll_light";
        break;
    case SCR_FIRE:
        name = "scroll_fire";
        break;
    case SCR_EARTH:
        name = "scroll_earth";
        break;
    case SCR_AMNESIA:
        name = "scroll_amnesia";
        break;
    case SCR_CREATE_MONSTER:
        name = "scroll_summon";
        break;
    case SCR_PUNISHMENT:
        name = "scroll_punish";
        break;
    case SCR_CONFUSE_MONSTER:
        name = "scroll_confuse";
        break;
    case SCR_DESTROY_ARMOR:
        name = "scroll_destroy_armor";
        break;
    case SCR_GENOCIDE:
        name = "scroll_genocide";
        break;
    case SCR_CHARGING:
        name = "scroll_charging";
        break;
    case SCR_STINKING_CLOUD:
        name = "scroll_stink";
        break;
    case SCR_TAMING:
        name = "scroll_taming";
        break;
    default: /* blank paper, mail, scare monster: just the paper */
        return;
    }
    ma_play_named(name, 0.8f, pitch, 0.0f);
}

/* pickup sound per object class; falls back to a generic one */
static void
ma_pickup(int oclass, int otyp, boolean drop, int weight)
{
    const char *name;
    float pitch = ma_frand(0.95f, 1.08f);
    static DWORD drop_window;
    static int drop_count;

    switch (oclass) {
    case COIN_CLASS:
        name = "pickup_coin";
        break;
    case POTION_CLASS:
        name = "pickup_potion";
        pitch = ma_frand(1.1f, 1.3f); /* small bottle clink */
        break;
    case WAND_CLASS:
        name = "pickup_wand";
        pitch = ma_frand(1.2f, 1.4f); /* light wooden tap */
        break;
    case SCROLL_CLASS:
        name = "pickup_scroll";
        break;
    case SPBOOK_CLASS:
        name = "pickup_book";
        break;
    case RING_CLASS:
    case AMULET_CLASS:
        name = "pickup_ring";
        pitch = ma_frand(1.3f, 1.5f); /* tiny metal clink */
        break;
    case GEM_CLASS:
        name = "pickup_gem";
        pitch = ma_frand(1.2f, 1.4f);
        break;
    case WEAPON_CLASS:
        name = "pickup_weapon";
        break;
    case ARMOR_CLASS:
        name = "pickup_armor";
        break;
    case FOOD_CLASS:
        name = "pickup_food";
        break;
    case TOOL_CLASS:
        switch (otyp) {
        case SACK:
        case OILSKIN_SACK:
        case BAG_OF_HOLDING:
        case BAG_OF_TRICKS:
        case TOWEL:
        case BLINDFOLD:
            name = "pickup_bag"; /* soft cloth, not a clatter */
            break;
        case LEASH:
        case SADDLE:
            name = "pickup_food"; /* the leather pouch sound */
            pitch = ma_frand(0.85f, 0.95f);
            break;
        default:
            name = "pickup_tool";
            break;
        }
        break;
    default:
        name = "pickup";
        break;
    }
    if (drop) {
        DWORD now = GetTickCount();

        /* dropping many things at once: a few sounds, not a pile-up */
        if (now - drop_window > 150)
            drop_window = now, drop_count = 0;
        if (++drop_count > 3)
            return;
        /* set down: a little lower and softer than picking it up, and
           something heavy lands with a thud */
        ma_play_named(name, 0.5f, pitch * 0.88f, 0.0f);
        if (weight >= 100)
            ma_play_named("trap_fall", min(0.7f, 0.25f + weight / 800.0f),
                          ma_frand(0.75f, 0.85f), 0.0f);
        return;
    }
    ma_play_named(name, 0.6f, pitch, 0.0f);
}

/* a trap going off: trap_<kind>_N.wav. Traps whose code already plays
   a core sound effect (squeaky board, land mine) are left to that. */
static void
ma_trap(int ttyp, float vol, float pan)
{
    const char *name;
    float pitch = ma_frand(0.92f, 1.08f);

    switch (ttyp) {
    case ARROW_TRAP:
    case DART_TRAP:
        name = "trap_arrow";
        break;
    case ROCKTRAP:
        name = "trap_rock";
        break;
    case BEAR_TRAP:
        name = "trap_bear";
        break;
    case SLP_GAS_TRAP:
        name = "trap_gas";
        pitch = ma_frand(0.7f, 0.8f);
        break;
    case RUST_TRAP:
        name = "trap_water";
        break;
    case FIRE_TRAP:
        name = "trap_fire";
        break;
    case PIT:
    case SPIKED_PIT:
        name = "trap_fall";
        pitch = ma_frand(0.75f, 0.9f);
        break;
    case TRAPDOOR:
        /* latch, hinges, a rush of air, then the landing; it keeps
           playing while the next level loads */
        name = "trap_trapdoor";
        vol = 1.0f;
        break;
    case HOLE:
        name = "trap_hole";
        vol = 1.0f;
        break;
    case TELEP_TRAP:
    case LEVEL_TELEP:
    case MAGIC_PORTAL:
    case MAGIC_TRAP:
    case ANTI_MAGIC:
    case POLY_TRAP:
        name = "trap_magic";
        pitch = ma_frand(1.0f, 1.25f);
        break;
    case WEB:
        name = "trap_web";
        break;
    case STATUE_TRAP:
        name = "trap_statue";
        break;
    case ROLLING_BOULDER_TRAP:
        /* "Click!"; the boulder itself plays se_boulder_roll */
        name = "trap_click";
        break;
    default:
        return;
    }
    ma_play_named(name, 0.75f * vol, pitch, pan);
}

/* eating: a sound per kind of food (eat_<kind>_N.wav), each food with
   its own pitch so similar foods still sound different; corpses by how
   they tasted. Falls back to the plain chew (eat_N.wav). */
static const struct ma_food {
    int otyp;
    const char *kind;
    float pitch;
} ma_foods[] = {
    { APPLE, "eat_crunchy", 1.12f },
    { CARROT, "eat_crunchy", 1.0f },
    { FORTUNE_COOKIE, "eat_crunchy", 1.28f },
    { CRAM_RATION, "eat_crunchy", 0.88f },
    { LEMBAS_WAFER, "eat_crunchy", 1.05f },
    { ORANGE, "eat_juicy", 1.1f },
    { PEAR, "eat_juicy", 1.0f },
    { MELON, "eat_juicy", 0.88f },
    { BANANA, "eat_soft", 1.0f },
    { PANCAKE, "eat_soft", 0.94f },
    { CREAM_PIE, "eat_soft", 1.1f },
    { LUMP_OF_ROYAL_JELLY, "eat_soft", 1.22f },
    { SLIME_MOLD, "eat_soft", 1.05f },
    { KELP_FROND, "eat_leafy", 0.9f },
    { EUCALYPTUS_LEAF, "eat_leafy", 1.12f },
    { SPRIG_OF_WOLFSBANE, "eat_leafy", 1.0f },
    { CLOVE_OF_GARLIC, "eat_leafy", 1.22f },
    { MEATBALL, "eat_meaty", 1.06f },
    { MEAT_STICK, "eat_meaty", 1.16f },
    { ENORMOUS_MEATBALL, "eat_meaty", 0.8f },
    { TRIPE_RATION, "eat_meaty", 0.94f },
    { FOOD_RATION, "eat_meaty", 1.0f },
    { K_RATION, "eat_crunchy", 0.95f },
    { C_RATION, "eat_meaty", 1.1f },
    { CANDY_BAR, "eat_candy", 1.0f },
    { EGG, "eat_egg", 1.0f },
    { GLOB_OF_GRAY_OOZE, "eat_glob", 1.05f },
    { GLOB_OF_BROWN_PUDDING, "eat_glob", 0.92f },
    { GLOB_OF_GREEN_SLIME, "eat_glob", 1.15f },
    { GLOB_OF_BLACK_PUDDING, "eat_glob", 0.82f },
};

static void
ma_eat(int otyp, int texture)
{
    const char *kind = 0;
    float pitch = 1.0f;
    int i;

    if (otyp == CORPSE) {
        static const char *const tex[] = {
            "eat_meaty",     /* FXT_NONE: no taste message */
            "eat_soft",      /* FXT_OKAY (vegetarian) */
            "eat_stringy", "eat_gamey", "eat_fatty", "eat_tough",
            "eat_delicious", "eat_terrible"
        };

        if (texture >= 0 && texture < SIZE(tex))
            kind = tex[texture];
        pitch = 0.92f;
    } else {
        for (i = 0; i < SIZE(ma_foods); i++)
            if (ma_foods[i].otyp == otyp) {
                kind = ma_foods[i].kind;
                pitch = ma_foods[i].pitch;
                break;
            }
    }
    pitch *= ma_frand(0.97f, 1.03f);
    if (kind && ma_named_count(kind) > 0)
        ma_play_named(kind, 0.7f, pitch, 0.0f);
    else
        ma_play_named("eat", 0.7f, pitch, 0.0f);
}

/*
 * Weapons. Each weapon skill maps to a sample set: wpn_<set>_hit_N.wav
 * on a hit and wpn_<set>_swing_N.wav on a miss, at a pitch that sets
 * light and heavy weapons of the same family apart. A weapon's special
 * effect (Fire Brand burning, Frost Brand freezing, Mjollnir's lightning,
 * Stormbringer draining) adds a layer only when it actually lands, i.e.
 * isn't resisted (FXE_WEPFX, see ma_weapon_effect()).
 */
struct ma_wset {
    const char *set;
    float pitch;
};

static struct ma_wset
ma_weapon_set(int otyp)
{
    struct ma_wset w = { "blunt_wood", 1.0f };
    int skill;

    if (otyp <= STRANGE_OBJECT || otyp >= NUM_OBJECTS)
        return w;
    skill = objects[otyp].oc_skill;
    if (skill < 0)
        skill = -skill; /* ammo and missiles use the launcher's skill */
    switch (skill) {
    case P_DAGGER:
    case P_KNIFE:
        w.set = "blade", w.pitch = 1.22f;
        break;
    case P_SHORT_SWORD:
        w.set = "blade", w.pitch = 1.1f;
        break;
    case P_BROAD_SWORD:
    case P_LONG_SWORD:
    case P_SABER:
        w.set = "blade", w.pitch = 1.0f;
        break;
    case P_TWO_HANDED_SWORD:
        w.set = "blade", w.pitch = 0.82f;
        break;
    case P_AXE:
        w.set = "axe", w.pitch = 0.95f;
        break;
    case P_PICK_AXE:
        w.set = "pick", w.pitch = 1.0f;
        break;
    case P_CLUB:
    case P_QUARTERSTAFF:
        w.set = "blunt_wood", w.pitch = 1.0f;
        break;
    case P_MACE:
    case P_MORNING_STAR:
    case P_FLAIL:
        w.set = "blunt_metal", w.pitch = 1.0f;
        break;
    case P_HAMMER:
        w.set = "blunt_metal", w.pitch = 0.85f;
        break;
    case P_POLEARMS:
    case P_LANCE:
        w.set = "pierce", w.pitch = 0.85f;
        break;
    case P_SPEAR:
    case P_TRIDENT:
    case P_UNICORN_HORN:
        w.set = "pierce", w.pitch = 1.0f;
        break;
    case P_BOW:
    case P_CROSSBOW:
        /* arrows and bolts (melee with a bow is just a bonk) */
        if (objects[otyp].oc_skill < 0)
            w.set = "arrow", w.pitch = (skill == P_CROSSBOW) ? 0.88f : 1.0f;
        break;
    case P_SLING:
        if (objects[otyp].oc_skill < 0)
            w.set = "stone", w.pitch = 1.0f;
        break;
    case P_DART:
    case P_SHURIKEN:
        w.set = "arrow", w.pitch = 1.2f;
        break;
    case P_BOOMERANG:
        w.set = "blunt_wood", w.pitch = 1.2f;
        break;
    case P_WHIP:
        w.set = "whip", w.pitch = 1.0f;
        break;
    default:
        /* rocks and gems thrown by hand, odd things wielded */
        if (objects[otyp].oc_class == GEM_CLASS
            || objects[otyp].oc_class == ROCK_CLASS)
            w.set = "stone";
        break;
    }
    return w;
}

/* a weapon's special effect landed (it wasn't resisted) */
static void
ma_weapon_effect(int adtyp, float vol, float pan)
{
    switch (adtyp) {
    case AD_FIRE: /* "The fiery blade burns ..." */
        ma_play_group(MAG_HIT_FIRE, 0.7f * vol, ma_frand(1.0f, 1.15f), pan);
        break;
    case AD_COLD: /* "The ice-cold blade freezes ..." */
        ma_play_group(MAG_HIT_COLD, 0.75f * vol, ma_frand(0.95f, 1.1f), pan);
        break;
    case AD_ELEC: /* "Lightning strikes ..." */
        ma_play_named("wpn_layer_shock", 0.8f * vol, ma_frand(0.95f, 1.1f),
                      pan);
        break;
    case AD_DRLI: /* "The black blade draws the life from ..." */
        ma_play_named("wpn_layer_drain", 0.75f * vol, ma_frand(0.9f, 1.0f),
                      pan);
        break;
    case AD_MAGM:
        ma_play_group(MAG_HIT_MAGIC, 0.6f * vol, ma_frand(0.95f, 1.1f), pan);
        break;
    default:
        break;
    }
}

/* name buffers for the weapon sets (ma_named keeps the pointer, so these
   must stay valid: one static string per set+kind) */
static const char *
ma_wname(const char *set, const char *kind)
{
    static const struct {
        const char *set, *kind, *name;
    } names[] = {
        { "blade", "hit", "wpn_blade_hit" },
        { "blade", "swing", "wpn_blade_swing" },
        { "axe", "hit", "wpn_axe_hit" },
        { "axe", "swing", "wpn_heavy_swing" },
        { "pick", "hit", "wpn_pick_hit" },
        { "pick", "swing", "wpn_heavy_swing" },
        { "blunt_wood", "hit", "wpn_blunt_wood_hit" },
        { "blunt_wood", "swing", "wpn_heavy_swing" },
        { "blunt_metal", "hit", "wpn_blunt_metal_hit" },
        { "blunt_metal", "swing", "wpn_heavy_swing" },
        { "pierce", "hit", "wpn_pierce_hit" },
        { "pierce", "swing", "wpn_blade_swing" },
        { "whip", "hit", "wpn_whip_hit" },
        { "whip", "swing", "wpn_whip_swing" },
        { "arrow", "hit", "wpn_arrow_hit" },
        { "arrow", "swing", "wpn_arrow_miss" },
        { "stone", "hit", "wpn_stone_hit" },
        { "stone", "swing", "wpn_arrow_miss" },
    };
    int i;

    for (i = 0; i < SIZE(names); i++)
        if (!strcmp(names[i].set, set) && !strcmp(names[i].kind, kind))
            return names[i].name;
    return (const char *) 0;
}

/* an attack with a weapon or missile; returns FALSE if no weapon sound
   applies, so the caller can fall back to the generic hit/miss */
static boolean
ma_weapon_attack(const struct fx_event *ev, float vol, float pan)
{
    struct ma_wset w;
    const char *name;
    boolean hit = (ev->flags & FXF_HIT) != 0;

    if (ev->wep_otyp <= STRANGE_OBJECT) {
        /* the hero fighting bare-handed (not polymorphed) gets punches */
        if ((ev->flags & FXF_HERO_SOURCE) && !Upolyd && !uwep) {
            if (hit)
                ma_play_named("wpn_fist_hit", 0.75f * vol,
                              ma_frand(0.92f, 1.1f), pan);
            else
                ma_play_named("wpn_fist_swing", 0.5f * vol,
                              ma_frand(1.0f, 1.2f), pan);
            return TRUE;
        }
        return FALSE;
    }
    w = ma_weapon_set(ev->wep_otyp);
    if ((ev->flags & FXF_RANGED) && !hit
        && strcmp(w.set, "arrow") && strcmp(w.set, "stone"))
        return TRUE; /* a thrown dagger etc. missing: its launch whoosh
                        already played, so stay quiet */
    name = ma_wname(w.set, hit ? "hit" : "swing");
    if (!name)
        return FALSE;
    ma_play_named(name, (hit ? 0.8f : 0.55f) * vol,
                  w.pitch * ma_frand(0.94f, 1.06f), pan);
    return TRUE;
}

/* firing or throwing something */
static void
ma_launch(const struct fx_event *ev, float vol, float pan)
{
    int lskill = (ev->launcher_otyp > STRANGE_OBJECT
                  && ev->launcher_otyp < NUM_OBJECTS)
                     ? objects[ev->launcher_otyp].oc_skill : 0;

    if (lskill == P_BOW)
        ma_play_named("wpn_launch_bow", 0.8f * vol, ma_frand(0.95f, 1.08f),
                      pan);
    else if (lskill == P_CROSSBOW)
        ma_play_named("wpn_launch_crossbow", 0.8f * vol,
                      ma_frand(0.95f, 1.05f), pan);
    else if (lskill == P_SLING)
        ma_play_named("wpn_launch_sling", 0.7f * vol, ma_frand(1.0f, 1.15f),
                      pan);
    else if (ev->wep_otyp > STRANGE_OBJECT && ev->wep_otyp < NUM_OBJECTS)
        /* thrown by hand; heavier things whoosh lower */
        ma_play_named("wpn_launch_throw", 0.6f * vol,
                      objects[ev->wep_otyp].oc_weight >= 30
                          ? ma_frand(0.75f, 0.85f) : ma_frand(1.0f, 1.2f),
                      pan);
}

/*
 * Message sounds: shown messages are matched in order against these
 * wildcard patterns (pmatchi: '*' and '?', case-insensitive) and the first
 * match plays its sample set. Messages hidden by MSGTYPE=hide never get
 * here. A core sound effect that just played (ma.last_se) wins over a
 * pattern for the same moment.
 */
static struct ma_msgsnd {
    const char *pat;
    const char *snd;    /* sample set: <snd>_N.wav */
    float vol, pitch;
    DWORD last;         /* per-pattern cooldown */
} ma_msgsnds[] = {
    /* death */
    { "You die*", "msg_death", 0.9f, 1.0f, 0 },
    { "You turn to stone*", "msg_death", 0.9f, 1.0f, 0 },
    { "You have turned to stone*", "msg_death", 0.9f, 1.0f, 0 },
    { "You are a statue*", "msg_death", 0.9f, 1.0f, 0 },
    { "You suffocate*", "msg_death", 0.9f, 1.0f, 0 },
    { "Your last thought fades away*", "msg_death", 0.9f, 1.0f, 0 },
    { "You have become a green slime*", "msg_death", 0.9f, 1.0f, 0 },
    /* near death */
    { "*wailing of the Banshee*", "msg_neardeath", 0.9f, 1.0f, 0 },
    { "*howling of the CwnAnnwn*", "msg_neardeath", 0.9f, 1.0f, 0 },
    { "*all your powers will be lost*", "msg_neardeath", 0.9f, 1.0f, 0 },
    { "*your life force is running out*", "msg_neardeath", 0.9f, 1.0f, 0 },
    { "* is about to die*", "msg_neardeath", 0.9f, 1.0f, 0 },
    { "You are at Death's door*", "msg_neardeath", 0.9f, 1.0f, 0 },
    /* stoning: harsher each stage */
    { "You are slowing down*", "msg_stoning", 0.75f, 1.1f, 0 },
    { "Your limbs are stiffening*", "msg_stoning", 0.85f, 0.95f, 0 },
    { "Your limbs have turned to stone*", "msg_stoning", 0.95f, 0.8f, 0 },
    /* sliming: thicker each stage */
    { "You don't feel very well*", "msg_sliming", 0.7f, 1.15f, 0 },
    { "You suck in some slime*", "msg_sliming", 0.75f, 1.1f, 0 },
    { "You are turning a little green*", "msg_sliming", 0.75f, 1.05f, 0 },
    { "Your limbs are getting oozy*", "msg_sliming", 0.8f, 0.95f, 0 },
    { "Your skin begins to peel away*", "msg_sliming", 0.85f, 0.88f, 0 },
    { "You are turning into *", "msg_sliming", 0.9f, 0.8f, 0 },
    /* strangulation and suffocation */
    { "It constricts your throat*", "msg_choke", 0.85f, 1.0f, 0 },
    { "You find it hard to breathe*", "msg_choke", 0.8f, 1.05f, 0 },
    { "Your * is becoming constricted*", "msg_choke", 0.85f, 1.0f, 0 },
    { "You're gasping for air*", "msg_choke", 0.85f, 0.95f, 0 },
    { "Your blood is having trouble*", "msg_choke", 0.9f, 0.9f, 0 },
    { "You can no longer breathe*", "msg_choke", 0.9f, 0.88f, 0 },
    { "The pressure on your * increases*", "msg_choke", 0.9f, 0.85f, 0 },
    { "You're turning blue*", "msg_choke", 0.95f, 0.82f, 0 },
    { "Your consciousness is fading*", "msg_choke", 0.95f, 0.78f, 0 },
    /* illness and recovery */
    { "You feel deathly sick*", "msg_sick", 0.85f, 1.0f, 0 },
    { "You feel even worse*", "msg_sick", 0.85f, 0.92f, 0 },
    { "You feel much worse*", "msg_sick", 0.9f, 0.85f, 0 },
    { "What a relief*", "use_unihorn", 0.7f, 1.0f, 0 },
    { "You feel somewhat better*", "use_unihorn", 0.6f, 1.0f, 0 },
    { "You regain consciousness*", "use_unihorn", 0.6f, 0.9f, 0 },
    /* vomiting */
    { "You are feeling mildly nauseated*", "msg_queasy", 0.6f, 1.1f, 0 },
    { "You feel slightly confused*", "msg_queasy", 0.65f, 1.0f, 0 },
    { "You can't seem to think straight*", "msg_queasy", 0.7f, 0.92f, 0 },
    { "You feel incredibly sick*", "msg_queasy", 0.75f, 0.85f, 0 },
    { "You suddenly vomit*", "msg_vomit", 0.85f, 1.0f, 0 },
    /* mind flayers */
    { "You sense a faint wave of psychic energy*", "msg_psychic", 0.5f, 1.1f, 0 },
    { "A wave of psychic energy pours over you*", "msg_psychic", 0.85f, 0.9f, 0 },
    { "It locks on to your *", "msg_psychic", 0.8f, 0.8f, 0 },
    { "* tentacles suck you*", "msg_brain", 0.85f, 1.0f, 0 },
    { "Your brain is eaten*", "msg_brain", 0.95f, 0.85f, 0 },
    { "Your helmet blocks the attack*", "msg_ping", 0.6f, 1.0f, 0 },
    { "You don't seem harmed*", "msg_ping", 0.6f, 1.0f, 0 },
    /* theft, whips, grabs */
    { "* stole *", "msg_steal", 0.8f, 1.0f, 0 },
    { "* snatches *", "msg_steal", 0.8f, 1.05f, 0 },
    { "* yanks * from your *", "wpn_whip_hit", 0.8f, 1.0f, 0 },
    { "* yanks * to the *", "wpn_whip_hit", 0.8f, 1.0f, 0 },
    { "* wraps around * you're wielding*", "wpn_whip_hit", 0.75f, 0.9f, 0 },
    { "* flicks a bullwhip towards your *", "wpn_whip_swing", 0.75f, 1.0f, 0 },
    { "*swings itself around you*", "msg_grab", 0.85f, 1.0f, 0 },
    { "*grabs you*", "msg_grab", 0.8f, 1.05f, 0 },
    /* pets, omens, the Wizard */
    { "You have a sad feeling for a moment*", "msg_sad", 0.8f, 1.0f, 0 },
    { "* drowns.*", "se_water_splash_in", 0.6f, 0.9f, 0 },
    { "You feel a malignant aura surround*", "se_unholy_glow", 0.8f, 1.0f, 0 },
    { "You feel vaguely nervous*", "se_unholy_glow", 0.7f, 0.85f, 0 },
    { "You notice a black glow surrounding you*", "se_unholy_glow", 0.8f, 0.8f, 0 },
    /* skills and spells */
    { "You feel more confident in your *skills*", "msg_skill", 0.75f, 1.0f, 0 },
    { "You are now more skilled in *", "msg_skill", 0.75f, 1.1f, 0 },
    { "You are now most skilled in *", "msg_skill", 0.8f, 1.2f, 0 },
    { "You fail to cast the spell correctly*", "msg_fizzle", 0.7f, 1.0f, 0 },
    { "You don't have enough energy to cast*", "msg_fizzle", 0.6f, 0.85f, 0 },
    /* knights */
    { "You caitiff*", "se_god_angry", 0.5f, 1.2f, 0 },
    { "You joust *", "wpn_pierce_hit", 0.9f, 0.8f, 0 },
    /* shops and vaults, overheard */
    { "You hear someone counting money*", "pickup_coin", 0.45f, 0.9f, 0 },
    { "You hear Ebenezer Scrooge*", "pickup_coin", 0.45f, 0.9f, 0 },
    { "You hear the quarterback calling the play*", "pickup_coin", 0.45f, 0.9f, 0 },
    { "You hear the chime of a cash register*", "se_shop_pay", 0.35f, 1.0f, 0 },
    { "You hear Neiman and Marcus arguing*", "se_shop_pay", 0.35f, 1.0f, 0 },
    { "You hear someone cursing shoplifters*", "se_grunt", 0.4f, 0.9f, 0 },
    { "You hear the footsteps of a guard on patrol*", "msg_guard", 0.5f, 1.0f, 0 },
    /* other events */
    { "You feel a strange vibration *", "msg_vibrate", 0.8f, 1.0f, 0 },
    { "You wrest one last charge*", "wpn_layer_shock", 0.8f, 0.9f, 0 },
    { "You try to move the boulder*", "msg_boulder", 0.55f, 1.0f, 0 },
    { "* tries to move the boulder*", "msg_boulder", 0.45f, 1.0f, 0 },
    /* the game's own onomatopoeia */
    { "Boing!*", "msg_boing", 0.8f, 1.0f, 0 },
    /* a vampire's bat, wolf or fog form destroyed: it rises as itself */
    { "* and rises as *", "vampire_rise", 0.8f, 1.0f, 0 },
    { "Splat!*", "msg_splat", 0.8f, 1.0f, 0 },
    { "Splash!*", "se_water_splash_in", 0.6f, 1.1f, 0 },
    { "Plop!*", "se_dip_potion", 0.7f, 0.9f, 0 },
    { "Crash!  The ceiling*", "scroll_earth", 0.9f, 1.0f, 0 },
    { "Crash!  *secret*", "se_kick_door_it_crashes_open", 0.9f, 1.0f, 0 },
    { "Crash!*", "msg_glass", 0.8f, 1.0f, 0 },
    { "Whang!*", "trap_bear", 0.7f, 1.1f, 0 },
    { "Clang!*", "trap_bear", 0.7f, 1.2f, 0 },
    { "Whap!*", "se_force_bash", 0.7f, 1.1f, 0 },
    { "Flapp!*", "se_force_bash", 0.6f, 1.2f, 0 },
    { "Clink!*", "pickup_coin", 0.7f, 1.1f, 0 },
    { "Clonk!*", "wpn_blunt_wood_hit", 0.7f, 1.0f, 0 },
    { "THUD!*", "se_force_bash", 0.9f, 0.85f, 0 },
    { "Thump!*", "se_force_bash", 0.8f, 1.0f, 0 },
    { "Thwwpingg!*", "msg_coins_fly", 0.8f, 1.0f, 0 },
    { "Ouch!*", "hurt", 0.7f, 1.0f, 0 },
    { "*.  Ouch!*", "hurt", 0.7f, 1.0f, 0 },
    { "Snap!*", "wpn_whip_hit", 0.7f, 1.0f, 0 },
    /* intrinsics gained and lost (attribute changes such as "You
       feel strong!" already play se_stat_gain/se_stat_loss) */
    { "You feel perceptive!", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel warm!", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel full of hot air*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel very firm*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel totally together*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel cool!", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel a momentary chill*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You be chillin'*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel healthy*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel hardy!", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel especially healthy*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "Your vision becomes clear*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel insulated!", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "Your health currently feels amplified*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel grounded in reality*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel awake!", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel wide awake*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel quick!", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You seem faster*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel stealthy!", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel a strange mental acuity*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel in touch with the cosmos*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel controlled!", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel in control of yourself*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel centered in your personal space*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel very jumpy*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel diffuse*", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel sensitive!", "msg_intrinsic_gain", 0.75f, 1.0f, 0 },
    { "You feel less attractive*", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel less perceptive!", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel unaware!", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel cooler*", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel warmer*", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel paranoid*", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel less healthy!", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel less hardy!", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel a little sick!", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel vulnerable*", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You thought you saw something*", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You tawt you taw*", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel conductive!", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel tired!", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel slow!", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You seem slower*", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel slower*", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel less stealthy!", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel clumsy.", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "Your senses fail*", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel uncontrolled!", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel less jumpy*", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel less sensitive!", "msg_intrinsic_lose", 0.75f, 1.0f, 0 },
    { "You feel feverish*", "msg_sick", 0.8f, 1.05f, 0 },
    { "You feel purified*", "use_unihorn", 0.7f, 1.0f, 0 },
    /* exercise and abuse feedback */
    { "You must have been abusing*", "se_stat_loss", 0.7f, 1.0f, 0 },
    { "You must have been *", "se_stat_gain", 0.7f, 1.0f, 0 },
    { "You must be leading a healthy*", "se_stat_gain", 0.7f, 1.0f, 0 },
    { "You haven't been *", "se_stat_loss", 0.7f, 1.0f, 0 },
    /* the generic notice ping */
    { "*cockatrice corpse*", "msg_ping", 0.6f, 1.0f, 0 },
    { "*chickatrice corpse*", "msg_ping", 0.6f, 1.0f, 0 },
    { "*cockatrice egg*", "msg_ping", 0.6f, 1.0f, 0 },
    { "*Tsurugi of Muramasa*", "msg_ping", 0.6f, 1.0f, 0 },
    { "*Vorpal Blade*", "msg_ping", 0.6f, 1.0f, 0 },
    { "Perhaps that's why *", "msg_ping", 0.5f, 1.0f, 0 },
    { "*turns to flee*", "msg_ping", 0.4f, 1.1f, 0 },
    { "Sorry...*", "msg_ping", 0.5f, 0.9f, 0 },
    { "Oops...*", "msg_ping", 0.5f, 0.9f, 0 },
    { "Whoops...*", "msg_ping", 0.5f, 0.9f, 0 },
    { "*Cheapskate.*", "msg_ping", 0.5f, 0.85f, 0 },
    { "Interesting...*", "msg_ping", 0.45f, 1.0f, 0 },
};

/* a shown message: play its sound, or the distant-noise cue for
   "You hear ..." lines nothing more specific claimed */
static void
ma_message(const char *text)
{
    DWORD now = GetTickCount();
    int i;

    if (!ma.ok || !text)
        return;
    /* a core sound effect for this same moment already played */
    if (now - ma.last_se < 200)
        return;
    for (i = 0; i < SIZE(ma_msgsnds); i++) {
        struct ma_msgsnd *m = &ma_msgsnds[i];

        if (!pmatchi(m->pat, text))
            continue;
        if (now - m->last < 1500) /* the same message again: once is enough */
            return;
        m->last = now;
        ma_play_named(m->snd, m->vol, m->pitch * ma_frand(0.97f, 1.03f),
                      0.0f);
        return;
    }
    if (!strncmpi(text, "You hear ", 9) || !strncmpi(text, "You barely hear ", 16)
        || !strncmpi(text, "You dream that you hear ", 24))
        ma_play_group(MAG_HEAR, 0.4f, ma_frand(0.55f, 0.72f),
                      ma_frand(-0.5f, 0.5f));
}

/* death cry by monster class: death_<kind>_N.wav; silent if none */
static void
ma_death_cry(int mndx, float vol, float pan)
{
    const char *name;
    float pitch = ma_frand(0.9f, 1.1f);
    const struct permonst *ptr;

    if (mndx < LOW_PM || mndx >= NUMMONS)
        return;
    ptr = &mons[mndx];
    switch (ptr->mlet) {
    case S_DOG:
        name = "death_canine";
        break;
    case S_FELINE:
        name = "death_feline";
        break;
    case S_ANT:
    case S_SPIDER:
    case S_XAN:
        name = "death_bug";
        break;
    case S_SNAKE:
    case S_LIZARD:
    case S_NAGA:
        name = "death_snake";
        break;
    case S_BAT:
        name = "death_bat"; /* squeaks and wings, not a bird's cry */
        break;
    case S_COCKATRICE:
        name = "death_bird";
        break;
    case S_BLOB:
    case S_JELLY:
    case S_PUDDING:
    case S_FUNGUS:
        name = "death_slime";
        break;
    case S_ZOMBIE:
    case S_MUMMY:
    case S_LICH:
    case S_VAMPIRE:
    case S_WRAITH:
    case S_GHOST:
        name = "death_undead";
        break;
    case S_DRAGON:
        name = "death_dragon";
        pitch = ma_frand(0.75f, 0.9f);
        break;
    case S_HUMAN:
    case S_HUMANOID:
    case S_ORC:
    case S_GIANT:
    case S_OGRE:
    case S_TROLL:
        /* a deep, heavy grunt, and the body hitting the floor */
        if (ma_named_count("death_giant")) {
            ma_play_named("death_giant", 0.9f * vol, ma_frand(0.92f, 1.04f),
                          pan);
            return;
        }
        /*FALLTHRU*/
    case S_GNOME:
    case S_KOBOLD:
        name = "death_humanoid";
        if (bigmonst(ptr))
            pitch = ma_frand(0.75f, 0.88f); /* deeper for big ones */
        break;
    case S_QUADRUPED:
    case S_UNICORN:
    case S_YETI:
    case S_RODENT:
    case S_CENTAUR:
        name = "death_beast";
        if (verysmall(ptr))
            pitch = ma_frand(1.2f, 1.4f); /* squeakier rats */
        break;
    case S_DEMON:
    case S_IMP:
    case S_UMBER:
    case S_EYE:
        name = "death_monster";
        break;
    default:
        return; /* the thud alone */
    }
    ma_play_named(name, 0.7f * vol, pitch, pan);
}

/* quieter with distance from the hero, panned toward the side it's on */
static void
ma_spatial(coordxy x, coordxy y, float *volume, float *pan)
{
    int dx = x - u.ux, dy = y - u.uy;
    int d = max(abs(dx), abs(dy));

    *volume = 1.0f / (1.0f + 0.12f * (float) d);
    *pan = (float) dx / 12.0f;
    if (*pan < -0.8f)
        *pan = -0.8f;
    if (*pan > 0.8f)
        *pan = 0.8f;
}

/* a boom for the explosion's element: fire and cold have their own,
   everything else is the big rumble, with the element's hit sound laid
   over it. Only heard: quieter, duller and a little lower. */
static void
ma_explosion(const struct fx_event *ev, float vol, float pan)
{
    boolean unseen = (ev->flags & FXF_UNSEEN) != 0;
    float v = (unseen ? 0.45f : 1.0f) * max(vol, 0.35f);
    float pitch = ma_frand(0.94f, 1.06f) * (unseen ? 0.85f : 1.0f);

    switch (ev->adtyp) {
    case AD_FIRE:
        if (ma_named_count("expl_fire")) {
            ma_play_named("expl_fire", 0.95f * v, pitch, pan);
            return;
        }
        break;
    case AD_COLD:
        if (ma_named_count("expl_cold")) {
            ma_play_named("expl_cold", 0.95f * v, pitch, pan);
            return;
        }
        break;
    case AD_ELEC:
        ma_play_named("wpn_layer_shock", 0.8f * v, pitch, pan);
        break;
    case AD_DRST:
        ma_play_named("hit_poison", 0.8f * v, pitch * 0.9f, pan);
        break;
    case AD_ACID:
        ma_play_named("hit_acid", 0.8f * v, pitch * 0.9f, pan);
        break;
    }
    ma_play_named("se_kaboom", 0.9f * v, pitch, pan);
}

/* the sound of each beam wand hitting something */
static void
ma_beam_hit(int look, float vol, float pan)
{
    static const struct {
        int otyp;
        const char *name;
        float volume;
    } beams[] = {
        { WAN_CANCELLATION, "wand_cancel", 0.6f },  /* magic drained away */
        { WAN_SLOW_MONSTER, "wand_slow", 0.45f },   /* heavy falling wobble */
        { WAN_SPEED_MONSTER, "wand_speed", 0.6f },  /* quick rising zip */
        { WAN_POLYMORPH, "wand_poly", 0.42f },      /* bending warble */
        { WAN_PROBING, "wand_probe", 0.38f },       /* scanning blips */
        /* (opening and locking: doors and boxes give the key-turn sounds
           se_klick / se_klunk from the core instead) */
        { WAN_MAKE_INVISIBLE, "wand_invis", 0.5f }, /* shimmer fading out */
        { WAN_UNDEAD_TURNING, "wand_undead", 0.5f }, /* holy chord */
    };
    int i;

    for (i = 0; i < SIZE(beams); i++)
        if (beams[i].otyp == look) {
            ma_play_named(beams[i].name, beams[i].volume * vol,
                          ma_frand(0.95f, 1.05f), pan);
            return;
        }
}

/* a non-directional wand going off around the hero (and the digging
   beam); an unidentified one ('look' 0) just fizzes */
static void
ma_wand_self(int look)
{
    const char *name = 0;
    float v = 0.55f;

    switch (look) {
    case WAN_LIGHT:
        name = "wand_light";
        break;
    case WAN_SECRET_DOOR_DETECTION:
        name = "wand_secret";
        v = 0.5f;
        break;
    case WAN_ENLIGHTENMENT:
        name = "wand_enlight";
        break;
    case WAN_STASIS:
        name = "wand_stasis";
        break;
    case WAN_DIGGING:
        name = "wand_dig";
        v = 0.8f;
        break;
    case 0:
        name = "wand_fizz";
        v = 0.4f;
        break;
    default:
        return;
    }
    ma_play_named(name, v, ma_frand(0.97f, 1.03f), 0.0f);
}

/* ---- level ambience ----------------------------------------------- */

/* Gehennom's maze levels come in flavours: mostly ice, mostly water,
   walled with iron bars, or fire. Worked out once per level from its
   terrain. */
#define MA_HELL_FIRE 0
#define MA_HELL_ICE 1
#define MA_HELL_WATER 2
#define MA_HELL_BARS 3

static int
ma_hell_flavour(void)
{
    static d_level seen;
    static int flavour = MA_HELL_FIRE;
    static boolean have;
    int x, y, ice = 0, water = 0, bars = 0;

    if (have && on_level(&seen, &u.uz))
        return flavour;
    for (x = 1; x < COLNO; x++)
        for (y = 0; y < ROWNO; y++) {
            int typ = levl[x][y].typ;

            if (typ == ICE)
                ice++;
            else if (IS_POOL(typ) || typ == WATER)
                water++;
            else if (typ == IRONBARS)
                bars++;
        }
    flavour = (ice >= 80 && ice >= water) ? MA_HELL_ICE
              : (water >= 80) ? MA_HELL_WATER
              : (bars >= 30) ? MA_HELL_BARS : MA_HELL_FIRE;
    seen = u.uz;
    have = TRUE;
    return flavour;
}

/* the loop for the level the hero is on, and how loud it sits */
static const char *
ma_level_ambience(float *vol)
{
    d_level *z = &u.uz;

    *vol = 0.30f;
    if (Is_astralevel(z))
        return "amb_astral";
    if (Is_earthlevel(z))
        return "amb_earth";
    if (Is_airlevel(z))
        return "amb_air";
    if (Is_firelevel(z))
        return "amb_fire";
    if (Is_waterlevel(z))
        return "amb_water";
    if (isok(u.ux, u.uy) && In_W_tower(u.ux, u.uy, z))
        return "amb_mystic"; /* the Wizard's Tower: something arcane */
    if (In_V_tower(z))
        return "amb_haunted"; /* a haunted castle */
    if (Is_valley(z))
        return "amb_valley"; /* cold, still and full of the dead */
    if (In_hell(z) || Is_sanctum(z)) {
        int f = Is_sanctum(z) ? MA_HELL_FIRE : ma_hell_flavour();

        *vol = 0.42f; /* it should be felt */
        if (f == MA_HELL_ICE)
            return "amb_frozen";
        if (f == MA_HELL_WATER)
            return "amb_flood";
        return "amb_hell"; /* fire (barred levels add chains) */
    }
    if (Is_stronghold(z))
        return "amb_castle";
    if (Is_medusa_level(z))
        return "amb_sea";
    if (Is_knox(z))
        return "amb_vault"; /* Fort Ludios: a vast guarded vault */
    if (In_quest(z))
        return Is_qstart(z) ? "amb_quest" : "amb_deep";
    if (In_sokoban(z))
        return "amb_tower"; /* a closed stone tower */
    if (In_mines(z)) {
        if (Is_mineend_level(z))
            return "amb_deep";
        if (svl.level.flags.has_town)
            return "amb_town";
        return "amb_cave";
    }
    if (Is_oracle_level(z)) {
        *vol = 0.22f;
        return "amb_tower";
    }
    return "amb_cave";
}

/* start, change or stop the ambience to match where the hero is; a new
   level crossfades over a couple of seconds */
static void
ma_ambience_update(unsigned fade_ms)
{
    char path[MAX_PATH + 64];
    const char *name;
    float vol;

    if (!ma.ok)
        return;
    if (!iflags.fx_ambience || !iflags.sounds || Deaf || !u.uz.dlevel) {
        nhma_ambience((const char *) 0, 0.0f, 1500);
        return;
    }
    name = ma_level_ambience(&vol);
    Snprintf(path, sizeof path, "%s%s.flac", ma.dir, name);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        nhma_ambience((const char *) 0, 0.0f, 1500);
        return;
    }
    nhma_ambience(path, vol, fade_ms);
}

/* now and then, a distant sound that belongs to the place: pickaxes in
   the Mines, fire and groans in Gehennom, wails in the Valley, howls
   and creaks in Vlad's Tower, chimes in the Wizard's Tower, creaks in
   Sokoban. 'one_in' is the chance per step. */
struct ma_acc {
    const char *name;
    float vol, plo, phi; /* volume, pitch range */
};

static void
ma_ambience_accents(void)
{
    static const struct ma_acc mines[] = { { "se_dig_rock", 0.12f, 0.8f, 0.92f } },
      hell[] = { { "acc_fire", 0.4f, 0.85f, 1.05f },
                 { "acc_fire", 0.4f, 0.7f, 0.9f },
                 { "acc_lava", 0.45f, 0.85f, 1.05f },
                 { "acc_lava", 0.45f, 0.75f, 0.95f },
                 { "acc_rumble", 0.5f, 0.85f, 1.05f },
                 { "acc_groan", 0.3f, 0.8f, 1.0f },
                 { "acc_wail", 0.18f, 0.8f, 0.95f } },
      hellbars[] = { { "acc_fire", 0.4f, 0.85f, 1.05f },
                     { "acc_lava", 0.45f, 0.85f, 1.05f },
                     { "acc_rumble", 0.5f, 0.85f, 1.05f },
                     { "acc_chain", 0.5f, 0.85f, 1.05f },
                     { "acc_chain", 0.45f, 0.7f, 0.85f },
                     { "acc_groan", 0.3f, 0.8f, 1.0f } },
      frozen[] = { { "acc_ice", 0.5f, 0.85f, 1.1f },
                   { "acc_ice", 0.45f, 0.7f, 0.9f },
                   { "acc_rumble", 0.35f, 0.85f, 1.0f },
                   { "acc_wail", 0.14f, 0.85f, 1.0f } },
      flood[] = { { "acc_steam", 0.45f, 0.9f, 1.1f },
                  { "acc_lava", 0.45f, 1.15f, 1.35f },
                  { "acc_rumble", 0.4f, 0.85f, 1.0f },
                  { "acc_groan", 0.25f, 0.8f, 1.0f } },
      valley[] = { { "acc_bell", 0.3f, 0.97f, 1.03f },
                   { "acc_crow", 0.18f, 0.9f, 1.05f } },
      haunted[] = { { "acc_howl", 0.16f, 0.9f, 1.05f },
                    { "acc_owl", 0.14f, 0.9f, 1.0f },
                    { "acc_bat", 0.14f, 0.9f, 1.15f },
                    { "acc_creak", 0.2f, 0.85f, 1.05f } },
      mystic[] = { { "acc_chime", 0.18f, 0.9f, 1.2f } },
      tower[] = { { "acc_creak", 0.12f, 0.8f, 0.95f } },
      vault[] = { { "acc_clank", 0.3f, 0.9f, 1.05f },
                  { "acc_march", 0.32f, 0.95f, 1.05f },
                  { "acc_coins", 0.22f, 0.9f, 1.1f },
                  { "acc_dragon", 0.25f, 0.9f, 1.05f } };
    const struct ma_acc *set = 0;
    int n = 0, one_in = 0, k;
    d_level *z = &u.uz;

    if (!iflags.fx_ambience || !iflags.sounds || Deaf || !u.uz.dlevel)
        return;
#define PICK(arr, odds) (set = (arr), n = SIZE(arr), one_in = (odds))
    if (isok(u.ux, u.uy) && In_W_tower(u.ux, u.uy, z))
        PICK(mystic, 14);
    else if (In_V_tower(z))
        PICK(haunted, 16);
    else if (Is_valley(z))
        PICK(valley, 28); /* a quiet graveyard: rarely anything */
    else if (Is_sanctum(z))
        PICK(hell, 7);
    else if (In_hell(z)) {
        switch (ma_hell_flavour()) {
        case MA_HELL_ICE:
            PICK(frozen, 9);
            break;
        case MA_HELL_WATER:
            PICK(flood, 9);
            break;
        case MA_HELL_BARS:
            PICK(hellbars, 7);
            break;
        default:
            PICK(hell, 7);
            break;
        }
    }
    else if (In_sokoban(z))
        PICK(tower, 60);
    else if (Is_knox(z))
        PICK(vault, 18);
    else if (In_mines(z) && !svl.level.flags.has_town
             && !Is_mineend_level(z))
        PICK(mines, 45);
#undef PICK
    if (!set || ma_rand((unsigned) one_in) != 0)
        return;
    k = (int) ma_rand((unsigned) n);
    ma_play_named(set[k].name, set[k].vol, ma_frand(set[k].plo, set[k].phi),
                  ma_frand(-0.75f, 0.75f));
}

/* follow the player's volume options (cheap; done on every event) */
void
ma_apply_volumes(void)
{
    nhma_set_volumes((float) iflags.fx_volume / 100.0f,
                     (float) iflags.fx_ambience_volume / 100.0f);
}

/* a short sample at the current effects volume, after it's changed */
void
ma_volume_preview(void)
{
    ma_apply_volumes();
    ma_play_named("pickup_coin", 0.7f, 1.0f, 0.0f);
}

static void
ma_fx_event(const struct fx_event *ev)
{
    float vol, pan;

    ma_apply_volumes();
    ma_spatial(ev->x, ev->y, &vol, &pan);
    switch (ev->type) {
    case FXE_ATTACK:
        if (!(ev->flags & FXF_HIT))
            break; /* the miss gets its own event */
        if (ev->flags & FXF_HERO_TARGET) {
            /* getting hit: the weapon, if any, plus a heavy punch */
            (void) ma_weapon_attack(ev, 0.8f, 0.0f);
            if (ma.nvar[MAG_HURT])
                ma_play_group(MAG_HURT, 0.75f, ma_frand(0.9f, 1.08f), 0.0f);
            else
                ma_play_group(MAG_HIT, 0.9f, ma_frand(0.78f, 0.9f), 0.0f);
        } else if (!ma_weapon_attack(ev, vol, pan)) {
            /* claws, bites and other natural attacks */
            ma_play_group(MAG_HIT, 0.75f * vol, ma_frand(0.95f, 1.15f), pan);
        }
        break;
    case FXE_LAUNCH:
        ma_launch(ev, vol, pan);
        break;
    case FXE_APPLY:
        ma_apply(ev->otyp, ev->amount != 0);
        break;
    case FXE_READ:
        if (ev->adtyp == 0) /* (1 is its effect: visual only) */
            ma_read(ev->otyp, ev->amount != 0);
        break;
    case FXE_APPEAR:
        if (ev->mndx == PM_WIZARD_OF_YENDOR) {
            /* he teleports constantly; don't let it become a laugh track */
            static DWORD last_laugh = 0;
            DWORD now = GetTickCount();

            if (!last_laugh || now - last_laugh > 8000) {
                last_laugh = now;
                ma_play_named("wizard_laugh", 0.9f, ma_frand(0.97f, 1.05f),
                              pan);
            }
        }
        break;
    case FXE_WEPFX:
        ma_weapon_effect(ev->adtyp,
                         (ev->flags & FXF_HERO_TARGET) ? 1.0f : vol, pan);
        break;
    case FXE_DAMAGE:
        /* rays, spells and explosions have no attack event; give the
           kinds we have samples for their own sound */
        if (ev->adtyp == AD_FIRE)
            ma_play_group(MAG_HIT_FIRE, 0.7f * vol, ma_frand(0.92f, 1.08f),
                          pan);
        else if (ev->adtyp == AD_COLD)
            ma_play_group(MAG_HIT_COLD, 0.8f * vol, ma_frand(0.9f, 1.1f),
                          pan);
        else if (ev->adtyp == AD_MAGM)
            ma_play_group(MAG_HIT_MAGIC, 0.6f * vol, ma_frand(0.95f, 1.1f),
                          pan);
        else if (ev->adtyp == AD_ELEC)
            ma_play_named("wpn_layer_shock", 0.75f * vol,
                          ma_frand(0.95f, 1.1f), pan);
        else if (ev->adtyp == AD_DRST || ev->adtyp == AD_DRDX
                 || ev->adtyp == AD_DRCO)
            ma_play_named("hit_poison", 0.7f * vol, ma_frand(0.92f, 1.08f),
                          pan);
        else if (ev->adtyp == AD_ACID)
            ma_play_named("hit_acid", 0.75f * vol, ma_frand(0.92f, 1.08f),
                          pan);
        break;
    case FXE_MISS:
        if (!ma_weapon_attack(ev, vol, pan))
            ma_play_group(MAG_MISS, 0.55f * vol, ma_frand(0.92f, 1.12f),
                          pan);
        break;
    case FXE_DEATH:
        /* the body falling, plus a last cry that depends on what died */
        ma_play_group(MAG_DEATH, 0.9f * vol, ma_frand(0.88f, 1.05f), pan);
        ma_death_cry(ev->mndx, vol, pan);
        break;
    case FXE_SWAP:
        /* skip while travelling: the pet gets swapped over and over */
        if (!(ev->flags & FXF_BULK))
            ma_play_group(MAG_SWAP, 0.5f, ma_frand(0.95f, 1.15f), pan);
        break;
    case FXE_BUMP:
        ma_play_group(MAG_BUMP, 0.6f, ma_frand(0.9f, 1.1f), pan);
        break;
    case FXE_PICKUP:
        ma_pickup(ev->objclass, ev->otyp, (ev->flags & FXF_DROP) != 0,
                  ev->amount);
        break;
    case FXE_TRAP:
        ma_trap(ev->ttyp, (ev->flags & FXF_HERO_TARGET) ? 1.0f : vol, pan);
        break;
    case FXE_EAT:
        ma_eat(ev->otyp, ev->texture);
        break;
    case FXE_HEAR:
        /* the distant-noise cue now plays from FXE_MESSAGE, once the
           message text is known (so specific sounds can claim it) */
        break;
    case FXE_MESSAGE:
        ma_message(ev->text);
        break;
    case FXE_EXPLODE:
        ma_explosion(ev, vol, pan);
        break;
    case FXE_STATUS:
        /* a monster dropping off to sleep (sleep ray, spell, gas) */
        if ((ev->st_gain & FXA_ASLEEP) && !(ev->flags & FXF_HERO_TARGET))
            ma_play_named("fall_asleep", 0.45f * vol, ma_frand(0.97f, 1.03f),
                          pan);
        break;
    case FXE_WAND:
        /* a beam striking its target; only once the wand is identified
           ('otyp' is 0 before), so the sound never gives it away */
        if (ev->adtyp == FXW_HIT)
            ma_beam_hit(ev->otyp, vol, pan);
        else if (ev->adtyp == FXW_SELF)
            ma_wand_self(ev->otyp);
        break;
    case FXE_QUAFF:
        if (ev->adtyp == 0) {
            /* the cork and a couple of gulps */
            ma_play_named("quaff", 0.75f, ma_frand(0.96f, 1.04f), 0.0f);
        } else {
            switch (ev->otyp) {
            case POT_HEALING:
            case POT_EXTRA_HEALING:
            case POT_FULL_HEALING:
                ma_play_named("potion_heal", 0.55f,
                              ev->otyp == POT_FULL_HEALING ? 0.9f
                              : ev->otyp == POT_EXTRA_HEALING ? 0.95f
                                                              : 1.0f,
                              0.0f);
                break;
            case POT_GAIN_ENERGY:
                ma_play_named("potion_heal", 0.45f, 1.25f, 0.0f);
                break;
            case POT_ENLIGHTENMENT:
                ma_play_named("wand_enlight", 0.5f, 1.0f, 0.0f);
                break;
            case POT_SPEED:
                ma_play_named("wand_speed", 0.5f, 1.0f, 0.0f);
                break;
            case POT_POLYMORPH:
                ma_play_named("wand_poly", 0.4f, 1.0f, 0.0f);
                break;
            case POT_INVISIBILITY:
                ma_play_named("wand_invis", 0.45f, 1.0f, 0.0f);
                break;
            default:
                break;
            }
        }
        break;
    case FXE_UNIHORN:
        /* cured / nothing / cursed; the cure messages that follow
           ("You feel somewhat better") don't play over it */
        ma.last_se = GetTickCount();
        if (ev->amount > 0)
            ma_play_named("unihorn_cure", 0.55f, ma_frand(0.97f, 1.03f), 0.0f);
        else if (ev->amount == 0)
            ma_play_named("unihorn_none", 0.5f, ma_frand(0.95f, 1.05f), 0.0f);
        else
            ma_play_named("unihorn_bad", 0.45f, ma_frand(0.96f, 1.04f), 0.0f);
        break;
    case FXE_DETECT:
        if (ev->flags & FXF_HERO_TARGET)
            ma_play_named(ev->adtyp == FXD_MONSTER ? "detect_monster"
                                                   : "detect_object",
                          0.55f, 1.0f, 0.0f);
        break;
    case FXE_ARRIVE:
        ma_ambience_update(2500);
        break;
    case FXE_UI:
        if (ev->text && !strcmp(ev->text, "inventory"))
            ma_play_named("ui_inventory", 0.8f, ma_frand(0.95f, 1.05f),
                          0.0f);
        break;
    case FXE_MOVE:
        /* the hero stepping: keep the ambience right (the option, deafness,
           the Wizard's Tower) and maybe a distant sound */
        if (ev->flags & FXF_HERO_TARGET) {
            ma_ambience_update(1500);
            ma_ambience_accents();
        }
        break;
    case FXE_TELEPORT:
        /* a shimmering whoosh, where it was seen (or heard) to go */
        if (ev->flags & FXF_HERO_TARGET) {
            ma_play_named("teleport", 0.85f, ma_frand(0.96f, 1.04f), 0.0f);
        } else {
            float v2, p2;

            ma_spatial(ev->sx, ev->sy, &v2, &p2);
            if (!(ev->amount & 1) && (ev->amount & 2))
                v2 = vol, p2 = pan; /* only the arrival was seen */
            ma_play_named("teleport", 0.7f * v2, ma_frand(1.0f, 1.12f), p2);
        }
        break;
    case FXE_IMPACT:
        /* striking / force bolt: a force whump; a softer, higher one
           under the "Boing!" when it bounces off */
        if (ev->flags & FXF_RESISTED)
            ma_play_named("strike_hit", 0.4f * vol, ma_frand(1.2f, 1.3f),
                          pan);
        else
            ma_play_named("strike_hit", 0.9f * vol, ma_frand(0.92f, 1.08f),
                          pan);
        break;
    default:
        break;
    }
}

static void
ma_soundeffect(char *desc, int32_t seid, int32_t volume)
{
    float vol = (float) (volume > 0 ? volume : 50) / 100.0f;

    ma_apply_volumes(); /* NetHack Modern: the player's volume options */
    switch (seid) {
    case se_door_open:
    case se_door_unlock_and_open:
        if (ma.ok && ma.nvar[MAG_DOOR_OPEN]) {
            ma.last_se = GetTickCount();
            ma_play_group(MAG_DOOR_OPEN, max(vol, 0.6f),
                          ma_frand(0.95f, 1.05f), 0.0f);
            return;
        }
        break;
    case se_door_close:
        if (ma.ok && ma.nvar[MAG_DOOR_CLOSE]) {
            ma.last_se = GetTickCount();
            ma_play_group(MAG_DOOR_CLOSE, max(vol, 0.6f),
                          ma_frand(0.95f, 1.05f), 0.0f);
            return;
        }
        break;
    default:
        break;
    }
    /* any other effect with a se_<name>_N.wav sample of its own */
    if (ma.ok && seid > se_zero_invalid && seid < number_of_se_entries
        && iflags.sounds && !Deaf) {
        char path[MAX_PATH + 64];
        int n;

        if (ma.se_nvar[seid] < 0) {
            for (n = 1; n <= MA_MAXVAR; n++) {
                Snprintf(path, sizeof path, "%sse_%s_%d.wav", ma.dir,
                         ma_se_names[seid].name, n);
                if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
                    break;
            }
            ma.se_nvar[seid] = (signed char) (n - 1);
        }
        if (ma.se_nvar[seid] > 0) {
            Snprintf(path, sizeof path, "%sse_%s_%u.wav", ma.dir,
                     ma_se_names[seid].name,
                     1 + ma_rand((unsigned) ma.se_nvar[seid]));
            ma.last_se = GetTickCount();
            {
                char note[MAX_PATH + 96];
                int started = nhma_play(path, max(vol, 0.3f),
                                        ma_frand(0.95f, 1.05f), 0.0f);

                Snprintf(note, sizeof note, "miniaudio: play %s -> %s", path,
                         started ? "started" : "FAILED");
                fx_debug_log(note);
            }
            return;
        }
    }
#ifdef SND_LIB_WINDSOUND
    if (windsound_procs.sound_soundeffect)
        (*windsound_procs.sound_soundeffect)(desc, seid, volume);
#else
    nhUse(desc);
#endif
}

/* the rest: hand to the Windows soundlib if present */
static void
ma_achievement(schar ach1, schar ach2, int32_t repeat)
{
    /* our own level up / level drain; everything else goes to windsound */
    if (ach1 == 0 && ach2 == sa2_xplevelup
        && ma_named_count("lvl_up") > 0) {
        ma_play_named("lvl_up", 0.8f, 1.0f, 0.0f);
        return;
    }
    if (ach1 == 0 && ach2 == sa2_xpleveldown
        && ma_named_count("lvl_down") > 0) {
        ma_play_named("lvl_down", 0.85f, 1.0f, 0.0f);
        return;
    }
#ifdef SND_LIB_WINDSOUND
    if (windsound_procs.sound_achievement)
        (*windsound_procs.sound_achievement)(ach1, ach2, repeat);
#else
    nhUse(ach1), nhUse(ach2), nhUse(repeat);
#endif
}

static void
ma_hero_playnotes(int32_t instrument, const char *str, int32_t volume)
{
#ifdef SND_LIB_WINDSOUND
    if (windsound_procs.sound_hero_playnotes)
        (*windsound_procs.sound_hero_playnotes)(instrument, str, volume);
#else
    nhUse(instrument), nhUse(str), nhUse(volume);
#endif
}

static void
ma_play_usersound(char *filename, int32_t volume, int32_t idx)
{
#ifdef SND_LIB_WINDSOUND
    if (windsound_procs.sound_play_usersound)
        (*windsound_procs.sound_play_usersound)(filename, volume, idx);
#else
    nhUse(filename), nhUse(volume), nhUse(idx);
#endif
}

static void
ma_ambience(int32_t ambienceid, int32_t action, int32_t proximity)
{
#ifdef SND_LIB_WINDSOUND
    if (windsound_procs.sound_ambience)
        (*windsound_procs.sound_ambience)(ambienceid, action, proximity);
#else
    nhUse(ambienceid), nhUse(action), nhUse(proximity);
#endif
}

static void
ma_verbal(char *text, int32_t gender, int32_t tone, int32_t vol,
          int32_t moreinfo)
{
#ifdef SND_LIB_WINDSOUND
    if (windsound_procs.sound_verbal)
        (*windsound_procs.sound_verbal)(text, gender, tone, vol, moreinfo);
#else
    nhUse(text), nhUse(gender), nhUse(tone), nhUse(vol), nhUse(moreinfo);
#endif
}

#endif /* SND_LIB_MINIAUDIO */

/*masound.c*/
