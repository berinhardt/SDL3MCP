#!/bin/sh
# Point this clone at the tracked hooks directory. Run once after a fresh
# clone; it is idempotent.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
git -C "$root" config core.hooksPath scripts/hooks
printf 'check: core.hooksPath is scripts/hooks; git commit now runs scripts/check.sh\n'
