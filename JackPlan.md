# Focused Linux JACK Integration Plan

> **Status:** `jack-4.7d` frame-accurate synchronization is implemented locally
> from `jack-4.7c` commit `486c63a35c`. The focused tests and JACK-on/JACK-off
> Linux builds pass as recorded in section 5.3. Earlier PipeWire-JACK checks
> remain useful regression evidence, but neither they nor the automated tests
> qualify the corrected timing contract. Native JACK2 with Ardour remains the
> mandatory release gate described in section 5.2.
>
> **Goal in one sentence:** make current MuseScore Studio genuinely useful as a
> Linux JACK client, with stereo audio and bidirectional JACK transport control
> suitable for synchronizing playback with Ardour, at roughly the practical
> capability level of MuseScore 3 and the Lyrra MuseScore 4 work.
>
> **Scope lock:** this is a feature integration, not a redesign or formal
> qualification of MuseScore's audio infrastructure. The implementation should
> be as small as reasonably possible, use the current audio/playback APIs, and
> meet the real workflows in this document. It does not need to defend against
> every theoretical failure or prove every existing plugin path hard-real-time
> safe.

The code references below were rechecked on 2026-07-22 against branch
`jack-4.7` at the official `v4.7.4` tag, commit
`7688c005ad963ba09ee715aae53dbc7e8c9d5ef8`. Recheck the named files if
implementation begins after a substantial audio/playback refactor.

## 1. Scope authority

This document is the sole authoritative JACK implementation plan and complete
v1 contract.

A proposed task belongs in v1 only when at least one of these is true:

1. It is needed for a required behavior or acceptance case below.
2. The JACK changes introduce a realistic crash, use-after-free, deadlock,
   persistent corrupt setting, or data-loss path in the supported workflow.
3. The JACK-enabled Linux build otherwise cannot compile, start, switch
   backends, play audio, or synchronize transport.

Finding an unrelated pre-existing weakness is not enough. Record it in a
separate issue and continue; do not grow this plan without an approved product
decision. Broad RPC, Settings, export, graph, synthesizer, or
application-lifecycle work requires a concrete failing JACK test and the
smallest targeted correction; otherwise it is out of scope.

## 2. Product contract

### 2.1 Required user-visible behavior

The feature is complete when all of the following work on a normal Linux
desktop build:

1. **Optional build:** `MUSE_MODULE_AUDIO_JACK` remains off by default; setting
   it to `ON` discovers and links libjack. JACK-disabled and non-Linux builds
   compile without libjack or JACK UI, retain their existing backend choices,
   and do not regress because of this feature.
2. **Backend choice:** JACK appears alongside ALSA and available PipeWire in
   the existing Audio API selector. Compiling JACK must not force every backend
   request to create JACK, hide the other APIs, or make JACK the default.
