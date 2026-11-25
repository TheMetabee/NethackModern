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
    long dl_size;    /* this platform's download, in bytes (0 none) */
    int dl_signed;   /* ... and it has a signature beside it */
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
/* is the file at 'path' signed with the release key, per 'sigtext' (its
   .minisig's contents), with a signature naming 'name'? 1 if so; else 0,
   and why in 'why' */
int fxu_verify(const char *path, const char *sigtext, const char *name,
               char *why, size_t whysz);

/*
 * Updating in place (Windows, for now). The download is a job of its own,
 * on its own thread: the signature, then the zip (into the temp folder),
 * checked, then unpacked; only then is it READY, and a marker says so, so
 * a later run (back at the title screen) can install it. Installing is
 * done on the main thread, at the title screen: files from the update
 * replace the game's own (the running ones are renamed *.old first, which
 * Windows allows, and removed on a later start); the player's (record,
 * saves, sysconf...) are never touched; if anything fails, what was done
 * is undone.
 */
#define FXU_JOB_NONE 0
#define FXU_JOB_DOWNLOAD 1
#define FXU_JOB_VERIFY 2
#define FXU_JOB_UNPACK 3
#define FXU_JOB_READY 4
#define FXU_JOB_FAILED 5
/* can this copy update itself to the release found? (else the release
   page will do) */
int fxu_can_update(void);
/* start downloading it (once done, or with a check, fxu_get ok) */
void fxu_update_start(void);
/* FXU_JOB_*, and how far the download is (bytes; total 0 if unknown) */
int fxu_job(long *done, long *total);
const char *fxu_job_error(void);
/* is a downloaded update for a newer version waiting to be installed? */
int fxu_pending(void);
/* install it: 1 if done (restart now), else 0 and why */
int fxu_install(char *why, size_t whysz);
/* remove what earlier updates left (running files renamed *.old) */
void fxu_cleanup(void);
/* once installed, where to start the game again from, if not the usual
   way: the new AppImage (the running program is still the old one's) */
const char *fxu_restart_path(void);

#endif /* FXUPDATE_H */
