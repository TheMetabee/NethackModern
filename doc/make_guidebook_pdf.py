#!/usr/bin/env python3
"""Build a typeset Guidebook.pdf from doc/Guidebook.tex.

NetHack ships the Guidebook as plain text (Guidebook.txt, generated from
Guidebook.mn).  This script turns the LaTeX version into a styled HTML page
and prints it to PDF with headless Chrome or Edge, so no TeX installation
is needed.

Usage:
    python make_guidebook_pdf.py [--tex Guidebook.tex] [--out Guidebook.pdf]
                                 [--html Guidebook.html] [--browser PATH]

It only understands the subset of LaTeX that Guidebook.tex actually uses.
An unknown command is reported on stderr and its name is dropped, so new
markup in the Guidebook shows up as a warning rather than a broken build.
"""

import argparse
import html
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# Sentinels used while building the body: paragraph breaks and the edges of
# block-level elements, which must not end up inside a <p>.
PB = "\x02"          # paragraph break
BS = "\x03"          # block start
BE = "\x04"          # block end
VERB = "\x05"        # placeholder for verbatim text: VERB + index + VERB


def block(s):
    return BS + s + BE


def paragraphize(s):
    """Wrap runs of inline text in <p>, leaving block elements alone.

    Blocks nest (a <dd> holds paragraphs and lists), so walk the string and
    track depth rather than matching sentinels with a regex.
    """
    out = []
    depth = 0
    buf = []
    blk = []

    def flush_inline():
        text = "".join(buf)
        for para in text.split(PB):
            if para.strip():
                out.append("<p>" + para.strip() + "</p>")
        buf.clear()

    for ch in s:
        if ch == BS:
            if depth == 0:
                flush_inline()
            else:
                blk.append(ch)
            depth += 1
        elif ch == BE:
            depth -= 1
            if depth == 0:
                out.append("".join(blk).replace(BS, "").replace(BE, ""))
                blk.clear()
            else:
                blk.append(ch)
        elif depth:
            blk.append(ch)
        else:
            buf.append(ch)
    flush_inline()
    return "\n".join(out)


def slug(text):
    t = re.sub(r"<[^>]+>", "", text)
    t = html.unescape(t).lower()
    return re.sub(r"[^a-z0-9]+", "-", t).strip("-") or "section"


