#!/usr/bin/env bash
#
# Build the BiSheng C user manual site (4 editions) into ./site.
#
# Content source = the compiler repo's single-file manual, mirrored on THIS GitHub
# repo's compiler branches. We fetch the manual over the GitHub API (fast, reliable,
# no gitcode dependency, no giant clone) and split it into the mdBook chapter tree with
# split_manual.py. The site therefore always tracks the published compiler manual.
#
#   zh release  (manual on bishengc/15.0.4)         -> site/
#   zh preview  (manual on manual-preview branch) -> site/preview/
#   en release  (overlay/en/src)                    -> site/en/
#   en preview  (overlay/en-preview/src)            -> site/en/preview/
#
# Layout assumed: this script + overlay/ + split_manual.py live together on `book-ci`.
# Needs: mdbook, python3 (>=3.10), and either `gh` (with GITHUB_TOKEN) or curl to read
# the manual from this repo. Runs in GitHub Actions and locally.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OVERLAY="$HERE/overlay"
REPO="${GITHUB_REPOSITORY:-BiShengCLanguage/BiShgengCLanguage}"
REPO_WEB="https://github.com/$REPO"
MANUAL_PATH="clang/docs/BSC/BiShengCLanguageUserManual.md"
SITE="$HERE/site"
WORKROOT=$(mktemp -d)
TODAY=$(date +%Y/%m/%d)

# Which compiler branch feeds each Chinese edition.
ZH_MAIN_REF="bishengc/15.0.4"
ZH_PREV_REF="manual-preview"

rm -rf "$SITE"; mkdir -p "$SITE"

# Fetch a file from this repo at $1=ref $2=path -> stdout. Prefer gh; fall back to API URL.
fetch_file() {
  local ref="$1" path="$2"
  if command -v gh >/dev/null && [ -n "${GH_TOKEN:-${GITHUB_TOKEN:-}}" ]; then
    GH_TOKEN="${GH_TOKEN:-$GITHUB_TOKEN}" gh api \
      "repos/$REPO/contents/$path?ref=$ref" -H "Accept: application/vnd.github.raw"
  else
    curl -fsSL -H "Accept: application/vnd.github.raw" \
      ${GITHUB_TOKEN:+-H "Authorization: Bearer $GITHUB_TOKEN"} \
      "https://api.github.com/repos/$REPO/contents/$path?ref=$ref"
  fi
}

# Produce a split src/ tree for a compiler ref. $1=ref -> echoes the src dir path.
make_zh_src() {
  local ref="$1" d; d="$WORKROOT/manual-$(echo "$ref" | tr '/' '_')"
  mkdir -p "$d"
  fetch_file "$ref" "$MANUAL_PATH" > "$d/manual.md"
  python3 "$HERE/split_manual.py" "$d/manual.md" "$d/src" >/dev/null
  # record the source commit + date for the version stamp
  fetch_commit_meta "$ref" > "$d/.meta"
  echo "$d"
}

# Echo "shortsha<TAB>YYYY/MM/DD" for the tip of $1 (latest commit touching the manual).
fetch_commit_meta() {
  local ref="$1" json
  if command -v gh >/dev/null && [ -n "${GH_TOKEN:-${GITHUB_TOKEN:-}}" ]; then
    json=$(GH_TOKEN="${GH_TOKEN:-$GITHUB_TOKEN}" gh api \
      "repos/$REPO/commits?sha=$ref&path=$MANUAL_PATH&per_page=1" 2>/dev/null)
  else
    json=$(curl -fsSL ${GITHUB_TOKEN:+-H "Authorization: Bearer $GITHUB_TOKEN"} \
      "https://api.github.com/repos/$REPO/commits?sha=$ref&path=$MANUAL_PATH&per_page=1" 2>/dev/null)
  fi
  local sha date
  sha=$(printf '%s' "$json" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d[0]["sha"][:7] if d else "")' 2>/dev/null)
  date=$(printf '%s' "$json" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d[0]["commit"]["committer"]["date"][:10].replace("-","/") if d else "")' 2>/dev/null)
  printf '%s\t%s\n' "${sha:-unknown}" "${date:-$TODAY}"
}

apply_overlay() {  # $1 work dir, $2 toml name
  cp "$OVERLAY/$2" "$1/book.toml"
  mkdir -p "$1/theme"
  cp "$OVERLAY/custom.css"     "$1/theme/custom.css"
  cp "$OVERLAY/lang-switch.js" "$1/theme/lang-switch.js"
}

