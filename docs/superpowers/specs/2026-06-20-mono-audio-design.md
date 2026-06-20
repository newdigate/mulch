# Mono Audio Edges + Stereo Bridge Nodes — Design

**Date:** 2026-06-20
**Status:** Approved (brainstorming) — ready for implementation plan

## Goal

Make every audio edge **mono**. `AudioRef` drops its `channels` field, so a connection
always carries one channel. Nodes that were stereo expose separate `left`/`right` mono
ports. Two new bridge nodes convert between a mono signal and a stereo pair: **Mono to
Stereo** (pan a mono signal into L/R) and **Stereo to Mono** (downmix L/R to one channel).
The existing 4-channel **Audio Mix** stays, with its stereo output converted to two mono
outputs.

## Architecture

The pan/balance math moves into a GL-free, unit-tested helper (`core/AudioPan.h`) shared by
the mixer and the two new bridges. `AudioClip`/`AudioFile` (`src/audio/`) keep their own
`channels`/`frames()` and stay stereo internally — only the **edge** type (`AudioRef`)
becomes mono; the Audio Player deinterleaves its clip into two mono outputs at the boundary.

### Unit 1 — `AudioRef` goes mono (`src/core/Value.h`)

```cpp
struct AudioRef {
    const float* samples    = nullptr;
    std::size_t  count       = 0;     // mono sample count (== frames)
    int          sampleRate  = 0;
};
```
Remove `channels` and `frames()`. `count` is the mono sample count. Update every site that
read `.channels`/`.frames()` **on an `AudioRef`** (NOT `AudioClip` — that keeps its own):
`SpectrographNode.cpp:43-44`, `OscilloscopeNode.h:39-40`, `RecorderNode.cpp:60`,
`AudioMixerNode.h:39,49,53`, `AudioOutputNode.cpp:30,33`, `AudioPlayerNode.cpp` (the emit),
`AudioInputNode.cpp`, `tests/test_audio_mixer.cpp`, and `tests/gl_smoke.cpp:729`.
`AudioPlayerNode.cpp:34,74` use `clip_.frames()` (an `AudioClip`) and stay unchanged.

### Unit 2 — `core/AudioPan.h` (new, GL-free, unit-tested)

```cpp
namespace oss {
struct PanGains { float l, r; };
// Mono->stereo pan: the existing mixer's balance law. center -> both at unity,
// hard pan mutes the far side. pan in [-1,1].
inline PanGains panGains(float pan) {
    return { 1.0f - std::max(0.0f, pan), 1.0f + std::min(0.0f, pan) };
}
// Stereo->mono downmix crossfade: center -> 0.5L+0.5R, -1 -> L only, +1 -> R only.
inline PanGains downmixGains(float balance) {
    return { 0.5f * (1.0f - balance), 0.5f * (1.0f + balance) };
}
}
```

### Unit 3 — Existing nodes: split stereo I/O into `left`/`right` mono ports

Port lists below are the **logical** result; the implementer reads each node and adjusts the
positional `ctx.in/out` indices accordingly (splitting one audio port into two shifts later
indices).

| Node | File | Change |
|------|------|--------|
| **Audio Player** | `AudioPlayerNode.{h,cpp}` | outputs `left`(0) + `right`(1) (was one `audio`). Deinterleave the stereo `clip_` playback into two mono buffers; emit each. |
| **Audio Input** | `AudioInputNode.{h,cpp}` | outputs `left`(0) + `right`(1) (was one `audio`). Stereo device → deinterleave the captured block; mono device → both outputs carry the same samples. |
| **Audio Mix** | `AudioMixerNode.h` | inputs unchanged (in/gain/pan ×4 = 12); outputs `left`(0) + `right`(1) (was one interleaved `out`). Every input is now mono → drop the `channels==2` branch; place each by `panGains(pan)`; write `left`/`right` mono buffers, clamped to [−1,1]. |
| **Audio Out** | `AudioOutputNode.{h,cpp}` | inputs `left`(0) + `right`(1) (was one `audio`). Interleave the two mono inputs into the ring buffer the RT callback already drains (`ch0=L, ch1=R` mapping untouched). **Mirror:** if `right` is empty, feed `left` to both. |
| **Recorder** | `RecorderNode.{h,cpp}` | split the single audio in/out into `left`+`right` mono in and `left`+`right` mono out (pass-through), keeping the video port + controls. Record 2ch by interleaving `left`+`right`; **mirror** `left`→`right` when `right` is absent (so recordings aren't one-sided). |
| **Oscilloscope** | `OscilloscopeNode.h` | inputs `left`(0) + `right`(1) (was one `audio`), keeping the trailing controls (mode/gain/trigger). Waveform mode uses `left`; X-Y vectorscope uses `left`→x, `right`→y. |
| **Spectrograph** | `SpectrographNode.cpp` | one `audio` mono input (was mono-or-stereo; it already downmixed). Just treat input as mono — remove the channel branch. To analyze a stereo pair, wire a Stereo→Mono first. |

Already-mono nodes are unchanged: **Sine** (`SineWaveNode.h`), **Acid** (`AcidNode.h`),
**Video Player** audio out (`VideoPlayerNode.cpp`, mono).

### Unit 4 — Two new bridge nodes (`src/modules/`, header-only, GL-free, Audio category)

Each owns a small mono output buffer (`std::vector<float>`, like the other audio nodes) and
processes `n = max(countL, countR)` samples, zero-padding the shorter side.

**Mono to Stereo** (`MonoToStereoNode.h`):
- inputs: `mono`(0, Audio), `pan`(1, Float, 0.0, −1…1)
- outputs: `left`(0), `right`(1)
- `PanGains g = panGains(pan); left[i] = g.l * mono[i]; right[i] = g.r * mono[i];`

**Stereo to Mono** (`StereoToMonoNode.h`):
- inputs: `left`(0, Audio), `right`(1, Audio), `balance`(2, Float, 0.0, −1…1)
- output: `mono`(0)
- `PanGains g = downmixGains(balance); mono[i] = clamp(g.l*L[i] + g.r*R[i], -1, 1);`

### Registration / CMake

- `src/app/Application.cpp`: include both headers; `makeNode` returns them for
  `"Mono to Stereo"` / `"Stereo to Mono"`; add both to the **Audio** `nodeCategories` list.
- `CMakeLists.txt`: add `tests/test_audio_pan.cpp` (+ `tests/test_audio_bridges.cpp` if the
  bridge-node tests are a separate file) to `core_tests`. `core/AudioPan.h` and the two new
  nodes are header-only — no `APP_SOURCES` additions.
- `src/main.cpp`: rewire the `--screenshot` demo's audio edges to the new mono ports (any
  edge into Audio Out / out of Audio Player now targets `left`/`right`), and optionally add a
  Mono→Stereo / Stereo→Mono node to the demo.

