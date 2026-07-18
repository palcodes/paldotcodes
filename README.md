# pal.codes

My personal site. Written in Emacs org-mode, rendered to static HTML by a
single dependency-free C++ binary that also serves it.

```
content/words/*.org   articles            -> /words/<slug>/
content/pages/*.org   standalone pages    -> /<name>/
templates/*.html      page shells         ({{var}} + {{include:file}})
static/               css, images         (copied into dist/ as-is)
site.conf             title, url, author, redirects
src/                  the tool            (C++17, stdlib only)
```

## Build the tool

Any C++17 compiler, no libraries:

```sh
make                # -> ./palsite (palsite.exe on Windows)
make release        # static binary for deployment
```

or by hand: `g++ -std=c++17 -O2 -o palsite src/*.cpp` (add `-lws2_32` on
Windows, `-lpthread` on Linux).

> Windows note: Smart App Control judges each freshly compiled binary by
> hash reputation. If a build refuses to run ("Application Control policy
> has blocked this file"), just recompile — a new hash gets a new verdict.
> Do not name the binary `site.exe`; that name is permanently flagged.

## Use it

```sh
./palsite dev            # build + serve + rebuild on save  (writing mode)
./palsite build          # render content/ -> dist/
./palsite serve -p 8080  # production: serve dist/ from RAM
./palsite new "Title"    # scaffold content/words/title.org
```

`dev` watches `content/`, `templates/`, `static/` and `site.conf`; save in
Emacs, refresh the browser. The server pre-loads the whole site into memory,
answers with ETags/304s, and needs well under a millisecond per request.

Deploying is either of:
- copy `palsite` + `content/ templates/ static/ site.conf` to a box and run
  `palsite build && palsite serve -p 8080` behind your TLS proxy, or
- run `palsite build` anywhere and host `dist/` on any static host.

## Writing

Articles are plain org files; `/words/writing-here/` is the living reference.
The short version:

```org
#+TITLE: The title
#+DATE: 2026-07-18
#+SUBTITLE: Optional italic dek under the title.
#+DRAFT: t                        (excluded from the build while set)

* Headings, *bold*, /italic/, ~code~, =verbatim=, [[https://x][links]]

Sidenote/citation: text[fn:1] ... then anywhere:  [fn:1] The note.
Margin note (unnumbered): [fn::Straight into the margin.]

Math: \( e^{i\pi} \) inline, \[ ... \] display — becomes MathML at
build time, no JS. cases/pmatrix/aligned environments supported.

#+CAPTION: Figure caption.
[[file:/img/photo.png]]           (drop files in static/img/)

#+begin_src c ... #+end_src       code blocks with a language tag
#+begin_quote ... #+end_quote     with `-- Author` attribution line
#+begin_note ... #+end_note       framed aside box
| tables | like | org |           with |---| header rule
```

All styling lives in [static/style.css](static/style.css) — design tokens
(colors, fonts, measure) are CSS variables at the top of the file. Templates
are plain HTML; neither requires recompiling the tool.
