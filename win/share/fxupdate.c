/* NetHack Modern fxupdate.c */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * Is there a newer NetHack Modern?  See fxupdate.h.
 *
 * GitHub says which release is the latest at
 *   https://api.github.com/repos/TheMetabee/NethackModern/releases/latest
 * as JSON, of which only the tag, the name and the notes are read.  The
 * request is a plain GET with a User-Agent naming the game and its
 * version, nothing about the player.
 *
 * Windows asks through WinHTTP, part of Windows.  Linux uses libcurl if
 * the system has it (loaded when needed, so nothing is linked or
 * shipped), else the curl or wget program.
 *
 * Everything here may run on the checking thread, so it uses only the C
 * library and the system: no alloc(), no pline(), no game globals.  The
 * thread fills 'res' and only then sets the status to FXU_DONE; the
 * main thread reads 'res' only after seeing FXU_DONE.  The status is
 * read and written atomically, with the ordering that makes 'res'
 * complete by the time FXU_DONE is seen.
 *
 * Between runs a small state file in the player's config folder keeps
 * when GitHub was last asked, its answer (so the badge can show without
 * asking again within the day) and a skipped version.
 *
 * NETHACK_UPDATE_URL, for testing, points the check elsewhere: another
 * http(s) address, or a local file holding a release's JSON.  Nothing
 * is kept in the state file then.
 */

#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS /* (wcscat, fopen: used with care) */
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#endif
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fxupdate.h"
#include "monocypher/monocypher.h"
#include "monocypher/monocypher-ed25519.h"

#define FXU_API_URL \
    "https://api.github.com/repos/TheMetabee/NethackModern/releases/latest"
#define FXU_PAGE_URL "https://github.com/TheMetabee/NethackModern/releases"
#define FXU_STATE_FILE "nethackmodern-update.txt"
#define FXU_DAY (24L * 60L * 60L)
#define FXU_MAXREPLY (4L * 1024L * 1024L) /* far more than a reply needs */
#define FXU_WRAP 72                       /* the notes' line length */

static char fxu_dir[1024];    /* where the state file goes, or "" */
static char fxu_ver[40];      /* this game's version */
static char fxu_agent[80];    /* "NetHackModern/0.3.2" */
static char fxu_url[1024];    /* what to ask (NETHACK_UPDATE_URL) */
static int fxu_testing;       /* it's NETHACK_UPDATE_URL: keep nothing */
static char fxu_key[100];     /* the public key (a test's, in testing) */
static struct fxu_result res; /* the answer; see above for who may touch */

/* ---- the status, shared by the two threads -------------------------- */

/* a number both threads use: written whole and read whole, and what was
   written before a store is seen by whoever loads it after */
#ifdef _WIN32
typedef volatile LONG fxu_atomic;
#define FXU_ASTORE(a, v) ((void) InterlockedExchange(&(a), (LONG) (v)))
#define FXU_ALOAD(a) ((long) InterlockedCompareExchange(&(a), 0, 0))
#define FXU_AADD(a, n) ((void) InterlockedExchangeAdd(&(a), (LONG) (n)))
#else
typedef long fxu_atomic;
#define FXU_ASTORE(a, v) __atomic_store_n(&(a), (long) (v), __ATOMIC_RELEASE)
#define FXU_ALOAD(a) __atomic_load_n(&(a), __ATOMIC_ACQUIRE)
#define FXU_AADD(a, n) ((void) __atomic_add_fetch(&(a), (long) (n), \
                                                  __ATOMIC_ACQ_REL))
#endif

static fxu_atomic fxu_st = FXU_IDLE;
#define FXU_SET(v) FXU_ASTORE(fxu_st, (v))
#define FXU_GETST() ((int) FXU_ALOAD(fxu_st))

int
fxu_status(void)
{
    return FXU_GETST();
}

const struct fxu_result *
fxu_get(void)
{
    return (FXU_GETST() == FXU_DONE) ? &res : (const struct fxu_result *) 0;
}

int
fxu_badge(void)
{
    const struct fxu_result *r = fxu_get();

    return r && r->ok && r->newer && !r->skipped;
}

/* ---- small helpers ------------------------------------------------- */

/* a growing buffer, always NUL-terminated */
struct fxu_buf {
    char *p;
    size_t n, cap;
    int over; /* it would have passed FXU_MAXREPLY */
};

static int
fxu_put(struct fxu_buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > (size_t) FXU_MAXREPLY) {
        b->over = 1;
        return 0;
    }
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        char *q;

        while (cap < b->n + n + 1)
            cap *= 2;
        if (!(q = (char *) realloc(b->p, cap)))
            return 0;
        b->p = q, b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
    return 1;
}

static char *
fxu_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *) malloc(n);

    if (p)
        memcpy(p, s, n);
    return p;
}

static void
fxu_copy(char *dst, size_t sz, const char *src)
{
    size_t n = strlen(src);

    if (n >= sz)
        n = sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* a version's numbers ("v0.3.2" -> 0 3 2); returns how many, or -1 for
   a pre-release ("0.4.0-rc1") or something that isn't a version */
static int
fxu_vparse(const char *s, long v[4])
{
    int n = 0;

    v[0] = v[1] = v[2] = v[3] = 0;
    if (*s == 'v' || *s == 'V')
        s++;
    while (n < 4 && isdigit((unsigned char) *s)) {
        v[n++] = strtol(s, (char **) &s, 10);
        if (*s != '.')
            break;
        s++;
    }
    return (n == 0 || *s) ? -1 : n;
}

int
fxu_compare(const char *a, const char *b)
{
    long va[4], vb[4];
    int i, na = fxu_vparse(a, va), nb = fxu_vparse(b, vb);

    if (na < 0 || nb < 0)
        return (na < 0 && nb < 0) ? 0 : (na < 0) ? -1 : 1;
    for (i = 0; i < 4; i++)
        if (va[i] != vb[i])
            return (va[i] < vb[i]) ? -1 : 1;
    return 0;
}

/* only plain tag names go into a URL */
static int
fxu_tag_ok(const char *t)
{
    if (!*t)
        return 0;
    for (; *t; t++)
        if (!isalnum((unsigned char) *t) && !strchr("._-", *t))
            return 0;
    return 1;
}

/* ---- checking a download's signature ------------------------------- */

/*
 * Releases are signed with minisign (sys/share/nethackmodern.pub holds
 * the public key; this is its second line). A .minisig file has four
 * lines:
 *
 *   untrusted comment: ...          (not signed: ignored)
 *   base64 of: "ED" | key id (8) | signature (64)
 *   trusted comment: file:<name>
 *   base64 of: global signature (64)
 *
 * "ED" signs the BLAKE2b-512 hash of the file (the old "Ed", of the file
 * itself, is refused). The global signature covers the first signature and the
 * trusted comment together, so the comment can't be changed either. A
 * file is good only if the key id is ours, both signatures check out
 * (Ed25519, by Monocypher), and the comment names the file we asked for:
 * an older release, however well signed, can't pose as a newer one.
 */
#define FXU_PUBKEY "RWQe7MNhaufNWUvOOusnGoQShiJ5k8weSyA7RMXKS/o//35YPm6Omk2P"

/* standard base64 into out (at most max bytes); the length, or -1 */
static int
fxu_unbase64(const char *s, unsigned char *out, int max)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned long acc = 0;
    int bits = 0, n = 0;

    for (; *s && *s != '\r' && *s != '\n' && *s != '='; s++) {
        const char *p = strchr(tab, *s);

        if (!p || !*s)
            return -1;
        acc = (acc << 6) | (unsigned long) (p - tab);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= max)
                return -1;
            out[n++] = (unsigned char) ((acc >> bits) & 0xFF);
        }
    }
    return n;
}

/* the next line of text at *pp (its end cut off), or null */
static char *
fxu_line(char **pp)
{
    char *s = *pp, *e;

    if (!s || !*s)
        return 0;
    e = strchr(s, '\n');
    *pp = e ? e + 1 : s + strlen(s);
    if (e)
        *e = '\0';
    if (*s && s[strlen(s) - 1] == '\r')
        s[strlen(s) - 1] = '\0';
    return s;
}

/* the file's BLAKE2b-512 hash, read a piece at a time; 1 if read */
static int
fxu_hash_file(const char *path, unsigned char hash[64])
{
    crypto_blake2b_ctx ctx;
    unsigned char *buf;
    size_t n;
    FILE *fp;
    int ok;

    if (!(fp = fopen(path, "rb")))
        return 0;
    if (!(buf = (unsigned char *) malloc(1 << 16))) {
        fclose(fp);
        return 0;
    }
    crypto_blake2b_init(&ctx, 64);
    while ((n = fread(buf, 1, 1 << 16, fp)) > 0)
        crypto_blake2b_update(&ctx, buf, n);
    ok = !ferror(fp);
    fclose(fp);
    free(buf);
    crypto_blake2b_final(&ctx, hash);
    return ok;
}

/* is 'path' signed by our key, per 'sigtext' (the .minisig's contents),
   and does the signature name 'name'? 1 if so; else 0, with why in
   'why' */
