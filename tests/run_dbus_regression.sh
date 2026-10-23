#!/usr/bin/env bash
# Two private buses; this test never contacts the desktop or system bus.
set -euo pipefail
daemon=${1:?dbus-daemon required}
test_binary=${2:?test binary required}
runtime=$(mktemp -d /tmp/helix-dbus.XXXXXX)
pids=()
cleanup() { for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done; wait || true; rm -rf -- "$runtime"; }
trap cleanup EXIT
for name in session system; do
  "$daemon" --session --nofork --address="unix:path=$runtime/$name" >"$runtime/$name.log" 2>&1 &
  pids+=("$!")
done
for attempt in {1..100}; do
  [[ -S $runtime/session && -S $runtime/system ]] && break
  sleep .02
done
[[ -S $runtime/session && -S $runtime/system ]] || { cat "$runtime/"*.log >&2; exit 1; }
export DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/session"
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$runtime/system"
timeout 30 "$test_binary"
