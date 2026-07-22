#!/usr/bin/env bash
#
# Keep the English manual in sync with the Chinese source using AI translation,
# driven by the GLM gateway (Claude Code headless against an Anthropic-compatible
# endpoint). Diff-driven: only re-translates the Chinese pages that actually changed.
#
# For each edition (release / preview):
#   1. read the baseline SHA the English was last translated from (overlay/<ed>/.translated-from)
#   2. clone the current gitcode Chinese branch
#   3. git diff baseline..current -- src/  -> changed/added/deleted .md files
#   4. translate each changed file zh->en via the GLM gateway (key rotation + retry on
#      transient gateway errors), following overlay/TRANSLATION_GUIDE.md
#   5. advance the baseline marker to the current SHA
#
# Outputs (for the workflow): writes "changed=N" to $GITHUB_OUTPUT if set.
# Requires: git, and the `claude` CLI. Keys come from $GLM_ENV (the llmperfeval .env)
# or from GLM_BUNDLE_API_KEY / GLM_BUNDLE_API_KEY_2 / GLM_API_KEY in the environment
# (so CI can supply them as secrets). If no key is usable, it's a safe no-op.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OVERLAY="$HERE/overlay"
GUIDE="$OVERLAY/TRANSLATION_GUIDE.md"
BOOK_REPO="https://gitcode.com/bisheng_c_language_dep/book.git"
GATEWAY="${GLM_GATEWAY:-http://113.46.219.251:8080}"
MODEL="${GLM_MODEL:-GLM-5}"
GLM_ENV="${GLM_ENV:-/home/ziruichen/bsd/llmperfeval/.env}"

# edition -> gitcode branch
declare -A BRANCH=(
  [en]="bishengc/15.0.4"
  [en-preview]="bishengc/15.0.4-preview"
)

# Collect candidate keys: explicit env first, then the .env file (bundle1, bundle2, original).
rd(){ [ -f "$GLM_ENV" ] && grep -E "^$1=" "$GLM_ENV" | head -1 | cut -d= -f2- | tr -d "\"' " || true; }
KEYS=()
for v in "${GLM_BUNDLE_API_KEY:-}" "${GLM_BUNDLE_API_KEY_2:-}" "${GLM_API_KEY:-}"; do
  [ -n "$v" ] && KEYS+=("$v")
done
if [ "${#KEYS[@]}" -eq 0 ]; then
  for f in GLM_BUNDLE_API_KEY GLM_BUNDLE_API_KEY_2 GLM_API_KEY; do
    v="$(rd "$f")"; [ -n "$v" ] && KEYS+=("$v")
  done
fi

if [ "${#KEYS[@]}" -eq 0 ] || ! command -v claude >/dev/null; then
  echo "[skip] no GLM key or no claude CLI — leaving English unchanged."
  [ -n "${GITHUB_OUTPUT:-}" ] && echo "changed=0" >> "$GITHUB_OUTPUT"
  exit 0
fi

GUIDE_TEXT=$(cat "$GUIDE")
TOTAL_CHANGED=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

ROTATE_RE='rate.?limit|quota|exceeded|reached your|usage limit|too many requests|insufficient|429|invalid proxy server token|authentication error|unauthor|401'
TRANSIENT_RE='50[234]|server-side|no body|overloaded|temporarily|bad gateway|timed out|timeout|connection reset|EOF occurred|read timed'
MAX_TRY=4

