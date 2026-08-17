# bishengc_release — CI branch

This is the **default branch** of the `BiShengCLanguage/BiShengCLanguage` GitHub
repo. It carries all CI workflows + helper scripts in one place — manual
publishing, gitcode sync, and compiler release building. Schedules (`schedule:`
crons) fire natively from here; no overlay needs to be force-pushed onto the
compiler branch.

This branch is independent of the compiler mirror (`bishengc/15.0.4`) and the
generated site branch (`gh-pages`).

## Workflows (`.github/workflows/`)

| Workflow | Trigger | What it does |
|---|---|---|
| `mirror-compiler.yml` | daily 03:00 Beijing + push to self + manual | Force-pushes `bishengc/15.0.4` to match the gitcode tip **verbatim** — no overlay, no GitHub-side commits. The branch stays a pristine copy of gitcode's history. |
| `publish-manual.yml` | daily 04:00 Beijing + push to self + manual | Builds the 4 manual editions (zh/en × release/preview) and deploys to GitHub Pages. Chinese content is fetched directly from gitcode; English is the overlay on this branch. |
| `build-compiler-release.yml` | manual only | Builds the BiSheng C toolchain (clang + libcbs, X86, Release) on a GitHub-hosted runner and publishes a GitHub Release. Default `ref` input is `bishengc/15.0.4` (freshly synced); accepts any branch/tag/SHA. |

## Helper scripts

- `release-build.sh` — invoked by `build-compiler-release.yml`; pins gcc-13
  (LLVM 15.0.4 relies on transitive `<cstdint>` includes dropped by GCC 14+),
  uses lld + ccache, builds clang+libcbs in-tree, trims the install tree to
  only the binaries downstream consumers need.
- `build.sh` — fetches the compiler repo's single-file manual
  (`clang/docs/BSC/BiShengCLanguageUserManual.md`) directly from gitcode via
  its v5 REST API, splits it with `split_manual.py`, applies the overlay, and
  builds 4 mdBook editions into `./site`:
  | Edition | Source | URL path |
  |---|---|---|
  | 中文 release | gitcode branch `bishengc/15.0.4` | `/` |
  | 中文 preview | gitcode branch `bishengc_manual_preview` | `/preview/` |
  | English release | `overlay/en/src` (updated manually) | `/en/` |
  | English preview | `overlay/en-preview/src` (updated manually) | `/en/preview/` |
- `split_manual.py` — splits the single-file manual into a mdBook chapter
  tree.
- `inject_subtoc.py` — adds per-page subsection entries to the mdBook sidebar.
- `annotate_preview_diff.py` — inlines the preview-vs-release diff in the
  preview edition.
- `translate-sync.sh` — retired (manual English updates); kept for reference.

## Overlay (`overlay/`)

GitHub-only additions kept out of the pristine gitcode repo:

- `compiler-readme/` — project README that used to be overlaid onto
  `bishengc/15.0.4` for the repo homepage. Now the homepage reads
  `bishengc_release/README.md` directly, so this is kept for archival only.
- `book.zh.toml` / `book.en.toml` — themed mdBook config (Rust theme, dark
  ayu, MathJax, search).
- `custom.css`, `lang-switch.js` — visual polish + 中文/EN toggle.
- `en/`, `en-preview/` — the English manual (updated manually).
- `TRANSLATION_GUIDE.md` — glossary/rules for the translation.

## Source of truth

The Chinese manual lives in the compiler repo on gitcode
(`bisheng_c_language_dep/llvm-project`, file
`clang/docs/BSC/BiShengCLanguageUserManual.md`); `build.sh` reads it from
gitcode directly. To update the Chinese text, commit to the gitcode compiler
repo (branch `bishengc/15.0.4` for release, `bishengc_manual_preview` for
preview) — the daily `publish-manual.yml` schedule picks it up.

The English manual is updated MANUALLY: edit `overlay/en/src` /
`overlay/en-preview/src` here and push — the site republishes on push. (The
old AI translation pipeline is retired; `translate-sync.sh` is kept for
reference only.)

## Building a release

To publish a new BiSheng C toolchain release:

1. Wait for the daily mirror to land the latest gitcode tip on
   `bishengc/15.0.4` (or trigger `mirror-compiler.yml` manually first).
2. Go to **Actions → build-compiler-release → Run workflow** (or call
   `gh workflow run build-compiler-release.yml --ref bishengc_release \
   -f ref=bishengc/15.0.4`).
3. The run builds clang + libcbs (≈1h on a cold ccache, 5–15min on a warm
   one) and uploads the tarball as a GitHub Release:
   - **branch ref** → rolling prerelease `bsc-nightly`, asset
     `bishengc-<llvm_version>-linux-x64.tar.gz`
   - **tag ref** → release tagged with the tag name itself.

The ccache (~/.ccache) is persisted across runs via `actions/cache`, keyed
on the source SHA — same SHA hits cache fully, newer SHA reuses the bulk.

## Run locally

```sh
# manual site
cargo install mdbook        # or use a prebuilt binary
./build.sh                  # outputs ./site

# release toolchain (requires gcc-13, lld, ccache, ninja, cmake)
bash release-build.sh <src_dir> <install_dir>
```
