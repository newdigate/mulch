# Audio Player — Auto Play — Design

**Date:** 2026-09-22
**Status:** Implemented

## Goal

A new `auto play` Bool input on the **Audio File** node (`src/modules/AudioPlayerNode.{h,cpp}`)
that hands playback control to the global transport: press Play on the toolbar and the clip
plays, press Pause and it holds, press Stop and it rewinds ready to start again.

Today the node's `play` input is a manual toggle with no relationship to the transport. The only
transport-aware mode is `sync`, which bar-locks the clip and time-warps it to span `length` bars —
useful, but it forces the clip's *tempo*, which is not what someone wants when they simply want a
sound file to start with the song.

## Decisions (from brainstorm)

- **Follow the transport**, rather than restarting on every Play or gating without touching
  position. Pause holds, Play resumes, Stop rewinds.
- **Auto play overrides `play`.** With `auto play` on, the transport decides and `play` is ignored.
  With it off, `play` behaves exactly as it does today. One switch, and no ambiguity about which
  control is in charge.

## The port

```cpp
addInput("auto play", PortType::Bool, false);   // port index 6, appended after `length`
```

Default **false**, so every existing project behaves unchanged.

**It must be appended, not inserted.** `ProjectFile` stores control-type input defaults by port
index (`inb <port> <value>`), so adding a port anywhere but the end would silently reassign every
saved value after it — an existing project's `length` would be read as `sync`, and so on.

No UI work: `core/PanelSlot.h`'s `inputSlot` routes every Bool input to the **Properties** panel as
a checkbox, so it appears beside `play`, `loop` and `sync` automatically.

## Behaviour

Let `autoPlay = ctx.in<bool>(6) && ctx.transport != nullptr`.

**Effective play.**

```
effectivePlay = autoPlay ? ctx.transport->playing : play
```

The transport fallback matters: unit tests and any other caller may build an `EvalContext` with no
transport, and the node should keep working rather than going permanently silent. Falling back to
`play` mirrors how `syncActive` already requires `ctx.transport`.

**Following the position.** While `autoPlay` is on and `sync` is off, the node watches the
transport's position between frames and rewinds the clip to 0 whenever that position moves
**backwards**:

| Transport action | `transport->seconds` | Clip |
|---|---|---|
| Play | advances | plays on from where it was |
| Pause | stops moving | holds position |
| Stop | jumps to 0 | **rewinds to 0** (the next Play starts the file from the beginning) |
| Loop wrap | jumps back to the loop start | **rewinds to 0** (the clip restarts each time round) |
| Scrub forward | jumps forward | left alone — keeps playing at its own rate |

Restarting on a loop wrap is deliberate and musically useful. Leaving a forward scrub alone is also
deliberate: strict position locking is what `sync` is for, and auto play is about free-running
playback that the transport starts and stops.

**Interaction with `sync`.** When `sync` is on the playhead is derived from `transport->bars()`
every frame, so there is no position to rewind and the backwards-detection is skipped entirely.
`auto play` still gates whether the synced clip is audible, so a bar-locked clip falls silent when
the transport stops — coherent, and what you would want.

**The seam.** `evaluate` already silences a single block whenever the playhead jumps discontinuously
(`wrapped`), so a swept glitch is never emitted. The auto-play rewind sets the same flag, so a Stop
produces a clean cut rather than a click.

## Implementation sketch

In `AudioPlayerNode::evaluate`, after the existing input reads:

```cpp
bool autoPlay = ctx.in<bool>(6) && ctx.transport != nullptr;
bool effPlay  = autoPlay ? ctx.transport->playing : play;
```

`effPlay` then replaces `play` at its three existing uses: the free-run advance, the
`if (wrapped || !play)` silence gate, and `updateStatus`.

A new member tracks the transport between frames:

```cpp
double prevTransportSeconds_ = 0.0;   // to spot a backwards move (Stop / loop wrap) under auto play
bool   hadTransport_         = false; // so the first frame is not read as a jump
```

The rewind, applied only when `autoPlay && !syncActive`:

```cpp
if (hadTransport_ && ctx.transport->seconds < prevTransportSeconds_) {
    playhead_ = 0.0;
    wrapped   = true;   // reuse the existing seam-silencing path
}
```

`hadTransport_` guards the very first evaluate, where `prevTransportSeconds_` has no meaning yet;
without it a project loaded with the transport already at bar 8 would rewind the clip spuriously.

**Both members update on every evaluate that has a transport**, regardless of `auto play` or
`sync`. That matters: if they were only updated while auto play was active, then switching auto
play on after the song had been running would compare against a stale position from minutes ago and
rewind the clip immediately. The same applies to leaving `sync` — the tracker must have kept up
while the node was in the other mode.

The status line takes the effective flag, and says which control is in charge, so a clip that is
silent because the transport is stopped reads as stopped rather than broken — `(auto)` when playing
under auto play, `(auto, stopped)` when not.

## Testing

`AudioPlayerNode` is **not** in `core_tests` — it reaches FFmpeg through `audio/AudioFile`, which
`core_tests` does not link. It *is* in `gl_smoke`'s source list, so that is where this belongs, and
the existing `tests/assets/tone.mp3` fixture and the node's existing `playhead()` / `leftOut()` test
accessors mean no new surface is needed.

A `gl_smoke` scenario:

1. Build an Audio File node on `tests/assets/tone.mp3`, `auto play` on, `sync` off. Evaluate until
   `loading()` clears so the clip is decoded.
2. Transport **not** playing → the output block is silent and the playhead does not advance.
3. Transport playing → the block is non-silent and the playhead advances.
4. Transport paused (`playing = false`, position unchanged) → the playhead holds.
5. Transport stopped (`playing = false`, `seconds = 0`) → the playhead rewinds to 0.
6. `auto play` **off** with the transport stopped and `play` on → audible, proving the override is
   conditional and that existing behaviour is untouched.

Each assertion should be mutation-checked.

**Measured outcome.** Three mutations are caught: ignoring `auto play` entirely (fails 1), dropping
the rewind (fails 5), and rewinding on a forward move instead of a backwards one (fails 3, because
the clip is dragged back to 0 on every playing frame and never sounds).

**Two survive, and this spec was wrong to imply otherwise.** It originally claimed an
implementation that "rewinds in `sync` mode" would fail one of these — it does not, for two
reasons. The scenario never enables `sync`, and more fundamentally the guard is barely observable
even if it did: in sync mode the rewind's `playhead_ = 0` is overwritten by `barSyncPlayhead` two
lines later, so the only trace is one extra silenced block, which a bar-window wrap usually sets
anyway. The `!syncActive` guard is defensive clarity, not load-bearing behaviour. The second
survivor is the `ctx.transport != nullptr` fallback, which is unreachable from a `Graph` (it always
passes its own transport) and so cannot be reached from this test at all.

Both are recorded here rather than papered over with a test that would not really exercise them.

## Out of scope (YAGNI)

- Following a forward scrub or an arbitrary seek (that is `sync`'s job).
- A start-offset or a delay before the clip enters.
- Applying auto play to any other node. The Video Player has the same shape and may want it later;
  this change does not generalise for it.
- Persisting anything new: the port is a control default and persists through the existing
  `ProjectFile` path for free.
