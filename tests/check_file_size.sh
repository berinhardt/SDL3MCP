#!/bin/sh
# Fixture for the file-size rule in scripts/check-file-size.sh.
#
# Eight cases pin the counter and the rule: blank lines and comments are free,
# a code line with a trailing comment counts, strings, char literals and raw
# strings count, comments cannot hide a hard file or manufacture a warning, and
# both thresholds carry their exact message. scripts/check.sh invokes this
# fixture before the rule, so the rule and its proof cannot drift apart.
#
# The limits are the product rule: 150 warns, 200 fails. Cases 2, 3 and 4 pin
# both thresholds exactly.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rule=$root/scripts/check-file-size.sh
counter=$root/scripts/count-code-lines.awk
work=$(mktemp -d "${TMPDIR:-/tmp}/sdl3mcp-file-size.XXXXXX")
trap 'rm -rf "$work"' EXIT

fail() {
   printf 'file-size fixture: %s\n' "$1" >&2
   exit 1
}

# fill_file FILE COUNT writes COUNT lines that each count as code.
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

# line_comments FILE COUNT appends COUNT comment-only lines.
line_comments() {
   file=$1
   count=$2
   i=0
   while [ "$i" -lt "$count" ]; do
      printf '// comment %s\n' "$i" >> "$file"
      i=$((i + 1))
   done
}

# block_comment FILE COUNT appends a COUNT-line block comment, star lines and
# all, so only a lexer that understands the block can discount it.
block_comment() {
   file=$1
   count=$2
   printf '/*\n' >> "$file"
   i=0
   while [ "$i" -lt "$((count - 2))" ]; do
      printf ' * block line %s\n' "$i" >> "$file"
      i=$((i + 1))
   done
   printf ' */\n' >> "$file"
}

# run_rule FILE sets status and out for one rule invocation.
run_rule() {
   if out=$(sh "$rule" "$1" 2>&1); then
      status=0
   else
      status=$?
   fi
}

expect_count() {
   label=$1
   file=$2
   want=$3
   got=$(awk -f "$counter" "$file")
   [ "$got" = "$want" ] || fail "$label: counted $got lines, want $want"
}

expect_pass() {
   label=$1
   file=$2
   run_rule "$file"
   [ "$status" -eq 0 ] || fail "$label: exit $status: $out"
   [ -z "$out" ] || fail "$label: unexpected output: $out"
}

expect_warn() {
   label=$1
   file=$2
   lines=$3
   run_rule "$file"
   [ "$status" -eq 0 ] || fail "$label: exit $status: $out"
   want="file-size: warning: $file: $lines non-comment lines exceeds the 150-line soft limit"
   [ "$out" = "$want" ] || fail "$label: want '$want', got '$out'"
}

expect_fail() {
   label=$1
   file=$2
   lines=$3
   run_rule "$file"
   [ "$status" -ne 0 ] || fail "$label: passed: $out"
   want="file-size: $file: $lines non-comment lines exceeds the 200-line limit"
   [ "$out" = "$want" ] || fail "$label: want '$want', got '$out'"
}

[ -f "$rule" ] || fail "missing rule script: $rule"
[ -f "$counter" ] || fail "missing counter script: $counter"

# Case 1: 100 code lines + 200 line comments + blanks pass silently, and a file
# of comments alone counts zero.
comments=$work/comments.cpp
fill_file "$comments" 100
line_comments "$comments" 200
printf '\n\n   \n\t\n' >> "$comments"
expect_count "case 1 count" "$comments" 100
expect_pass "case 1 comments and blanks are free" "$comments"

only_comments=$work/only-comments.cpp
line_comments "$only_comments" 200
block_comment "$only_comments" 20
printf '\n\n' >> "$only_comments"
expect_count "case 1 comment-only count" "$only_comments" 0
expect_pass "case 1 comment-only file" "$only_comments"

# Case 2: the soft threshold is exact: 150 passes, 160 warns with 160.
soft_at=$work/soft-at.cpp
fill_file "$soft_at" 150
expect_pass "case 2 soft limit is not exceeded at 150" "$soft_at"

soft_over=$work/soft-over.cpp
fill_file "$soft_over" 160
expect_count "case 2 soft count" "$soft_over" 160
expect_warn "case 2 soft limit warns" "$soft_over" 160

# Case 3: the hard threshold is exact: 200 passes with a warning, 201 fails.
hard_at=$work/hard-at.cpp
fill_file "$hard_at" 200
expect_warn "case 3 hard limit is not exceeded at 200" "$hard_at" 200

hard_over=$work/hard-over.cpp
fill_file "$hard_over" 201
expect_count "case 3 hard count" "$hard_over" 201
expect_fail "case 3 hard limit fails" "$hard_over" 201

# Case 4: 201 code lines behind 300 comment and blank lines still fail, and the
# message reports the code lines only.
hidden=$work/hidden.cpp
fill_file "$hidden" 201
line_comments "$hidden" 200
block_comment "$hidden" 50
printf '\n\n' >> "$hidden"
expect_count "case 4 hidden count" "$hidden" 201
expect_fail "case 4 comments cannot hide a hard file" "$hidden" 201

# Case 5: 10 code lines plus a 400-line block comment pass silently; comments
# cannot manufacture a warning.
buried=$work/buried.cpp
fill_file "$buried" 10
block_comment "$buried" 400
expect_count "case 5 block-comment count" "$buried" 10
expect_pass "case 5 comments cannot manufacture a warning" "$buried"

# Case 6: 160 code lines each with a trailing comment warn, because trailing
# comments count as code.
trailing=$work/trailing.cpp
: > "$trailing"
i=0
while [ "$i" -lt 160 ]; do
   printf 'int value_%s; // note\n' "$i" >> "$trailing"
   i=$((i + 1))
done
expect_count "case 6 trailing-comment count" "$trailing" 160
expect_warn "case 6 trailing comments count as code" "$trailing" 160

# Case 7: every line of a raw string counts, including a line that begins with
# "//" and a line that is "/* ... */"; the same contents can push a file hard.
raw=$work/raw.cpp
cat > "$raw" <<'EOF'
const char* text = R"tag(
// not a comment
/* not a comment */
plain raw content
)tag";
EOF
expect_count "case 7 raw-string count" "$raw" 5
expect_pass "case 7 raw string is short" "$raw"

raw_hard=$work/raw-hard.cpp
fill_file "$raw_hard" 100
printf 'const char* text = R"tag(\n' >> "$raw_hard"
i=0
while [ "$i" -lt 201 ]; do
   printf 'raw content %s\n' "$i" >> "$raw_hard"
   i=$((i + 1))
done
printf ')tag";\n' >> "$raw_hard"
expect_count "case 7 raw-string hard count" "$raw_hard" 303
expect_fail "case 7 raw strings count toward the hard limit" "$raw_hard" 303

# Case 8: a comment marker inside a string or char literal is content, so
# "http://x", "/*" and '/' all count as code.
strings=$work/strings.cpp
cat > "$strings" <<'EOF'
const char* url = "http://x";
const char* star = "/*";
char slash = '/';
char quote = '\'';
EOF
expect_count "case 8 string and char literals" "$strings" 4
expect_pass "case 8 string and char literals are short" "$strings"

printf 'file-size fixture: ok\n'