3. **Stereo output:** selecting JACK opens one client named `MuseScore`
   (accepting JACK's unique-name suffix), publishes two float output ports
   named `audio_out_left` and `audio_out_right`, and produces MuseScore audio.
   Users connect those ports with Ardour, QjackCtl, Helvum, or another graph
   tool. MuseScore does not guess connections.
4. **Server-owned timing:** the active output sample rate and period come from
   JACK. The engine is initialized with the values actually returned by the
   server, not stale preference values. MuseScore presents those values as
   read-only while JACK is active; users change them in their JACK server.
5. **Local playback remains available:** when `Use JACK transport` is off,
   Play, Pause, Stop, seeking, note audition, loops, and count-in behave as they
   do with the other audio drivers. Choosing JACK must never turn MuseScore
   into an external-controller-only player.
6. **External control:** when transport synchronization is **Effective**, an
   Ardour/JACK locate, start, stop, and locate-while-rolling is reflected by
   MuseScore. MuseScore prepares/seeks before declaring itself ready to JACK and
   begins from the shared position.
7. **MuseScore control:** while synchronization is **Effective**, MuseScore's
   own Play, Pause, Pause-and-select, Stop, toolbar seek/rewind, user-initiated
   notation/Timeline/beat seeks, and play-from-selection actions request the
   corresponding change from JACK. Ardour and other clients observe it.
8. **No command echo:** applying a JACK-originated event locally must not send
   the same command back to JACK. Explicit user actions (including shortcuts,
   MIDI remote, and scripts that dispatch the public playback actions) count as
   MuseScore-originated requests.
9. **Practical backend switching:** switching ALSA/PipeWire to JACK and back in
   Preferences while playback is stopped works without restarting or crashing
   when both servers can own the output (for example JACK dummy or
   PipeWire-JACK). Native JACK may exclusively hold ALSA; in that configuration
   a clear failure followed by successful restoration of JACK is acceptable.
   The selector is disabled unless the global playback status is exactly
   `Stopped`; the driver controller retains a stopped-playback precondition and
   gains no playback dependency. A failed JACK open restores the
   previous/default working backend and does not save JACK as active.
10. **Ordinary failure safety:** a missing server, port/activation failure,
    server shutdown, xrun, and a usual JACK period change do not crash or leave
    callbacks using freed memory. Server loss makes synchronization ineffective,
    clears pending preparation/render eligibility, puts the local player in
    `Stopped` without commanding JACK, and reports the loss. Selecting a backend
    again or restarting MuseScore is
    allowed. Automatic reconnect is not required.

### 2.2 Transport semantics

JACK is the shared transport authority only while JACK is the active audio API
and transport synchronization is **Effective**. A requested-but-Pending setting
is not yet effective and keeps the ordinary local controls.

| Operation | Off | Pending | Effective |
| --- | --- | --- | --- |
| MuseScore Play | Start/resume locally | Start/resume locally | Locate JACK to MuseScore's cursor when needed, then start JACK |
| MuseScore Pause | Pause locally | Pause locally | Stop JACK and keep the shared position |
| MuseScore Stop | Use existing local semantics | Use existing local semantics | Stop JACK, locate it to frame 0, then put MuseScore at 0 |
| MuseScore user seek/rewind | Seek locally | Seek locally | Call `jack_transport_locate()`; reapply the position when JACK reports it |
| JACK/Ardour start | No effect | Before the fresh Stopped arm, do not join; after it, prepare this start to finish activation | Prepare/seek MuseScore, acknowledge JACK slow sync, then run |
| JACK/Ardour stop | No effect | Converge at the fresh Stopped frame and become Effective | Pause MuseScore at the observed position (do not rewind) |
| JACK/Ardour locate | No effect | A stopped locate can arm/converge; an already-rolling position is not joined, but the next Starting after arming can finish activation | Seek MuseScore; if rolling, prepare before JACK resumes |

Additional decisions:

- Register `jack_set_sync_callback()` before activation and gate it by the
  callback-armed state described in section 3.4. A new start or rolling
  relocation publishes one preparation request to the main thread; it returns
  not-ready until that request finishes. Repeated callbacks for the same start
  must not repeatedly seek or prepare.
- Do not change the server-wide sync timeout. If preparation fails or the
  server times it out, let the rest of the JACK graph continue, keep MuseScore
  silent/stopped for that start, and report the problem. Do not strand Ardour
  in `Starting` forever.
- The current score/player is the only target. Closing or changing the score
  invalidates a pending preparation. It must not stop the global JACK transport.
- Natural score end pauses/silences MuseScore and does not stop Ardour. Only an
  explicit MuseScore Pause or Stop command controls the shared transport.
- A MuseScore Pause-and-select request performs its ordinary selection side
  effect after the corresponding JACK Stopped observation is applied locally;
  that side effect must not emit another transport command. `PlaybackController`
  owns this one-shot pending flag. Clear it after use, on a superseding user
  command, request fallback/failure, sync disable, driver/server loss, or
  score/player replacement.
- Route outbound commands only at explicit user-command boundaries. The
  dispatcher Play, Play-from-selection, Pause, Pause-and-select, Stop, and
  Rewind/toolbar-seek actions are outbound, as are user-initiated notation,
  Timeline, and keyboard beat/selection seeks that already move playback.
  Incidental selection changes, MIDI note-entry positioning, score/part
  replacement, natural EOF, tempo refresh, and other internal calls remain
  local; changing the score cursor may update the desired start position, and
  the next explicit Play locates JACK there. Pass user/internal seek origin
  explicitly; do not infer it merely because an internal helper is named
  `pause()`, `stop()`, or `seek()`.
- While Effective, disable loop and count-in controls with a short explanation.
  On entry, turn off any active engine loop without erasing its saved
  boundaries; on exit, restore the UI state through the existing `updateLoop()`
  path. Suppress count-in for synchronized starts. Off and Pending leave both
  features unchanged.
- While Effective and JACK is Stopped, keep processing ordinary engine audio so
  note audition remains usable; the main-thread Stopped handler owns pausing and
  seeking score playback. Starting and unprepared Rolling remain silent.
- Use one source-time contract everywhere: audio written during JACK cycle
  `[F, F+nframes)` represents score time `[F, F+nframes)`. Player, source, UI,
  track/master FX metadata, and JACK positions are all anchored to the same
  logical frame `F`. Downstream graph playback latency must never become a
  future source seek or a hidden render lead.
- Preserve slow-sync ordering and silence. On `Starting(F)`, pause the local
  player, seek the logical player and every current source to `F`, call the
  zero-argument `prepareToPlay()`, and wait for asynchronous source readiness.
  Immediately before resolving readiness, re-seek/flush every current source
  to the clock's latest logical position. Then activate playback, wait for a
  new Running notification caused by that command, and only then acknowledge
  JACK ready. Starting remains silent; only the first eligible Rolling callback
  renders.
- The post-readiness re-anchor applies even to a same-position restart and is
  allowed to observe a newer locate. A stale generation/token remains unable
  to release JACK, while an older asynchronous completion re-anchors sources to
  the latest logical clock rather than restoring its obsolete frame. Adding or
  unmuting a source likewise seeks it to the current logical position.
- Playback-speed/tempo-multiplier changes are unsupported while Effective and
  are rejected with a short explanation. Retiming JACK or exchanging BBT is not
  v1. Incidental internal tempo refresh remains local and must not emit a JACK
  command. Activation does not silently rewrite an existing multiplier; timing
  qualification uses 100%, and musical alignment to Ardour at other multipliers
  is unsupported.
- Every false-to-true request, and every newly installed JACK driver with a
  persisted true request, starts **Pending**. Do not decide from a cached or
   default transport state. A fresh callback-domain observation of Stopped
  atomically arms synchronization; main-thread handling enqueues local pause
  then seek in that order and updates the desired position before publishing
  Effective, unless an immediately following `Starting` preparation owns
  convergence. The stopped path requires ordered enqueueing, not a new RPC
  acknowledgement; a following Starting path uses `prepareToPlay()` as its
  fence. A first fresh observation of Starting/Rolling stays Pending, so
  MuseScore never joins mid-episode. Disabling cancels Pending and must not stop
  Ardour.

### 2.3 Frame-accuracy evidence and target

The following 48 kHz measurements motivated `jack-4.7d` and are durable
negative controls. They are strongly proportional to the JACK period:

| Period | Direct MuseScore-to-track residual | MuseScore-to-input-bus-to-track residual |
| --- | --- | --- |
| 2,048 frames (42.67 ms) | approximately 43 ms | approximately 86 ms |
| 4,096 frames (85.33 ms) | approximately 88 ms | approximately 176 ms |

Historical behavior must remain distinguishable:

| Version | Relevant design | Required lesson |
| --- | --- | --- |
| `jack-4.7b` / `3bfd371511` | Output-only ports, no render lead, one preparation before the engine free-runs | Merely deleting 4.7c's lead without repairing source preparation is an unacceptable regression. |
| `c9aa11920d` | Terminal-source metadata and frame-discontinuity diagnostics, still no lead | Useful diagnostic baseline; its native timing was never qualified. |
| `jack-4.7c` / `486c63a35c` | Downstream `JackPlaybackLatency` becomes a future source seek | Route-dependent compensation corrupts the first note and leaves period-scaled residuals. |
| `jack-4.7d` | Source anchored to JACK frame `F`, exact block progression, route latency owned by JACK/Ardour | Must eliminate both prior failures. |

Use one deterministic fixture throughout native qualification: 48 kHz, 100%
playback speed, fixed 120 BPM, dry FluidSynth/SoundFont output with reverb and
effects disabled, repeated sharp clicks, and repeated identical sustained
notes. Export the same score to WAV and place it exactly on Ardour's grid. Keep
a transport-aware recorder connected directly to MuseScore's JACK outputs and
already rolling before the transport start, and separately record Ardour's
direct-track and input-bus-to-track routes. Record the JACK period,
periods-per-buffer, capture/playback latency ranges, first Rolling frame, first
captured transient frame, Ardour alignment choice, and offset from the WAV.

This separates the boundary under test. If the direct JACK-port capture is
late, MuseScore's preparation/render cursor is wrong. If direct output is
correct but Ardour placement is late, inspect JACK/Ardour latency metadata and
alignment without moving score time. If both contribute, fix the MuseScore
boundary first. Do not add source lead, a guessed period count, or an
Ardour-specific cursor offset.

At each valid start or locate, direct-port emission and Ardour-compensated
placement must be within 1 ms of the reference WAV. Native JACK2 qualification
must cover direct and bus routes at periods 256, 1,024, 2,048, and a confirmed
4,096 frames. There must be no residual proportional to one or more periods.
The first sustained note must match a later identical note in pitch, attack,
and duration; an already-running recorder or pre-roll is required when judging
its attack. An immediate Ardour recording may miss the first attack, matching
the accepted Hydrogen/MuseScore 3 startup behavior, but all subsequent material
must be aligned.

In a ten-minute 48 kHz run, the early-to-final offset change must remain within
1 ms. Carry the division remainder when consecutive audio blocks advance the
Mixer clock and dry event sequencer through existing microsecond APIs; reset it
on absolute seek, source replacement, output-spec/sample-rate change, and
sequence reset. Cumulative numeric conversion error must remain below one
microsecond. This is not a new sample-domain renderer or a promise of
sample-exact behavior through every plugin.

Keep output ports marked `JackPortIsOutput | JackPortIsTerminal`. MuseScore
advertises no invented source-side latency: a driver rendering the requested
block in the current callback has zero driver-internal capture latency. Never
query downstream `JackPlaybackLatency` to move score content. Test Ardour
`Automatic` alignment first, then explicit **Align with Capture Time**. If
direct output is frame-correct and only explicit capture-time alignment passes,
record Automatic as an accepted Ardour limitation rather than changing correct
terminal metadata or shifting the score cursor.

### 2.4 Explicit non-goals

- JACK MIDI, MIDI routing, audio input, or more than two output channels.
- Windows, macOS, FreeBSD, mobile, or Web JACK support. Code should not break
  those builds, but only Linux JACK is implemented and qualified.
- JACK timebase-master support, BBT/tempo-map publication, SMPTE, JACK session
  management, or JACK-controlled loops.
- Starting/configuring a JACK server, choosing among named servers in the UI,
  automatic port connection, saved graph routing, or automatic reconnect.
- Freewheel/offline export integration.
- Seamless backend migration or guaranteed preservation of an in-flight note.
- A new renderer thread, period ring, generalized transport framework, RPC
  protocol redesign, Settings transaction system, export ownership protocol,
  graph mutation protocol, or application startup/reset redesign.
- Proving all existing engine, synthesizer, sampler, and plugin code
  allocation-free or wait-free. New JACK callbacks still follow the bounded
  rules in section 3.3.
- Exhaustive fault injection, formal concurrency proofs, or release
  qualification beyond the focused tests in section 5.

## 3. Selected implementation design

### 3.1 Keep and repair the existing extension points

The checked-in JACK directory is a stale scaffold, not working current code:

- `jackaudiodriver.h` overrides driver methods that no longer exist and misses
  the current `defaultDevice()` contract.
- `jackaudiodriver.cpp` uses process-global state, removed `Spec.userdata` and
  `Spec.format` fields, and the old callback signature. Its close path frees
  memory without deactivating and closing the client.
- `AudioDriverController::createDriver()` returns JACK for every requested API
  whenever JACK is compiled, and `availableAudioApiList()` returns only JACK.

Rewrite that small driver against today's `IAudioDriver`; do not preserve its
globals or obsolete compatibility methods. Retain these useful current paths:

- `IAudioDriver::Spec::callback` is the audio pull boundary.
- `AudioDriverController` owns API selection, active-spec publication, and
  fallback.
- `StartAudioController` already adapts a driver callback to the engine.
- The normal `APP` configuration uses `WorkerRpcMode`: RPC has its worker while
  `EngineController::process(nframes)` runs from the driver callback. Keep that
  direct path for JACK. Do not switch JACK to the developer `WorkerMode`, whose
  pre-render buffer adds stale seek/start latency, and do not redesign that
  buffer as part of v1.
- `PlaybackController` already owns user Play/Pause/Stop/seek behavior and the
  current `IPlayer`.
- Preferences already exposes the Audio API selector.

### 3.2 Minimal component/API additions

Use three focused pieces. Names may follow repository conventions, but the
separation and behavior should remain:

1. **`JackAudioDriver`** implements current `IAudioDriver` plus a small internal
   optional transport-capability interface. It owns the `jack_client_t`, two
   ports, interleaved scratch buffer, callback state, one priority pending
   preparation slot, and one coalesced ordinary-observation slot. No
   process-global JACK state and no generalized event queue.
2. **`AudioDriverController` transport surface** exposes backend-neutral,
   optional operations through `IAudioDriverController`: availability,
   `setTransportSyncEnabled(bool)`, requested/effective sync state plus a
   state-change notification,
   `requestPlay(position)`, `requestPause()`, `requestStop()`,
   `requestSeek(position)`, a typed main-thread transport-event channel
   (including an unavailable/fault event), and
   `completePreparation(driverGeneration, token, success)`, plus one
   `cancelPendingTransportWork()` operation used when the current score/player
   is replaced. Non-JACK drivers return unsupported/no-op. Each request returns
   `bool handled`: `false` for a non-JACK driver, Off, Pending, or an unavailable
   bridge, so the caller runs its existing local action; `true` whenever the
   Effective bridge owns the request. A failed JACK call still returns `true`,
   reports the failure, and preserves the current authoritative JACK
   state/position rather than falling back to an independent local timeline. In
   particular, a rejected locate for Play must not start locally. The stub
   returns `false` for requests and otherwise no-ops. The controller observes
   the configuration setting, forwards it whenever the active driver changes,
   and atomically gates the already-registered JACK callbacks; Preferences does
   not reach into the driver directly.
3. **`PlaybackController` integration** subscribes to the typed transport
   events and has private apply methods for JACK-originated prepare, rolling,
   stopped, and locate events. Public user actions ask the transport controller
   first; when it does not handle the action, the existing local path runs.
   Private apply methods call `IPlayer` directly and never re-enter the public
   outbound path. Split the registered user action adapters from local-only
   play/pause/stop/seek/resume helpers: reset, EOF, score/part changes, tempo
   refresh, JACK-originated events, and teardown use only the local helpers.
   Maintain a synchronous desired playback position when issuing a seek; do not
   derive a new JACK Play request from the asynchronously reported player
   position. Reset that desired position and pending-seek identity whenever the
   current score or player changes, and call
   `cancelPendingTransportWork()` at that boundary.

Do not send JACK observations through the public `play`, `pause`, `stop`, or
`rewind` action codes: those codes also represent real user/MIDI/script intent
and lose event origin. The existing `TransportEventsController` may remain for
its current users; JACK can use the new typed path without redesigning its RPC.

Suggested focused file map:

| Area | Expected files |
| --- | --- |
| Build | `src/framework/cmake/MuseDeclareOptions.cmake`, `SetupConfigure.cmake`, `buildscripts/cmake/FindJack.cmake`, `src/framework/audio/driver/CMakeLists.txt` |
| Driver | `src/framework/audio/driver/platform/jack/jackaudiodriver.{h,cpp}` and one small internal transport-capability header if useful; remove the stale JACK device-listener dependency |
| Driver selection/bridge | `src/framework/audio/iaudiodrivercontroller.h`, `src/framework/audio/main/internal/audiodrivercontroller.{h,cpp}`, `src/framework/stubs/audio/audiodrivercontrollerstub.{h,cpp}`, `src/framework/audio/main/audiomodule.cpp` ticker/init wiring |
| Transport types/settings | `src/framework/audio/common/audiotypes.h`, `src/framework/audio/main/iaudioconfiguration.h`, `src/framework/audio/main/internal/audioconfiguration.{h,cpp}`, `src/framework/stubs/audio/audioconfigurationstub.{h,cpp}` |
| Playback commands | `src/playback/internal/playbackcontroller.{h,cpp}`, `playbackuiactions.{h,cpp}` and `src/playback/qml/MuseScore/Playback/playbacktoolbarmodel.{h,cpp}` for action/control enablement; `src/framework/audio/main/internal/player.cpp` only for the ordered-seek correction below |
| Preference UI | `src/preferences/qml/MuseScore/Preferences/audiomidipreferencesmodel.{h,cpp}`, `commonaudioapiconfigurationmodel.{h,cpp}`, `internal/AudioApiSection.qml`, and `internal/CommonAudioApiConfiguration.qml` |
| Focused tests | `src/framework/audio/tests/` plus narrow playback or Preferences model/helper tests only where needed for the typed bridge and API reconciliation |

Avoid touching files outside this map unless a compile error or named
acceptance test demonstrates why. Document that reason in the change.

### 3.3 Driver lifecycle and audio callback

`open()` should perform one linear, reversible sequence:

1. Reject an invalid spec or a non-stereo output request.
2. Open `MuseScore` with `JackNoStartServer`; a server must already be running.
3. Read `jack_get_sample_rate()` and `jack_get_buffer_size()` and construct the
   returned active spec from those values.
4. Allocate an interleaved stereo float scratch buffer before activation. Give
   it capacity for at least `max(currentPeriod, MAXIMUM_BUFFER_SIZE)` frames so
   common runtime period changes do not allocate in a callback.
5. Register both output ports as terminal synthesized sources with
   `JackPortIsOutput | JackPortIsTerminal`, then register the process, shutdown,
   xrun, buffer-size, sample-rate, and sync callbacks. Check every
   port/callback registration API that returns status and unwind on failure;
   `jack_on_shutdown()` itself returns `void`. Do not install a playback-latency
   callback or cache downstream playback ranges: graph latency is not a source
   render offset.
6. Before activation, install the controller-assigned driver generation and
   requested-sync state and initialize all callback-visible flags/slots. This
   prevents an activation callback from observing a half-configured bridge.
7. Activate only after the object is fully initialized. On success, store the
   server-derived active spec, write it through `activeSpec` when that pointer is
   non-null, and publish it exactly once through `activeSpecChanged()` (or one
   controller-equivalent publication) on the controlling thread so runtime
   switches update the engine and UI. Then return `true`. On any failure, unwind
   the partial client and return `false` without publishing a spec.

`defaultDevice()` and `availableOutputDevices()` expose the single existing
`DEFAULT_DEVICE_ID` pseudo-device. The available rate and period lists expose
the server's current values; the JACK driver does not pretend that MuseScore's
generic rate/period setters can configure the server.

`JackPortIsTerminal` correctly identifies MuseScore's ports as graph source
endpoints. It does not itself move samples, claim MuseScore-internal latency, or
replace the connected graph's playback-latency ranges.

The process callback uses the current `nframes`, not the preferred buffer size:

1. Obtain both JACK port buffers and zero them first.
2. If closing, server shutdown is flagged, the period is oversized, JACK is
   `Starting`, or the callback-domain sync bridge is armed and Rolling without
   render eligibility for that episode, return
   silence. Effective/Stopped still calls the renderer so note audition works;
   its typed main-thread event pauses/seeks score playback.
3. Otherwise call the existing `Spec::callback` once into the preallocated
   interleaved buffer with a byte count of
   `nframes * 2 * sizeof(float)`, then copy left/right samples to JACK's planar
   ports.
   The first eligible Rolling period renders in that same process cycle; it is
   not queued behind an additional driver period.
4. Do not allocate, take a blocking lock, log, notify Qt, destroy resources, or
   call JACK graph/transport mutators from new callback code. Bounded publication
   through explicitly lock-free scalar atomics is allowed. The RT-safe reads
   `jack_port_get_buffer()` and `jack_transport_query()` are the intentional
   libjack calls; start/stop/locate remain main-thread calls.

MuseScore publishes no invented source latency. The output ports' terminal
metadata lets JACK and Ardour calculate their own graph boundary; no latency
range is consulted to advance the score. Any future production port-flag
change requires native evidence that it fixes Ardour Automatic alignment
without breaking direct or bus timing and a corresponding product decision in
this plan.

Calling the existing render callback from JACK's process callback is an
intentional v1 design choice and matches MuseScore's driver pull architecture.
The project does not need a producer thread or period ring merely to add JACK.
Existing engine/plugin behavior inside that callback is not re-qualified here.

`close()` is idempotent. Mark closing, deactivate the client and wait for that
call to return so process/sync callbacks are quiescent, close the client, then
release ports/buffers/member state. The JACK shutdown callback only marks
server shutdown in bounded callback-safe state; the main poll/controller
reports it and performs ordinary cleanup. Never free callback state or remove
the driver from a JACK callback.

Period changes up to the preallocated capacity update the effective period and
are published to the controller on its next poll. If a period grows beyond
capacity, output silence and report that restarting MuseScore is required.
Sample-rate changes stop/silence JACK output and request restart rather than
attempting a live engine-wide retime. Xruns increment a counter; if an xrun
causes a rendered-frame discontinuity, the continuity gate keeps later Rolling
cycles silent until a fresh stop/start or locate is prepared. Restarting
transport is an accepted recovery action.

### 3.4 Transport bridge and thread boundary

Use a tiny tokenized state exchange:

- The controller gives each installed driver a monotonically increasing
  generation. A preparation identity is `{driverGeneration, token}`, where
  the driver generates a nonzero token on each transition into a new JACK
  `Starting` episode. The frame is not the identity: stop/start or relocate to
  the same frame still creates a new token, while repeated sync callbacks in
  one episode reuse it. A completion must match both fields; replacing the
  driver invalidates the old generation before destruction.
- The JACK callback domain publishes only fixed-size scalar data. Use a
  priority pending-preparation slot that ordinary observations cannot
  overwrite, plus a separate latest-wins observation slot for
  state/stopped-frame changes. JACK's process and sync callbacks share the
  process callback domain and are the only writers to those slots. A newer
  Prepare may supersede the priority slot.
  Shutdown, sample-rate, buffer-size, and xrun callbacks use independent sticky
  atomic flags/value/counters, so their separate callback threads never become
  additional writers to the transport slot. Publish coherent fields with a
  version or equivalent release/acquire scheme made only from atomics that are
  explicitly lock-free on the supported target; do not use a large
  `std::atomic<struct>` that may hide a lock. Snapshot reads make one bounded
  attempt and defer to the next main poll if the version changes; no callback
  spins or retries. A newer Starting episode cancels and supersedes an older
  preparation. No general-purpose queue is needed.
- While synchronization is requested (including Pending) or Effective, the
  process callback calls
  `jack_transport_query()` once per cycle and compares the state/frame with its
  last observation. Publish only a state transition or a changed frame while
  stopped. Each eligible Rolling render compares the current frame with the
  expected next frame. A mismatch publishes the exact expected and observed
  frames, revokes render eligibility without clearing that diagnostic, and
  outputs silence for the current and later Rolling cycles until a fresh
  stop/start or locate completes preparation. It never rebases and continues,
  generates a locate, seeks sources from the callback, renders catch-up blocks,
  or commands JACK. Ordinary contiguous Rolling advance is not an event.
- Give every enable request and newly installed JACK driver a new request
  epoch, and reset the callback-domain comparison baseline for that epoch. The
  first query/sync observation in it is therefore fresh even when JACK's state
  and frame equal values seen while Off or by the preceding driver. While Off,
  the sync callback returns ready and the process callback follows the ordinary
  local-audio path; neither publishes transport events nor updates the next
  epoch's observation baseline. While unavailable or closing, the sync callback
  returns ready and the process callback emits silence.
- If synchronization is requested but Pending, the first fresh callback-domain
  observation of Stopped atomically arms slow sync. This lets an immediately
  following start enter preparation even if the main ticker has not drained the
  stopped observation. The UI/controller remains Pending until main-thread
  convergence: either it enqueues pause then seek to that stopped frame in order,
  or the immediately following authoritative Prepare does so. Ordered enqueue is
  sufficient on the stopped path; only Starting preparation needs the async
  fence. Then publish Effective as specified below.
- The existing audio-module main ticker asks the active driver/controller to
  drain pending preparation before the coalesced observation. Main-thread code
  converts the frame to seconds and sends a typed event to
  `PlaybackController`.
- Main-thread preparation completion writes
  `{driverGeneration, token, success}` back through the controller. A stale
  generation or token is ignored, preventing completion for a closed score,
  old locate, or replaced driver from releasing a newer start.
- Transition and locate events are de-duplicated. Normal Rolling frame advance
  must not cause a main-thread seek every period.

Register the sync callback before activation. While Off, unavailable, or
closing, it returns ready immediately without publishing or retaining a
transport observation. Otherwise it records the fresh state, arms a Pending
request on Stopped, and publishes JACK's stopped/new-position callback as an
authoritative locate event distinct from an ordinary process-query observation.
It returns ready immediately when the bridge is not callback-armed or JACK is
not Starting. All
`jack_transport_start()`, `jack_transport_stop()`, and
`jack_transport_locate()` calls originate on the main thread. Check the result
from `jack_transport_locate()`; libjack's start/stop calls return `void`, so the
later queried transport state is their acknowledgement. If a required locate
is rejected, report it and do not apply the requested seek locally. A failed
Play locate does not start; a failed seek while JACK is already Rolling keeps
following the old Rolling position. Neither case silently starts an independent
local timeline or stops the shared transport merely because the locate failed.

For a JACK/Ardour start or rolling locate:

1. On entry into `JackTransportStarting`, the sync callback publishes
   `Prepare(generation, token, frame)`, then returns `0`. Repeated calls in the
   same Starting episode return the current readiness without republishing.
2. `PlaybackController` pauses if needed, logically seeks the current player to
   `F`, calls `prepareToPlay()`, and rechecks the active driver generation,
   token, score, and player before and after every asynchronous continuation.
   The seek is always enqueued before preparation: remove
   `Player::seek()`'s early comparison with its asynchronously reported
   `m_playbackPosition` and let the engine's seek path handle a same-position
   no-op. This also preserves rapid A -> B -> A seek ordering.
   After source readiness resolves, and before the preparation promise resolves,
   re-seek/flush every current source to the clock's latest logical position.
   This final anchor is required for immediate and asynchronous readiness,
   same-position restarts, and a newer locate arriving while an older
   preparation is pending.
   If this is the Pending activation race, successful preparation completes
   local convergence; publish Effective on the main thread now, before issuing
   start/resume. Until that publication, user commands continue to use their
   local paths.
3. On success it installs a one-shot player-status subscription before issuing
   start/resume and waits for a **new** `Running` notification caused after that
   command; reading a cached Running state is not an acknowledgement. Recheck
   driver/token/score/player again before completing.
   While JACK remains `Starting`, the driver process callback does not render,
   so the player clock cannot consume inaudible early periods.
4. The sync callback, not the main-thread completion, marks the current token as
   released when it actually returns `1` for that token while still Starting.
   Only a released **successful** token gains render eligibility. When JACK
   becomes Rolling, the first rendered period starts from source position `F`;
   JACK observation and rendered-frame continuity use that same frame.
5. On failure/cancellation, acknowledge failure so the sync callback returns
   ready without playing MuseScore; report the error on the main thread.

If JACK reaches Rolling because its server-side slow-sync timeout expired
before step 3, expire that token and keep/pause MuseScore locally. A later async
completion for it must not start playback in the middle of the run. Any later
fresh Starting episode—including a rolling relocate or a stop/start—creates a
new token and can recover.

Once the callback-domain bridge is armed, the process callback may render
Rolling audio only when that exact transport episode has a successfully
completed-and-released preparation identity. Seeing Rolling without the
sync-callback release marker is a server timeout even if main-thread completion
arrives just afterward; expire it rather than joining mid-run. Starting is
silent. A timeout, failed/cancelled preparation, Stop, relocation, score
replacement, sync enable/disable, driver replacement, or shutdown clears the
episode's render eligibility. An
unprepared Rolling observation remains silent until a fresh Starting episode;
disabling synchronization removes this special gate and restores the ordinary
local playback path.

For a stopped locate, publish a typed Stopped/Locate event and seek/pause the
player directly. For a transition to Rolling that lacks a successfully prepared
token (for example attaching to an already-rolling server), do not pretend it is
synchronized; wait for the next Starting episode and give a concise diagnostic.

Transport requests are serialized by the main thread. If the user or another
client changes direction quickly, the newest observed JACK state/frame wins;
invalidate the older preparation token and converge once, subject only to the
one-target stale-observation guard below. Do not add retries, locks, or a
general command-arbitration framework.

`PlaybackController` keeps a synchronous desired score position, initialized
from the current player and updated immediately by accepted local/JACK seeks and
by ordinary position notifications when no newer seek is pending. An explicit
Play uses it rather than waiting for a future `playbackPositionChanged()`. The
transport controller converts requests and keeps the pending local locate in
canonical JACK frames. When issuing one, discard or version-suppress already
queued ordinary stopped-query observations and mark its target pending; ignore
only later **ordinary query** observations until that target is seen. The sync
callback's stopped/new-position event and any Starting Prepare are
authoritative: either may acknowledge the target or replace it with a different,
later external locate. This prevents the sequence "MuseScore locates A, Ardour
supersedes with B" from remaining stuck waiting for A despite JACK's delayed
locate application. Clear the pending target on locate rejection,
score/player/driver change, sync disable, or shutdown. This is a one-target
stale-observation guard, not a command-arbitration protocol.

`cancelPendingTransportWork()` clears the controller's pending local JACK-frame
target and invalidates any preparation/render identity for the current score or
player. It does not stop, seek, or otherwise control the global JACK transport.

Frame conversion is centralized in the controller using the active JACK rate:

```text
frame   = round(max(0, seconds) * sampleRate)
seconds = frame / sampleRate
```

The controller clamps only to nonnegative values representable by JACK's frame
type. `PlaybackController`, which knows the score, clamps user/external targets
to `[0, totalPlayTime]` before applying them. Do not use tempo/BBT for v1.

### 3.5 Settings, Preferences, and backend switching

Add one normal shared setting, default `false`, exposed as `Use JACK transport`.
Show/enable it only when JACK support is built and JACK is the active API. This
is an ordinary boolean preference; it does not require transaction, reset, IPC,
or multi-process Settings changes.

While JACK is active, show its actual sample rate and period but disable the
generic controls that would try to change them. A short hint should direct the
user to the JACK server configuration. The model/controller also rejects a
programmatic rate or period edit while JACK is active; a one-item combo box is
not the enforcement boundary. Publish the server values to the active engine
spec/UI only; do not overwrite the saved generic ALSA/PipeWire rate or period.

Treat the persisted checkbox as requested state and expose Off/Pending/Effective
state (or equivalent requested/effective accessors), an Unavailable fault
presentation for a dead active JACK client, and one change notification from the
controller and stub. The configuration setting has its normal changed
notification. With JACK active, Off means the request is false, Pending means a
live client is waiting for activation convergence, Effective means the bridge
owns shared transport, and Unavailable means the selected JACK client has died;
the requested boolean retains its prior value. Another active backend has no
effective bridge but retains the saved request for the next JACK installation.
A false-to-true change enters Pending and waits for the fresh callback-domain
observation/convergence in section 3.4, even if a cached value says Stopped.
Install the same Pending state before activating a replacement or startup JACK
driver when the setting is already true. While Pending, the bridge normally
does not follow or control JACK and existing local playback remains available.
Its sole exception is an authoritative Prepare after the fresh Stopped
observation has armed callbacks; that Prepare may complete the rapid stop/start
convergence described above. User commands remain local until Effective is
published.

Disabling—including a normal Preferences reset—cancels Pending/Effective,
invalidates tokens, restores local playback behavior, and stops following
future JACK events; it must not stop the shared transport merely because the
setting changed.

Repair API selection as part of the same focused change:

- Keep preferred configuration distinct from live identity:
  `IAudioConfiguration::currentAudioApi()` is the staged/saved preference, while
  `IAudioDriverController::currentAudioApi()` reports the actually installed
  driver's canonical `name()` once a driver exists. UI indexes and failure
  messages use the latter.
- On Linux, `createDriver(name)` creates exactly that canonical compiled backend
  or returns null/failure; it never silently returns ALSA for an unknown,
  unavailable, or failed PipeWire/JACK request. `JackAudioDriver::name()` is
  exactly `"JACK"`. Fallback is owned only by the open/switch transaction.
  Non-Linux implementations only need to compile with the shared interface and
  retain their existing factory behavior.
- `availableAudioApiList()` includes all compiled Linux APIs.
- Change `IAudioDriverController::changeCurrentAudioApi()` to return success
  (a `bool` is sufficient) and update its few callers/stub. Before closing,
  capture the current canonical API and active spec. Create/init/open the exact
  requested driver, and commit/publish its API only on success. On failure,
  close the partial driver and recreate/reopen the previous API with its
  captured spec, trying the platform default only if restoration fails. Return
  `false`
  for the rejected request while leaving `currentAudioApi()` on whichever
  backend was restored. Every successful requested/restored open publishes its
  returned active spec and calls the existing engine output-spec update; do not
  leave the old backend's rate/period active. The Preferences model derives and
  emits its index from that result rather than optimistically publishing the
  requested index, and uses its existing `IInteractive` dependency for a
  concise failure message. Publish the resulting API synchronously before the
  call returns. Startup fallback may log the error, but after a successful
  fallback must publish and persist that opened backend; a failed switch must
  never persist the rejected choice.
- Preserve the current live-preview behavior inside Preferences' existing
  Settings transaction. A selector change calls the controller first and writes
  the staged preferred API only after a successful switch; Apply commits that
  already-matching value. `AudioMidiPreferencesModel` also observes
  `IAudioConfiguration::currentAudioApiChanged()`: when Cancel rollback or
  factory Reset changes the staged/preferred API away from the live identity,
  it invokes the same switch transaction under a self-notification guard, but
  only at exact `Stopped` status. On reconciliation failure, keep/persist the
  backend actually restored and report it. If Reset/rollback arrives while
  Running or Paused, do not switch under playback; retain/report the live
  identity and tell the user the preferred API will apply after restart. This is
  a focused Preferences-model policy, not a Settings transaction redesign.
- Preserve `MUSESCORE_FORCE_ALSA` as an explicit override: expose only ALSA
  and canonicalize selection requests to ALSA before calling the exact factory,
  so `currentAudioApi()` and persisted state report ALSA.
- Before destroying/replacing JACK, invalidate transport tokens and close it
  using the lifecycle above. In a JACK-enabled Linux build, the Preferences
  model uses `IGlobalContext::playbackState()`, subscribes to exact
  `PlaybackStatus`, and enables the selector only when it is `Stopped` (Paused
  is still disabled). Keep this conditional so JACK-off/non-Linux Preferences
  builds do not gain a new dependency or behavior. The controller documents
  Stopped as a precondition rather than gaining an audio-to-playback dependency;
  any future non-UI caller must satisfy it.
- Leaving Effective for explicit disable, driver replacement, or server loss
  synchronously publishes `effective=false`, invalidates preparations/render
  eligibility, restores loop/count-in/speed UI and engine behavior, and makes
  later transport requests use the local path. Driver replacement and loss do
  not change the persisted requested boolean; a replacement JACK driver with a
  true request starts Pending.
- On JACK server loss, additionally stop the local player without sending a
  transport command, publish Unavailable with the fault, and notify the UI. Do
  not issue transport commands from the shutdown callback. The selected API may
  still read JACK while its client is unavailable; the saved/published identity
  invariant applies after successful startup/switch, not after an asynchronous
  server failure.
- Use the existing default pseudo-device contract for JACK. Do not add a
  per-backend Settings system merely to remember devices.

## 4. Implementation phases

Each phase is a usable vertical slice. Do not begin adjacent hardening work
because a later phase mentions testing.

### Phase 1 — Buildable, selectable JACK audio

- Restrict the feature to Linux (`OS_IS_LIN`, not MinGW/FreeBSD) and make JACK
  discovery clear when explicitly enabled. Normalize the option to OFF before
  `MuseSetupConfiguration.cmake` generates `muse_framework_config.h`; do not
  leave the JACK macro defined where no JACK source is built.
- Rewrite `JackAudioDriver` against current `IAudioDriver` with member-owned
  lifecycle and correct cleanup; remove its obsolete device-listener use.
- Fix exact Linux API enumeration/creation, canonical identity, and failed-open
  fallback.
- Use server rate/period, reject edits while JACK is active, and produce stereo
  audio with transport sync off.
- Add the default-off setting plumbing if useful, but keep the checkbox hidden
  until Phase 2 makes it functional; do not expose an inert control.
- Enable the API selector only at exact `Stopped` status and report a failed
  switch while restoring the actual active index.

**Exit:** JACK-on and JACK-off builds compile; JACK, ALSA, and available
PipeWire can be selected; audio and note audition work; switching both ways
works when the servers coexist; an exclusive-device reopen failure and a
missing JACK server are clear and safe.

### Phase 2 — External JACK transport follows correctly

- Add the small optional transport capability, main-thread polling, typed
  event channel, separate preparation/observation slots, driver-generation
  token cancellation, and slow-sync callback.
- Apply stopped locate, start, stop, and rolling relocate to the current player.
- Gate rendering by successful preparation and handle timeout, score
  close/change, backend loss, and preparation failure without holding JACK.
- Expose the checkbox and implement Off/Pending/Effective activation from a
  fresh observation on every enable/new JACK driver.

**Exit:** Ardour can locate/start/stop/relocate MuseScore repeatedly and both
applications agree on position without a public-action echo.

### Phase 3 — MuseScore controls the shared transport

- Route explicit Play/Pause/Pause-and-select/Stop/toolbar-seek,
  notation/Timeline/keyboard seek, and play-from-selection through the
  transport controller when it handles them.
- Split those registered user adapters from local-only helpers used by EOF,
  reset, score/part changes, tempo maintenance, and external observations.
- Maintain the desired cursor synchronously and preserve ordered seeks,
  including rapid A -> B -> A. Preserve the old local paths when
  synchronization is off.
- Apply user Stop versus external Stop semantics correctly; disable/reset and
  later restore loop state, suppress count-in, and reject playback-speed changes
  while Effective.

**Exit:** starting and navigating from either MuseScore or Ardour works, so the
feature is not follower-only.

### Phase 4 — Focused reliability and handoff

- Exercise server shutdown, xruns (including no false relocation), common
  period changes, backend switching, already-rolling activation, and one longer
  drift case.
- Make only corrections required by those tests; document accepted limits.
- Add a short Linux/JACK usage note: start server, select JACK, connect ports,
  enable transport, and recover from server loss.

**Exit:** every required acceptance case below passes on JACK2. Record a basic
PipeWire-JACK smoke pass when that compatibility layer is available; its absence
does not block completion.

## 5. Verification and definition of done

### 5.1 Focused automated checks and code audit

Do not build a fake universe around libjack. Add small tests where they buy
clear confidence. Put the callback-domain state/token/seek arbitration in a
small Qt-free helper testable from the existing `muse_audio_tests` target.
Verify the PlaybackController routing boundary and API/Preferences
reconciliation through direct adapter review plus the manual matrix. Do not add
a production abstraction solely to create a unit-test seam or instantiate the
full production Preferences page catalog merely for this feature.

- Transport state/token logic: a Prepare cannot be overwritten by a same-cycle
  Starting observation; one prepare per episode; two same-frame Starting
  episodes get distinct tokens; stale token and old-driver generation
  completions are ignored; stopped locate and Rolling transition are delivered;
  ordinary Rolling/xrun frame gaps produce no locate or per-period seek spam.
- Render eligibility: Starting is silent; timeout/failure and an unprepared
  Rolling episode remain silent; a matching successful preparation enables
  Rolling rendering only after the sync callback records that it returned ready;
  a late main-thread success after Rolling cannot enable it. Stopped continues
  engine rendering for audition; stop/relocate/replacement clears Rolling
  eligibility; a fresh rolling relocate can recover after timeout.
- Rendered-frame continuity: Starting is silent; the first Rolling callback
  must equal the prepared frame and each later callback must equal the previous
  frame plus its actual `nframes`. A first-block or later mismatch reports and
  preserves the exact expected/observed pair, revokes render eligibility, and
  keeps current and later Rolling callbacks silent. A fresh prepared
  stop/start or locate restores eligibility; invalidation, sync disable, driver
  replacement, and server loss clear the old episode.
- User/external routing: a user Play becomes one JACK play request; an external
  Play applies locally without an outbound echo; sync-off uses the original
  local methods; a handled JACK-call failure does not start locally; a rejected
  rolling seek retains the old shared position. Reset, natural EOF, part/score
  changes, and tempo maintenance never emit a JACK command. Cover the
  Pause-and-select one-shot application and cancellation in the adapter/action
  audit.
- Desired-position ordering: play-from-selection uses its newly requested
  position, rapid A -> B -> A seeks enqueue and apply the final A, and the
  sequence "local locate A, then authoritative external locate B before A is
  observed" converges to B rather than waiting forever for A.
- Async preparation: immediate and asynchronous readiness both perform a final
  source re-anchor to the latest logical clock. Same-position restarts flush
  sources; a newer locate cannot be overwritten by an older completion;
  closing/changing the score or replacing the player/driver at each
  continuation makes completion stale; and only a new Running notification
  after this token's start/resume can acknowledge it.
- Driver selection: a requested backend is created exactly, failed JACK open
  preserves the actual fallback identity, and forced ALSA reports ALSA. Inside
  a Preferences transaction, Apply preserves the successful live choice,
  Cancel/Reset at Stopped reconciles the live driver, self-notification does not
  recurse, and Reset while Running/Paused does not switch under playback.
- Active spec: a successful JACK activation publishes the server-derived spec
  exactly once and drives the controller's engine/UI update; failed activation
  publishes none.
- Requested/effective setting: every enable/new JACK driver begins Pending;
  its request epoch forces a fresh observation, Off callbacks are discarded,
  fresh Stopped arms it, Starting/Rolling does not, stop/start faster than one
  main tick cannot bypass preparation, stopped pause/seek commands are ordered
  before Effective, and disabling/reset cancels Pending/Effective. Driver
  replacement and server loss publish ineffective and restore restrictions
  without changing the requested preference; server loss presents Unavailable.
- Seconds/frame conversion, including zero, a nonzero cursor, and clamping.
- Remainder-carrying advancement: ten simulated minutes at 48 kHz and periods
  256, 1,024, 2,048, and 4,096 retain less than one microsecond of cumulative
  conversion error for Mixer and dry event-sequencer progression. Absolute
  seek, source replacement, output-spec/sample-rate change, and sequence reset
  clear the remainder.
- Source-time separation has been removed: preparation publishes logical `F`
  only; player, source, UI, FX, and JACK positions contain no hidden lead; and
  newly added or unmuted tracks seek to the current logical position.
- Existing callback lifetime, stale-token, timeout, server-loss, and
  no-command-echo tests remain green. Existing audio tests plus compile smoke
  with JACK disabled; existing non-Linux CI (or an equivalent compile smoke)
  remains green after the shared interface/stub changes.

A tiny injectable libjack function table may be used for open/cleanup tests if
it remains smaller than the behavior being tested. It is not a requirement;
the required manual dummy/real-server pass covers the real ABI.

### 5.2 Required manual/integration matrix

Native JACK2 with Ardour at 48 kHz is the sole release gate. Run the deterministic
fixture from section 2.3 at periods 256, 1,024, 2,048, and a confirmed 4,096
frames through both MuseScore-to-recording-track and
MuseScore-to-input-bus-to-recording-track routes. Test Ardour Automatic
alignment first and explicit **Align with Capture Time** second. Runs with an
xrun, overload, or unexpected frame-discontinuity warning are invalid timing
measurements and must be repeated. PipeWire-JACK remains an optional smoke test.

| Case | Expected result |
| --- | --- |
| Build with JACK off | No libjack/JACK UI; normal API list and local playback work |
| Build with JACK on | JACK plus ALSA and compiled PipeWire are listed |
| First run/default setting, then restart after enabling | Transport defaults off; the requested setting persists |
| Server absent at startup with JACK saved | Clear fallback; opened backend is published/saved; no crash |
| Server absent, then JACK is selected at runtime | Clear switch failure; previous backend/index is restored |
| Select another API, then Apply / Cancel while Stopped | Apply keeps and persists it; Cancel restores the prior live/saved API |
| Factory Reset while Stopped / while Running | Stopped reconciles the default live API; Running does not hot-switch and reports restart-required preference |
| JACK rate/period UI and generic saved values | Server values are read-only; prior ALSA/PipeWire preferences are not overwritten |
| Sync off, control either MuseScore or Ardour | MuseScore controls stay local; Ardour transport does not move MuseScore |
| Sync off, edit/audition notes | Audible through JACK ports |
| Enable sync while JACK is stopped | Pending briefly; local pause then seek are enqueued before Effective |
| Effective sync, start from Ardour at 0 and nonzero frame | MuseScore starts at the same musical position |
| Effective sync, Ardour stop and stopped locate | MuseScore pauses and moves without rewinding unexpectedly |
| Effective sync, Ardour locate while rolling | JACK waits for preparation and MuseScore resumes at the new position |
| Effective sync, MuseScore Play at 0/nonzero cursor | Ardour starts at the same position |
| Effective sync, MuseScore Pause/Pause-and-select, Stop, toolbar/notation/Timeline/keyboard seek, play-from-selection | Ardour observes the specified shared-transport semantics while stopped and rolling |
| Effective loop/count-in/speed controls | Loop is locally disarmed/restored; count-in is suppressed; speed changes are rejected with a hint |
| Effective sync, JACK stopped, edit/audition notes | Audition remains audible without starting the shared transport |
| Score reaches natural end while Ardour rolls | MuseScore becomes silent; Ardour keeps rolling |
| Enable sync while JACK is already rolling | Pending hint appears; no late join; stopping once converges MuseScore and arms the next start |
| Repeat start/stop/seek 20 times | No duplicate commands, stuck Starting state, or crash |
| Switch selector while MuseScore is Running or Paused | Selector is disabled; active backend is unchanged |
| Switch ALSA/PipeWire -> JACK -> previous API where servers coexist | Audio returns; no crash; a dropout/stop is acceptable |
| Native JACK exclusively owns ALSA, then select ALSA | Clear reopen failure and successful JACK restoration are acceptable |
| Change JACK period 256 <-> 1024 | Continues or safely reconfigures without overflow/crash |
| Induce an xrun during Rolling | No synthetic locate or crash; if frame continuity is lost, later Rolling cycles stay silent and a fresh stop/start or locate restores synchronized rendering |
| Kill JACK server during playback | Unavailable is reported; MuseScore becomes locally Stopped, restores Effective-only restrictions, and remains safe; backend reselection is possible |
| Start at zero, a nonzero barline, mid-measure on a note; stopped locate; rolling locate; commands from Ardour and MuseScore; twenty repeated cycles | Each preparation anchors the first rendered block to the requested frame without a stale first note or duplicate command |
| Native timing, direct JACK-port capture, periods 256/1,024/2,048/4,096 | First emitted transient versus the grid-aligned reference WAV is within 1 ms with no period-scaled residual |
| Ardour Automatic, direct and bus routes, periods 256/1,024/2,048/4,096 | Compensated recorded placement versus the reference WAV is within 1 ms on both routes; record separately whether Automatic passes |
| Explicit Align with Capture Time, direct and bus routes, periods 256/1,024/2,048/4,096 | Supported fallback is within 1 ms on both routes when Automatic is not; do not shift MuseScore source time to compensate |
| First sustained note with pre-roll/already-running recorder | Pitch, attack, and duration match a later identical note; an immediate Ardour recording may omit only the initial attack |
| Ten-minute native direct and Ardour-routed runs | Initial absolute offset and early-to-final offset are recorded separately; the change is within 1 ms |
| PipeWire-JACK smoke, if installed | Open, stereo audio, start/stop/locate in both directions |

For every timing case, record the JACK and Ardour versions, rate, period,
periods-per-buffer, reported capture/playback ranges, routing graph, alignment
choice, first Rolling frame, first transient frame, offset against the WAV, and
the early/final offsets where applicable. Compare captured dry transients, not
only the UI cursor.

The frame-discontinuity warning identifies a run that would otherwise produce
period-sized lag. The render gate deliberately chooses silence: restart or
relocate transport to restore a prepared baseline rather than chasing from the
real-time callback.

Release requires both historical negative controls to be absent: no
`jack-4.7b`-style delay against WAV playback, and no `jack-4.7c`-style first-note
corruption or route-dependent period residual.

### 5.3 Verified local `jack-4.7d` evidence

On 2026-08-03, from local branch `jack-4.7d` based on
`486c63a35c83cc2b5cc36d9b1904827aeab02c48`:

- `cmake --build build.debug --target muse_audio_jack_tests muse_audio_tests
  MuseScoreStudio -j6` passed with `MUSE_MODULE_AUDIO_JACK=ON`.
- `build.debug/src/framework/audio/tests/muse_audio_jack_tests` passed all 17
  callback/transport tests.
- `build.debug/src/framework/audio/tests/muse_audio_tests` passed all 36 shared
  audio tests, including preparation re-anchoring and ten-minute fractional
  clock progression at 256, 1,024, 2,048, and 4,096 frames.
- A fresh `build.nojack` configured with `MUSE_MODULE_AUDIO_JACK=OFF` built the
  full `MuseScoreStudio` target and `muse_audio_tests`; all 36 tests passed.
  `ldd`, `readelf -d`, and the generated Ninja commands contained no libjack
  dependency or JACK driver source for that executable.
- This host provides the PipeWire JACK ABI (`pkg-config jack` 3.1608.0) and
  `pw-jack`, but no `jackd`, Ardour, or JACK capture utility. Consequently no
  native timing offset, route, Automatic-alignment, first-note, or ten-minute
  drift measurement was made. Section 5.2 remains unchecked and is the release
  gate.

### 5.4 Completion checklist

Checked items below have direct build, automated-test, code-audit, or recorded
PipeWire-JACK/Ardour evidence. Unchecked items remain external manual/CI
qualification; they do not imply unfinished implementation code.

- [x] This plan's goal, scope lock, and non-goals were kept.
- [x] JACK-on and JACK-off Linux builds compile.
- [ ] Shared interface/stub changes retain existing non-Linux CI/build
      compatibility without libjack or JACK UI.
- [x] The stale driver was rewritten with no global state and safe close order.
- [ ] JACK coexists with ALSA/PipeWire in selection and failure fallback.
- [x] Linux driver creation is exact; after each successful startup/switch the
      published/saved API matches the opened backend, and failed switches do not
      persist the rejected API.
- [ ] Preferences Apply/Cancel/Reset follows the focused live-driver
      reconciliation policy without recursive switching or switching under
      Running/Paused playback.
- [x] Server rate/period become the active engine spec.
- [x] Successful open/switch publishes the server-derived active spec exactly
      once; failed activation publishes none.
- [x] Stereo score audio works with sync off.
- [ ] Note audition works with sync off and while sync is Effective with JACK
      Stopped.
- [x] `Use JACK transport` is persisted, default-off, and clearly presented.
- [x] Every enable/new JACK driver starts Pending from a fresh observation;
      Off/Pending/Effective/Unavailable presentation changes notify the UI and
      Off callbacks cannot affect playback or poison the next enable; rapid
      stop/start cannot bypass preparation.
- [x] In JACK-enabled Linux Preferences, the API selector is enabled only at
      exact `Stopped` status; Paused/Running keep it disabled.
- [x] Ardour-originated locate/start/stop/rolling-locate work.
- [x] MuseScore-originated Play/Pause/Pause-and-select/Stop/toolbar-seek/
      play-from-selection work.
- [ ] MuseScore-originated notation, Timeline, and keyboard beat/selection
      seeks relocate Ardour while stopped and rolling.
- [x] External events cannot echo through the outbound user-action path.
- [x] Internal reset/EOF/content-maintenance calls cannot control JACK.
- [x] Preparation delivery, generation identity, ordered seeks, and render
      eligibility cover overwrite, stale completion, and timeout cases.
- [x] Pending preparation is cancelled on newer locate, score change, driver
      change, shutdown, or sync disable.
- [x] Same-frame episodes get distinct tokens, an external locate can supersede
      a pending local locate, and an xrun/frame gap cannot synthesize a locate.
- [x] Loop/count-in/speed restrictions apply only while Effective and restore
      ordinary local behavior after disable, driver replacement, or server loss.
- [ ] Missing server, normal backend switching, period change, xrun, and server
      loss meet the practical safety behavior.
- [x] The 4.7d focused callback/state and shared-audio automated checks pass;
      the user/external routing and API/Preferences boundaries passed focused
      code audit. Earlier PipeWire-JACK workflow evidence is regression-only.
- [ ] The extended native JACK2 manual matrix passes.
- [x] Accepted limitations are recorded.
- [x] PipeWire-JACK smoke is recorded when available (optional).

## 6. Deferred work

The following may be valuable later but must not delay v1: JACK MIDI, automatic
connections, multiple output pairs, timebase/BBT, loops/count-in on the shared
timeline, freewheel export, automatic reconnect, seamless hot switching,
joining an already-rolling transport, exhaustive plugin timing, non-Linux
support, AppImage/distribution packaging, and broad audio/RPC/Settings
hardening.

If a deferred item becomes important, create a separate plan after the focused
JACK audio/transport workflow works.

## 7. References

- Hydrogen 1.2.6, commit `be87f2decf36f0c86586801cc05c0482a8af9dd1`,
  [`JackAudioDriver.cpp`](/home/rynti/dev/hydrogen/src/core/IO/JackAudioDriver.cpp)
  — local read-only behavioral reference for querying the first frame of the
  current JACK cycle before rendering, with no downstream-latency source lead.
- MuseScore 3.7, commit `e37e22e43119153192a573fe2d558b703311bb74`,
  [`jackaudio.cpp`](/home/rynti/dev/MuseScore-3.7/audiodrivers/jackaudio.cpp)
  — local read-only behavioral reference for comparing the sequencer cursor
  with the current JACK frame during processing.
- Current MuseScore 4 implementation,
  [`jackaudiodriver.cpp`](/home/rynti/dev/MuseScore/src/framework/audio/driver/platform/jack/jackaudiodriver.cpp).
- [Lyrra MuseScore 4 JACK/MIDI pull request #19246](https://github.com/musescore/MuseScore/pull/19246) — historical behavior and research; closed and not directly mergeable.
- [Lyrra transport implementation commit](https://github.com/musescore/MuseScore/commit/c27a329a669913c45695f7bd2d084c011cf5d655) and [preference commit](https://github.com/musescore/MuseScore/commit/892497418b709b50196ffe22ebdf1371dad88720) — behavior references, not code to copy.
- [MuseScore 3 JACK driver](https://github.com/musescore/MuseScore/blob/v3.6.2/audio/drivers/jackaudio.cpp) — parity reference for the basic audio/transport workflow only.
- [JACK transport design](https://jackaudio.org/api/transport-design.html) and [transport-control API](https://jackaudio.org/api/group__TransportControl.html) — normative callback, state, locate, and slow-sync behavior.
- [JACK client callbacks](https://jackaudio.org/api/group__ClientCallbacks.html) and [port API](https://jackaudio.org/api/group__PortFunctions.html) — normative real-time and lifecycle API reference.
- [Audacity source](https://github.com/audacity/audacity) — useful only as evidence that practical JACK audio can rely on an existing callback abstraction; Audacity does not provide the transport design used here.
