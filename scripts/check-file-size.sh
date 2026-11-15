#!/bin/sh
# The file-size rule from CODING_STANDARDS.md, over the first-party product
# sources.
#
# Lines are counted by scripts/count-code-lines.awk, so blank lines and comments
# do not count and a code line with a trailing comment does. A source over 200
# non-comment lines fails; one over 150 warns and must be listed in
# scripts/soft-limit-exceptions.tsv. With no arguments the rule checks the
# tracked C/C++ product sources it belongs to (include/ and src/, which carries
# src/common/, src/lib/ and src/handler/) and cross-checks the ledger; with
# arguments it checks exactly those files, which is how tests/check_file_size.sh
# proves the threshold outcomes. SDL3MCP_SOFT_LIMIT_EXCEPTIONS overrides the
# ledger path for the fixture. The rule reads only; it never modifies the tree.
set -eu

soft_limit=150
hard_limit=200
counter=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/count-code-lines.awk
script_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ledger=${SDL3MCP_SOFT_LIMIT_EXCEPTIONS:-$script_root/scripts/soft-limit-exceptions.tsv}
# A caller-relative override is resolved now, before the tree scan moves to the
# repository root.
if [ "${ledger#/}" = "$ledger" ]; then
   ledger_dir=$(dirname -- "$ledger")
   ledger_base=$(basename -- "$ledger")
   resolved_dir=$(CDPATH= cd -- "$ledger_dir" 2>/dev/null && pwd) ||
      resolved_dir=
   [ -z "$resolved_dir" ] || ledger=$resolved_dir/$ledger_base
fi
failed=0

count_file() {
   awk -f "$counter" "$1"
}

require_file() {
   [ -f "$1" ] || {
      printf 'file-size: %s: no such file\n' "$1" >&2
      exit 1
   }
}

warn_soft() {
   printf 'file-size: warning: %s: %s non-comment lines exceeds the %s-line soft limit\n' \
      "$1" "$2" "$soft_limit" >&2
}

# ledger_count FILE prints the recorded counts for FILE, one per matching line.
ledger_count() {
   awk -F '\t' -v file="$1" '
      /^[[:space:]]*#/ { next }
      NF == 0 { next }
      $1 == file && $2 ~ /^[0-9]+$/ { print $2 }
   ' "$ledger"
}

# check_size FILE LINES applies the hard and soft thresholds.
check_size() {
   file=$1
   lines=$2
   if [ "$lines" -gt "$hard_limit" ]; then
      printf 'file-size: %s: %s non-comment lines exceeds the %s-line limit\n' \
         "$file" "$lines" "$hard_limit" >&2
      failed=1
   elif [ "$lines" -gt "$soft_limit" ]; then
      warn_soft "$file" "$lines"
   fi
}

# check_ledger FILE LINES requires one matching entry for a soft file.
check_ledger() {
   file=$1
   lines=$2
   [ "$lines" -gt "$soft_limit" ] || return 0
   counts=$(ledger_count "$file")
   matches=$(printf '%s\n' "$counts" | awk 'NF { n++ } END { print n + 0 }')
   case $matches in
   0)
      printf 'file-size: %s: %s non-comment lines exceeds the %s-line soft limit and has no entry in %s\n' \
         "$file" "$lines" "$soft_limit" "$ledger" >&2
      failed=1
      ;;
   1)
      recorded=$(printf '%s\n' "$counts" | awk 'NF { print; exit }')
      if [ "$recorded" -ne "$lines" ]; then
         printf 'file-size: %s: stale count in %s: recorded %s, counted %s\n' \
            "$file" "$ledger" "$recorded" "$lines" >&2
         failed=1
      fi
      ;;
   *)
      printf 'file-size: %s: duplicate entries in %s\n' \
         "$file" "$ledger" >&2
      failed=1
      ;;
   esac
}

validate_ledger() {
   list=$1
   entries=$2
   awk -F '\t' '
      /^[[:space:]]*#/ { next }
      NF == 0 { next }
      NF < 3 || $2 !~ /^[0-9]+$/ || $3 ~ /^[[:space:]]*$/ { print "!" $0; next }
      { print $1 }
   ' "$ledger" > "$entries"
   while IFS= read -r path; do
      case $path in
      '!'*)
         printf 'file-size: %s: malformed exception ledger line: %s\n' \
            "$ledger" "${path#!}" >&2
         failed=1
         continue
         ;;
      esac
      if [ ! -e "$path" ]; then
         printf 'file-size: %s: %s no longer exists; remove its entry\n' \
            "$ledger" "$path" >&2
         failed=1
         continue
      fi
      if ! grep -Fqx "$path" "$list"; then
         printf 'file-size: %s: %s is not a tracked product source\n' \
            "$ledger" "$path" >&2
         failed=1
         continue
      fi
      lines=$(count_file "$path")
      if [ "$lines" -le "$soft_limit" ]; then
         printf 'file-size: %s: %s is at or below the %s-line soft limit; remove its entry\n' \
            "$ledger" "$path" "$soft_limit" >&2
         failed=1
      fi
   done < "$entries"
}

if [ "$#" -gt 0 ]; then
   for file in "$@"; do
      require_file "$file"
      lines=$(count_file "$file")
      check_size "$file" "$lines"
   done
else
   repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
      printf 'file-size: not a git checkout; cannot list the product sources\n' >&2
      exit 1
   }
   cd "$repo_root"
   [ -f "$ledger" ] || {
      printf 'file-size: missing exception ledger: %s\n' "$ledger" >&2
      exit 1
   }
   tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/sdl3mcp-file-size.XXXXXX")
   trap 'rm -rf "$tmpdir"' EXIT
   list=$tmpdir/list
   entries=$tmpdir/entries
   git ls-files -- 'include/*.c' 'include/*.h' 'include/*.cpp' 'include/*.hpp' \
      'src/*.c' 'src/*.h' 'src/*.cpp' 'src/*.hpp' > "$list"
   while IFS= read -r file; do
      lines=$(count_file "$file")
      check_size "$file" "$lines"
      check_ledger "$file" "$lines"
   done < "$list"
   validate_ledger "$list" "$entries"
fi

[ "$failed" -eq 0 ] || exit 1