int
fxu_verify(const char *path, const char *sigtext, const char *name,
           char *why, size_t whysz)
{
    unsigned char pk[42], sig[74], gsig[64], hash[64], *msg = 0;
    char *text, *p, *line, *comment;
    size_t clen;
    int ok = 0;

#define FXU_FAIL(w) do { fxu_copy(why, whysz, (w)); goto done; } while (0)
    why[0] = '\0';
    if (!(text = fxu_strdup(sigtext)))
        return 0;
    p = text;
    if (fxu_unbase64(fxu_key[0] ? fxu_key : FXU_PUBKEY, pk, sizeof pk) != 42
        || pk[0] != 'E' || pk[1] != 'd')
        FXU_FAIL("the game's own key is damaged");
    /* the four lines */
    if (!(line = fxu_line(&p)) || strncmp(line, "untrusted comment:", 18))
        FXU_FAIL("the signature file is not a minisign signature");
    if (!(line = fxu_line(&p)) || fxu_unbase64(line, sig, sizeof sig) != 74)
        FXU_FAIL("the signature is damaged");
    if (!(line = fxu_line(&p)) || strncmp(line, "trusted comment: ", 17))
        FXU_FAIL("the signature has no trusted comment");
    comment = line + 17;
    if (!(line = fxu_line(&p)) || fxu_unbase64(line, gsig, sizeof gsig) != 64)
        FXU_FAIL("the signature is damaged");

    /* our key? */
    if (memcmp(sig + 2, pk + 2, 8))
        FXU_FAIL("it was signed with another key");
    /* the file itself */
    if (sig[0] == 'E' && sig[1] == 'D') {
        if (!fxu_hash_file(path, hash))
            FXU_FAIL("the download could not be read");
        if (crypto_ed25519_check(sig + 10, pk + 10, hash, 64) != 0)
            FXU_FAIL("the download does not match its signature");
    } else {
        /* (the old "Ed" kind, of the whole file, is made only by asking
           minisign for it: never by our release steps) */
        FXU_FAIL("the signature is of an unknown kind");
    }
    /* the comment, under the global signature */
    clen = strlen(comment);
    if (!(msg = (unsigned char *) malloc(64 + clen)))
        FXU_FAIL("out of memory");
    memcpy(msg, sig + 10, 64);
    memcpy(msg + 64, comment, clen);
    if (crypto_ed25519_check(gsig, pk + 10, msg, 64 + clen) != 0)
        FXU_FAIL("the signature's comment was changed");
    /* and the file it names */
    if (strncmp(comment, "file:", 5) || strcmp(comment + 5, name))
        FXU_FAIL("the signature is for another file");
    ok = 1;
#undef FXU_FAIL
done:
    free(msg);
    free(text);
    return ok;
}

/* ---- reading GitHub's JSON ----------------------------------------- */

/* Enough JSON to walk the top level of one object and take some of its
   strings: everything else is skipped, nested or not. */

static const char *
fxu_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

static void
fxu_utf8(struct fxu_buf *b, unsigned long c)
{
    char u[4];
    size_t n;

    /* 1 to 4 bytes: the lead byte says how many, the rest carry 6 bits
       each */
    if (c < 0x80) {
        u[0] = (char) c, n = 1;
    } else if (c < 0x800) {
        u[0] = (char) (0xC0 | (c >> 6));
        u[1] = (char) (0x80 | (c & 0x3F)), n = 2;
    } else if (c < 0x10000) {
        u[0] = (char) (0xE0 | (c >> 12));
        u[1] = (char) (0x80 | ((c >> 6) & 0x3F));
        u[2] = (char) (0x80 | (c & 0x3F)), n = 3;
    } else {
        u[0] = (char) (0xF0 | (c >> 18));
        u[1] = (char) (0x80 | ((c >> 12) & 0x3F));
        u[2] = (char) (0x80 | ((c >> 6) & 0x3F));
        u[3] = (char) (0x80 | (c & 0x3F)), n = 4;
    }
    (void) fxu_put(b, u, n);
}

static unsigned long
fxu_hex4(const char *p)
{
    unsigned long v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        int c = p[i];

        if (!isxdigit((unsigned char) c))
            return 0xFFFFFFFFUL;
        v = v * 16 + (unsigned long) (isdigit(c) ? c - '0'
                                                : (tolower(c) - 'a' + 10));
    }
    return v;
}

/* a string at p (which is at its opening quote): decoded into 'out' if
   given (UTF-8), else just passed over; returns what follows it, or
   null if it is broken */
