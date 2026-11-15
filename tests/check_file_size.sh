#!/bin/sh
# Fixture for the file-size rule in scripts/check-file-size.sh.
#
# Cases pin the counter, the rule and the exception ledger: blank lines and
# comments are free, a code line with a trailing comment counts, strings, char
# literals and raw strings count, comments cannot hide a hard file or
# manufacture a warning, both thresholds carry their exact message, and the
# ledger rejects a missing, stale, duplicate, malformed, or no-longer-needed
# entry. scripts/check.sh invokes this fixture before the rule, so the rule and
# its proof cannot drift apart.
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

# make_repo DIR creates a throwaway git checkout with a src/ directory, so the
# rule's tree scan sees exactly the files a ledger case adds.
make_repo() {
   repo=$1
   mkdir -p "$repo/src"
   git -C "$repo" init -q
}

# run_tree_rule REPO LEDGER runs the rule from REPO with the exception ledger
# overridden, and sets status and out.
run_tree_rule() {
   repo=$1
   ledger=$2
   if out=$(cd "$repo" &&
      SDL3MCP_SOFT_LIMIT_EXCEPTIONS="$ledger" sh "$rule" 2>&1); then
      status=0
   else
      status=$?
   fi
}

# Case 9: a file over 150 with a matching ledger entry passes and still warns;
# the ledger is the record, not a count copied into a ticket.
repo=$work/listed
make_repo "$repo"
fill_file "$repo/src/soft.cpp" 160
git -C "$repo" add src/soft.cpp
ledger=$work/listed.tsv
printf 'src/soft.cpp\t160\tone table\n' > "$ledger"
run_tree_rule "$repo" "$ledger"
[ "$status" -eq 0 ] || fail "case 9 matching ledger failed: $out"
case $out in
*"warning: src/soft.cpp: 160"*) ;;
*) fail "case 9 matching ledger did not warn: $out" ;;
esac

# Case 10: a file over 150 with no ledger entry fails, naming the file and the
# ledger.
repo=$work/unlisted
make_repo "$repo"
fill_file "$repo/src/soft.cpp" 160
git -C "$repo" add src/soft.cpp
ledger=$work/unlisted.tsv
: > "$ledger"
run_tree_rule "$repo" "$ledger"
[ "$status" -ne 0 ] || fail "case 10 unlisted soft file passed: $out"
case $out in
*"src/soft.cpp"*"$ledger"*) ;;
*) fail "case 10 does not name the file and the ledger: $out" ;;
esac

# Case 11: a ledger count that no longer matches the counter fails as stale.
repo=$work/stale
make_repo "$repo"
fill_file "$repo/src/soft.cpp" 160
git -C "$repo" add src/soft.cpp
ledger=$work/stale.tsv
printf 'src/soft.cpp\t161\tone table\n' > "$ledger"
run_tree_rule "$repo" "$ledger"
[ "$status" -ne 0 ] || fail "case 11 stale count passed: $out"
case $out in
*stale*) ;;
*) fail "case 11 does not say stale: $out" ;;
esac

# Case 12: an entry for a file at or below 150 fails, and so does an entry for
# a file the tree no longer tracks.
repo=$work/below
make_repo "$repo"
fill_file "$repo/src/small.cpp" 100
git -C "$repo" add src/small.cpp
ledger=$work/below.tsv
printf 'src/small.cpp\t100\tno longer large\n' > "$ledger"
run_tree_rule "$repo" "$ledger"
[ "$status" -ne 0 ] || fail "case 12 entry at 100 passed: $out"
case $out in
*"at or below"*) ;;
*) fail "case 12 does not say the entry is no longer needed: $out" ;;
esac

repo=$work/gone
make_repo "$repo"
fill_file "$repo/src/keep.cpp" 100
git -C "$repo" add src/keep.cpp
ledger=$work/gone.tsv
printf 'src/gone.cpp\t160\tremoved\n' > "$ledger"
run_tree_rule "$repo" "$ledger"
[ "$status" -ne 0 ] || fail "case 12 entry for a missing file passed: $out"
case $out in
*"no longer exists"*) ;;
*) fail "case 12 does not say the file no longer exists: $out" ;;
esac

# Case 13: two ledger entries for one file fail as duplicates.
repo=$work/duplicate
make_repo "$repo"
fill_file "$repo/src/soft.cpp" 160
git -C "$repo" add src/soft.cpp
ledger=$work/duplicate.tsv
printf 'src/soft.cpp\t160\tone\nsrc/soft.cpp\t160\ttwo\n' > "$ledger"
run_tree_rule "$repo" "$ledger"
[ "$status" -ne 0 ] || fail "case 13 duplicate entries passed: $out"
case $out in
*duplicate*) ;;
*) fail "case 13 does not say duplicate: $out" ;;
esac

# Case 14: a ledger line whose count is not a number fails as malformed.
repo=$work/malformed
make_repo "$repo"
fill_file "$repo/src/soft.cpp" 160
git -C "$repo" add src/soft.cpp
ledger=$work/malformed.tsv
printf 'src/soft.cpp\tmany\tone\n' > "$ledger"
run_tree_rule "$repo" "$ledger"
[ "$status" -ne 0 ] || fail "case 14 malformed entry passed: $out"
case $out in
*malformed*) ;;
*) fail "case 14 does not say malformed: $out" ;;
esac

# Case 15: an entry with no reason is malformed; the third column is part of
# the record.
repo=$work/no-reason
make_repo "$repo"
fill_file "$repo/src/soft.cpp" 160
git -C "$repo" add src/soft.cpp
ledger=$work/no-reason.tsv
printf 'src/soft.cpp\t160\n' > "$ledger"
run_tree_rule "$repo" "$ledger"
[ "$status" -ne 0 ] || fail "case 15 entry without a reason passed: $out"
case $out in
*malformed*) ;;
*) fail "case 15 does not say malformed: $out" ;;
esac

# Case 16: an entry for a file that exists but is not tracked is a scope
# error, not a deleted file.
repo=$work/untracked
make_repo "$repo"
fill_file "$repo/src/keep.cpp" 100
git -C "$repo" add src/keep.cpp
fill_file "$repo/src/extra.cpp" 160
ledger=$work/untracked.tsv
printf 'src/extra.cpp\t160\tone table\n' > "$ledger"
run_tree_rule "$repo" "$ledger"
[ "$status" -ne 0 ] || fail "case 16 untracked entry passed: $out"
case $out in
*"not a tracked product source"*) ;;
*) fail "case 16 does not say untracked: $out" ;;
esac

# Case 17: a relative ledger path resolves from the caller's directory even
# though the rule moves to the repository root.
repo=$work/relative
make_repo "$repo"
fill_file "$repo/src/soft.cpp" 160
git -C "$repo" add src/soft.cpp
printf 'src/soft.cpp\t160\tone table\n' > "$work/relative.tsv"
if out=$(cd "$repo/src" &&
   SDL3MCP_SOFT_LIMIT_EXCEPTIONS=../../relative.tsv sh "$rule" 2>&1); then
   status=0
else
   status=$?
fi
[ "$status" -eq 0 ] || fail "case 17 relative ledger failed: $out"

printf 'file-size fixture: ok\n'