# translate one Chinese markdown file -> English on stdout. Rotates keys on quota/auth,
# retries the same key on transient gateway errors. Returns 1 if all keys are exhausted.
translate_file() {
  local zh_path="$1" out="$2"
  local prompt; prompt="You are translating one page of the BiSheng C language user manual from Chinese to English. Follow this translation guide EXACTLY:

$GUIDE_TEXT

Output ONLY the translated Markdown for this single file — no preamble, no surrounding code fence, no commentary. Preserve all Markdown structure, heading levels and their section numbers, tables, links, and code blocks; translate only prose, headings, table text, and code comments. Keep all BiSheng C keywords, identifiers, and file names verbatim.

--- BEGIN PAGE ---
$(cat "$zh_path")
--- END PAGE ---"
  local key attempt log rc
  for key in "${KEYS[@]}"; do
    attempt=0
    while :; do
      attempt=$((attempt+1))
      log="$TMP/run.log"
      set +e
      ANTHROPIC_BASE_URL="$GATEWAY" ANTHROPIC_AUTH_TOKEN="$key" \
        claude -p "$prompt" --model "$MODEL" --dangerously-skip-permissions </dev/null >"$out" 2>"$log"
      rc=$?
      set -e 2>/dev/null || true
      if [ "$rc" -eq 0 ] && [ -s "$out" ]; then return 0; fi
      if grep -qiE "$ROTATE_RE" "$out" "$log" 2>/dev/null; then
        echo "    (key exhausted/limited — rotating)" >&2; break
      fi
      if grep -qiE "$TRANSIENT_RE" "$out" "$log" 2>/dev/null && [ "$attempt" -lt "$MAX_TRY" ]; then
        echo "    (transient gateway error — retry $attempt/$MAX_TRY)" >&2; sleep $((attempt*5)); continue
      fi
      break   # non-transient failure on this key — try the next key
    done
  done
  return 1
}

for ED in en en-preview; do
  BR="${BRANCH[$ED]}"
  BASE_FILE="$OVERLAY/$ED/.translated-from"
  BASE=$(tr -d '[:space:]' < "$BASE_FILE" 2>/dev/null || true)
  SRC="$TMP/$ED-src"
  git clone --quiet --depth 50 --branch "$BR" --single-branch "$BOOK_REPO" "$SRC"
  CUR=$(git -C "$SRC" rev-parse HEAD)

  if [ "$BASE" = "$CUR" ]; then
    echo "[$ED] up to date ($CUR) — nothing to translate."
    continue
  fi
  echo "[$ED] baseline $BASE -> current $CUR"

  if [ -n "$BASE" ] && git -C "$SRC" cat-file -e "$BASE" 2>/dev/null; then
    mapfile -t DIFF < <(git -C "$SRC" diff --name-status "$BASE" "$CUR" -- 'src/*.md')
  else
    echo "[$ED] baseline not in history; retranslating all pages."
    mapfile -t DIFF < <(cd "$SRC" && find src -name '*.md' | sed 's/^/A\t/')
  fi

  ed_failed=0
  for entry in "${DIFF[@]}"; do
    [ -z "$entry" ] && continue
    status=$(printf '%s' "$entry" | cut -f1)
    path=$(printf '%s' "$entry" | cut -f2)
    dest="$OVERLAY/$ED/$path"
    case "$status" in
      D*)
        rm -f "$dest"; echo "  [del] $path"; TOTAL_CHANGED=$((TOTAL_CHANGED+1)) ;;
      A*|M*|R*)
        echo "  [tr ] $path"
        mkdir -p "$(dirname "$dest")"
        if translate_file "$SRC/$path" "$dest.new"; then
          # sanity: balanced code fences (catch truncation/garbled output)
          if [ $(( $(grep -c '^```' "$dest.new") % 2 )) -ne 0 ]; then
            echo "  [FAIL] $path (unbalanced code fences — kept old)" >&2
            rm -f "$dest.new"; ed_failed=1
          else
            mv "$dest.new" "$dest"; TOTAL_CHANGED=$((TOTAL_CHANGED+1))
          fi
        else
          rm -f "$dest.new"; echo "  [FAIL] $path (all keys exhausted — kept old)" >&2
          ed_failed=1
        fi ;;
    esac
  done

  # Only advance the baseline if every changed page translated cleanly, so a failed
  # page is retried next run instead of being silently skipped forever.
  if [ "$ed_failed" -eq 0 ]; then
    echo "$CUR" > "$BASE_FILE"
  else
    echo "[$ED] some pages failed — baseline NOT advanced (will retry next run)" >&2
  fi
done

echo "[done] files changed: $TOTAL_CHANGED"
[ -n "${GITHUB_OUTPUT:-}" ] && echo "changed=$TOTAL_CHANGED" >> "$GITHUB_OUTPUT"
exit 0
