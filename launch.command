#!/bin/bash

cd "$(dirname "$0")" || exit 1
URL="http://127.0.0.1:8769"

if curl -fsS --max-time 0.5 "$URL/api/state" >/dev/null 2>&1; then
  open "$URL"
  printf 'Dashboard already running at %s\n' "$URL"
  exit 0
fi

make || exit 1

python3 tools/dashboard.py &
pid=$!

cleanup() {
  trap - EXIT INT TERM HUP
  if kill -0 "$pid" 2>/dev/null; then
    kill -INT "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM HUP

ready=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if curl -fsS --max-time 0.3 "$URL/api/state" >/dev/null 2>&1; then
    ready=1
    break
  fi
  if ! kill -0 "$pid" 2>/dev/null; then
    printf 'Dashboard server exited before it was ready.\n' >&2
    exit 1
  fi
  sleep 0.15
done

open "$URL"
if [[ "$ready" -ne 1 ]]; then
  printf 'Server did not answer on %s yet.\n' "$URL" >&2
fi
wait "$pid"
