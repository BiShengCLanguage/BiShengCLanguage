# book-ci — manual build & publish

This branch builds the **BiSheng C user manual** site and deploys it to GitHub Pages
via GitHub Actions. It is independent of the compiler branch (`bishengc/15.0.4`) and the
generated site branch (`gh-pages`).

## Contents

- `build.sh` — fetches the compiler repo's single-file manual
  (`clang/docs/BSC/BiShengCLanguageUserManual.md`) directly from gitcode via its v5
  REST API, splits it with `split_manual.py`, applies the overlay, and builds
  4 mdBook editions into `./site`:
  | Edition | Source | URL path |
  |---|---|---|
  | 中文 release | gitcode branch `bishengc/15.0.4` | `/` |
  | 中文 preview | gitcode branch `bishengc_manual_preview` | `/preview/` |
  | English release | `overlay/en/src` (updated manually) | `/en/` |
  | English preview | `overlay/en-preview/src` (updated manually) | `/en/preview/` |
- `overlay/` — GitHub-only additions kept out of the pristine gitcode repo:
  - `book.zh.toml` / `book.en.toml` — themed mdBook config (Rust theme, dark ayu, MathJax, search)
  - `custom.css`, `lang-switch.js` — visual polish + 中文/EN toggle
  - `en/`, `en-preview/` — the English translation
  - `TRANSLATION_GUIDE.md` — glossary/rules used for the translation
- `.github/workflows/publish-manual.yml` — daily schedule + manual trigger; builds and
  deploys to Pages.
- `.github/workflows/mirror-compiler.yml` — daily mirror of the gitcode compiler branch
  into this repo's `bishengc/15.0.4` with an overlay commit on top: the homepage README
  (`overlay/compiler-readme/`) plus copies of the two pipeline workflow files. Carrying
  the workflows onto the default branch is what makes their `schedule:` crons fire —
  GitHub only schedules workflows from the default branch.

## Source of truth

The Chinese manual lives in the compiler repo on gitcode
(`bisheng_c_language_dep/llvm-project`, file `clang/docs/BSC/BiShengCLanguageUserManual.md`);
`build.sh` reads it from gitcode directly. To update the Chinese text, commit to the
gitcode compiler repo (branch `bishengc/15.0.4` for release, `bishengc_manual_preview`
for preview). The English manual is updated MANUALLY: edit `overlay/en/src` /
`overlay/en-preview/src` here and push — the site republishes on push. (The old AI
translation pipeline is retired; `translate-sync.sh` is kept for reference only.)

## Run locally

```sh
cargo install mdbook        # or use a prebuilt binary
./build.sh                  # outputs ./site
```
