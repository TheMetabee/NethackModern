/* NetHack Modern nhminiaudio.c */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * The only file that compiles miniaudio. It deliberately does not
 * include hack.h; see nhminiaudio.h.
 */

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio.h"
#include "nhminiaudio.h"

#define NHMA_VOICES 32 /* sounds that can play at once */

static ma_engine engine;
static int engine_ok = 0;
static ma_sound voices[NHMA_VOICES];
static int voice_used[NHMA_VOICES];
static float fx_scale = 1.0f, amb_scale = 1.0f; /* player's volumes */
static float amb_base[2];                       /* each loop's own level */
static ma_sound amb[2];
static int amb_used[2], amb_cur = -1;
static char amb_path[2][512];

int
nhma_init(void)
{
    if (engine_ok)
        return 1;
    if (ma_engine_init(NULL, &engine) != MA_SUCCESS)
        return 0;
    engine_ok = 1;
    return 1;
}

void
nhma_shutdown(void)
{
    int i;

    if (!engine_ok)
        return;
    for (i = 0; i < NHMA_VOICES; i++)
        if (voice_used[i]) {
            ma_sound_uninit(&voices[i]);
            voice_used[i] = 0;
        }
    for (i = 0; i < 2; i++)
        if (amb_used[i]) {
            ma_sound_uninit(&amb[i]);
            amb_used[i] = 0;
        }
    amb_cur = -1;
    ma_engine_uninit(&engine);
    engine_ok = 0;
}

void
nhma_set_master_volume(float volume)
{
    if (engine_ok)
        (void) ma_engine_set_volume(&engine, volume);
}

int
nhma_play(const char *path, float volume, float pitch, float pan)
{
    int i, slot = -1;

    if (!engine_ok || !path)
        return 0;
    /* recycle voices that have finished; take the first free one */
    for (i = 0; i < NHMA_VOICES; i++) {
        if (voice_used[i] && !ma_sound_is_playing(&voices[i])) {
            ma_sound_uninit(&voices[i]);
            voice_used[i] = 0;
        }
        if (!voice_used[i] && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return 0; /* everything busy: drop this one */
    if (ma_sound_init_from_file(&engine, path, MA_SOUND_FLAG_DECODE, NULL,
                                NULL, &voices[slot]) != MA_SUCCESS)
        return 0;
    voice_used[slot] = 1;
    ma_sound_set_volume(&voices[slot], volume * fx_scale);
    ma_sound_set_pitch(&voices[slot], pitch);
    ma_sound_set_pan(&voices[slot], pan);
    if (ma_sound_start(&voices[slot]) != MA_SUCCESS) {
        ma_sound_uninit(&voices[slot]);
        voice_used[slot] = 0;
        return 0;
    }
    return 1;
}

/* NetHack Modern: one looping ambience at a time, streamed from disk.
   Changing it fades the old loop out while the new one fades in; a NULL
   path fades out to silence. Two slots, so the old one can finish its
   fade while the new one starts. */

void
nhma_ambience(const char *path, float volume, unsigned fade_ms)
{
    int next;

    if (!engine_ok)
        return;
    /* same loop already playing: just follow the volume */
    if (path && amb_cur >= 0 && !strcmp(amb_path[amb_cur], path)) {
        amb_base[amb_cur] = volume;
        ma_sound_set_volume(&amb[amb_cur], volume * amb_scale);
        return;
    }
    if (!path && amb_cur < 0)
        return;
    /* fade the current one out (it keeps the slot until next time) */
    if (amb_cur >= 0)
        ma_sound_stop_with_fade_in_milliseconds(&amb[amb_cur], fade_ms);
    if (!path) {
        amb_cur = -1;
        return;
    }
    next = (amb_cur == 0) ? 1 : 0;
    if (amb_used[next]) {
        ma_sound_uninit(&amb[next]);
        amb_used[next] = 0;
    }
    if (ma_sound_init_from_file(&engine, path, MA_SOUND_FLAG_STREAM, NULL,
                                NULL, &amb[next]) != MA_SUCCESS) {
        amb_cur = -1;
        return;
    }
    amb_used[next] = 1;
    (void) strncpy(amb_path[next], path, sizeof amb_path[next] - 1);
    amb_path[next][sizeof amb_path[next] - 1] = '\0';
    ma_sound_set_looping(&amb[next], MA_TRUE);
    amb_base[next] = volume;
    ma_sound_set_volume(&amb[next], volume * amb_scale);
    ma_sound_set_fade_in_milliseconds(&amb[next], 0.0f, 1.0f, fade_ms);
    if (ma_sound_start(&amb[next]) != MA_SUCCESS) {
        ma_sound_uninit(&amb[next]);
        amb_used[next] = 0;
        amb_cur = -1;
        return;
    }
    amb_cur = next;
}

/* NetHack Modern: the player's volumes for effects and for ambience,
   0.0 .. 1.0; a playing loop follows at once */
void
nhma_set_volumes(float effects, float ambience)
{
    fx_scale = effects;
    if (ambience != amb_scale) {
        amb_scale = ambience;
        if (engine_ok && amb_cur >= 0)
            ma_sound_set_volume(&amb[amb_cur],
                                amb_base[amb_cur] * amb_scale);
    }
}
