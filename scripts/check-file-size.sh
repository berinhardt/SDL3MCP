#!/bin/sh
# The file-size rule from STYLE.md, over the first-party product sources.
#
# A source over 300 lines fails; one over 150 warns. With no arguments the rule
# checks the product tree it belongs to (include/ and src/, which carries
# src/handler/); with arguments it checks exactly those files, which is how
# tests/check_file_size.sh proves each outcome. The rule reads only; it never
# modifies the tree.
set -eu

soft_limit=150
hard_limit=300

hard_failed=0

check_file() {
   file=$1
   [ -f "$file" ] || {
      printf 'file-size: %s: no such file\n' "$file" >&2
      exit 1
   }
   lines=$(wc -l < "$file")
   if [ "$lines" -gt "$hard_limit" ]; then
      printf 'file-size: %s: %s lines exceeds the %s-line limit\n' \
         "$file" "$lines" "$hard_limit" >&2
      hard_failed=1
   elif [ "$lines" -gt "$soft_limit" ]; then
      printf 'file-size: warning: %s: %s lines exceeds the %s-line soft limit\n' \
         "$file" "$lines" "$soft_limit" >&2
   fi
}

if [ "$#" -gt 0 ]; then
   for file in "$@"; do
      check_file "$file"
   done
else
   root=$(git rev-parse --show-toplevel 2>/dev/null) || {
      printf 'file-size: not a git checkout; cannot list the product sources\n' >&2
      exit 1
   }
   cd "$root"
   list=$(mktemp "${TMPDIR:-/tmp}/sdl3mcp-file-size.XXXXXX")
   trap 'rm -f "$list"' EXIT
   git ls-files -- 'include/*' 'src/*' > "$list"
   while IFS= read -r file; do
      check_file "$file"
   done < "$list"
fi

[ "$hard_failed" -eq 0 ] || exit 1
