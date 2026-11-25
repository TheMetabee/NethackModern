/* NetHack Modern fxupdate.h */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * Checking GitHub for a newer NetHack Modern, shared by the SDL and
 * NetHackW front ends.  The asking happens on a thread of its own, so
 * fxupdate.c uses nothing of the game's (its allocator, messages and
 * globals are not made for two threads): the front end hands it what it
 * needs, starts a check, and looks at fxu_status() now and then (the
 * title screens redraw every half second anyway).  Only once it says
 * FXU_DONE may the result be read.
 */

#ifndef FXUPDATE_H
#define FXUPDATE_H

#define FXU_IDLE 0 /* nothing asked yet */
#define FXU_BUSY 1 /* the thread is asking */
#define FXU_DONE 2 /* fxu_get() has the answer */

struct fxu_result {
    int ok;          /* there is an answer: from GitHub just now, or the
                      * one kept from an earlier check */
    int reached;     /* GitHub answered this time (else 'ok' means the
                      * kept answer, or nothing) */
    int newer;       /* that release is newer than this game */
    int skipped;     /* ... and the player chose to skip it */
    char tag[40];    /* the release's tag, such as "v0.3.3" */
    char title[160]; /* its name on GitHub */
    char *notes;     /* its notes, as plain wrapped lines; never null
                      * once ok */
};

/* once, before anything else: the folder for the small state file (the
   player's config folder; null or "" keeps nothing between runs) and
   this game's version, such as "0.3.2" */
void fxu_init(const char *statedir, const char *version);
/* start a check unless one is running. Unless 'forced', GitHub is
   asked at most once a day; in between, the answer kept from the last
   check is used at once (no thread). */
void fxu_start(int forced);
int fxu_status(void); /* FXU_* */
/* the answer, once fxu_status() is FXU_DONE; else null */
const struct fxu_result *fxu_get(void);
/* is there a newer release the player hasn't skipped? (only once
   done) */
int fxu_badge(void);
/* hide the badge until the release after this one */
void fxu_skip(void);
/* open the release's page (or the releases list) in the browser */
int fxu_open_page(void);
/* -1, 0 or 1 as version a is older, the same or newer than b ("v0.3.2"
   and "0.3.2" are the same; pre-releases such as "0.4.0-rc1" count as
   older than anything) */
int fxu_compare(const char *a, const char *b);

#endif /* FXUPDATE_H */
