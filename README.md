# netpkt: a TCP vs UDP video-streaming benchmark

`netpkt` streams a prerecorded video from a server to a client over **TCP** or
**UDP**, using the same frames, same JPEG settings and same pacing, and measures
what the client actually observes. It is a teaching project: small, direct POSIX
socket code, no networking libraries. It does not assume UDP is faster or TCP
is slower; it records numbers and lets you compare them.

## 1. TCP and UDP: what matters for this experiment

| | TCP | UDP |
|---|---|---|
| Delivery | reliable: lost segments are retransmitted | best effort: lost datagrams are gone |
| Order | in order | any order, duplicates possible |
| Unit | byte stream (no message boundaries) | datagrams (boundaries kept) |
| Control | flow control + congestion control | none |
| Effect of one lost packet | everything after it waits (head-of-line blocking), so frames arrive late but complete | only that fragment is lost, so its frame is unusable, but later frames are unaffected |

So the two transports fail differently. TCP trades **latency** for completeness;
UDP trades **completeness** for timeliness. Whether that trade pays off depends
on the network. On clean loopback you will likely see almost no difference.

*Network packet loss is not application frame loss.* TCP retransmissions hide
packet loss from the application (it shows up as delay). A UDP frame of ~30
fragments is lost if **any** fragment is lost, so a 2% fragment loss rate costs
about 1 - 0.98^30 = 45% of frames. `netpkt` reports frame loss, not packet loss.

## 2. Architecture and data flow

```
video file -> [server] read frame -> JPEG encode -> pace to --fps -> timestamp -> send
                                                                         |
                  TCP: 20-byte header + JPEG, one message per frame      |
                  UDP: JPEG cut into <=1170-byte fragments               v
[client] receive thread: (TCP: read message | UDP: reassemble fragments)
             -> frame complete: record arrival time -> bounded FrameQueue
         main thread: pop -> JPEG decode -> show (or --no-display) -> record
         at exit: per-frame CSV + summary CSV + printed summary
```

* **Receive thread** owns the socket and the arrival log. It never waits for the
  display: `FrameQueue::push` drops the *oldest* queued frame if the queue is full
  and logs it (`queue_dropped`).
* **Main thread** decodes and shows frames (OpenCV windows want the main thread).
* The only shared object is the `FrameQueue` (one mutex + condition variable).
  The arrival log and the play log are each written by one thread and read after
  `join()`.
* **Playback policy (same for TCP and UDP):** show each complete frame as soon
  as it is popped, in completion order. No jitter buffer, no catch-up. UDP frames
  that are not complete are skipped, never decoded.
* **UDP specifics:** the client sends `HELLO` (repeated every 500 ms until the
  server answers) so the server learns its address. Fragments of a frame are
  reassembled; a frame still incomplete 500 ms after its first fragment
  (`--reasm-ms`) is abandoned and logged as `incomplete`. At most 32 frames are
  assembled at once; memory grows only with bytes actually received. The server
  sends `END` 5 times because one datagram could be lost. There is **no**
  retransmission, ACK, or congestion control, on purpose.

## 3. Repository structure

```
CMakeLists.txt
include/common/  frame.h (Frame, JPEG helpers) protocol.h (wire format) util.h frame_queue.h
include/net/     fd.h (RAII socket) tcp.h udp.h (incl. UdpAssembler)
include/bench/   metrics.h (records, summary, CSV)
src/common/      frame.cpp protocol.cpp util.cpp
src/net/         tcp.cpp udp.cpp
src/bench/       metrics.cpp
src/server/      main.cpp          src/client/main.cpp
tests/           unit tests, gen_video.cpp, integration_test.sh
scripts/         run_experiment.sh
assets/          put your video here (ignored by git)
results/         generated CSVs (ignored by git)
```

