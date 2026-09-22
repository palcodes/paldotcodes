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
./palsite publish        # build, and make a running `serve` reload it
./palsite serve -p 8080  # production: serve dist/ from RAM (127.0.0.1 only;
                         #   add --lan to listen on all interfaces)
./palsite new "Title"    # scaffold content/words/title.org
```

`dev` watches `content/`, `templates/`, `static/` and `site.conf`; save in
Emacs, refresh the browser. The server pre-loads the whole site into memory,
answers with ETags/304s, and needs well under a millisecond per request.

## Hosting

**Self-hosted on this machine, reached through Tailscale Funnel.** Funnel
terminates HTTPS on a `*.ts.net` name and forwards to `palsite serve` on
`127.0.0.1:8080`; nothing listens on the LAN, and no router ports are open.

```
internet -> https://<machine>.<tailnet>.ts.net -> tailscaled -> 127.0.0.1:8080 (palsite serve)
```

`scripts/selfhost.ps1` keeps the server running:

```powershell
.\scripts\selfhost.ps1 install     # static build into .selfhost\, start at every logon
.\scripts\selfhost.ps1 status      # task, listener, funnel, log tail
.\scripts\selfhost.ps1 restart     # after changing src/: recompile + restart
.\scripts\selfhost.ps1 uninstall
```

It registers a logon task (no admin needed) that runs a hidden supervisor,
which restarts the server within 5 s if it ever dies. The server binary is
compiled with `-static` because the default MinGW build loads
`libstdc++-6.dll` from `PATH`, and outside Git Bash that can be a
mismatched copy (exit `0xC0000139`).

**Publishing is manual:** edit, preview with `palsite dev`, then run
`./palsite publish`. That rebuilds `dist/` and rewrites `.published`; the live
server notices within a second and swaps the new site in atomically. A failed
build leaves the stamp alone, so the old site stays up.

Funnel setup (once): install Tailscale, `tailscale up`, enable HTTPS and
Funnel for the tailnet in the admin console, then
`tailscale funnel --bg 8080`. The funnel config persists across reboots. Set
`url` in `site.conf` to the `https://….ts.net` address so canonical links
and the feed point to the right place.

The site is only up while this machine is on and not asleep.

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
