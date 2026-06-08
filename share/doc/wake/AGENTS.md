# share/doc/wake/ — User-facing Documentation

Markdown and AsciiDoc files describing Wake to users (not to runtime/library implementors). Reachable from the repo root as `./docs/` (a symlink). Generated HTML is published at https://sifiveinc.github.io/wake/.

## Layout

```
README.md                          (none — start with tutorial.md or quickref.md)
basic_wake_tutorial.md             Beginner-friendly intro
tutorial.md                        Full language tour with C/C++ build examples
quickref.md                        Cheat sheet: syntax, operators, CLI, runner model
logging.md                         Loggers, log routing, fd 3/4 conventions
datastructures.txt                 Big-O table for stdlib data structures
wake-for-fsharp-developers.md      F# → Wake translation guide
wake-for-scala-developers.adoc     Scala → Wake translation guide

tour/
  packages.adoc                    Package, export, import, re-export
  targets.adoc                     `target` memoization
  tuples.adoc                      tuple types, accessors, backwards compat

how-to/
  create-a-release.adoc            Release procedure
  test-the-vscode-extension.adoc   Extension dev workflow

syntax/                            Editor syntax files (emacs, joe, vim)
```

## When updating

- **`tutorial.md` and `basic_wake_tutorial.md`** are the entry points for new users — keep examples runnable against current `wake`. Test snippets manually.
- **`quickref.md` has known drift** with the live stdlib (e.g., `runJob` is no longer a top-level function — `runJobWith` replaced it; `RunnerFilter` is no longer a Plan field). When you change the stdlib in a way that affects the cheat sheet, update this file.
- **`logging.md`** must match `share/wake/lib/core/print.wake` (LogLevel definitions) and the CLI flag table in `tools/wake/cli_options.h`.
- **`datastructures.txt`** must match the actual implementation complexities in `share/wake/lib/core/{list,tree,vector,map}.wake`.
- **`tour/*.adoc`** assumes current syntax. If you change package import syntax, `target`, or tuple accessor naming, update these.
- **`how-to/create-a-release.adoc`** is the canonical release procedure. Keep it in sync with `debian/changelog.in`, `wake.spec.in`, and `.github/workflows/build.yaml`.

## House style

- Markdown for general docs; AsciiDoc (`.adoc`) for the tour/how-to series (legacy choice — match neighbors).
- Code samples use Wake's actual syntax, not pseudo-code. Prefer self-contained snippets.
- Cite stdlib paths with full `share/wake/lib/...` paths so readers can navigate.

## Cross-references

- For developer-facing summaries (intended for coding agents), see `../../../knowledge/`. The KB summarizes these docs and flags drift; the source-of-truth for *concepts* lives here, but the source-of-truth for *current API* lives in `share/wake/lib/`.