static const char *
fxu_jstring(const char *p, struct fxu_buf *out)
{
    if (*p++ != '"')
        return 0;
    while (*p && *p != '"') {
        if (*p != '\\') {
            const char *q = p;

            while (*q && *q != '"' && *q != '\\')
                q++;
            if (out)
                (void) fxu_put(out, p, (size_t) (q - p));
            p = q;
            continue;
        }
        p++;
        switch (*p) {
        case 'n': if (out) (void) fxu_put(out, "\n", 1); break;
        case 't': if (out) (void) fxu_put(out, "\t", 1); break;
        case 'r': case 'b': case 'f': break;
        case 'u': {
            unsigned long c = fxu_hex4(p + 1);

            if (c == 0xFFFFFFFFUL)
                return 0;
            p += 4;
            /* a pair of surrogates makes one character */
            if (c >= 0xD800 && c < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
                unsigned long lo = fxu_hex4(p + 3);

                if (lo >= 0xDC00 && lo < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            if (out)
                fxu_utf8(out, c);
            break;
        }
        case '\0':
            return 0;
        default: /* \" \\ \/ */
            if (out)
                (void) fxu_put(out, p, 1);
            break;
        }
        p++;
    }
    return (*p == '"') ? p + 1 : 0;
}

/* pass over any value (a string, a number, true, an object, a list...);
   returns what follows it, or null */
static const char *
fxu_jskip(const char *p)
{
    int depth = 0; /* how many { or [ we're inside */

    for (;;) {
        p = fxu_ws(p);
        if (!*p)
            return 0;
        if (*p == '"') { /* strings can hold brackets: pass them whole */
            if (!(p = fxu_jstring(p, 0)))
                return 0;
            if (!depth)
                return p;
        } else if (*p == '{' || *p == '[') {
            depth++, p++;
        } else if (*p == '}' || *p == ']') {
            if (!depth)
                return 0;
            depth--, p++;
            if (!depth)
                return p;
        } else if (!depth) { /* a bare value: up to what ends it */
            while (*p && !strchr(",}] \t\r\n", *p))
                p++;
            return p;
        } else {
            p++; /* inside: a comma, a colon, a number... */
        }
    }
}

/* take the top-level strings named in 'keys' (null-terminated) into
   'vals'; returns 1 if the object read cleanly */
static int
fxu_jobject(const char *p, const char *const *keys, struct fxu_buf *vals)
{
    p = fxu_ws(p);
    if (*p++ != '{')
        return 0;
    for (;;) {
        struct fxu_buf key = { 0, 0, 0, 0 };
        int k = -1, i;

        p = fxu_ws(p);
        if (*p == '}')
            return 1;
        if (!(p = fxu_jstring(p, &key))) {
            free(key.p);
            return 0;
        }
        for (i = 0; keys[i]; i++)
            if (key.p && !strcmp(key.p, keys[i]))
                k = i;
        free(key.p);
        p = fxu_ws(p);
        if (*p++ != ':')
            return 0;
        p = fxu_ws(p);
        if (k >= 0 && *p == '"' && !vals[k].p) {
            if (!(p = fxu_jstring(p, &vals[k])))
                return 0;
            if (!vals[k].p) /* an empty string */
                (void) fxu_put(&vals[k], "", 0);
        } else if (!(p = fxu_jskip(p))) {
            return 0;
        }
        p = fxu_ws(p);
        if (*p == ',')
            p++;
        else if (*p != '}')
            return 0;
    }
}

/* the release's list of downloads ("assets": [{"name": ..., "size": ...},
   ...]): the size of the one named 'want' (0 if it isn't there), and
   whether want.minisig is there too */
static void
fxu_jassets(const char *p, const char *want, long *size, int *sig)
{
    size_t wl = strlen(want);

    *size = 0, *sig = 0;
    p = fxu_ws(p);
    if (*p++ != '{')
        return;
    for (;;) { /* the release's keys */
        struct fxu_buf key = { 0, 0, 0, 0 };
        int is_assets;

        p = fxu_ws(p);
        if (*p != '"' || !(p = fxu_jstring(p, &key))) {
            free(key.p);
            return;
        }
        is_assets = key.p && !strcmp(key.p, "assets");
        free(key.p);
        p = fxu_ws(p);
        if (*p++ != ':')
            return;
        p = fxu_ws(p);
        if (is_assets && *p == '[') {
            for (p++;;) { /* each download */
                struct fxu_buf name = { 0, 0, 0, 0 };
                double sz = 0;

                p = fxu_ws(p);
                if (*p++ != '{')
                    return; /* (the list's end, or something odd) */
                for (;;) { /* its keys */
                    struct fxu_buf k = { 0, 0, 0, 0 };

                    p = fxu_ws(p);
                    if (*p == '}') {
                        p++;
                        break;
                    }
                    if (!(p = fxu_jstring(p, &k)) || *(p = fxu_ws(p)) != ':') {
                        free(k.p), free(name.p);
                        return;
                    }
                    p = fxu_ws(p + 1);
                    if (k.p && !strcmp(k.p, "name") && *p == '"' && !name.p) {
                        p = fxu_jstring(p, &name);
                    } else if (k.p && !strcmp(k.p, "size")
                               && isdigit((unsigned char) *p)) {
                        char *e;

                        sz = strtod(p, &e);
                        p = e;
                    } else {
                        p = fxu_jskip(p);
                    }
                    free(k.p);
                    if (!p) {
                        free(name.p);
                        return;
                    }
                    p = fxu_ws(p);
                    if (*p == ',')
                        p++;
                }
                if (name.p && !strcmp(name.p, want))
                    *size = (long) sz;
                else if (name.p && !strncmp(name.p, want, wl)
                         && !strcmp(name.p + wl, ".minisig"))
                    *sig = 1;
                free(name.p);
                p = fxu_ws(p);
                if (*p != ',')
                    return; /* (that was the last) */
                p++;
            }
        }
        if (!(p = fxu_jskip(p)))
            return;
        p = fxu_ws(p);
        if (*p++ != ',')
            return;
    }
}

/* this platform's download from a release, by name */
static void
fxu_asset_name(const char *tag, char *buf, size_t sz)
{
#ifdef _WIN32
    snprintf(buf, sz, "NetHackModern-%s-windows-x64.zip", tag);
#else
    snprintf(buf, sz, "NetHackModern-%s-x86_64.AppImage", tag);
#endif
}

/* ---- the notes as plain text --------------------------------------- */

/* The notes are Markdown, in UTF-8.  The front ends show them as plain
   lines (NetHackW's menus are not UTF-8), so: headings and bullets lose
   their marks, bold and code marks go, links keep their words, common
   typographic characters become their ASCII look-alikes, and lines are
   wrapped. */

/* one UTF-8 character at *pp as ASCII; moves *pp past it */
static const char *
fxu_ascii(const char **pp)
{
    const unsigned char *p = (const unsigned char *) *pp;
    unsigned long c;
    int n, i;
    static char one[2];

    if (*p < 0x80) {
        one[0] = (char) *p, one[1] = '\0';
        *pp += 1;
        return one;
    }
    n = (*p >= 0xF0) ? 4 : (*p >= 0xE0) ? 3 : (*p >= 0xC0) ? 2 : 1;
    c = (n == 4) ? (*p & 0x07UL) : (n == 3) ? (*p & 0x0FUL)
                                             : (*p & 0x1FUL);
    for (i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            n = i;
            break;
        }
        c = (c << 6) | (p[i] & 0x3FUL);
    }
    *pp += n;
    switch (c) {
    case 0x2018: case 0x2019: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x2033: return "\"";
    case 0x2013: case 0x2014: case 0x2212: return "-";
    case 0x2026: return "...";
    case 0x2022: case 0x00B7: return "*";
    case 0x00A0: case 0x2009: case 0x202F: return " ";
    case 0x2192: return "->";
    case 0x2190: return "<-";
    case 0x00D7: return "x";
    case 0x00E9: return "e";
    default: return ""; /* (emoji and the like: nothing) */
    }
}

/* one line of Markdown, inline marks taken out, as ASCII, into b */
static void
fxu_inline(const char *s, size_t n, struct fxu_buf *b)
{
    const char *e = s + n;

    while (s < e) {
        if ((*s == '*' && s + 1 < e && s[1] == '*')
            || (*s == '_' && s + 1 < e && s[1] == '_')) {
            s += 2; /* bold */
        } else if (*s == '`') {
            s++;
        } else if (*s == '!' && s + 1 < e && s[1] == '[') {
            s++; /* an image: its words, as a link's */
        } else if (*s == '[') {
            /* [words](address): just the words */
            const char *close = memchr(s, ']', (size_t) (e - s));

            if (close && close + 1 < e && close[1] == '(') {
                const char *end = memchr(close, ')', (size_t) (e - close));

                if (end) {
                    fxu_inline(s + 1, (size_t) (close - s - 1), b);
                    s = end + 1;
                    continue;
                }
            }
            (void) fxu_put(b, s++, 1);
        } else {
            const char *a = fxu_ascii(&s);

            (void) fxu_put(b, a, strlen(a));
        }
    }
}

/* 'text' wrapped to FXU_WRAP, the first line after 'lead' and the rest
   after as many spaces, into out */
static void
fxu_wrap(const char *lead, const char *text, struct fxu_buf *out)
{
    static const char spaces[] = "                ";
    size_t ll = strlen(lead), width = FXU_WRAP - ll, n;
    int first = 1;

    while (*text == ' ')
        text++;
    do {
        n = strlen(text);
        if (n > width) {
            size_t cut = width;

            while (cut > 0 && text[cut] != ' ') /* back to a space */
                cut--;
            n = cut ? cut : width; /* a word longer than a line: cut it */
        }
        /* "  - " on the first line, as many spaces under it after */
        (void) fxu_put(out, first ? lead : spaces, ll);
        (void) fxu_put(out, text, n);
        (void) fxu_put(out, "\n", 1);
        text += n;
        while (*text == ' ')
            text++;
        first = 0;
    } while (*text);
}

/* the notes, Markdown in, plain wrapped lines out (malloc'd) */
static char *
fxu_plain(const char *md)
{
    struct fxu_buf out = { 0, 0, 0, 0 };
    int blank = 1; /* the last line put out was blank (or none yet) */

    while (md && *md) {
        const char *nl = strchr(md, '\n'), *s = md, *e;
        struct fxu_buf line = { 0, 0, 0, 0 };
        char lead[16];
        size_t indent;

        e = nl ? nl : md + strlen(md);
        md = nl ? nl + 1 : e;
        while (e > s && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
            e--;
        for (indent = 0; s < e && (*s == ' ' || *s == '\t'); s++)
            indent++;
        if (s == e) { /* a blank line: one at most, never at the top */
            if (!blank)
                (void) fxu_put(&out, "\n", 1), blank = 1;
            continue;
        }
        lead[0] = '\0';
        if (*s == '#') { /* a heading: its words */
            while (s < e && *s == '#')
                s++;
        } else if ((*s == '-' || *s == '*' || *s == '+')
                   && (s + 1 == e || s[1] == ' ')) {
            s++; /* a bullet; an empty one is left out */
            while (s < e && *s == ' ')
                s++;
            if (s == e)
                continue;
            strcpy(lead, indent >= 2 ? "    - " : "  - ");
        } else if (!strncmp(s, "---", 3) || !strncmp(s, "***", 3)) {
            continue; /* a rule */
        }
        fxu_inline(s, (size_t) (e - s), &line);
        if (line.p && *line.p) {
            fxu_wrap(lead, line.p, &out);
            blank = 0;
        }
        free(line.p);
    }
    while (out.n > 0 && out.p[out.n - 1] == '\n') /* no blank at the end */
        out.p[--out.n] = '\0';
    if (!out.p)
        (void) fxu_put(&out, "", 0);
    return out.p ? out.p : fxu_strdup("");
}

/* ---- asking -------------------------------------------------------- */

/* a local file's contents (for NETHACK_UPDATE_URL) */
static char *
fxu_read_file(const char *path)
{
    struct fxu_buf b = { 0, 0, 0, 0 };
    char tmp[8192];
    size_t n;
    FILE *fp;

    if (!strncmp(path, "file://", 7))
        path += 7;
    if (!(fp = fopen(path, "rb")))
        return 0;
    while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0)
        if (!fxu_put(&b, tmp, n))
            break;
    fclose(fp);
    if (b.over) {
        free(b.p);
        return 0;
    }
    return b.p;
}

/* where what's fetched goes, a piece at a time: 0 to stop */
typedef int (*fxu_sink_fn)(void *ud, const char *p, size_t n);

static int
fxu_sink_mem(void *ud, const char *p, size_t n)
{
    return fxu_put((struct fxu_buf *) ud, p, n);
}

#ifdef _WIN32

/* GET url, handing what comes to sink; *total (if given) is set from the
   reply's length, when it says; 1 if it all came */
static int
fxu_http_get(const char *url, fxu_sink_fn sink, void *ud, long *total)
{
    wchar_t wurl[1024], host[256], path[1024], extra[1024], agent[80];
    URL_COMPONENTS uc;
    HINTERNET ses = 0, con = 0, req = 0;
    DWORD code = 0, sz = sizeof code, got = 0, len = 0;
    char tmp[16384];
    int good = 0;

    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024)
        || !MultiByteToWideChar(CP_UTF8, 0, fxu_agent, -1, agent, 80))
        return 0;
    if (total)
        *total = 0;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host, uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path, uc.dwUrlPathLength = 1024;
    uc.lpszExtraInfo = extra, uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc))
        return 0;
    if (wcslen(path) + wcslen(extra) < 1024)
        wcscat(path, extra); /* the ?query, if any */

    /* the system's proxy settings, as a browser would (Windows 8.1 and
       later); else whatever WinHTTP was told */
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
    ses = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#endif
    if (!ses)
        ses = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        return 0;
    /* resolve, connect, send, receive: a slow network gives up soon */
    (void) WinHttpSetTimeouts(ses, 8000, 8000, 8000, 15000);
    if ((con = WinHttpConnect(ses, host, uc.nPort, 0)) != 0
        && (req = WinHttpOpenRequest(con, L"GET", path, NULL,
                                     WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     (uc.nScheme == INTERNET_SCHEME_HTTPS)
                                         ? WINHTTP_FLAG_SECURE
                                         : 0)) != 0
        && WinHttpSendRequest(req,
                              /* (GitHub's API asks for this; downloads
                                 come from elsewhere) */
                              strstr(url, "//api.github.com/")
                                  ? L"Accept: application/vnd.github+json\r\n"
                                  : WINHTTP_NO_ADDITIONAL_HEADERS,
                              (DWORD) -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(req, NULL)
        && WinHttpQueryHeaders(req,
                               WINHTTP_QUERY_STATUS_CODE
                                   | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                               WINHTTP_NO_HEADER_INDEX)
        && code == 200) {
        sz = sizeof len;
        if (total && WinHttpQueryHeaders(req,
                                         WINHTTP_QUERY_CONTENT_LENGTH
                                             | WINHTTP_QUERY_FLAG_NUMBER,
                                         WINHTTP_HEADER_NAME_BY_INDEX, &len,
                                         &sz, WINHTTP_NO_HEADER_INDEX))
            *total = (long) len;
        /* (a download is many reads: the timeouts above are for each) */
        good = 1;
        for (;;) {
            if (!WinHttpReadData(req, tmp, sizeof tmp, &got)) {
                good = 0; /* the connection broke */
                break;
            }
            if (got == 0)
                break; /* the end */
            if (!sink(ud, tmp, got)) {
                good = 0;
                break;
            }
        }
    }
    if (req)
        WinHttpCloseHandle(req);
    if (con)
        WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return good;
}

