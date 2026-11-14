#!/usr/bin/env bash

# Runs the headless Godot checks in tests/godot against what's in bin/.
# Build first: scons platform=linux arch=x86_64
#
#   tests/run_tests.sh <path to a Songodot binary> [modes...]
#
# The binary needs Songodot's songo_player_harness module, which provides
# SongoPlayerHarness (eg: ../Songodot/bin/godot.linuxbsd.editor.x86_64.llvm).
#
# Everything but the alsa mode uses the null backend, so it's silent. The
# alsa mode opens the real sound device at volume 0; it only runs when named.

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GODOT="${1:?usage: run_tests.sh <songodot binary> [modes...]}"
shift
MODES=("$@")
if [ ${#MODES[@]} -eq 0 ]; then
    MODES=(inproc daemon compare blend failures latency stream_buffer handoff)
fi

PROJECT="$ROOT/tests/godot"
AUDIO="$PROJECT/audio"
# Unix socket paths are capped at 108 bytes, so not somewhere deep. Its own
# name, so it never talks to a real songo_bgplayer.
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}"
SOCKET="$RUNTIME_DIR/songo_harness_test.sock"

# Where SongoPlayerHarness looks for the engine library and the daemon when
# a project runs from its folder, as Songo-5 does from the editor
HARNESS_BIN="$PROJECT/addons/songo_player_harness/bin"
mkdir -p "$HARNESS_BIN"
touch "$HARNESS_BIN/.gdignore"
for f in "$ROOT"/bin/libsongo_engine.linux.x86_64.so "$ROOT"/bin/songo_bgplayer.linux.x86_64; do
    [ -e "$f" ] || { echo "Missing $f, build first: scons platform=linux arch=x86_64" >&2; exit 1; }
    ln -sf "$f" "$HARNESS_BIN/$(basename "$f")"
done
python3 -I "$ROOT/tests/make_tones.py" "$AUDIO"

# Imports the tones (compare loads one as a resource)
"$GODOT" --headless --path "$PROJECT" --editor --quit > /dev/null 2>&1

# The stalling radio stream for the stream_buffer mode
STREAM_PORT=18765
python3 -I "$ROOT/tests/stream_server.py" "$STREAM_PORT" &
STREAM_SERVER=$!
trap 'kill "$STREAM_SERVER" 2>/dev/null' EXIT

run() {
    echo "==== $1"
    "$GODOT" --headless --path "$PROJECT" -s test.gd -- "$1" "$AUDIO" "$SOCKET" "http://127.0.0.1:$STREAM_PORT/radio.wav" 2>&1 | grep -v "^Godot Engine"
    return "${PIPESTATUS[0]}"
}

failed=0
for mode in "${MODES[@]}"; do
    if [ "$mode" = handoff ]; then
        run handoff_leave || failed=$((failed + 1))
        sleep 1
        run handoff_return || failed=$((failed + 1))
        rm -f "$SOCKET.hook_ran"
        run handoff_switch || failed=$((failed + 1))
        sleep 1
        echo "daemon PID in its lock file: $(cat "$SOCKET.lock")"
        run handoff_return || failed=$((failed + 1))
        # Stopped and nobody connected, it quits within a few seconds and runs the hook
        sleep 4
        if [ -f "$SOCKET.hook_ran" ]; then
            echo "exit hook ran"
            rm -f "$SOCKET.hook_ran"
        else
            echo "FAIL: exit hook didn't run"
            failed=$((failed + 1))
        fi
    else
        run "$mode" || failed=$((failed + 1))
    fi
done

echo
if [ "$failed" -eq 0 ]; then
    echo "All modes passed"
else
    echo "$failed mode(s) had failures, daemon log: $RUNTIME_DIR/songo_bgplayer.log"
fi
exit "$failed"
