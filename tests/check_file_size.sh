#!/bin/sh
# Fixture for the file-size rule in scripts/check-file-size.sh.
#
# The rule has three outcomes, and this proves all of them against files made in
# a throwaway directory: over 300 lines fails, 150 to 300 warns without failing,
# and a short file passes silently. scripts/check.sh invokes this fixture, so the
# rule and its proof cannot drift apart.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rule=$root/scripts/check-file-size.sh
work=$(mktemp -d "${TMPDIR:-/tmp}/sdl3mcp-file-size.XXXXXX")
trap 'rm -rf "$work"' EXIT

fail() {
   printf 'file-size fixture: %s\n' "$1" >&2
   exit 1
}

# fill_file FILE COUNT writes COUNT lines, so wc -l sees exactly COUNT.
fill_file() {
   file=$1
   count=$2
   : > "$file"
   i=0
   while [ "$i" -lt "$count" ]; do
      printf 'int value_%s;\n' "$i" >> "$file"
      i=$((i + 1))
   done
}

[ -f "$rule" ] || fail "missing rule script: $rule"

short=$work/short.cpp
soft=$work/soft.cpp
hard=$work/hard.cpp
fill_file "$short" 100
fill_file "$soft" 200
fill_file "$hard" 301

# A short file passes without a word.
if ! out=$(sh "$rule" "$short" 2>&1); then
   fail "a 100-line file failed: $out"
fi
case $out in
*warning*) fail "a 100-line file warned: $out" ;;
esac

# A file over 150 warns with the soft limit and still passes.
if ! out=$(sh "$rule" "$soft" 2>&1); then
   fail "a 200-line file failed: $out"
fi
case $out in
*warning:*"150-line soft limit"*) ;;
*) fail "a 200-line file did not warn with the soft limit: $out" ;;
esac

# A file over 300 fails, and the message names the file and the limit.
if out=$(sh "$rule" "$hard" 2>&1); then
   fail "a 301-line file passed: $out"
fi
case $out in
*"$hard"*) ;;
*) fail "the failure does not name the file: $out" ;;
esac
case $out in
*"300-line limit"*) ;;
*) fail "the failure does not name the 300-line limit: $out" ;;
esac

printf 'file-size fixture: ok\n'