attach_version_info() {  # $1 work, $2 ref, $3 lang, $4 meta-file
  local work="$1" ref="$2" lang="$3" meta="$4" sha date readme
  sha=$(cut -f1 "$meta"); date=$(cut -f2 "$meta")
  readme="$work/src/README.md"
  {
    echo ""
    if [ "$lang" = en ]; then
      echo "> Version: [$ref]($REPO_WEB/tree/$ref/) - [$sha]($REPO_WEB/commit/$sha)"
      echo ">"; echo "> Updated: $date"; echo ">"; echo "> Released: $TODAY"
    else
      echo "> 版本说明：[$ref]($REPO_WEB/tree/$ref/) - [$sha]($REPO_WEB/commit/$sha)"
      echo ">"; echo "> 更新日期：$date"; echo ">"; echo "> 发布日期：$TODAY"
    fi
  } >> "$readme"
}

attach_preview_link() {  # $1 work, $2 url, $3 lang, $4 dir(toprev|torel)
  local work="$1" url="$2" lang="$3" dir="$4" readme line body
  readme="$work/src/README.md"; body=$(cat "$readme")
  if [ "$lang" = en ]; then
    [ "$dir" = toprev ] \
      && line="> 📖 You are reading the **release** edition. A [**preview edition**]($url) documents upcoming, not-yet-released features." \
      || line="> 📖 You are reading the **preview** edition (upcoming, unreleased features). Go to the [**release edition**]($url)."
  else
    [ "$dir" = toprev ] \
      && line="> 📖 当前为**正式版**。[**预览版**]($url)包含即将发布、尚未正式发布的特性。" \
      || line="> 📖 当前为**预览版**（即将发布、尚未正式发布的特性）。前往[**正式版**]($url)。"
  fi
  printf '%s\n\n%s\n' "$line" "$body" > "$readme"
}

# Pre-split the two Chinese editions from the compiler manual.
ZH_MAIN=$(make_zh_src "$ZH_MAIN_REF")
ZH_PREV=$(make_zh_src "$ZH_PREV_REF")

# Annotate the PREVIEW pages with an inline red/green diff vs the release edition and
# a "what differs" summary on the preview intro page. The annotator rewrites files in
# place, so we always run it on a THROWAWAY copy — never on the committed overlay.
#
# zh: $ZH_PREV/src is already a fresh temp split dir → safe to annotate in place.
echo "[INFO] annotating preview zh pages with release diff"
python3 "$HERE/annotate_preview_diff.py" "$ZH_MAIN/src" "$ZH_PREV/src" zh || \
  echo "[WARN] preview zh diff annotation failed — building preview without diffs"
# en: copy the committed en-preview overlay to a temp dir, annotate THAT.
EN_PREV_SRC="$WORKROOT/en-preview-src"
cp -r "$OVERLAY/en-preview/src" "$EN_PREV_SRC"
echo "[INFO] annotating preview en pages with release diff"
python3 "$HERE/annotate_preview_diff.py" "$OVERLAY/en/src" "$EN_PREV_SRC" en || \
  echo "[WARN] preview en diff annotation failed — building preview without diffs"

build_edition() {  # $1 src dir, $2 toml, $3 lang, $4 dest sub, $5 ref, $6 meta-file
  local src="$1" toml="$2" lang="$3" sub="$4" ref="$5" meta="$6" work
  work=$(mktemp -d)
  cp -r "$OVERLAY/assets" "$work/assets"        # search JS (assets/js/*)
  cp -r "$src" "$work/src"
  apply_overlay "$work" "$toml"
  attach_version_info "$work" "$ref" "$lang" "$meta"
  case "$sub" in
    ""|"en")                attach_preview_link "$work" "./preview/" "$lang" toprev ;;
    "preview"|"en/preview") attach_preview_link "$work" "../"        "$lang" torel ;;
  esac
  echo "[INFO] building $lang -> ${sub:-<root>}"
  mdbook build "$work" --dest-dir "$SITE${sub:+/$sub}"
  rm -rf "$work"
}

build_edition "$ZH_MAIN/src"            book.zh.toml zh ""           "$ZH_MAIN_REF" "$ZH_MAIN/.meta"
build_edition "$ZH_PREV/src"            book.zh.toml zh "preview"    "$ZH_PREV_REF" "$ZH_PREV/.meta"
build_edition "$OVERLAY/en/src"         book.en.toml en "en"         "$ZH_MAIN_REF" "$ZH_MAIN/.meta"
build_edition "$EN_PREV_SRC"            book.en.toml en "en/preview" "$ZH_PREV_REF" "$ZH_PREV/.meta"

touch "$SITE/.nojekyll"
rm -rf "$WORKROOT"
echo "[INFO] site built at $SITE"