static char *
fxu_fetch_net(const char *url)
{
    struct fxu_buf b = { 0, 0, 0, 0 };

    if (!fxu_http_get(url, fxu_sink_mem, &b, (long *) 0) || b.over) {
        free(b.p);
        return 0;
    }
    return b.p ? b.p : fxu_strdup("");
}

#else /* !_WIN32 */

/* libcurl, if the system has it.  Its few calls are looked up by name;
   the option numbers are part of libcurl's fixed ABI (curl.h). */
#define FXU_CURLOPT_WRITEDATA 10001
#define FXU_CURLOPT_URL 10002
#define FXU_CURLOPT_USERAGENT 10018
#define FXU_CURLOPT_WRITEFUNCTION 20011
#define FXU_CURLOPT_TIMEOUT 13
#define FXU_CURLOPT_FAILONERROR 45
#define FXU_CURLOPT_FOLLOWLOCATION 52
#define FXU_CURLOPT_MAXREDIRS 68
#define FXU_CURLOPT_CONNECTTIMEOUT 78
#define FXU_CURLOPT_NOSIGNAL 99

typedef void *(*fxu_curl_init_fn)(void);
typedef int (*fxu_curl_setopt_fn)(void *, int, ...);
typedef int (*fxu_curl_perform_fn)(void *);
typedef void (*fxu_curl_cleanup_fn)(void *);

#define FXU_CURLOPT_LOW_SPEED_LIMIT 19
#define FXU_CURLOPT_LOW_SPEED_TIME 20

struct fxu_curl_to {
    fxu_sink_fn sink;
    void *ud;
};

static size_t
fxu_curl_write(char *p, size_t size, size_t n, void *ud)
{
    struct fxu_curl_to *to = (struct fxu_curl_to *) ud;

    return to->sink(to->ud, p, size * n) ? size * n : 0;
}

/* GET url with libcurl, if the system has it, handing what comes to
   sink: 1 if it all came, 0 if not, -1 if there's no libcurl. 'big' (a
   download) has no time limit, only a limit on stalling */
static int
fxu_fetch_libcurl(const char *url, fxu_sink_fn sink, void *ud, int big)
{
    static const char *const libs[] = {
        "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so", 0
    };
    void *lib = 0, *h;
    fxu_curl_init_fn c_init;
    fxu_curl_setopt_fn c_set;
    fxu_curl_perform_fn c_perform;
    fxu_curl_cleanup_fn c_cleanup;
    struct fxu_curl_to to;
    int i, rc;

    for (i = 0; libs[i] && !lib; i++)
        lib = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return -1;
    /* (dlsym gives a data pointer; ISO C won't cast one to a function
       pointer, so its bytes are copied, as POSIX suggests) */
#define FXU_SYM(var, name) \
    do {                                        \
        void *sym_ = dlsym(lib, name);          \
        memcpy(&(var), &sym_, sizeof (var));    \
    } while (0)
    FXU_SYM(c_init, "curl_easy_init");
    FXU_SYM(c_set, "curl_easy_setopt");
    FXU_SYM(c_perform, "curl_easy_perform");
    FXU_SYM(c_cleanup, "curl_easy_cleanup");
#undef FXU_SYM
    if (!c_init || !c_set || !c_perform || !c_cleanup || !(h = c_init())) {
        dlclose(lib);
        return -1;
    }
    to.sink = sink, to.ud = ud;
    (void) c_set(h, FXU_CURLOPT_URL, url);
    (void) c_set(h, FXU_CURLOPT_USERAGENT, fxu_agent);
    (void) c_set(h, FXU_CURLOPT_WRITEFUNCTION, fxu_curl_write);
    (void) c_set(h, FXU_CURLOPT_WRITEDATA, (void *) &to);
    (void) c_set(h, FXU_CURLOPT_FOLLOWLOCATION, 1L);
    (void) c_set(h, FXU_CURLOPT_MAXREDIRS, 5L);
    (void) c_set(h, FXU_CURLOPT_FAILONERROR, 1L);
    (void) c_set(h, FXU_CURLOPT_CONNECTTIMEOUT, 8L);
    if (big) { /* give up only if under 1 KB a second for 30 seconds */
        (void) c_set(h, FXU_CURLOPT_LOW_SPEED_LIMIT, 1024L);
        (void) c_set(h, FXU_CURLOPT_LOW_SPEED_TIME, 30L);
    } else {
        (void) c_set(h, FXU_CURLOPT_TIMEOUT, 20L);
    }
    (void) c_set(h, FXU_CURLOPT_NOSIGNAL, 1L); /* we're not the main thread */
    rc = c_perform(h);
    c_cleanup(h);
    /* (libcurl stays loaded: unloading it can upset its TLS library) */
    return rc == 0;
}

/* a program's output (curl or wget) handed to sink; 1 if it all came */
static int
fxu_fetch_program(const char *argv[], fxu_sink_fn sink, void *ud)
{
    char tmp[16384];
    int fd[2], status = 0, ok = 1;
    ssize_t n;
    pid_t pid;

    if (pipe(fd) < 0)
        return 0;
    if ((pid = fork()) < 0) {
        close(fd[0]), close(fd[1]);
        return 0;
    }
    if (pid == 0) {
        /* (only calls safe after fork in a program with threads) */
        int devnull = open("/dev/null", O_RDWR);

        dup2(fd[1], 1);
        if (devnull >= 0)
            dup2(devnull, 0), dup2(devnull, 2);
        close(fd[0]), close(fd[1]);
        /* (execvp's argv isn't const for history's sake; it's not
           changed) */
        execvp(argv[0], (char *const *) argv);
        _exit(127);
    }
    close(fd[1]);
    while ((n = read(fd[0], tmp, sizeof tmp)) > 0)
        if (!sink(ud, tmp, (size_t) n)) {
            ok = 0;
            kill(pid, SIGTERM);
            break;
        }
    close(fd[0]);
    (void) waitpid(pid, &status, 0);
    return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* GET url by whatever this system has: libcurl, else curl, else wget */
static int
fxu_net_get(const char *url, fxu_sink_fn sink, void *ud, int big)
{
    const char *curl[] = { "curl", "-sfL", "-A", 0, 0, 0, 0, 0, 0, 0 };
    const char *wget[] = { "wget", "-q", "-O", "-", "--timeout=30", "-U",
                           0, 0, 0 };
    int got = fxu_fetch_libcurl(url, sink, ud, big);

    if (got >= 0)
        return got;
    curl[3] = fxu_agent;
    if (big) /* (as for libcurl: no limit but stalling) */
        curl[4] = "--speed-limit", curl[5] = "1024",
        curl[6] = "--speed-time", curl[7] = "30", curl[8] = url;
    else
        curl[4] = "--max-time", curl[5] = "20", curl[6] = url;
    if (fxu_fetch_program(curl, sink, ud))
        return 1;
    wget[6] = fxu_agent, wget[7] = url;
    return fxu_fetch_program(wget, sink, ud);
}

static char *
fxu_fetch_net(const char *url)
{
    struct fxu_buf b = { 0, 0, 0, 0 };

    if (!fxu_net_get(url, fxu_sink_mem, &b, 0) || b.over) {
        free(b.p);
        return 0;
    }
    return b.p ? b.p : fxu_strdup("");
}

#endif /* !_WIN32 */

static char *
fxu_fetch(const char *url)
{
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7))
        return fxu_read_file(url);
    return fxu_fetch_net(url);
}

