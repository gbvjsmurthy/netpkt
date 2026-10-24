#!/usr/bin/env bash
# Run the SAME stream over TCP and then UDP and store everything in results/<stamp>/.
#
# usage: scripts/run_experiment.sh VIDEO [SERVER_OPTIONS...]
# env:   HOST=127.0.0.1   server address as seen by the client (for a LAN run start the
#                         server on the other machine yourself; see README "LAN test")
#        REPEAT=1         runs per protocol
#        PORT=9000


# Example: scripts/run_experiment.sh assets/sample.mp4 --fps 30 --quality 80 --width 640
set -eu
VIDEO=${1:?usage: $0 VIDEO [server options]}; shift
BUILD="$(cd "$(dirname "$0")/.." && pwd)/build"
HOST=${HOST:-127.0.0.1}; PORT=${PORT:-9000}; REPEAT=${REPEAT:-1}
OUT="$(dirname "$BUILD")/results/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT"

{   echo "date:      $(date -Is)"
    echo "kernel:    $(uname -srm)"
    echo "cpu:       $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "host:      $HOST  port: $PORT  repeat: $REPEAT"
    echo "video:     $VIDEO"
    echo "server opts: $*"
    echo "rmem_max:  $(cat /proc/sys/net/core/rmem_max 2>/dev/null)"
    echo "tc qdisc on lo:"; tc qdisc show dev lo 2>/dev/null || echo "  (tc not available)"
} > "$OUT/environment.txt"

trap 'kill $(jobs -p) 2>/dev/null || true' EXIT
for i in $(seq 1 "$REPEAT"); do
  for proto in tcp udp; do
    id="${proto}_$i"
    echo "== $id"
    "$BUILD/video_server" --video "$VIDEO" --protocol $proto --port "$PORT" \
        --run-id "$id" --log "$OUT/server_$id.csv" "$@" > "$OUT/server_$id.txt" 2>&1 &
    spid=$!
    sleep 1   # give the server time to open the socket; the UDP client also retries HELLO
    "$BUILD/video_client" --host "$HOST" --port "$PORT" --protocol $proto --no-display \
        --run-id "$id" --log "$OUT/client_$id.csv" | tee "$OUT/client_$id.txt" | grep -E "loss|throughput|stalls"
    wait $spid || echo "warning: server for $id exited with an error (see $OUT/server_$id.txt)"
  done
done
echo "results in $OUT"
