#pragma once

namespace oss {

struct Transport;

// Draws the transport toolbar as a top main-menu bar: Play/Pause, Stop, Rewind,
// Fast-forward, an editable decimal tempo field, and the song position read out
// as bars.beats, beats, and minutes:seconds.milliseconds. Mutates `t` in place
// (button presses and tempo edits). Call inside an active ImGui frame.
void drawTransportBar(Transport& t);

} // namespace oss
