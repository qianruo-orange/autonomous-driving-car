# Autonomous Driving Car video playback checks

The Autonomous Driving Car Home page uses one H.264/fMP4 connection through Media Source Extensions
(MSE), with MJPEG as the fallback when MSE is unavailable.

The H.264 stream emits a continuous decode timeline. For every adjacent pair of
samples, the next sample's `tfdt` equals the previous sample's `tfdt + duration`.
Source timestamps still control network pacing; they are not mixed with the
accumulated media decode timestamps.

The browser reads and appends data in one asynchronous chain. Each SourceBuffer
operation finishes at `updateend` before another network chunk is appended.
Played media is removed on the same chain. Playback that falls behind catches up
inside its existing buffer; an ordinary backlog does not reopen the connection.
Callbacks from an aborted connection cannot stop a newer connection.

Connection pacing starts at the newest encoded frame. Historical GOP samples
are sent immediately in decode order, followed by normal live pacing. The player
primes near the live edge with a 40 ms Chrome buffer target (200 ms for other
browsers), catches larger backlogs
by seeking, and drains small backlogs at 1.05x playback speed.

The FPS badge reports displayed frames when playback-quality counters are
available. Hover over the badge to compare displayed and received frame rates.
`window.getCarVideoDiagnostics()` returns read-only playback counters,
buffer lead, decoder frame drops, reconnect count, and the most recent error.
These counters are local to the browser and are not uploaded.
The status line below the video shows the essential values without developer
tools. The lower-level stream delay measures encoder output to presentation;
the displayed total estimate adds device capture-to-encoder processing time.
Chrome frame callbacks are used to observe submitted frames rather than merely
the media element's advanced playhead. This remains an estimate, not proof of a
100 ms physical camera-to-screen bound.

## Regression checks

On the development board:

```sh
bash scripts/build.sh -R
ctest --test-dir build --output-on-failure
```

On any machine with Node.js:

```sh
node tests/video_player_test.cjs
```

The C test runs the production stream client against a local socketpair and
checks MP4 metadata for variable intervals, non-VCL frames, zero intervals,
large gaps, source-clock wraparound, immediate historical-GOP catch-up, and
interruptible live pacing. The JavaScript test uses a simulated
SourceBuffer and network reader; browser playback still requires a live check.
