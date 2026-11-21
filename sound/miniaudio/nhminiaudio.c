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
    ma_sound_set_volume(&voices[slot], volume);
    ma_sound_set_pitch(&voices[slot], pitch);
    ma_sound_set_pan(&voices[slot], pan);
    if (ma_sound_start(&voices[slot]) != MA_SUCCESS) {
        ma_sound_uninit(&voices[slot]);
        voice_used[slot] = 0;
        return 0;
    }
    return 1;
}
