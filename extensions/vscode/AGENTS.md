# extensions/vscode/ — VSCode Extension

Client-side glue: TextMate grammar for syntax highlighting, language configuration (brackets, comments, indent rules), and a TypeScript LSP client that spawns `lsp-wake` (in `../../tools/lsp-wake/`).

## Layout

```
syntaxes/wake.tmLanguage.json     TextMate grammar (syntax highlighting)
language-configuration.json       Comments / brackets / indent
src/extension.ts                  VSCode extension entry; spawns lsp-wake
package.json                      Manifest (commands, activation, deps)
vscode.wake                       Build rule (drives `make vscode`)
```

## Build & run

```bash
# From repo root:
make vscode                       # builds the .vsix package
```

To test the extension end-to-end, see [`../../share/doc/wake/how-to/test-the-vscode-extension.adoc`](../../share/doc/wake/how-to/test-the-vscode-extension.adoc).

## Conventions

- **Don't reimplement language features here.** Hover, completion, diagnostics, and go-to-definition come from `lsp-wake`. The extension is a thin client.
- **Update grammar and `lsp-wake` together.** When you add new syntax to Wake (parser change in `../../src/parser/`), update `syntaxes/wake.tmLanguage.json` so highlighting follows.
- **`language-configuration.json`** is JSON — comments require careful editing.
- **TypeScript style**: match what's already there (no new lint configuration without good reason).

## Releasing

The marketplace publish is a manual post-release step — see `../../share/doc/wake/how-to/create-a-release.adoc`. CI does not push to the marketplace automatically; it produces the `.vsix` artifact and a human uploads it.

## Other editors

Smaller, file-only support (syntax highlighting) for emacs, joe, and vim lives in [`../../share/doc/wake/syntax/`](../../share/doc/wake/syntax/). They don't talk to `lsp-wake`; if you want full IDE features in those editors, configure them as generic LSP clients pointing at the `lsp-wake` binary.