class Converter:
    SIMPLE = {
        "textunderscore": "_", "#": "#", "%": "%", "&": "&amp;", "$": "$",
        "textdollar": "$", "textasciicircum": "^", "textbackslash": "\\",
        "textasciigrave": "`", "textbraceleft": "{", "textbraceright": "}",
        "{": "{", "}": "}", "textquotesingle": "'", "textasciitilde": "~",
        "ldots": "\u2026", "times": "\u00d7", " ": " ", "space": " ",
        ",": "\u2009", ":": "\u2005", "!": "", "-": "\u00ad",
        "_": "_", "LaTeX": "LaTeX",
    }
    # Commands whose effect we ignore (and whose arguments we discard).
    SKIP_ARGS = {
        "hyphenation": 1, "newlength": 1, "settowidth": 2, "addtolength": 2,
        "usepackage": 1, "documentclass": 1, "setlist": 2,
    }
    IGNORE = {"noindent", "small", "LARGE", "Large", "large", "normalsize",
              "hline", "endhead", "relax", "centering", "sloppy"}
    # Declarations that change the font for the rest of the group.
    DECL = {
        "tt": ("<code>", "</code>"), "ttfamily": ("<code>", "</code>"),
        "it": ("<em>", "</em>"), "itshape": ("<em>", "</em>"),
        "rm": ('<span class="rm">', "</span>"),
        "mdseries": ('<span class="md">', "</span>"),
        "bf": ("<strong>", "</strong>"), "bfseries": ("<strong>", "</strong>"),
    }
    WRAP = {
        "textit": ("<em>", "</em>"), "emph": ("<em>", "</em>"),
        "textbf": ("<strong>", "</strong>"),
        "texttt": ("<code>", "</code>"),
        "textrm": ('<span class="rm">', "</span>"),
        "textmd": ('<span class="md">', "</span>"),
        "mbox": ('<span class="nowrap">', "</span>"),
    }

    def __init__(self, src):
        self.verbatim = []
        self.warned = set()
        self.title = self.author = self.date = ""
        self.toc = []
        self.secno = 0
        self.ids = set()
        self.code_depth = 0
        self.src = self.prepare(src)
        self.pos = 0

    # ---- preprocessing -------------------------------------------------
    def stash(self, text, kind):
        self.verbatim.append((kind, text))
        return VERB + str(len(self.verbatim) - 1) + VERB

    def prepare(self, s):
        s = s.replace("\r\n", "\n")
        # Verbatim blocks first: they may contain % and backslashes.
        s = re.sub(r"\\begin\{verbatim\}\n?(.*?)\\end\{verbatim\}",
                   lambda m: self.stash(m.group(1), "pre"), s, flags=re.S)
        s = re.sub(r"\\verb(.)(.*?)\1",
                   lambda m: self.stash(m.group(2), "code"), s)
        # Comments: an unescaped % eats the rest of the line, the newline
        # and the next line's leading blanks, just as TeX does.
        s = re.sub(r"(?<!\\)%[^\n]*\n[ \t]*", "", s)
        # Blank lines are paragraph breaks.
        s = re.sub(r"\n[ \t]*\n\s*", PB, s)
        m = re.search(r"\\begin\{document\}(.*)\\end\{document\}", s, re.S)
        self.preamble = s[:m.start()] if m else ""
        return m.group(1) if m else s

    def warn(self, msg):
        if msg not in self.warned:
            self.warned.add(msg)
            print("warning: " + msg, file=sys.stderr)

    # ---- low-level reading ---------------------------------------------
    def peek(self, n=0):
        p = self.pos + n
        return self.src[p] if p < len(self.src) else ""

    def skip_ws(self, newlines=True):
        while self.pos < len(self.src) and self.src[self.pos] in " \t\n":
            if self.src[self.pos] == "\n" and not newlines:
                break
            self.pos += 1

    def read_raw_group(self):
        """Return the raw text of the next {...} without converting it."""
        self.skip_ws()
        if self.peek() != "{":
            # A single token argument, e.g. \texttt x
            ch = self.peek()
            self.pos += 1
            return ch
        depth = 0
        start = self.pos
        while self.pos < len(self.src):
            ch = self.src[self.pos]
            if ch == "\\":
                self.pos += 2
                continue
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    self.pos += 1
                    return self.src[start + 1:self.pos - 1]
            self.pos += 1
        return self.src[start + 1:]

    def read_raw_optional(self):
        """Return the raw text of a following [...] or None."""
        save = self.pos
        self.skip_ws()
        if self.peek() != "[":
            self.pos = save
            return None
        # As in LaTeX, only braces nest: the first ] outside braces ends
        # the argument, so \item[{[}] has the label "[".
        depth = 0
        start = self.pos
        self.pos += 1
        while self.pos < len(self.src):
            ch = self.src[self.pos]
            if ch == "\\":
                self.pos += 2
                continue
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            elif ch == "]" and depth == 0:
                self.pos += 1
                return self.src[start + 1:self.pos - 1]
            self.pos += 1
        return self.src[start + 1:]

    def sub(self, text):
        """Convert a fragment of LaTeX with a fresh reader."""
        save_src, save_pos = self.src, self.pos
        self.src, self.pos = text, 0
        try:
            return self.parse(stop=None)
        finally:
            self.src, self.pos = save_src, save_pos

    def read_arg(self):
        return self.sub(self.read_raw_group())

    def read_cmd_name(self):
        # Assumes self.src[self.pos] == "\\"
        self.pos += 1
        m = re.compile(r"[A-Za-z@]+\*?").match(self.src, self.pos)
        if m:
            self.pos = m.end()
            # As in TeX, blanks after a control word are not text.
            self.skip_ws()
            return m.group(0)
        ch = self.peek()
        self.pos += 1
        return ch

    # ---- main parser ---------------------------------------------------
    def text_char(self, ch):
        """Translate one ordinary character, handling TeX ligatures."""
        s = self.src
        p = self.pos
        if ch == "~":
            self.pos += 1
            return "\u00a0"
        if self.code_depth == 0:
            if s.startswith("---", p):
                self.pos += 3
                return "\u2014"
            if s.startswith("--", p):
                self.pos += 2
                return "\u2013"
            if s.startswith("``", p):
                self.pos += 2
                return "\u201c"
            if s.startswith("''", p):
                self.pos += 2
                return "\u201d"
            if ch == "`":
                self.pos += 1
                return "\u2018"
            if ch == "'":
                self.pos += 1
                return "\u2019"
        self.pos += 1
        if ch == "\n":
            return " "
        return html.escape(ch, quote=False)

    def parse(self, stop):
        """Convert until `stop` is reached.

        stop is None (end of input), "}" (end of group), "]" (end of an
        optional argument) or ("env", name) for \\end{name}.  Returns HTML.
        """
        out = []
        while self.pos < len(self.src):
            ch = self.src[self.pos]
            if stop == "}" and ch == "}":
                return "".join(out)
            if stop == "]" and ch == "]":
                return "".join(out)
            if ch == "{":
                self.pos += 1
                inner = self.parse("}")
                self.pos += 1
                out.append(inner)
            elif ch == "}":
                # Stray close brace (unbalanced input): skip it.
                self.pos += 1
            elif ch == VERB:
                end = self.src.index(VERB, self.pos + 1)
                kind, text = self.verbatim[int(self.src[self.pos + 1:end])]
                self.pos = end + 1
                if kind == "pre":
                    out.append(block('<pre class="screen">' +
                                     html.escape(text.rstrip("\n").rstrip(),
                                                 quote=False)
                                     + "</pre>"))
                else:
                    out.append('<code class="verb">' +
                               html.escape(text, quote=False) + "</code>")
            elif ch == "$":
                self.pos += 1                 # math shift: just drop it
            elif ch == "\\":
                if self.src.startswith("\\end{", self.pos):
                    if isinstance(stop, tuple) and stop[0] == "env":
                        save = self.pos
                        self.read_cmd_name()
                        name = self.read_raw_group()
                        if name == stop[1]:
                            return "".join(out)
                        self.pos = save
                    if isinstance(stop, tuple):
                        return "".join(out)
                out.append(self.command(stop))
            elif ch == PB:
                self.pos += 1
                out.append(PB)
            elif ch == "&":
                self.pos += 1
                out.append("&amp;")
            else:
                out.append(self.text_char(ch))
        return "".join(out)

    def rest_of_group(self, stop):
        # Used for declarations like {\tt foo}: everything up to the end of
        # the enclosing group, argument or environment.
        return self.parse(stop)

    def command(self, stop):
        name = self.read_cmd_name()
        if name == "\\":
            self.read_raw_optional()          # \\[2mm]
            return "<br>"
        if name in self.SIMPLE:
            return self.SIMPLE[name]
        if name in ('"', "`", "'", "^", "~"):
            arg = self.read_raw_group()
            accents = {'"': "\u0308", "`": "\u0300", "'": "\u0301",
                       "^": "\u0302", "~": "\u0303"}
            return html.escape(arg) + accents[name]
        if name in self.IGNORE:
            return ""
        if name in self.SKIP_ARGS:
            for _ in range(self.SKIP_ARGS[name]):
                self.read_raw_optional()
                self.read_raw_group()
            return ""
        if name in self.WRAP:
            a, b = self.WRAP[name]
            if a == "<code>":
                self.code_depth += 1
            try:
                return a + self.read_arg() + b
            finally:
                if a == "<code>":
                    self.code_depth -= 1
        if name in self.DECL:
            a, b = self.DECL[name]
            if a == "<code>":
                self.code_depth += 1
            try:
                return a + self.rest_of_group(stop) + b
            finally:
                if a == "<code>":
                    self.code_depth -= 1
        if name in ("medskip", "bigskip", "smallskip"):
            return block('<div class="%s"></div>' % name)
        if name in ("clearpage", "newpage", "pagebreak"):
            return block('<div class="pagebreak"></div>')
        if name == "vbox":
            return block('<div class="keep">' +
                         paragraphize(self.read_arg()) + "</div>")
        if name == "makebox":
            width = self.read_raw_optional()
            self.read_raw_optional()
            inner = self.read_arg()
            if not inner.strip() and width:
                return '<span class="gap"></span>'
            return inner
        if name == "newcommand":
            self.read_raw_group()
            self.read_raw_optional()
            self.read_raw_group()
            return ""
        if name == "numbox":
            return '<code class="numbox">' + self.read_arg() + "</code>"
        if name == "url":
            url = self.read_raw_group()
            return '<a href="%s">%s</a>' % (html.escape(url),
                                            html.escape(url, quote=False))
        if name == "title":
            self.title = self.read_arg()
            return ""
        if name == "author":
            self.author = self.read_arg()
            return ""
        if name == "date":
            self.date = self.read_arg()
            return ""
        if name == "maketitle":
            return block("\x06TITLEPAGE\x06")
        if name in ("section", "section*", "subsection", "subsection*",
                    "subsubsection", "subsubsection*"):
            return self.heading(name)
        if name == "begin":
            return self.environment(self.read_raw_group())
        if name == "item":
            # Only reached for an \item outside a list; treat as text.
            self.read_raw_optional()
            return ""
        if name == "caption":
            self.read_raw_optional()
            self.read_raw_group()
            return ""
        self.warn("unknown command \\" + name)
        return ""

    def heading(self, name):
        text = self.read_arg().strip()
        level = {"section": 2, "subsection": 3, "subsubsection": 4}[
            name.rstrip("*")]
        num = ""
        if name == "section":
            self.secno += 1
            num = '<span class="secno">%d</span> ' % self.secno
        hid = slug(text)
        base, n = hid, 2
        while hid in self.ids:
            hid = "%s-%d" % (base, n)
            n += 1
        self.ids.add(hid)
        if level <= 3:
            self.toc.append((level, self.secno if level == 2 else None,
                             text, hid))
        return block('<h%d id="%s">%s%s</h%d>' % (level, hid, num, text,
                                                   level))

    # ---- environments --------------------------------------------------
    def environment(self, env):
        if env == "description":
            opts = self.read_raw_optional() or ""
            return block(self.description(opts))
        if env == "itemize" or env == "enumerate":
            self.read_raw_optional()
            tag = "ul" if env == "itemize" else "ol"
            return block("<%s>%s</%s>" % (tag, self.items(env), tag))
        if env == "center":
            inner = self.parse(("env", env))
            return block('<div class="center">' + paragraphize(inner) +
                         "</div>")
        if env in ("sloppypar", "flushleft", "quote", "quotation"):
            inner = self.parse(("env", env))
            return block('<div class="%s">' % env + paragraphize(inner) +
                         "</div>")
        if env in ("tabular", "longtable"):
            spec = self.read_raw_group()
            return block(self.table(env, spec))
        self.warn("unknown environment " + env)
        return self.parse(("env", env))

    def body_until_end(self, env):
        """Raw source up to the matching \\end{env}; position moves past it."""
        depth = 1
        pat = re.compile(r"\\(begin|end)\{" + re.escape(env) + r"\}")
        for m in pat.finditer(self.src, self.pos):
            depth += 1 if m.group(1) == "begin" else -1
            if depth == 0:
                body = self.src[self.pos:m.start()]
                self.pos = m.end()
                return body
        body = self.src[self.pos:]
        self.pos = len(self.src)
        return body

    def split_items(self, body):
        """Split list source into (label, content) pairs at top-level \\item."""
        items = []
        depth = 0
        env_depth = 0
        i = 0
        starts = []
        while i < len(body):
            ch = body[i]
            if ch == "\\":
                if body.startswith("\\begin{", i):
                    env_depth += 1
                elif body.startswith("\\end{", i):
                    env_depth -= 1
                elif body.startswith("\\item", i) and depth == 0 and \
                        env_depth == 0 and not body[i + 5:i + 6].isalpha():
                    starts.append(i)
                i += 2
                continue
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            i += 1
        lead = body[:starts[0]] if starts else body
        for k, st in enumerate(starts):
            end = starts[k + 1] if k + 1 < len(starts) else len(body)
            chunk = body[st + 5:end]
            label = None
            # Optional label in [...]
            save_src, save_pos = self.src, self.pos
            self.src, self.pos = chunk, 0
            label = self.read_raw_optional()
            rest = self.src[self.pos:]
            self.src, self.pos = save_src, save_pos
            items.append((label, rest))
        return lead, items

    def description(self, opts):
        body = self.body_until_end("description")
        lead, items = self.split_items(body)
        tt = "ttfamily" in opts
        compact = "labelwidth=*" in opts
        cls = ["desc"]
        if compact:
            cls.append("compact")
        if tt:
            cls.append("tt")
        rows = []
        widest = 0
        for label, content in items:
            if tt:
                self.code_depth += 1
            lab = self.sub(label or "").strip()
            if tt:
                self.code_depth -= 1
            plain = html.unescape(re.sub(r"<[^>]+>", "", lab))
            widest = max(widest, len(plain))
            # Rough width in "average lowercase letters"; capitals in the
            # bold label font are much wider than lowercase.
            est = sum(1.45 if c.isupper() else 1.0 for c in plain)
            long_label = (not compact) and est > 14.5
            dd = paragraphize(self.sub(content))
            rows.append('<div class="item%s"><dt>%s</dt><dd>%s</dd></div>'
                        % (" long" if long_label else "", lab, dd))
        style = ""
        if compact:
            # Size the label column to the longest label, as the LaTeX
            # version does with \settowidth.
            style = ' style="--lw: %.1fch"' % (widest * (1.0 if tt else 1.1)
                                                + 1)
        out = ['<dl class="%s"%s>' % (" ".join(cls), style)]
        if lead.strip(PB + " \n"):
            out.append(paragraphize(self.sub(lead)))
        out += rows
        out.append("</dl>")
        return "\n".join(out)

    def items(self, env):
        body = self.body_until_end(env)
        lead, items = self.split_items(body)
        return "".join("<li>%s</li>" % paragraphize(self.sub(c))
                       for _, c in items)

    def split_top(self, text, sep):
        parts, depth, cur, i = [], 0, [], 0
        while i < len(text):
            if text.startswith(sep, i) and depth == 0:
                parts.append("".join(cur))
                cur = []
                i += len(sep)
                continue
            ch = text[i]
            if ch == "\\" and not text.startswith(sep, i):
                cur.append(text[i:i + 2])
                i += 2
                continue
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            cur.append(ch)
            i += 1
        parts.append("".join(cur))
        return parts

    def table(self, env, spec):
        body = self.body_until_end(env)
        caption = None
        m = re.search(r"\\caption(\[[^\]]*\])?\{([^}]*)\}\s*\\\\", body)
        if m:
            caption = self.sub(m.group(2))
            body = body[:m.start()] + body[m.end():]
        head_rows = []
        if "\\endhead" in body:
            head, body = body.split("\\endhead", 1)
            head_rows = [r for r in self.split_top(head, "\\\\")
                         if r.replace("\\hline", "").strip(" \n" + PB)]
        aligns = [c for c in spec if c in "lcr"]
        rows = [r for r in self.split_top(body, "\\\\")
                if r.replace("\\hline", "").strip(" \n" + PB)]
        cls = "symbols" if env == "longtable" else "tab"
        if any(VERB in r for r in rows):
            cls += " verbtab"
        out = ['<table class="%s">' % cls]
        if caption:
            out.append("<caption>%s</caption>" % caption)

        def cells(row, tag):
            cs = self.split_top(row.replace("\\hline", ""), "&")
            html_cells = []
            for k, c in enumerate(cs):
                a = aligns[k] if k < len(aligns) else "l"
                content = self.sub(c.strip(" \n\t" + PB)).strip()
                html_cells.append('<%s class="a%s">%s</%s>' % (tag, a,
                                                               content, tag))
            return "<tr>" + "".join(html_cells) + "</tr>"

        if head_rows:
            out.append("<thead>" + "".join(cells(r, "th") for r in head_rows)
                       + "</thead>")
        out.append("<tbody>" + "".join(cells(r, "td") for r in rows) +
                   "</tbody>")
        out.append("</table>")
        return "\n".join(out)

    # ---- whole document -----------------------------------------------
    def convert(self):
        body = paragraphize(self.parse(None))
        return body


