#!/bin/sh
# Send a Codex lifecycle event to the locally running AI Clock Bridge.
#
# Install this script outside the repository (see README) because Codex hooks
# run from the user's ~/.codex configuration and must survive repository moves.

event="$1"

# Codex provides hook context as JSON on stdin. This bridge needs only the
# lifecycle event passed as the first argument, but draining stdin prevents a
# blocked pipe from delaying Codex.
cat >/dev/null

curl -fsS --connect-timeout 0.5 --max-time 1 \
  -H 'Content-Type: application/json' \
  -d "{\"agent\":\"codex\",\"event\":\"$event\"}" \
  http://127.0.0.1:8765/event >/dev/null 2>&1 || true
