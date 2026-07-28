# Linux JACK audio and transport

MuseScore Studio can be built as an optional stereo JACK client on Linux. The
backend uses an already-running JACK server and can synchronize the shared JACK
transport in both directions, including with Ardour. JACK MIDI, automatic port
connections, BBT/timebase-master support, and server startup are not included.

## Build

Install the JACK development files so `pkg-config jack` succeeds, then enable
the backend explicitly:

```sh
cmake -P build.cmake install -GNinja -DMUSE_MODULE_AUDIO_JACK=ON
```

The normal direct CMake workflow also accepts
`-DMUSE_MODULE_AUDIO_JACK=ON`; run `cmake --install <build-dir>` after building
so MuseScore can find its soundfont, templates, translations, and other runtime
resources. JACK remains off by default. Launch the installed application with
`cmake -P build.cmake run`, optionally prefixed with `pw-jack` when required by
your PipeWire setup. Do not run the raw build-tree executable.

## Configure audio

1. Start JACK2, or start the normal PipeWire desktop services and launch
   MuseScore through `pw-jack` if your distribution requires that wrapper.
2. Open **Preferences > Audio & MIDI** while playback is stopped and choose
   **JACK** as the audio driver.
3. In Ardour, QJackCtl, Helvum, or another graph tool, connect
   `MuseScore:audio_out_left` and `MuseScore:audio_out_right` to the desired
   playback or Ardour input ports. MuseScore intentionally does not create
   graph connections itself. The client is normally named `MuseScore`; JACK
   may append a unique-name suffix when that name is already occupied. The
   `audio_out_left` and `audio_out_right` port basenames remain unchanged.

JACK owns the sample rate and period. Their values are shown read-only in
MuseScore and must be changed in the JACK/PipeWire configuration.

## Use shared transport

Enable **Use JACK transport** in the same Audio preferences section. The first
activation waits for a fresh stopped JACK state; if the server was already
rolling, stop it once before trying to synchronize. In Ardour, also enable its
JACK transport integration (the exact control name depends on the Ardour
version). Audio-port connections and transport synchronization are separate
settings. When the status reads active:

- Ardour/JACK start, stop, and locate commands are followed by MuseScore.
- MuseScore Play, Pause, Stop, Rewind/seek, and Play from selection control the
  shared JACK transport.
- MuseScore Stop returns the shared transport to zero; Pause keeps its current
  position.
- Loop, count-in, and playback-speed changes are unavailable while shared
  transport is active. Note audition while stopped remains available.
- Natural end of a score silences MuseScore without stopping Ardour.

Turn **Use JACK transport** off to keep JACK audio output but return MuseScore's
playback controls to a local timeline.

## Minimal Ardour check

Use a short score with a sharp first note or metronome click and an Ardour track
on the same timeline:

1. With transport synchronization off, confirm MuseScore can play, pause, seek,
   audition notes, loop, and use count-in locally through its JACK ports.
2. Enable synchronization while JACK is stopped. Start, stop, and locate from
   Ardour, including a locate while rolling; confirm MuseScore follows.
3. Start, pause, pause-and-select, stop, rewind, and play from selection in
   MuseScore; confirm Ardour follows and Stop returns both applications to
   zero.
4. Repeat a few rapid locates and switch from JACK to another available backend
   and back while stopped.
5. For timing qualification, compare the first transients after several starts
   and the relative offset near the beginning and end of a ten-minute 48 kHz
   run. Variation should stay within two JACK periods under normal load.

If the JACK server exits, MuseScore silences the backend and marks transport
unavailable. Restart/select an audio backend again; automatic reconnect is not
part of this implementation.
