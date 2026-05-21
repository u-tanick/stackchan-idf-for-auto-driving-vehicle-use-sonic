#!/usr/bin/env bash
# Extract the <script>…</script> blocks from each HTML file under tools/,
# docs/, and the device-embedded component web pages (components/*/web/*.html,
# e.g. the Wi-Fi settings page), then run `node --check` over them. Catches
# typos, unbalanced braces, and dangling identifiers before they ship to the
# GitHub Pages site or get flashed into the firmware.
#
# Usage: ./script/check_html_js.sh [extra-html-file ...]
#
# Exit code 0 on success, non-zero on the first failure. Honors the
# `<script type="module">` attribute so ESM imports parse correctly.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

if ! command -v node >/dev/null 2>&1; then
    echo "node not in PATH — install Node.js (any modern version) for syntax checking." >&2
    exit 127
fi

targets=()
if [ "$#" -gt 0 ]; then
    targets=("$@")
else
    while IFS= read -r -d '' f; do
        targets+=("$f")
    done < <(
        find "$root/tools" "$root/docs" -maxdepth 2 -name '*.html' -print0 2>/dev/null
        # Component web pages embedded into the firmware (served over HTTP).
        find "$root/components" -path '*/web/*.html' -print0 2>/dev/null
    )
fi

if [ "${#targets[@]}" -eq 0 ]; then
    echo "No HTML files to check."
    exit 0
fi

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

# Split the input HTML into per-block files in $tmp_dir/$basename.NN.{js,mjs}.
# A script tag with `type="module"` lands in .mjs so `node --check` parses it
# as ESM. Everything else is .js. Echoes the list of generated files.
split_blocks() {
    local html="$1"
    local base
    base="$(basename "$html")"
    awk -v base="$base" -v tmp_dir="$tmp_dir" '
        BEGIN { inside = 0; idx = 0; current = ""; mod = 0 }
        /<script[^>]*>/ {
            inside = 1
            if (match($0, /type\s*=\s*"module"/) || match($0, /type\s*=\s*\047module\047/)) {
                mod = 1
            } else {
                mod = 0
            }
            idx += 1
            ext = mod ? "mjs" : "js"
            current = sprintf("%s/%s.%02d.%s", tmp_dir, base, idx, ext)
            printf "" > current
            next
        }
        /<\/script>/ {
            inside = 0
            if (current != "") print current
            current = ""
            next
        }
        inside && current != "" { print > current }
    ' "$html"
}

failures=0
for html in "${targets[@]}"; do
    block_count=0
    block_files=()
    while IFS= read -r blockfile; do
        [ -n "$blockfile" ] && block_files+=("$blockfile")
    done < <(split_blocks "$html")
    block_count="${#block_files[@]}"

    ok=1
    for bf in "${block_files[@]}"; do
        if ! node --check "$bf" 2>/tmp/check_html_js.err; then
            idx=$(basename "$bf" | awk -F. '{print $(NF-1)}')
            echo "FAIL $html (block $idx, $(basename "$bf" | awk -F. '{print $NF}')):"
            sed 's/^/    /' /tmp/check_html_js.err
            ok=0
        fi
    done

    if [ "$ok" -eq 1 ]; then
        echo "OK   $html ($block_count block$([ "$block_count" -ne 1 ] && echo s || echo ''))"
    else
        failures=$((failures + 1))
    fi
done

if [ "$failures" -gt 0 ]; then
    echo
    echo "$failures file(s) failed."
    exit 1
fi
