#!/usr/bin/env bash
# One build at a time on this machine, across the checkout and every worktree.
# Several sessions that compile at once run this Mac out of memory, so every
# configure, build, run of the whole test suite and Linux container run goes
# through this lock:
#
#   engine/scripts/build-locked.sh cmake --preset macos-llvm -B build/macos-llvm-o2
#   engine/scripts/build-locked.sh cmake --build build/macos-llvm-o2 -j 4
#   engine/scripts/build-locked.sh ctest --test-dir build/macos-llvm-o2 -j 4
#   engine/scripts/build-locked.sh engine/scripts/linux.sh test
#
# The command waits for its turn, for as long as it takes, and says who holds
# the lock while it waits. The lock is a file lock held by this process: when
# the process ends, however it ends, the lock is free, so nothing goes stale.
# One test binary run with a --gtest_filter needs no lock.
set -euo pipefail

if [[ $# -eq 0 ]]; then
  echo "usage: $(basename "$0") <command> [arguments...]" >&2
  exit 2
fi

lock_dir="${PONI_BUILD_LOCK_DIR:-$HOME/.cache/poni-gbp}"
lock="$lock_dir/build.lock"
holder="$lock_dir/build.holder"
mkdir -p "$lock_dir"

if [[ "${PONI_BUILD_LOCK_HELD:-}" != "$lock" ]]; then
  # Not inside the lock yet. Say so if someone else has it, then wait for it.
  if ! lockf -k -s -t 0 "$lock" true; then
    echo "build lock: waiting; held by $(cat "$holder" 2>/dev/null || echo 'another build')" >&2
  fi
  export PONI_BUILD_LOCK_HELD="$lock"
  exec lockf -k "$lock" "$0" "$@"
fi

# Inside the lock (or called by a command that already holds it).
echo "pid $$ in $(pwd), since $(date '+%H:%M:%S'): $*" > "$holder"
status=0
"$@" || status=$?
: > "$holder"
exit "$status"
