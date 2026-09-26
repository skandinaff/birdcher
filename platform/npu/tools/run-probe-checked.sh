#!/usr/bin/env bash
# Usage: run-probe-checked.sh LOG_PREFIX COMMAND [ARG ...]
# Example: ... /tmp/test env TEFLON_LIB=/tmp/libteflon.so ./probe model rgb npu 329
# Run on an otherwise idle board. Kernel errors cannot be attributed safely
# when other GPU/NPU workloads are active.
set -euo pipefail
if (( $# < 2 )); then
    echo "usage: $0 LOG_PREFIX COMMAND [ARG ...]" >&2
    exit 2
fi
prefix=$1
shift
mkdir -p "$(dirname "$prefix")"
journalctl -k -n 0 --show-cursor --no-pager > "$prefix.cursor"
cursor=$(sed -n 's/^-- cursor: //p' "$prefix.cursor")
if [[ -z "$cursor" ]]; then
    echo "Cannot read kernel journal cursor; refusing an unchecked run" >&2
    exit 4
fi
printf '%q ' "$@" > "$prefix.command"
printf '\n' >> "$prefix.command"
status=0
"$@" > "$prefix.log" 2>&1 || status=$?
# Allow asynchronous hang recovery to be logged, then flush journald before
# reading. Absence of a fault is still not proof of numerical correctness.
sleep 2
journalctl --sync
journalctl -k --after-cursor="$cursor" --no-pager > "$prefix.kernel.log"
if grep -Eiq 'MMU.*fault|recover hung GPU|GPU.*hang|GPU.*fault' "$prefix.kernel.log"; then
    echo "INVALID: kernel GPU/NPU fault; command exit=$status; see $prefix.kernel.log" >&2
    exit 3
fi
if (( status != 0 )); then
    echo "FAILED: command exit=$status; see $prefix.log" >&2
    exit "$status"
fi
echo "Command succeeded with no matching kernel faults; accuracy still needs comparison."