## 4. Dependencies (Linux Mint / Ubuntu)

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config libopencv-dev
# optional, for the loss/delay experiments:  sudo apt install iproute2
```

Requires a C++17 compiler (GCC 9+), CMake 3.16+, OpenCV 4.x with GUI support
(`libopencv-dev` includes it). IPv4 only. Linux only (POSIX sockets, `MSG_NOSIGNAL`).

## 5. Build

```bash
cmake -S . -B build
cmake --build build -j
```

Produces `build/video_server`, `build/video_client`, the tests, and
`build/gen_video` (makes a synthetic test video if you have none):

```bash
./build/gen_video assets/sample.avi 150 30 640 360
```

## 6. Run

Start the **server first**, one transport per run. The server waits for a client, streams
the video once, sends END, and exits.

```bash
# terminal 1
./build/video_server --video assets/sample.avi --protocol tcp --port 9000
# terminal 2
mkdir -p results
./build/video_client --host 127.0.0.1 --port 9000 --protocol tcp --log results/tcp.csv
```

```bash
./build/video_server --video assets/sample.avi --protocol udp --port 9000
./build/video_client --host 127.0.0.1 --port 9000 --protocol udp --log results/udp.csv
```

(TCP and UDP ports are separate namespaces, so 9000 works for both. Use
`--no-display` on the client over SSH or without a desktop.) ESC in the video
window or Ctrl-C stops a run; the summary is then marked partial.

**Server options:** `--video FILE` (required), `--port N` (9000), `--protocol tcp|udp`
(tcp), `--fps X` (video's own rate, else 30), `--quality 1..100` (80), `--width W`
(0 = native), `--max-frames N` (0 = all), `--unpaced` (send as fast as possible),
`--frag-gap-us N` (UDP: pause between fragments, default 0), `--log FILE`
(per-frame send log), `--run-id ID`.

**Client options:** `--host H` (127.0.0.1), `--port N`, `--protocol tcp|udp`, `--log FILE`
(per-frame CSV), `--summary FILE` (default: log name with `_summary.csv`),
`--run-id ID`, `--no-display`, `--queue N` (32), `--stall-ms N` (100), `--reasm-ms N`
(500, UDP), `--idle-ms N` (3000, UDP: stop if END was lost).

Exit codes: 0 ok, 1 runtime error, 2 usage error.

## 7. Experiments

### Fair comparison checklist
Keep identical between the TCP and UDP run: video, `--width`, `--quality`, `--fps`,
`--max-frames`, client `--queue`/`--stall-ms`. Change only `--protocol`. Run each several
times (`REPEAT=5`); one run is an anecdote. The server prints its effective settings at start.

### Loopback (baseline)
```bash
scripts/run_experiment.sh assets/sample.avi --fps 30 --quality 80 --width 640
REPEAT=5 scripts/run_experiment.sh assets/sample.avi --fps 30
```
This writes `results/<timestamp>/` with per-frame and summary CSVs, the console output
and `environment.txt`. Loopback has no real network, so expect ~0% loss and
sub-millisecond latency for both. That is a baseline, not a finding about TCP vs UDP.

### LAN
Machines A (server) and B (client), same stream settings:
```bash
# on A
./build/video_server --video assets/sample.avi --protocol udp --port 9000 --run-id lan_udp
# on B
./build/video_client --host <A's IP> --port 9000 --protocol udp --no-display --log results/lan_udp.csv --run-id lan_udp
```
Open the port on A's firewall if needed (`sudo ufw allow 9000`). Repeat with `--protocol tcp`.
Use `rel_delay_ms`, not raw latency, across machines (section 9).

### Controlled loss and delay with `tc netem` (optional, needs root)
`netpkt` never changes your network settings. If you choose to, on **loopback** for a
local test (the rule applies to all loopback traffic while it exists):
```bash
sudo tc qdisc add dev lo root netem loss 2% delay 20ms 5ms   # add
tc qdisc show dev lo                                         # inspect
# ... run the experiment ...
sudo tc qdisc del dev lo root                                # REMOVE when done
```
Replace `lo` with your interface (`ip -br link`) for a LAN test; apply it on the sender
side to affect server-to-client packets. `netem` applies per **packet**: for UDP
that is per fragment, so frame loss will be far higher than 2%. `environment.txt`
records `tc qdisc show dev lo` so a leftover rule is visible in your results.

## 8. Wire protocol

All integers big-endian. Fields are written one by one; structs are never sent raw.

**TCP message** (20-byte header, then `len` bytes):

| off | size | field | meaning |
|---|---|---|---|
| 0 | u16 | magic | `0x4E50` ("NP"); catches desynchronized streams |
| 2 | u8 | type | 1 = FRAME, 2 = END |
| 3 | u8 | reserved | must be 0 |
| 4 | u32 | frame_id | 0,1,2,... |
| 8 | u64 | timestamp_us | server steady clock just before sending (0 for END) |
| 16 | u32 | len | FRAME: JPEG size, 1..8 MiB. END: 12 |

**UDP datagram** (30-byte header, then `payload_len` bytes; total <= 1200 bytes):

| off | size | field | meaning |
|---|---|---|---|
| 0 | u16 | magic | `0x4E50` |
| 2 | u8 | type | 1 = FRAME, 2 = END, 3 = HELLO |
| 3 | u8 | reserved | 0 |
| 4 | u32 | session | random id chosen by the client; datagrams with another id are ignored (stale runs) |
| 8 | u32 | frame_id | |
| 12 | u64 | timestamp_us | same as TCP |
| 20 | u32 | frame_len | total JPEG size of the frame |
| 24 | u16 | frag_idx | 0-based |
| 26 | u16 | frag_cnt | must equal ceil(frame_len / 1170) |
| 28 | u16 | payload_len | must equal the fragment's expected size (1170, or the remainder for the last) |

**END payload** (12 bytes, both transports): `u32 frames_sent, u64 payload_bytes_sent`.
**HELLO**: UDP only, no payload.

Validation happens before allocation: frame size <= 8 MiB, frame_id < 2^20, fragment
index/count/length consistent with `frame_len`, datagram size equal to header + declared
payload. Fragments of one frame must agree on `frame_len`, `frag_cnt`, `timestamp_us`.

## 9. Metrics, definitions and CSV columns

**Timestamps.** `send_us` is taken on the server *after* JPEG encoding and *after* the pacing
wait, immediately before the first byte is handed to the socket. `recv_us` is taken on the
client the moment the **last** byte/fragment of the frame is received (frame complete).
Both are `std::chrono::steady_clock` microseconds.

**Latency** = `recv_us - send_us` (ms). This subtracts clocks from two processes:
* *Same host* (loopback): both read the same `CLOCK_MONOTONIC`, so it is valid.
* *Different hosts*: the clocks are unrelated and the value is meaningless (even negative)
  unless they are synchronized (chrony/PTP, and even then to within their accuracy).
  Use **`rel_delay_ms` = latency - (smallest latency in the run)**. A constant clock offset
  cancels, so it shows how much *extra* delay each frame suffered (queueing, retransmission
  waits). Assumptions: negligible clock drift over the run, and the fastest frame is close
  to the true minimum delay. It is a relative number, not the true one-way delay.

**Per-frame CSV** (`--log`; one row per frame id 0..offered-1; unknown values are empty):
`protocol, run_id, frame_id, size_bytes, send_us, recv_us, latency_ms, rel_delay_ms,
rx_status (complete|incomplete|missing), decode_status (ok|fail|none), decode_us, play_us,
queue_dropped (0|1)`.
`incomplete` = some UDP fragments arrived but reassembly was abandoned; `missing` = nothing of
that frame was seen. `play_us` is when the frame was handed to the display (after `imshow`,
or after decode with `--no-display`).

**Summary CSV** (`--summary`) and printed summary:

| column | definition |
|---|---|
| frames_offered | frames the server says it sent (END message). If END was lost: highest id seen + 1, flagged by `end_received=0`; frames lost after the last one seen then cannot be counted |
| frames_complete / incomplete / missing | by `rx_status` |
| frame_loss_pct | `100 * (offered - complete) / offered` |
| tx_payload_bytes | JPEG bytes the server sent (from END; empty if END lost) |
| rx_payload_bytes | JPEG bytes of complete frames |
| throughput_mbps | **application goodput**: JPEG bytes of complete frames *except the first-arrived*, x8, divided by time between first and last complete-frame arrival. Excludes TCP/UDP/IP/Ethernet headers and retransmissions, so it is **not** link throughput. In a paced run it just reflects the video bitrate |
| arrival_fps | `(complete - 1) / time between first and last arrival` |
| latency_* / reldelay_* | mean, median, p95, p99 (ms) over complete frames. Percentiles use nearest-rank; p95 needs >= 20 samples and p99 >= 100, otherwise empty |
| decode_failures, decode_mean_ms | frames that failed `imdecode`; mean decode time |
| queue_drops | complete frames discarded because the play queue was full |
| frames_shown, shown_fps | frames decoded and handed to the display; their rate |
| stall_count, stall_total_ms | a **stall** is a shown frame whose gap since the previous shown frame exceeds the sender's own spacing (difference of `send_us`) by more than `--stall-ms`. The excess is added to the total. Lost frames are not counted as stalls (they are in frame loss). Meaningless with `--unpaced` |
| udp_* | datagrams read, bad header, foreign session, inconsistent fragment, duplicate, late (frame already finished/abandoned), reordered (arrived after a later position in the send order). Empty for TCP |

Everything is computed from the observed records in `src/bench/metrics.cpp`; nothing
depends on which protocol is in use.

## 10. Tests

```bash
cmake --build build -j
(cd build && ctest --output-on-failure)
```

| test | covers |
|---|---|
| `test_protocol` | exact byte layout, round trips, every rejection path (short, magic, type, reserved, oversize/zero length, frame id, fragment index/count/length, END/HELLO), fragment-size arithmetic |
| `test_tcp` | `send_all`/`recv_all` with 7-byte writes and a 4 MiB transfer (partial I/O), EOF at boundary vs mid-message, frame/END round trip with message boundaries, oversized/truncated/garbage input (over `socketpair`) |
| `test_udp` | reassembly in order, shuffled, reversed; duplicates; missing fragment + timeout (just before / at the limit); late fragment after timeout does not resurrect a frame; inconsistent/invalid fragments; memory bound; flush at END; full sender-to-assembler path |
| `test_metrics` | percentiles, CSV escaping/round trip, loss/throughput/latency/stall numbers against hand-computed values, edge cases (empty run, END lost) |
| `test_frame` | JPEG round trip, garbage/truncated input, resize, queue FIFO/drop-oldest/close wake-up |
| `integration_loopback` | real server and client processes for TCP and UDP, error exits (bad file/port/protocol, refused, port in use), CSV row counts, byte accounting, and TCP's invariant that every frame arrives |

The integration test uses `build/gen_video`, so it needs no video file. UDP assertions are
invariants (counts add up, bytes received <= sent) rather than "no loss", since UDP may drop.

## 11. Limitations and caveats

* One client, one video pass, IPv4, Linux only. No multi-client, no rate adaptation.
* Frames are independent JPEGs (no inter-frame codec), so a lost UDP frame does not corrupt
  later ones. Real video codecs behave differently.
* UDP sends each frame's fragments back-to-back (a burst). On a real network this burst may
  overflow queues; `--frag-gap-us` spreads it. The client requests a 4 MiB receive buffer, but
  Linux caps it at `net.core.rmem_max`; the granted size is printed. Lost datagrams caused by a
  small buffer look like network loss.
* Playback has no jitter buffer, so TCP delay shows as stalls and UDP loss as skipped frames.
  A different policy would change the stall numbers (not the loss/latency numbers).
* Throughput is application payload only (section 9). Packet-level overhead and network packet
  loss are **not** measured.
* Raw latency across machines is not meaningful (section 9). Loopback numbers say little about
  real networks.
* With `--unpaced`, or if the server cannot keep up (`behind_schedule_frames` > 0 in the server
  summary), the stream is not at the target rate; stall numbers are then misleading.
* Under ThreadSanitizer the only reports were a lock-order warning inside OpenCV's bundled GDAL
  plugin (first JPEG call); none involved netpkt code.

## 12. Troubleshooting

| symptom | cause / fix |
|---|---|
| `cannot bind TCP/UDP port N: Address already in use` | another process (maybe an old server) holds it: `ss -tulpn \| grep 9000`, or use `--port` |
| `cannot connect ...: Connection refused` | server not running yet, wrong port, or protocol mismatch. Start the server first |
| UDP client: `no response ... after 10 HELLOs` | server not running / wrong `--protocol` on the server / firewall blocking the UDP port |
| client hangs on LAN | firewall (`sudo ufw status`), wrong IP; try `ping` and `nc -vz HOST PORT` (TCP) |
| `no display available ... use --no-display` | no desktop session (SSH?). Use `--no-display`, or `ssh -X` / run in a desktop terminal |
| `OpenCV cannot decode 'file'` | codec/container not supported by your OpenCV build; convert: `ffmpeg -i in.mkv -c:v libx264 out.mp4` |
| `Could NOT find OpenCV` | `sudo apt install libopencv-dev` |
| UDP shows high loss on loopback | receive buffer too small: `sudo sysctl -w net.core.rmem_max=8388608`, or try `--frag-gap-us 50` |
| `behind_schedule_frames` > 0 | encoding is slower than the frame period: lower `--width`/`--quality`/`--fps` |
| a `tc` rule is still active | `tc qdisc show dev lo`; remove with `sudo tc qdisc del dev lo root` |

## 13. Naming and style

`snake_case` files, functions and variables; `PascalCase` types; `UPPER_SNAKE_CASE`
constants; `.h` headers; `-Wall -Wextra -Wpedantic` with no warnings suppressed. The only
global mutable state is the stop flag set by the signal handler (`util.cpp`).
