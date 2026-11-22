/* NetHack Modern nhminiaudio.h */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * Thin wrapper around miniaudio (sound/miniaudio/miniaudio.h, public
 * domain / MIT-0). It keeps miniaudio's large header, and the Windows
 * headers it pulls in, out of every file that includes hack.h.
 */

#ifndef NHMINIAUDIO_H
#define NHMINIAUDIO_H

int nhma_init(void);                /* 1 on success */
void nhma_shutdown(void);
void nhma_set_master_volume(float); /* 0.0 .. 1.0 */
/* play a sound file once; volume 0..1, pitch 1.0 = normal,
   pan -1 (left) .. +1 (right); returns 1 if it started */
int nhma_play(const char *path, float volume, float pitch, float pan);
/* NetHack Modern: a looping ambience, crossfaded; NULL fades it out */
void nhma_ambience(const char *path, float volume, unsigned fade_ms);
/* NetHack Modern: the player's effect and ambience volumes, 0.0 .. 1.0 */
void nhma_set_volumes(float effects, float ambience);
/* NetHack Modern: music, one track at a time streamed from disk (the
   menu music, looping; the death tune, once).  A path fades it in
   (resuming where it was stopped, if it's the same track); NULL fades it
   out. */
void nhma_music(const char *path, float volume, unsigned fade_ms, int loop);

#endif /* NHMINIAUDIO_H */
