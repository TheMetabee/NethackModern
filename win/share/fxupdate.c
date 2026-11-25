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
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fxupdate.h"

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
static struct fxu_result res; /* the answer; see above for who may touch */

/* ---- the status, shared by the two threads -------------------------- */

#ifdef _WIN32
static volatile LONG fxu_st = FXU_IDLE;
#define FXU_SET(v) ((void) InterlockedExchange(&fxu_st, (LONG) (v)))
#define FXU_GETST() ((int) InterlockedCompareExchange(&fxu_st, 0, 0))
#else
static int fxu_st = FXU_IDLE;
#define FXU_SET(v) __atomic_store_n(&fxu_st, (v), __ATOMIC_RELEASE)
#define FXU_GETST() __atomic_load_n(&fxu_st, __ATOMIC_ACQUIRE)
#endif

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

#ifdef _WIN32

static char *
fxu_fetch_net(const char *url)
{
    wchar_t wurl[1024], host[256], path[1024], extra[1024], agent[80];
    URL_COMPONENTS uc;
    HINTERNET ses = 0, con = 0, req = 0;
    DWORD code = 0, sz = sizeof code, got = 0;
    struct fxu_buf b = { 0, 0, 0, 0 };
    char tmp[8192];
    int good = 0;

    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024)
        || !MultiByteToWideChar(CP_UTF8, 0, fxu_agent, -1, agent, 80))
        return 0;
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
                              L"Accept: application/vnd.github+json\r\n",
                              (DWORD) -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(req, NULL)
        && WinHttpQueryHeaders(req,
                               WINHTTP_QUERY_STATUS_CODE
                                   | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                               WINHTTP_NO_HEADER_INDEX)
        && code == 200) {
        good = 1;
        while (WinHttpReadData(req, tmp, sizeof tmp, &got) && got > 0)
            if (!fxu_put(&b, tmp, got)) {
                good = 0;
                break;
            }
    }
    if (req)
        WinHttpCloseHandle(req);
    if (con)
        WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    if (!good) {
        free(b.p);
        return 0;
    }
    return b.p;
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

static size_t
fxu_curl_write(char *p, size_t size, size_t n, void *ud)
{
    return fxu_put((struct fxu_buf *) ud, p, size * n) ? size * n : 0;
}

/* 1 if libcurl was there and answered (*out set, or null on failure);
   0 if there's no libcurl */
static int
fxu_fetch_libcurl(const char *url, char **out)
{
    static const char *const libs[] = {
        "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so", 0
    };
    void *lib = 0, *h;
    fxu_curl_init_fn c_init;
    fxu_curl_setopt_fn c_set;
    fxu_curl_perform_fn c_perform;
    fxu_curl_cleanup_fn c_cleanup;
    struct fxu_buf b = { 0, 0, 0, 0 };
    int i, rc;

    *out = 0;
    for (i = 0; libs[i] && !lib; i++)
        lib = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return 0;
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
        return 0;
    }
    (void) c_set(h, FXU_CURLOPT_URL, url);
    (void) c_set(h, FXU_CURLOPT_USERAGENT, fxu_agent);
    (void) c_set(h, FXU_CURLOPT_WRITEFUNCTION, fxu_curl_write);
    (void) c_set(h, FXU_CURLOPT_WRITEDATA, (void *) &b);
    (void) c_set(h, FXU_CURLOPT_FOLLOWLOCATION, 1L);
    (void) c_set(h, FXU_CURLOPT_MAXREDIRS, 5L);
    (void) c_set(h, FXU_CURLOPT_FAILONERROR, 1L);
    (void) c_set(h, FXU_CURLOPT_CONNECTTIMEOUT, 8L);
    (void) c_set(h, FXU_CURLOPT_TIMEOUT, 20L);
    (void) c_set(h, FXU_CURLOPT_NOSIGNAL, 1L); /* we're not the main thread */
    rc = c_perform(h);
    c_cleanup(h);
    /* (libcurl stays loaded: unloading it can upset its TLS library) */
    if (rc != 0 || b.over) {
        free(b.p);
        return 1;
    }
    *out = b.p;
    return 1;
}

/* a program's output (curl or wget), or null */
static char *
fxu_fetch_program(const char *argv[])
{
    struct fxu_buf b = { 0, 0, 0, 0 };
    char tmp[8192];
    int fd[2], status = 0;
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
        if (!fxu_put(&b, tmp, (size_t) n))
            break;
    close(fd[0]);
    (void) waitpid(pid, &status, 0);
    if (b.over || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(b.p);
        return 0;
    }
    return b.p;
}

static char *
fxu_fetch_net(const char *url)
{
    char *got;
    const char *curl[] = { "curl", "-sfL", "--max-time", "20", "-A", 0, 0, 0 };
    const char *wget[] = { "wget", "-q", "-O", "-", "--timeout=20", "-U",
                           0, 0, 0 };

    if (fxu_fetch_libcurl(url, &got))
        return got;
    curl[5] = fxu_agent, curl[6] = url;
    if ((got = fxu_fetch_program(curl)) != 0)
        return got;
    wget[6] = fxu_agent, wget[7] = url;
    return fxu_fetch_program(wget);
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
    fprintf(fp, "checked=%ld\nlatest=%s\nskip=%s\ntitle=%s\nnotes:\n%s",
            st->checked, st->latest, st->skip, st->title,
            st->notes ? st->notes : "");
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
