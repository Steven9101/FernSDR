#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The lab's own PulseAudio: one null sink and a socket of its own, so that
# the browser plays into a device with a real clock.
#
#   bench/browser/pulse.sh start|stop|status
#
# Headless Chromium's built-in fake audio output skips ticks when the
# machine is busy, and its AudioContext clock then runs several percent
# slow; a receiver streaming in real time sees its client buffer grow and
# reacts, which is the lab's fault, not the receiver's. A null sink renders
# on the system clock and underruns the way a real sound card does. This
# instance runs beside any other PulseAudio on the machine without touching
# it: no D-Bus name, no default configuration, its own directory.
set -eu
DIR=${FB_PULSE_DIR:-/run/fernbench-pulse}

running() {
  [ -f "$DIR/pid" ] && kill -0 "$(cat "$DIR/pid")" 2>/dev/null
}

case "${1:-}" in
start)
  if running; then
    echo "unix:$DIR/native"
    exit 0
  fi
  mkdir -p "$DIR"
  chmod 700 "$DIR"
  rm -f "$DIR/native"
  env -u DBUS_SESSION_BUS_ADDRESS DBUS_SESSION_BUS_ADDRESS=disabled: \
    XDG_RUNTIME_DIR="$DIR" PULSE_RUNTIME_PATH="$DIR" HOME="$DIR" \
    pulseaudio -n --daemonize=no --use-pid-file=no --exit-idle-time=-1 --disallow-exit=yes \
      --log-target=file:"$DIR/pulse.log" \
      -L "module-null-sink sink_name=fb rate=48000" \
      -L "module-native-protocol-unix socket=$DIR/native auth-anonymous=1" \
      >"$DIR/stdout.log" 2>&1 &
  echo $! >"$DIR/pid"
  i=0
  while [ ! -S "$DIR/native" ] && [ $i -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
  done
  if [ ! -S "$DIR/native" ]; then
    echo "pulse.sh: PulseAudio did not start, see $DIR/pulse.log" >&2
    exit 1
  fi
  echo "unix:$DIR/native"
  ;;
stop)
  if running; then
    kill "$(cat "$DIR/pid")"
    rm -f "$DIR/pid"
  fi
  ;;
status)
  if running; then echo "running, unix:$DIR/native"; else echo "stopped"; exit 1; fi
  ;;
*)
  echo "usage: pulse.sh start|stop|status" >&2
  exit 2
  ;;
esac