TITLE_ART = r"""
------------
|..........|
|.....d....|        -------
|...@......+#######+.....|
|..........|        |..>..|
|..)....[..|        |...$.|
------------        -------
""".strip("\n")


def title_art():
    lines = TITLE_ART.split("\n")
    colors = {"@": "hero", "d": "dog", "$": "gold", ">": "stairs",
              ")": "item", "[": "item", "+": "door", "#": "corr"}
    out = []
    for ln in lines:
        row = []
        for ch in ln:
            e = html.escape(ch, quote=False)
            row.append('<span class="%s">%s</span>' % (colors[ch], e)
                       if ch in colors else e)
        out.append("".join(row))
    return "\n".join(out)


def build_html(conv, body, pages=None):
    """Assemble the page.  `pages` maps heading ids to page numbers; on the
    first pass it is None and the TOC gets placeholders of the same width."""
    def pg(hid):
        if pages is None:
            return '<span class="pg">000</span>'
        return '<span class="pg">%s</span>' % pages.get(hid, "")

    toc = ['<nav class="toc"><h2 class="toc-title">Contents</h2><ol>']
    open_sub = False
    for level, num, text, hid in conv.toc:
        if level == 2:
            if open_sub:
                toc.append("</ol></li>")
                open_sub = False
            else:
                if len(toc) > 1:
                    toc.append("</li>")
            toc.append('<li class="sec"><a href="#%s"><span class="n">%d'
                       '</span><span class="t">%s</span><span class="dots">'
                       '</span>%s</a>' % (hid, num, text, pg(hid)))
        else:
            if not open_sub:
                toc.append('<ol class="sub">')
                open_sub = True
            toc.append('<li><a href="#%s"><span class="t">%s</span>'
                       '<span class="dots"></span>%s</a></li>'
                       % (hid, text, pg(hid)))
    if open_sub:
        toc.append("</ol>")
    toc.append("</li></ol></nav>")

    title = conv.title
    # The tex title is "A Guide to ...:<br>Guidebook for NetHack"
    parts = [p.strip().rstrip(":") for p in title.split("<br>")]
    main = parts[0]
    subtitle = parts[1] if len(parts) > 1 else ""
    authors = conv.author.split("<br>")
    titlepage = """
<section class="titlepage">
  <pre class="titleart" aria-hidden="true">%s</pre>
  <h1>%s</h1>
  <p class="subtitle">%s</p>
  <div class="rule"></div>
  <p class="author">%s</p>
  <p class="date">%s</p>
</section>
""" % (title_art(), main, subtitle,
       "<br>".join(a.strip() for a in authors), conv.date)
    body = body.replace("<p>\x06TITLEPAGE\x06</p>", "")
    body = body.replace("\x06TITLEPAGE\x06", titlepage + "\n" + "".join(toc))

    with open(os.path.join(HERE, "guidebook.css"), encoding="utf-8") as f:
        css = f.read()
    plain_title = re.sub(r"<[^>]+>", "", main)
    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>%s</title>
