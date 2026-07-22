# book-ci — manual build & publish

This branch builds the **BiSheng C user manual** site and deploys it to GitHub Pages
via GitHub Actions. It is independent of the compiler branch (`bishengc/15.0.4`) and the
generated site branch (`gh-pages`).

## Contents

- `build.sh` — fetches the compiler repo's single-file manual
  (`clang/docs/BSC/BiShengCLanguageUserManual.md`) from THIS repo's mirror branches over
  the GitHub API, splits it with `split_manual.py`, applies the overlay, and builds
  4 mdBook editions into `./site`:
  | Edition | Source | URL path |
  |---|---|---|
  | 中文 release | branch `bishengc/15.0.4` (mirror of gitcode) | `/` |
  | 中文 preview | branch `manual-preview` (mirror of gitcode `bishengc_manual_preview`) | `/preview/` |
  | English release | `overlay/en/src` | `/en/` |
  | English preview | `overlay/en-preview/src` | `/en/preview/` |
- `overlay/` — GitHub-only additions kept out of the pristine gitcode repo:
  - `book.zh.toml` / `book.en.toml` — themed mdBook config (Rust theme, dark ayu, MathJax, search)
  - `custom.css`, `lang-switch.js` — visual polish + 中文/EN toggle
  - `en/`, `en-preview/` — the English translation
  - `TRANSLATION_GUIDE.md` — glossary/rules used for the translation
- `.github/workflows/publish-manual.yml` — daily schedule + manual trigger; builds and
  deploys to Pages.

## Source of truth

The Chinese manual lives in the compiler repo on gitcode
(`bisheng_c_language_dep/llvm-project`, file `clang/docs/BSC/BiShengCLanguageUserManual.md`),
mirrored to this repo's branches by the daily local cron (`sync-bishengc-github.sh`).
To update the Chinese text, commit to the gitcode compiler repo. To update the English
text or styling, edit `overlay/` here. (`translate-sync.sh` still diffs the legacy
gitcode `book.git` split-tree repo to drive AI translation of English pages.)

## Run locally

```sh
cargo install mdbook        # or use a prebuilt binary
./build.sh                  # outputs ./site
```
