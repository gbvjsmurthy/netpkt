#!/usr/bin/env bash
# Loopback integration test: real server + real client processes, TCP and UDP.
# usage: integration_test.sh BUILD_DIR
set -u
BUILD=${1:?usage: $0 BUILD_DIR}
TMP=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$TMP"' EXIT
FAILS=0
FRAMES=60

fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
check() { # check "description" command...
    local d=$1; shift
    if "$@"; then echo "ok:   $d"; else fail "$d"; fi
}
# value of column NAME in the 2nd line of a CSV with a header
col() { awk -F, -v n="$2" 'NR==1{for(i=1;i<=NF;i++) if($i==n) c=i} NR==2{print $c}' "$1"; }
eq() { [ "$1" = "$2" ]; }
le() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a<=b)}'; }
gt() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a>b)}'; }

"$BUILD/gen_video" "$TMP/t.avi" $FRAMES 30 320 240 >/dev/null || { echo "FAIL: gen_video"; exit 1; }

# ---- error handling: must fail with a message, not crash or hang ----
"$BUILD/video_server" --video /nonexistent.avi --port 19999 >/dev/null 2>"$TMP/e1"; rc=$?
check "server: missing video exits 1 with a message" bash -c "[ $rc -eq 1 ] && grep -q 'cannot read video' '$TMP/e1'"
"$BUILD/video_server" --video "$TMP/t.avi" --port 70000 >/dev/null 2>&1; rc=$?
check "server: invalid port is a usage error (exit 2)" eq $rc 2
"$BUILD/video_client" --protocol sctp >/dev/null 2>&1; rc=$?
check "client: invalid protocol is a usage error (exit 2)" eq $rc 2
"$BUILD/video_client" --no-display --protocol tcp --port 19998 >/dev/null 2>"$TMP/e2"; rc=$?
check "client: connection refused exits 1 with a message" bash -c "[ $rc -eq 1 ] && grep -q 'cannot connect' '$TMP/e2'"
# a port already in use
python3 -c "
import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('0.0.0.0',19997)); s.listen(1); time.sleep(5)" &
sleep 0.5
"$BUILD/video_server" --video "$TMP/t.avi" --port 19997 >/dev/null 2>"$TMP/e3"; rc=$?
check "server: port in use exits 1 with a message" bash -c "[ $rc -eq 1 ] && grep -q 'cannot bind' '$TMP/e3'"
kill %1 2>/dev/null

# ---- one full run per protocol ----
for proto in tcp udp; do
    port=$((20000 + RANDOM % 20000))
    echo "== $proto on port $port"
    timeout 60 "$BUILD/video_server" --video "$TMP/t.avi" --protocol $proto --port $port \
        --fps 60 --run-id it --log "$TMP/srv_$proto.csv" >"$TMP/srv_$proto.out" 2>&1 &
    spid=$!
    sleep 0.5
    timeout 60 "$BUILD/video_client" --protocol $proto --port $port --no-display --run-id it \
        --log "$TMP/$proto.csv" >"$TMP/cli_$proto.out" 2>&1
    crc=$?
    wait $spid; src=$?
    check "$proto: client exit 0" eq $crc 0
    check "$proto: server exit 0" eq $src 0

    S="$TMP/${proto}_summary.csv"
    check "$proto: summary file written" test -s "$S"
    offered=$(col "$S" frames_offered); comp=$(col "$S" frames_complete)
    inc=$(col "$S" frames_incomplete);  miss=$(col "$S" frames_missing)
    txb=$(col "$S" tx_payload_bytes);   rxb=$(col "$S" rx_payload_bytes)
    check "$proto: END received" eq "$(col "$S" end_received)" 1
    check "$proto: server offered $FRAMES frames" eq "$offered" $FRAMES
    check "$proto: complete+incomplete+missing == offered" eq $((comp + inc + miss)) "$offered"
    check "$proto: per-frame CSV has header + $FRAMES rows" eq "$(wc -l < "$TMP/$proto.csv")" $((FRAMES + 1))
    check "$proto: server log has header + $FRAMES rows" eq "$(wc -l < "$TMP/srv_$proto.csv")" $((FRAMES + 1))
    srv_sum=$(awk -F, 'NR>1{s+=$4} END{print s}' "$TMP/srv_$proto.csv")
    check "$proto: END byte count matches the server's own log" eq "$txb" "$srv_sum"
    check "$proto: received bytes <= sent bytes" le "$rxb" "$txb"
    check "$proto: no frame passed the decoder in a broken state" eq "$(col "$S" decode_failures)" 0
    check "$proto: some frames complete" gt "$comp" 0

    if [ $proto = tcp ]; then
        # TCP is reliable and ordered: on a live loopback connection every frame must arrive.
        check "tcp: all frames complete" eq "$comp" $FRAMES
        check "tcp: received bytes == sent bytes" eq "$rxb" "$txb"
        check "tcp: all frames shown" eq "$(col "$S" frames_shown)" $FRAMES
    fi
done

[ $FAILS -eq 0 ] && echo "ALL OK" || echo "$FAILS check(s) FAILED"
[ $FAILS -eq 0 ]
