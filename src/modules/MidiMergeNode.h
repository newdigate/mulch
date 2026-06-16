#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "core/Node.h"
#include "core/Value.h"

namespace oss {

// Merges up to four MIDI inputs into one output stream: each frame it
// concatenates the events from every input, in input order. GL-free. The MIDI
// analogue of the Audio Mix node -- e.g. layer several Step Seq drum voices
// (kick / snare / hat) into a single MIDI Out.
class MidiMergeNode : public Node {
public:
    MidiMergeNode() : Node("MIDI Merge") {
        for (int i = 0; i < kInputs; ++i)
            addInput("in " + std::to_string(i + 1), PortType::Midi, MidiRef{});
        addOutput("midi", PortType::Midi);
    }

    void evaluate(EvalContext& ctx) override {
        out_.clear();
        for (int i = 0; i < kInputs; ++i) {
            MidiRef in = ctx.in<MidiRef>((std::size_t)i);
            for (std::size_t e = 0; e < in.count; ++e) out_.push_back(in.events[e]);
        }
        ctx.out<MidiRef>(0, MidiRef{out_.data(), out_.size()});
    }

private:
    static constexpr int kInputs = 4;
    std::vector<MidiEvent> out_;   // merged events this frame (owns MidiRef storage)
};

} // namespace oss