/* ---- the state file ------------------------------------------------ */

struct fxu_state {
    long checked;    /* when GitHub last answered (time_t), 0 never */
    char latest[40]; /* its latest tag then */
    char skip[40];   /* a tag the player skipped */
    char title[160];
    long dl_size;    /* this platform's download's size, 0 none */
    int dl_signed;   /* ... with its signature beside it */
    char *notes;     /* malloc'd, or null */
};

static int
fxu_state_path(char *buf, size_t sz, const char *name)
{
    size_t n = strlen(fxu_dir);

    if (!n || n + strlen(name) + 2 > sz)
        return 0;
    strcpy(buf, fxu_dir);
    if (buf[n - 1] != '/' && buf[n - 1] != '\\')
        strcat(buf, "/");
    strcat(buf, name);
    return 1;
}

/* the file: key=value lines, then "notes:" and the notes to the end */
static void
fxu_state_read(struct fxu_state *st)
{
    char path[1100], *text, *p, *nl;

    memset(st, 0, sizeof *st);
    if (!fxu_state_path(path, sizeof path, FXU_STATE_FILE)
        || !(text = fxu_read_file(path)))
        return;
    for (p = text; p && *p; p = nl) {
        if ((nl = strchr(p, '\n')) != 0)
            *nl++ = '\0';
        if (*p && p[strlen(p) - 1] == '\r')
            p[strlen(p) - 1] = '\0';
        if (!strncmp(p, "checked=", 8))
            st->checked = atol(p + 8);
        else if (!strncmp(p, "latest=", 7))
            fxu_copy(st->latest, sizeof st->latest, p + 7);
        else if (!strncmp(p, "skip=", 5))
            fxu_copy(st->skip, sizeof st->skip, p + 5);
        else if (!strncmp(p, "title=", 6))
            fxu_copy(st->title, sizeof st->title, p + 6);
        else if (!strncmp(p, "dlsize=", 7))
            st->dl_size = atol(p + 7);
        else if (!strncmp(p, "dlsigned=", 9))
            st->dl_signed = atoi(p + 9);
        else if (!strcmp(p, "notes:")) {
            st->notes = fxu_strdup(nl ? nl : "");
            break;
        }
    }
    free(text);
}

/* written whole to a new file, then put in place of the old one, so a
   game closed halfway never leaves half a file */
static void
fxu_state_write(const struct fxu_state *st)
{
    char path[1100], tmp[1100];
    FILE *fp;

    /* a test's made-up release mustn't show in the player's next game */
    if (fxu_testing)
        return;
    if (!fxu_state_path(path, sizeof path, FXU_STATE_FILE)
        || !fxu_state_path(tmp, sizeof tmp, FXU_STATE_FILE ".new")
        || !(fp = fopen(tmp, "wb")))
        return;
    fprintf(fp, "# NetHack Modern's update check; safe to delete\n");
    fprintf(fp, "checked=%ld\nlatest=%s\nskip=%s\ntitle=%s\n"
            "dlsize=%ld\ndlsigned=%d\nnotes:\n%s",
            st->checked, st->latest, st->skip, st->title, st->dl_size,
            st->dl_signed, st->notes ? st->notes : "");
    if (fclose(fp) != 0) {
        remove(tmp);
        return;
    }
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        remove(tmp);
#else
    if (rename(tmp, path) != 0)
        remove(tmp);
#endif
}

/* fill 'res' from what was kept */
static void
fxu_from_state(struct fxu_state *st)
{
    res.ok = (st->latest[0] != '\0');
    fxu_copy(res.tag, sizeof res.tag, st->latest);
    fxu_copy(res.title, sizeof res.title, st->title);
    free(res.notes);
    res.notes = st->notes ? st->notes : fxu_strdup("");
    st->notes = 0;
    res.newer = res.ok && fxu_compare(res.tag, fxu_ver) > 0;
    res.skipped = res.ok && st->skip[0] && !strcmp(st->skip, res.tag);
    res.dl_size = st->dl_size;
    res.dl_signed = st->dl_signed;
}

/* ---- the check itself (on its own thread) -------------------------- */

static void
fxu_check(void)
{
    static const char *const keys[] = { "tag_name", "name", "body", 0 };
    struct fxu_buf vals[3];
    struct fxu_state st;
    char *reply;

    memset(vals, 0, sizeof vals);
    fxu_state_read(&st);
    reply = fxu_fetch(fxu_url);
    if (reply && fxu_jobject(reply, keys, vals) && vals[0].p
        && fxu_tag_ok(vals[0].p)) {
        /* an answer: kept, with the time, for the rest of the day */
        st.checked = (long) time((time_t *) 0);
        fxu_copy(st.latest, sizeof st.latest, vals[0].p);
        st.title[0] = '\0';
        if (vals[1].p) {
            struct fxu_buf t = { 0, 0, 0, 0 };

            fxu_inline(vals[1].p, strlen(vals[1].p), &t);
            if (t.p)
                fxu_copy(st.title, sizeof st.title, t.p);
            free(t.p);
        }
        free(st.notes);
        st.notes = fxu_plain(vals[2].p);
        {
            /* this platform's download, and is it signed? */
            char want[200];

            fxu_asset_name(st.latest, want, sizeof want);
            fxu_jassets(reply, want, &st.dl_size, &st.dl_signed);
        }
        fxu_state_write(&st);
        fxu_from_state(&st);
        res.reached = 1;
    } else {
        /* no answer: whatever was kept before, if anything */
        fxu_from_state(&st);
        res.reached = 0;
    }
    free(st.notes);
    free(reply);
    free(vals[0].p), free(vals[1].p), free(vals[2].p);
    FXU_SET(FXU_DONE); /* last: 'res' is complete */
}

#ifdef _WIN32
static DWORD WINAPI
fxu_thread(LPVOID arg)
{
    (void) arg;
    fxu_check();
    return 0;
}
#else
static void *
fxu_thread(void *arg)
{
    (void) arg;
    fxu_check();
    return 0;
}
#endif

/* ---- what the front ends call (main thread) ------------------------ */

void
fxu_init(const char *statedir, const char *version)
{
    fxu_copy(fxu_dir, sizeof fxu_dir, statedir ? statedir : "");
    fxu_copy(fxu_ver, sizeof fxu_ver, version);
    snprintf(fxu_agent, sizeof fxu_agent, "NetHackModern/%s", fxu_ver);
    /* testing from the start: a download waiting from a test isn't one
       for a normal game (see fxu_workdir) */
    {
        const char *env = getenv("NETHACK_UPDATE_URL");

        fxu_testing = (env && *env);
    }
}

void
fxu_start(int forced)
{
    const char *env = getenv("NETHACK_UPDATE_URL");
    struct fxu_state st;

    if (FXU_GETST() == FXU_BUSY)
        return; /* (it will answer for both) */
    fxu_copy(fxu_url, sizeof fxu_url, (env && *env) ? env : FXU_API_URL);
    fxu_testing = (env && *env);
    /* a test release may be signed with a test key, but only in testing:
       a player's game knows just the one */
    {
        const char *k = getenv("NETHACK_UPDATE_KEY");

        fxu_copy(fxu_key, sizeof fxu_key,
                 (fxu_testing && k && *k) ? k : FXU_PUBKEY);
    }

    if (!forced) {
        long now = (long) time((time_t *) 0);

        fxu_state_read(&st);
        if (st.checked && now >= st.checked && now - st.checked < FXU_DAY
            && !(env && *env)) {
            /* asked today: the kept answer will do */
            fxu_from_state(&st);
            res.reached = 0;
            free(st.notes);
            FXU_SET(FXU_DONE);
            return;
        }
        free(st.notes);
    }

    FXU_SET(FXU_BUSY);
#ifdef _WIN32
    {
        HANDLE t = CreateThread(NULL, 0, fxu_thread, NULL, 0, NULL);

        if (t)
            CloseHandle(t); /* it ends by itself */
        else
            fxu_check(); /* no thread: ask here, then */
    }
#else
    {
        pthread_t t;

        if (pthread_create(&t, NULL, fxu_thread, NULL) == 0)
            (void) pthread_detach(t);
        else
            fxu_check();
    }
#endif
}

void
fxu_skip(void)
{
    struct fxu_state st;

    if (FXU_GETST() != FXU_DONE || !res.ok)
        return;
    res.skipped = 1;
    fxu_state_read(&st);
    fxu_copy(st.skip, sizeof st.skip, res.tag);
    if (!st.latest[0]) { /* (a file kept nothing: keep this answer) */
        fxu_copy(st.latest, sizeof st.latest, res.tag);
        fxu_copy(st.title, sizeof st.title, res.title);
        st.dl_size = res.dl_size, st.dl_signed = res.dl_signed;
        free(st.notes);
        st.notes = fxu_strdup(res.notes ? res.notes : "");
    }
    fxu_state_write(&st);
    free(st.notes);
}

