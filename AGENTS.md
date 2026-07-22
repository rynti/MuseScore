# JACK Work Ground Rules

These repository-level instructions govern work on the Linux JACK feature. For
unrelated tasks, they do not create JACK-specific obligations.

## Mission

Deliver a usable MuseScore Studio JACK backend on Linux with stereo output and
bidirectional JACK transport synchronization, especially for working with
Ardour. The practical compatibility target is MuseScore 3 and the useful parts
of the Lyrra MuseScore 4 JACK work: users can hear MuseScore through JACK,
follow an external JACK transport, and start/pause/stop/seek the shared
transport from MuseScore itself.

`JackPlan.md` is the sole authoritative implementation plan and complete v1
specification.

## Scope guard

This work is a focused feature integration, not a mandate to redesign or
formally qualify MuseScore's audio infrastructure. Prefer the smallest change
that passes `JackPlan.md`'s real Linux/JACK/Ardour workflows.

Before making a task mandatory, identify which `JackPlan.md` behavior or test
it enables. A task is in scope only if it:

1. implements a stated JACK behavior;
2. fixes a build/runtime failure in that behavior; or
3. fixes a realistic severe defect introduced or exposed on the supported JACK
   path (crash, use-after-free, deadlock, persistent setting corruption, or
   data loss).

Pre-existing theoretical races, unrelated cleanup, generalized robustness,
and hardening for unsupported workflows go to a separate issue or the
implementation handoff's accepted/deferred notes; do not add them to
`JackPlan.md`. They do not block the JACK feature. If a proposed fix crosses
several unrelated subsystems, first demonstrate the focused failing acceptance
case and consider a smaller boundary fix.

Scope expansion—JACK MIDI, cross-platform JACK, timebase/BBT, freewheel export,
automatic routing/reconnect, a new render architecture, broad RPC/Settings/
export/graph protocols—requires explicit user approval and an update to
`JackPlan.md`.

## Engineering rules

- Reuse the current `IAudioDriver`, `AudioDriverController`,
  `StartAudioController`, `IPlayer`, and `PlaybackController` paths.
- Rewriting the stale JACK driver is expected; rewriting adjacent systems is
  not.
- It is acceptable for JACK's process callback to invoke the existing driver
  render callback. Do not introduce a producer/ring architecture unless a
  repeatable acceptance failure proves it is necessary.
- New JACK callback code must be bounded: no allocation, blocking lock, Qt/UI
  call, logging, or resource destruction. This does not require proving every
  pre-existing renderer/plugin path hard-real-time safe.
- Own JACK resources per driver instance. Make open failure reversible and
  close idempotent; deactivate/close the client before freeing callback state.
- Keep external transport events typed and distinct from public playback
  actions so they cannot echo back to JACK.
- Keep JACK optional and Linux-only. JACK-disabled and non-Linux builds must
  keep compiling with no libjack dependency or JACK UI and retain their
  existing backend choices; do not implement or qualify JACK elsewhere.
- Implement in vertical slices: selectable audio first, external follow second,
  MuseScore-originated control third, focused reliability last.
- Preserve unrelated user changes. Do not mix opportunistic refactors into a
  JACK patch.

## Quality bar

The goal is dependable musician-facing software, not spacecraft certification.

Required: builds cleanly, works in the named workflows, does not have obvious
callback lifetime bugs or deadlocks, fails understandably when the JACK server
is absent/lost, and has focused regression tests for transport routing and
state. Manual JACK2 + Ardour verification is first-class evidence; a
PipeWire-JACK smoke test is desirable when available.

Not required: exhaustive fault injection, proof of all callback interleavings,
sample-exact behavior for every plugin, seamless behavior under arbitrary
server reconfiguration, or fixes for unrelated infrastructure defects.

Review feedback must distinguish:

- **Blocker:** violates a stated `JackPlan.md` behavior/test or creates a
  realistic severe defect on that path.
- **Focused improvement:** useful and small, but not necessary for completion.
- **Deferred/out of scope:** theoretical, unrelated, generalized, or for an
  explicitly unsupported workflow.

Do not promote the latter two into release gates without updating
`JackPlan.md` and obtaining explicit scope approval.

## Handoff expectations

For each implementation phase, report the behavior completed, tests actually
run, and accepted limitations. Update `JackPlan.md` only when a product decision
or verified code fact changes; do not grow it with speculative threat models.
When a real blocker needs adjacent work, document the concrete failing case,
the minimal chosen fix, and why a JACK-local alternative was insufficient.