<style>
%s
</style>
</head>
<body>
<main>
%s
</main>
</body>
</html>
""" % (html.escape(plain_title), css, body)


def pdf_dest_pages(pdf_path):
    """Map named destinations (the HTML heading ids) to 1-based page numbers.

    Chrome writes an uncompressed /Dests dictionary of
    "/id [N 0 R /XYZ ...]" entries; page numbers come from walking the
    /Pages tree in order.  Returns {} if the layout isn't recognised, in
    which case the TOC simply has no page numbers.
    """
    with open(pdf_path, "rb") as f:
        data = f.read()
    objs = {}
    for m in re.finditer(rb"(?m)^(\d+) 0 obj\s*(.*?)endobj", data, re.S):
        objs[int(m.group(1))] = m.group(2)
    order = []

    def walk(n, depth=0):
        o = objs.get(n, b"")
        if depth > 50:
            return
        if re.search(rb"/Type\s*/Pages\b", o):
            kids = re.search(rb"/Kids\s*\[([^\]]*)\]", o)
            if kids:
                for k in re.findall(rb"(\d+) 0 R", kids.group(1)):
                    walk(int(k), depth + 1)
        elif re.search(rb"/Type\s*/Page\b", o):
            order.append(n)

    roots = [n for n, o in objs.items()
             if re.search(rb"/Type\s*/Pages\b", o) and b"/Parent" not in o]
    if not roots:
        return {}
    walk(roots[0])
    pageno = {n: i + 1 for i, n in enumerate(order)}
    m = re.search(rb"/Dests\s+(\d+) 0 R", data)
    if not m:
        return {}
    dests = objs.get(int(m.group(1)), b"")
    out = {}
    for name, ref in re.findall(rb"/([^\s\[/]+)\s*\[\s*(\d+) 0 R", dests):
        if int(ref) in pageno:
            out[name.decode("latin-1")] = pageno[int(ref)]
    return out


def find_browser(explicit):
    if explicit:
        return explicit
    candidates = []
    for env in ("PROGRAMFILES", "PROGRAMFILES(X86)", "LOCALAPPDATA"):
        base = os.environ.get(env)
        if base:
            candidates += [
                os.path.join(base, "Google", "Chrome", "Application",
                             "chrome.exe"),
                os.path.join(base, "Microsoft", "Edge", "Application",
                             "msedge.exe"),
            ]
    candidates += [
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    for name in ("google-chrome", "google-chrome-stable", "chromium",
                 "chromium-browser", "chrome", "msedge", "microsoft-edge"):
        p = shutil.which(name)
        if p:
            return p
    return None


def print_pdf(browser, html_path, pdf_path):
    profile = tempfile.mkdtemp(prefix="gbk-chrome-")
    try:
        url = "file:///" + os.path.abspath(html_path).replace("\\", "/") \
            .lstrip("/")
        cmd = [browser, "--headless", "--disable-gpu", "--no-first-run",
               "--no-default-browser-check", "--user-data-dir=" + profile,
               "--no-pdf-header-footer", "--generate-pdf-document-outline",
               "--print-to-pdf=" + os.path.abspath(pdf_path), url]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=300)
    finally:
        shutil.rmtree(profile, ignore_errors=True)
    if not os.path.exists(pdf_path):
        sys.exit("error: the browser did not write " + pdf_path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tex", default=os.path.join(HERE, "Guidebook.tex"))
    ap.add_argument("--out", default=os.path.join(HERE, "Guidebook.pdf"))
    ap.add_argument("--html", help="also keep the intermediate HTML here")
    ap.add_argument("--browser", help="path to chrome or msedge")
    ap.add_argument("--html-only", action="store_true",
                    help="write the HTML (--html) and skip the PDF")
    args = ap.parse_args()

    with open(args.tex, encoding="utf-8", errors="replace") as f:
        src = f.read()
    conv = Converter(src)
    body = conv.convert()

    html_path = args.html
    tmpdir = None
    if not html_path:
        tmpdir = tempfile.mkdtemp(prefix="gbk-")
        html_path = os.path.join(tmpdir, "Guidebook.html")

    def write_html(pages):
        with open(html_path, "w", encoding="utf-8") as f:
            f.write(build_html(conv, body, pages))

    try:
        if args.html_only:
            write_html({})
            print("wrote " + html_path)
            return
        browser = find_browser(args.browser)
        if not browser:
            sys.exit("error: no Chrome, Chromium or Edge found; "
                     "use --browser PATH")
        # Pass 1 lays the book out with placeholder TOC numbers; pass 2
        # fills in the pages each heading landed on.  The placeholders are
        # as wide as the real numbers, so nothing moves between passes.
        write_html(None)
        print_pdf(browser, html_path, args.out)
        pages = pdf_dest_pages(args.out)
        if not pages:
            print("warning: could not read page numbers back from the PDF; "
                  "the contents will have none", file=sys.stderr)
        write_html(pages)
        print_pdf(browser, html_path, args.out)
        print("wrote " + args.out)
    finally:
        if tmpdir:
            shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    main()