## Data flow (example)

```
Audio Player ─ left ─┐                          ┌─ left ─► Audio Out (L speaker)
             ─ right ┴► (process per channel) ──┤
Acid ─► Mono to Stereo ─ left ─► Audio Mix in1 ─┘─ right ─► Audio Out (R speaker)
              (pan)    ─ right ─► Audio Mix in2

Stereo pair ─ left ─┐
            ─ right ┴► Stereo to Mono ─ mono ─► Spectrograph / Acid sidechain / ...
```

## Edge cases

- **Unconnected audio input** → default `AudioRef{}` (count 0). Nodes skip empty inputs.
- **Mirror rule** (Audio Out, Recorder): `right` empty → use `left` for both, so a single
  mono wire plays/records on both channels. If `left` is empty too → silence.
- **Count mismatch** between `left`/`right` in a given frame → process `max(countL,countR)`,
  zero-padding the shorter (per-frame producers normally agree, this is just safety).
- **Mono capture device** (Audio Input) → both outputs carry the captured mono signal.
- **`AudioClip` unchanged** → the decoder stays 48 kHz stereo internally; only the Audio
  Player's *edges* are mono. `AudioFile`/`VideoEncoder`/`VideoDecoder` are untouched
  (VideoEncoder still takes an interleaved buffer + channel count from the Recorder).
- **`gl_smoke` audio-file scenario** asserts `clip.channels == 2` (an `AudioClip`) — leave
  those; only the `AudioRef` line (`gl_smoke.cpp:729`) and the node graphs change.

## Testing

- **`tests/test_audio_pan.cpp`** (`core_tests`): `panGains(0) == {1,1}`, `panGains(-1) == {1,0}`,
  `panGains(1) == {0,1}`; `downmixGains(0) == {0.5,0.5}`, `downmixGains(-1) == {1,0}`,
  `downmixGains(1) == {0,1}`.
- **Bridge nodes** (`core_tests`, GL-free): Mono→Stereo with a constant mono input — pan 0 →
  `left==right==mono`; pan −1 → `right` all 0, `left==mono`. Stereo→Mono — balance 0 →
  `mono==0.5(L+R)`; balance −1 → `mono==L`; clamp holds for large inputs.
- **`tests/test_audio_mixer.cpp`** — rewrite the stereo assertions: the mixer now has two
  mono outputs; assert `out[0]` (left) and `out[1]` (right) each have `count == frames` and
  the expected per-channel mixed/panned values (the same numbers the old interleaved buffer
  held, now split across the two ports).
- **`tests/test_sine_wave.cpp`** — unaffected (already mono).
- **`gl_smoke`** — update the audio-node scenario(s) for the split ports (e.g. an
  `AudioRef`-output check at line 729) so they still build and pass; keep the `AudioClip`
  stereo-decode checks as-is.

## Docs

- **CLAUDE.md** — update the `AudioRef` bullet: audio edges are **mono** (one channel,
  `count` = samples); stereo is two `left`/`right` mono ports; nodes split their stereo I/O;
  the `core/AudioPan.h` laws back the mixer + the **Mono to Stereo** / **Stereo to Mono**
  bridges. Note `AudioClip` stays stereo internally.
- **README.md** — the audio module rows reflect the split ports; add **Mono to Stereo** and
  **Stereo to Mono** rows.
</content>