int
fxu_open_page(void)
{
    char url[256];
    const struct fxu_result *r = fxu_get();

    /* the address is made here, never taken from the reply */
    if (r && r->ok && fxu_tag_ok(r->tag))
        snprintf(url, sizeof url, "%s/tag/%s", FXU_PAGE_URL, r->tag);
    else
        snprintf(url, sizeof url, "%s/latest", FXU_PAGE_URL);
#ifdef _WIN32
    return (INT_PTR) ShellExecuteA(NULL, "open", url, NULL, NULL,
                                   SW_SHOWNORMAL) > 32;
#else
    {
        /* xdg-open, in a grandchild so nothing waits for the browser */
        pid_t pid = fork();
        int status = 0;

        if (pid < 0)
            return 0;
        if (pid == 0) {
            if (fork() == 0) {
                int devnull = open("/dev/null", O_RDWR);

                if (devnull >= 0)
                    dup2(devnull, 0), dup2(devnull, 1), dup2(devnull, 2);
                execlp("xdg-open", "xdg-open", url, (char *) 0);
                _exit(127);
            }
            _exit(0);
        }
        (void) waitpid(pid, &status, 0);
        return 1;
    }
#endif
}

/* ---- updating in place --------------------------------------------- */

/* the download job's state, shared by the threads like fxu_st: the job
   writes fxu_jerr before FXU_JOB_FAILED; fxu_jdone grows as it comes */
static fxu_atomic fxu_jst = FXU_JOB_NONE, fxu_jdone, fxu_jtotal;
static char fxu_jerr[240];

int
fxu_job(long *done, long *total)
{
    if (done)
        *done = FXU_ALOAD(fxu_jdone);
    if (total)
        *total = FXU_ALOAD(fxu_jtotal);
    return (int) FXU_ALOAD(fxu_jst);
}

const char *
fxu_job_error(void)
{
    return (FXU_ALOAD(fxu_jst) == FXU_JOB_FAILED) ? fxu_jerr : "";
}

/* what the job is fetching: set before it starts, then only read */
static char fxu_jtag[40], fxu_jname[200], fxu_jbase[1100];

static void
fxu_job_fail(const char *why)
{
    fxu_copy(fxu_jerr, sizeof fxu_jerr, why);
    FXU_ASTORE(fxu_jst, FXU_JOB_FAILED); /* last: fxu_jerr is written */
}

static int
fxu_sink_file(void *ud, const char *p, size_t n)
{
    if (fwrite(p, 1, n, (FILE *) ud) != n)
        return 0;
    FXU_AADD(fxu_jdone, (long) n);
    return 1;
}

/* url (or, testing, a local file) into fp, counting as it comes */
static int
fxu_fetch_to(const char *url, FILE *fp)
{
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) {
        char tmp[65536];
        size_t n;
        int ok = 1;
        FILE *in = fopen(url, "rb");

        if (!in)
            return 0;
        while ((n = fread(tmp, 1, sizeof tmp, in)) > 0)
            if (!fxu_sink_file(fp, tmp, n)) {
                ok = 0;
                break;
            }
        fclose(in);
        return ok;
    }
#ifdef _WIN32
    return fxu_http_get(url, fxu_sink_file, fp, (long *) 0);
#else
    return fxu_net_get(url, fxu_sink_file, fp, 1);
#endif
}

/* the signature, then the download into 'path' (created), checked: 1 if
   it's ours; else the job has failed, saying why */
static int
fxu_job_fetch(const char *path)
{
    char url[1400], why[160], msg[sizeof fxu_jerr];
    char *sigtext;
    FILE *fp;
    int ok;

    /* the signature first: it's small, and without it there's no point */
    snprintf(url, sizeof url, "%s%s.minisig", fxu_jbase, fxu_jname);
    if (!(sigtext = fxu_fetch(url))) {
        fxu_job_fail("its signature could not be downloaded");
        return 0;
    }
    if (!(fp = fopen(path, "wb"))) {
        free(sigtext);
        fxu_job_fail("the download can't be saved");
        return 0;
    }
    snprintf(url, sizeof url, "%s%s", fxu_jbase, fxu_jname);
    ok = fxu_fetch_to(url, fp);
    if (fclose(fp) != 0)
        ok = 0;
    if (!ok) {
        (void) remove(path);
        free(sigtext);
        fxu_job_fail("the download broke off");
        return 0;
    }
    /* is it ours? */
    FXU_ASTORE(fxu_jst, FXU_JOB_VERIFY);
    ok = fxu_verify(path, sigtext, fxu_jname, why, sizeof why);
    free(sigtext);
    if (!ok) {
        (void) remove(path);
        snprintf(msg, sizeof msg, "it was refused: %s", why);
        fxu_job_fail(msg);
        return 0;
    }
    return 1;
}

#ifdef _WIN32

#define FXU_ZIP_TOP "NethackModernW"   /* the folder inside the zip */
#define FXU_OLD_LIST "nethackmodern-update-old.txt"
#define FXU_WORK "NetHackModern-update" /* in the temp folder */

/* where downloads are kept until installed: %TEMP%\NetHackModern-update\ */
static int
fxu_workdir(char *buf, size_t sz)
{
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof tmp, tmp);

    if (!n || n >= sizeof tmp || n + sizeof FXU_WORK + 8 > sz)
        return 0;
    /* a test release's download stays apart, so that a game started
       normally never installs it */
    snprintf(buf, sz, "%s%s%s\\", tmp, FXU_WORK, fxu_testing ? "-test" : "");
    return 1;
}

/* the game's own folder, where its .exe is (with the backslash) */
static int
fxu_installdir(char *buf, size_t sz)
{
    char *sep;
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD) sz);

    if (!n || n >= sz || !(sep = strrchr(buf, '\\')))
        return 0;
    sep[1] = '\0';
    return 1;
}

