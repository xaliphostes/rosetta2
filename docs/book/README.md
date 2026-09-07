# The rosetta book

`rosetta-book.pdf` — the user manual, in book form: what rosetta is, how the
pipeline works, the complete manifest reference, annotations, the tool, the
runtime object model, writing a backend, and worked examples.

This is the **manual**. The academic write-up of the same system — its thesis,
its experimental evaluation, its related work — is [`../../paper/en`](../../paper/en).
Keep the two apart: the book says how to use rosetta, the paper says why it is
interesting.

## Building

```bash
make          # three pdflatex passes (toc + cross-references settle)
make check    # build, then fail if any \ref is undefined
make quick    # one pass, for a look while writing
make open     # macOS: build and open
make clean    # remove .aux/.log/.out/.toc
```

Needs only `pdflatex` from a **basic** TeX Live: `listings`, `xcolor`,
`booktabs`, `longtable`, `tabularx`, `enumitem`, `fancyhdr`, `parskip`,
`geometry`, `microtype`, `hyperref`, `tikz`. Nothing from `tcolorbox`,
`framed`, `titlesec` or `multirow` — the callout and code styles are built from
primitives so the book compiles wherever pdflatex does. If you add a package,
check it against a basic install first.

The only external file is the title-page logo, `../../media/logo-rosetta.png`.
It is guarded by `\IfFileExists`, so a copy of `docs/book` on its own still
builds — the title page just loses the image. Note that `\IfFileExists` does
**not** consult `\graphicspath`: give it the same literal path you give
`\includegraphics`, or the guard silently reports "missing" and skips a file
that is right there.

## Layout

```
rosetta-book.tex     preamble + \input list. Everything shared lives here.
ch/00-preface.tex    front matter
ch/01..17            the chapters, in reading order
ch/app-a..app-d      the reference appendices
Makefile
```

One file per chapter, `\input` from the main file. Part divisions are in
`rosetta-book.tex`, not in the chapter files.

## Conventions

- `\code{...}` is inline monospace. **Specials must be escaped in the source** —
  `\code{cpp26\_root}`, `\code{\$ENV\{HOME\}}`. Deliberate: the macro stays
  trivial and the compiler catches every miss.
- `\code` rebinds `\_` so every escaped underscore is also a **line-break
  opportunity**. Without that, one long manifest name (`compile_definitions`,
  `std::shared_ptr<Backend>`) overruns the margin in a narrow table cell. Use
  `\bk` to add a break by hand where there is no underscore — a long path, a
  `::` run. Do not "fix" a wide cell by shrinking the font; widen the column.
- `\field{...}` for a manifest field name, `\lang{...}` for a target name. Both
  wrap `\code`; they exist so a future restyling can tell them apart.
- **Inside a tikz node, use `\texttt`, not `\code`.** Nodes set `\scriptsize`
  and `\code` forces `\small` back *up*, which pushes the label past the node's
  `text width`.
- `\begin{note}` and `\begin{warn}` are the two callouts. Use `warn` for things
  that cost an afternoon, `note` for things worth knowing off the main path.
- Code listings: `\begin{lstlisting}[style=X]` with X one of `cpp`, `json`,
  `py`, `js`, `sh`, `plain`. Listings need no escaping.
- `\bk` is a zero-width break opportunity, for long paths in narrow table cells.

## Keeping it true

The book is written from the reference documents in `docs/`, and both are
hand-maintained — there is no generation step, so they can drift. When a
manifest field, a target or an annotation changes, the places to update are:

| Changed | Update |
|---|---|
| a manifest field | `docs/MANIFEST.md`, `ch/04`–`ch/09`, `ch/app-a-manifest.tex` |
| an annotation kind | `docs/ANNOTATIONS.md`, `ch/10`, `ch/app-b-annotations.tex` |
| what the doc harvester reads, or where a backend puts it | `docs/MANIFEST.md`, `ch/11`, `ch/app-c-targets.tex` (§ Documentation output) |
| a target, or what one supports | `docs/MANIFEST.md`, `ch/06`, `ch/app-c-targets.tex` |
| a `rosetta_gen` mode or flag | `docs/ROSETTA_GEN.md`, `ch/12` |
| a coverage reason slug | `docs/COVERAGE.md`, `ch/13`, `ch/app-d-troubleshooting.tex` |

Chapter numbers shift when one is inserted; the numbers above are the current
ones. `ch/11-documentation.tex` was added after `ch/10`, moving the old
`ch/11`–`ch/16` up by one.

`make check` catches broken cross-references. Nothing catches a stale fact —
that is what the table above is for.
