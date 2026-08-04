#!/usr/bin/env bash
# Design-token lint for the macOS shell (docs/design-handoff-macos.md).
#
# The 2026-08-04 audit found the design system (DesignSystem.swift) faithful
# but barely adopted: 89 SF Pro call sites, 38 SF Mono, 13 brand-font — which
# is exactly why the Mac build "almost matches but feels wrong". Fonts and
# status colors are the design language; this gate keeps them from regressing.
#
# Rules (mac-shell/Sources, DesignSystem.swift exempt — it defines the tokens):
#   - No .system(size:) fonts        -> .grotesk() / .plexMono()
#   - No system text styles          -> explicit sizes from the spec
#   - No design: .monospaced (SF Mono) -> .plexMono()
#   - No raw system colors in foregroundStyle/foregroundColor/tint
#                                    -> Studio.* tokens only
#   - No Color.white.opacity(...) ad-hoc fills -> Studio.border/line2/surface
#
# A line that is genuinely fine (e.g. sizing an Image(systemName:) SF Symbol)
# may carry the marker  // design-lint: allow  — use it sparingly and only
# for icons, never for text.

set -u
shopt -s nullglob
fail=0

scan() {
  local pattern="$1" message="$2" file hits
  for file in mac-shell/Sources/*/*.swift; do
    [[ "$file" == */DesignSystem.swift ]] && continue
    hits=$(grep -nE "$pattern" "$file" | grep -v 'design-lint: allow' || true)
    if [[ -n "$hits" ]]; then
      fail=1
      echo "FAIL [$message] in $file:"
      echo "$hits" | sed 's/^/  /'
    fi
  done
}

scan '\.system\(size' \
  'SF Pro leak - use .grotesk()/.plexMono(); icons may carry // design-lint: allow'
scan '\.font\(\.(largeTitle|title2?3?|headline|subheadline|body|callout|caption2?|footnote)\b' \
  'system text style - use explicit .grotesk()/.plexMono() sizes from the handoff spec'
scan 'design:[[:space:]]*\.monospaced' \
  'SF Mono leak - use .plexMono()'
scan '(foregroundStyle|foregroundColor|tint)\(\.(white|black|yellow|red|blue|green|orange|gray|grey|secondary|primary)\b' \
  'raw system color - use Studio.* tokens (amber/red/accent/textPrimary/secondary/textDim)'
scan 'Color\.white\.opacity' \
  'ad-hoc white-alpha color - use Studio.border/Studio.line2 or a surface token'

if [[ $fail -ne 0 ]]; then
  echo ""
  echo "Design lint FAILED. Fix per docs/design-handoff-macos.md, then verify"
  echo "with side-by-side screenshots against docs/design-reference/ before"
  echo "claiming compliance (the verification protocol is mandatory)."
  exit 1
fi
echo "Design lint passed."