static int
fxu_exists(const char *p)
{
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

/* a folder and all in it */
static void
fxu_rmtree(const char *dir)
{
    char pat[MAX_PATH * 2], sub[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pat, sizeof pat, "%s\\*", dir);
    if ((h = FindFirstFileA(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                fxu_rmtree(sub);
            else
                (void) DeleteFileA(sub);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    (void) RemoveDirectoryA(dir);
}

/* the files under root\rel, as paths relative to root, into the list */
static void
fxu_collect(const char *root, const char *rel, char ***list, int *n,
            int *cap)
{
    char pat[MAX_PATH * 2], sub[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(pat, sizeof pat, "%s%s*", root, rel);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        snprintf(sub, sizeof sub, "%s%s", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            strcat(sub, "\\");
            fxu_collect(root, sub, list, n, cap);
        } else {
            if (*n >= *cap) {
                int more_n = *cap ? *cap * 2 : 64;
                char **more = (char **) realloc(*list, (size_t) more_n
                                                           * sizeof (char *));

                if (!more)
                    break;
                *list = more;
                *cap = more_n;
            }
            (*list)[(*n)++] = fxu_strdup(sub);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* are these two files the same, byte for byte? (an update leaves those
   alone: fewer files moved, fewer that can be in use) */
static int
fxu_same_file(const char *a, const char *b)
{
    WIN32_FILE_ATTRIBUTE_DATA fa, fb;
    static char ba[65536], bb[65536]; /* (main thread only) */
    FILE *pa, *pb;
    size_t na, nb;
    int same = 1;

    if (!GetFileAttributesExA(a, GetFileExInfoStandard, &fa)
        || !GetFileAttributesExA(b, GetFileExInfoStandard, &fb)
        || fa.nFileSizeLow != fb.nFileSizeLow
        || fa.nFileSizeHigh != fb.nFileSizeHigh)
        return 0;
    if (!(pa = fopen(a, "rb")))
        return 0;
    if (!(pb = fopen(b, "rb"))) {
        fclose(pa);
        return 0;
    }
    do {
        na = fread(ba, 1, sizeof ba, pa);
        nb = fread(bb, 1, sizeof bb, pb);
        if (na != nb || memcmp(ba, bb, na))
            same = 0;
    } while (same && na > 0);
    fclose(pa);
    fclose(pb);
    return same;
}

/* the player's own files: an update never writes over these, even if it
   has one (the release zip has an empty score list, for a new install) */
static int
fxu_players(const char *rel)
{
    static const char *const mine[] = {
        "record", "logfile", "xlogfile", "livelog", "perm", "sysconf",
        ".nethackrc", "nethackmodern.rc", "nethackmodern-update.txt",
        FXU_OLD_LIST, 0
    };
    int i;

    if (!_strnicmp(rel, "save\\", 5))
        return 1;
    for (i = 0; mine[i]; i++)
        if (!_stricmp(rel, mine[i]))
            return 1;
    return 0;
}

/* can this copy update itself? It must be an installed game (not a build
   folder), able to write its own folder, with tar.exe (Windows 10 and
   later) to unpack, and the release must have a signed download for it.
   (Worked out once for each release found.) */
int
fxu_can_update(void)
{
    static char known_for[40];
    static int known;
    const struct fxu_result *r = fxu_get();
    char dir[MAX_PATH], p[MAX_PATH + 64];
    HANDLE h;

    if (!r || !r->ok || !r->newer || r->dl_size <= 0 || !r->dl_signed)
        return 0;
    if (known_for[0] && !strcmp(known_for, r->tag))
        return known;
    fxu_copy(known_for, sizeof known_for, r->tag);
    known = 0;
    if (!fxu_installdir(dir, sizeof dir))
        return 0;
    /* a build folder (binary\Release\x64 or vsbinary): the source tree is
       beside it, and a release mustn't overwrite what was built */
    snprintf(p, sizeof p, "%s..\\include\\fx.h", dir);
    if (fxu_exists(p))
        return 0;
    snprintf(p, sizeof p, "%s..\\..\\..\\include\\fx.h", dir);
    if (fxu_exists(p))
        return 0;
    if (!GetSystemDirectoryA(p, MAX_PATH))
        return 0;
    strcat(p, "\\tar.exe");
    if (!fxu_exists(p))
        return 0;
    /* can it write here? (not in Program Files, say) */
    snprintf(p, sizeof p, "%snethackmodern-update.tmp", dir);
    h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                    NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    CloseHandle(h);
    known = 1;
    return 1;
}

/* run a program (its full command line), hidden; 1 if it ended well */
static int
fxu_run(char *cmd, DWORD wait_ms)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                        NULL, &si, &pi))
        return 0;
    if (WaitForSingleObject(pi.hProcess, wait_ms) != WAIT_OBJECT_0)
        (void) TerminateProcess(pi.hProcess, 1);
    (void) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

/* the job: the signature, the zip, the check, the unpacking */
static void
fxu_job_run(void)
{
    char dir[MAX_PATH], zip[MAX_PATH + 256], unpack[MAX_PATH + 16],
        probe[MAX_PATH + 64], cmd[3 * MAX_PATH + 64];
    FILE *fp;
    int ok;

    if (!fxu_workdir(dir, sizeof dir)) {
        fxu_job_fail("there's no temporary folder");
        return;
    }
    (void) CreateDirectoryA(dir, NULL);
    snprintf(probe, sizeof probe, "%sready.txt", dir);
    (void) DeleteFileA(probe); /* (nothing is ready until this ends) */

    /* the signature, the zip, the check (shared with Linux) */
    snprintf(zip, sizeof zip, "%s%s", dir, fxu_jname);
    if (!fxu_job_fetch(zip))
        return;

    /* only now, unpacked (by Windows' own tar) */
    FXU_ASTORE(fxu_jst, FXU_JOB_UNPACK);
    snprintf(unpack, sizeof unpack, "%sunpack", dir);
    fxu_rmtree(unpack);
    (void) CreateDirectoryA(unpack, NULL);
    if (!GetSystemDirectoryA(probe, MAX_PATH)) {
        (void) DeleteFileA(zip);
        fxu_job_fail("tar.exe is missing");
        return;
    }
    snprintf(cmd, sizeof cmd, "\"%s\\tar.exe\" -xf \"%s\" -C \"%s\"", probe,
             zip, unpack);
    ok = fxu_run(cmd, 5 * 60 * 1000);
    (void) DeleteFileA(zip);
    snprintf(probe, sizeof probe, "%s\\" FXU_ZIP_TOP "\\NetHackW.exe", unpack);
    if (!ok || !fxu_exists(probe)) {
        fxu_rmtree(unpack);
        fxu_job_fail(ok ? "the download isn't a NetHack Modern release"
                        : "it could not be unpacked");
        return;
    }

    /* ready: a later run may install it, if this one doesn't */
    snprintf(probe, sizeof probe, "%sready.txt", dir);
    if ((fp = fopen(probe, "wb")) != 0) {
        fprintf(fp, "%s\n", fxu_jtag);
        fclose(fp);
    }
    FXU_ASTORE(fxu_jst, FXU_JOB_READY);
}

static DWORD WINAPI
fxu_job_thread(LPVOID arg)
{
    (void) arg;
    fxu_job_run();
    return 0;
}

/* the job on a thread of its own; 0 if it can't be */
static int
fxu_job_spawn(void)
{
    HANDLE t = CreateThread(NULL, 0, fxu_job_thread, NULL, 0, NULL);

    if (!t)
        return 0;
    CloseHandle(t); /* it ends by itself */
    return 1;
}

/* the version a finished download is for, if there is one */
static int
fxu_ready_tag(char *tag, size_t sz)
{
    char dir[MAX_PATH], p[MAX_PATH + 64], *t, *e;

    tag[0] = '\0';
    if (!fxu_workdir(dir, sizeof dir))
        return 0;
    snprintf(p, sizeof p, "%sready.txt", dir);
    if (!(t = fxu_read_file(p)))
        return 0;
    if ((e = strpbrk(t, "\r\n")) != 0)
        *e = '\0';
    fxu_copy(tag, sz, t);
    free(t);
    return fxu_tag_ok(tag);
}

int
fxu_pending(void)
{
    char tag[40], dir[MAX_PATH], p[MAX_PATH + 64];

    if (!fxu_ready_tag(tag, sizeof tag) || !fxu_workdir(dir, sizeof dir))
        return 0;
    snprintf(p, sizeof p, "%sunpack\\" FXU_ZIP_TOP, dir);
    if (fxu_compare(tag, fxu_ver) > 0 && fxu_exists(p)) {
        fxu_copy(fxu_jtag, sizeof fxu_jtag, tag);
        return 1;
    }
    /* for this version or an older one: done with, so cleared away */
    dir[strlen(dir) - 1] = '\0';
    fxu_rmtree(dir);
    return 0;
}

int
fxu_install(char *why, size_t whysz)
{
    char inst[MAX_PATH], work[MAX_PATH], root[MAX_PATH + 32];
    char dst[MAX_PATH * 2], src[MAX_PATH * 2], old[MAX_PATH * 2 + 8];
    char **list = 0, *done = 0;
    int n = 0, cap = 0, i = 0, j, ok = 1;
    FILE *fp;

    why[0] = '\0';
    if (!fxu_installdir(inst, sizeof inst)
        || !fxu_workdir(work, sizeof work)) {
        fxu_copy(why, whysz, "the game's folder can't be found");
        return 0;
    }
    snprintf(root, sizeof root, "%sunpack\\" FXU_ZIP_TOP "\\", work);
    fxu_collect(root, "", &list, &n, &cap);
    if (n == 0 || !(done = (char *) calloc((size_t) n, 1))) {
        fxu_copy(why, whysz, "the update has no files");
        ok = 0;
    }
    /* done[i]: 1 put in place, 2 put in place over an old one (kept as
       .old until all is done) */
    for (i = 0; ok && i < n; i++) {
        const char *rel = list[i];
        char *bs;

        snprintf(dst, sizeof dst, "%s%s", inst, rel);
        if (fxu_players(rel) && fxu_exists(dst))
            continue; /* the player's: never */
        snprintf(src, sizeof src, "%s%s", root, rel);
        if (fxu_exists(dst) && fxu_same_file(src, dst))
            continue; /* unchanged since the last version: left alone */
        snprintf(old, sizeof old, "%s.old", dst);
        /* its folder, if new (sounds\, say) */
        for (bs = strchr(dst + strlen(inst), '\\'); bs;
             bs = strchr(bs + 1, '\\')) {
            *bs = '\0';
            (void) CreateDirectoryA(dst, NULL);
            *bs = '\\';
        }
        if (fxu_exists(dst)) {
            /* moved aside, not deleted: a running .exe or .dll can be
               renamed, though not written over */
            (void) DeleteFileA(old);
            if (!MoveFileExA(dst, old, 0)) {
                snprintf(why, whysz, "%s is in use", rel);
                ok = 0;
                break;
            }
            done[i] = 2;
        }
        /* copied (not moved), so it takes the folder's permissions */
        if (!CopyFileA(src, dst, FALSE)) {
            snprintf(why, whysz, "%s could not be written", rel);
            if (done[i] == 2)
                (void) MoveFileExA(old, dst, MOVEFILE_REPLACE_EXISTING);
            done[i] = 0;
            ok = 0;
            break;
        }
        if (!done[i])
            done[i] = 1;
    }
    if (!ok) {
        /* undo it all: the game is as it was */
        for (j = i - 1; j >= 0; j--) {
            if (!done || !done[j])
                continue;
            snprintf(dst, sizeof dst, "%s%s", inst, list[j]);
            snprintf(old, sizeof old, "%s.old", dst);
            (void) DeleteFileA(dst);
            if (done[j] == 2)
                (void) MoveFileExA(old, dst, MOVEFILE_REPLACE_EXISTING);
        }
    } else {
        /* the old ones away; those still running (this .exe, its DLLs)
           can't be yet, so they're listed for a later start */
        snprintf(src, sizeof src, "%s" FXU_OLD_LIST, inst);
        fp = fopen(src, "ab");
        for (i = 0; i < n; i++) {
            if (done[i] != 2)
                continue;
            snprintf(old, sizeof old, "%s%s.old", inst, list[i]);
            if (!DeleteFileA(old) && fp)
                fprintf(fp, "%s.old\r\n", list[i]);
        }
        if (fp)
            fclose(fp);
        work[strlen(work) - 1] = '\0';
        fxu_rmtree(work);
    }
    for (i = 0; i < n; i++)
        free(list[i]);
    free(list);
    free(done);
    return ok;
}

void
fxu_cleanup(void)
{
    char inst[MAX_PATH], p[MAX_PATH + 64], f[MAX_PATH * 2];
    char *text, *line, *next;
    struct fxu_buf keep = { 0, 0, 0, 0 };
    FILE *fp;

    if (!fxu_installdir(inst, sizeof inst))
        return;
    snprintf(p, sizeof p, "%s" FXU_OLD_LIST, inst);
    if (!(text = fxu_read_file(p)))
        return;
    for (next = text; (line = fxu_line(&next)) != 0;) {
        size_t l = strlen(line);

        /* only what an update put there: name.old, inside this folder */
        if (l < 5 || strcmp(line + l - 4, ".old") || strstr(line, "..")
            || strchr(line, ':') || line[0] == '\\' || line[0] == '/')
            continue;
        snprintf(f, sizeof f, "%s%s", inst, line);
        if (!DeleteFileA(f) && fxu_exists(f)) {
            (void) fxu_put(&keep, line, l);
            (void) fxu_put(&keep, "\r\n", 2);
        }
    }
    free(text);
    if (keep.n && (fp = fopen(p, "wb")) != 0) {
        fwrite(keep.p, 1, keep.n, fp);
        fclose(fp);
    } else if (!keep.n) {
        (void) DeleteFileA(p);
    }
    free(keep.p);
}

const char *
fxu_restart_path(void)
{
    return (const char *) 0; /* (the .exe is where it was: as ever) */
}

#else /* !_WIN32: the AppImage */

/*
 * On Linux, only the AppImage updates itself: it's a single file, and
 * its launcher says where it is ($APPIMAGE). The new one is downloaded
 * beside it (the same folder, so the same file system), checked, made
 * runnable and only then given its waiting name, with a marker saying
 * which version it is. Installing is one rename over the old file, which
 * on Linux happens whole or not at all; the running game carries on from
 * the old file (it's kept until nothing has it open). A distribution's
 * or a source build has no $APPIMAGE: it gets the release page.
 */
#define FXU_PART ".nethackmodern-update.part"
#define FXU_NEW ".nethackmodern-update.AppImage"
#define FXU_READY ".nethackmodern-update.ready"

static char fxu_restart[1024]; /* the AppImage, once updated: run it */

/* the AppImage this game runs from, and its folder (ending in '/') */
static int
fxu_appimage(char *path, size_t psz, char *dir, size_t dsz)
{
    const char *a = getenv("APPIMAGE");
    struct stat sb;

    if (!a || a[0] != '/' || strlen(a) >= psz || strlen(a) >= dsz
        || stat(a, &sb) != 0 || !S_ISREG(sb.st_mode))
        return 0;
    strcpy(path, a);
    strcpy(dir, a);
    strrchr(dir, '/')[1] = '\0';
    return 1;
}

/* dir/name into buf; 0 if it won't fit */
static int
fxu_in(char *buf, size_t sz, const char *dir, const char *name)
{
    return snprintf(buf, sz, "%s%s", dir, name) < (int) sz;
}

int
fxu_can_update(void)
{
    const struct fxu_result *r = fxu_get();
    char path[1024], dir[1024];

    if (!r || !r->ok || !r->newer || r->dl_size <= 0 || !r->dl_signed)
        return 0;
    /* an AppImage, in a folder the player may write in */
    return fxu_appimage(path, sizeof path, dir, sizeof dir)
           && access(dir, W_OK) == 0;
}

/* the job: the signature, the AppImage, the check, then it waits */
static void
fxu_job_run(void)
{
    char path[1024], dir[1024], part[1100], neu[1100], ready[1100];
    FILE *fp;

    if (!fxu_appimage(path, sizeof path, dir, sizeof dir)
        || !fxu_in(part, sizeof part, dir, FXU_PART)
        || !fxu_in(neu, sizeof neu, dir, FXU_NEW)
        || !fxu_in(ready, sizeof ready, dir, FXU_READY)) {
        fxu_job_fail("the AppImage can't be found");
        return;
    }
    (void) remove(ready); /* (nothing is ready until this ends) */
    if (!fxu_job_fetch(part))
        return;
    /* runnable, and then under its waiting name: only a whole, checked
       file ever has it */
    FXU_ASTORE(fxu_jst, FXU_JOB_UNPACK);
    if (chmod(part, 0755) != 0 || rename(part, neu) != 0) {
        (void) remove(part);
        fxu_job_fail("it can't be saved beside the game");
        return;
    }
    if ((fp = fopen(ready, "w")) != 0) {
        fprintf(fp, "%s\n", fxu_jtag);
        fclose(fp);
    }
    FXU_ASTORE(fxu_jst, FXU_JOB_READY);
}

static void *
fxu_job_thread(void *arg)
{
    (void) arg;
    fxu_job_run();
    return 0;
}

/* the job on a thread of its own; 0 if it can't be */
static int
fxu_job_spawn(void)
{
    pthread_t t;

    if (pthread_create(&t, NULL, fxu_job_thread, NULL) != 0)
        return 0;
    (void) pthread_detach(t); /* it ends by itself */
    return 1;
}

int
fxu_pending(void)
{
    char path[1024], dir[1024], neu[1100], ready[1100], tag[40], *t, *e;
    struct stat sb;

    if (!fxu_appimage(path, sizeof path, dir, sizeof dir)
        || !fxu_in(neu, sizeof neu, dir, FXU_NEW)
        || !fxu_in(ready, sizeof ready, dir, FXU_READY)
        || !(t = fxu_read_file(ready)))
        return 0;
    if ((e = strpbrk(t, "\r\n")) != 0)
        *e = '\0';
    fxu_copy(tag, sizeof tag, t);
    free(t);
    if (fxu_tag_ok(tag) && fxu_compare(tag, fxu_ver) > 0
        && stat(neu, &sb) == 0 && S_ISREG(sb.st_mode)) {
        fxu_copy(fxu_jtag, sizeof fxu_jtag, tag);
        return 1;
    }
    /* for this version or an older one: done with */
    (void) remove(neu);
    (void) remove(ready);
    return 0;
}

int
fxu_install(char *why, size_t whysz)
{
    char path[1024], dir[1024], neu[1100], ready[1100];

    why[0] = '\0';
    if (!fxu_appimage(path, sizeof path, dir, sizeof dir)
        || !fxu_in(neu, sizeof neu, dir, FXU_NEW)
        || !fxu_in(ready, sizeof ready, dir, FXU_READY)) {
        fxu_copy(why, whysz, "the AppImage can't be found");
        return 0;
    }
    /* the one step: all of the new file in place of the old, or nothing */
    if (rename(neu, path) != 0) {
        snprintf(why, whysz, "%s", strerror(errno));
        return 0;
    }
    (void) remove(ready);
    fxu_copy(fxu_restart, sizeof fxu_restart, path);
    return 1;
}

void
fxu_cleanup(void)
{
    char path[1024], dir[1024], part[1100];

    /* a download cut off when the game closed */
    if (fxu_appimage(path, sizeof path, dir, sizeof dir)
        && fxu_in(part, sizeof part, dir, FXU_PART))
        (void) remove(part);
}

/* after an update, the game starts again from this (the new AppImage):
   not from the running program, which is the old one's */
const char *
fxu_restart_path(void)
{
    return fxu_restart[0] ? fxu_restart : (const char *) 0;
}

#endif /* !_WIN32 */

/* start downloading the release found (both systems) */
void
fxu_update_start(void)
{
    const struct fxu_result *r = fxu_get();
    int st = (int) FXU_ALOAD(fxu_jst);

    if (!fxu_can_update()
        || (st != FXU_JOB_NONE && st != FXU_JOB_FAILED))
        return; /* (can't, or under way, or done) */
    fxu_copy(fxu_jtag, sizeof fxu_jtag, r->tag);
    fxu_asset_name(r->tag, fxu_jname, sizeof fxu_jname);
    if (fxu_testing) {
        /* a test release: its files beside its JSON */
        char *sep;

        fxu_copy(fxu_jbase, sizeof fxu_jbase, fxu_url);
        sep = strrchr(fxu_jbase, '/');
        if (!sep || strrchr(fxu_jbase, '\\') > sep)
            sep = strrchr(fxu_jbase, '\\');
        if (sep)
            sep[1] = '\0';
        else
            fxu_jbase[0] = '\0';
    } else {
        /* made here from the tag, never taken from the reply */
        snprintf(fxu_jbase, sizeof fxu_jbase, "%s/download/%s/",
                 FXU_PAGE_URL, r->tag);
    }
    FXU_ASTORE(fxu_jdone, 0);
    FXU_ASTORE(fxu_jtotal, r->dl_size);
    FXU_ASTORE(fxu_jst, FXU_JOB_DOWNLOAD);
    if (!fxu_job_spawn())
        fxu_job_fail("it could not be started");
}
