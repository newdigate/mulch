#pragma once
#include <string>
#include <vector>
#include <cstddef>
#include <glm/vec2.hpp>
#include "core/Port.h"
#include "core/Value.h"
#include "core/Transport.h"

namespace oss {

class Graph;
struct Preferences;

// Per-frame, fully-resolved evaluation context handed to a node.
struct EvalContext {
    const std::vector<Value>& inputs;   // one resolved value per input port
    std::vector<Value>&       outputs;  // node writes one value per output port
    float                     dt;        // seconds since previous frame
    const Transport*          transport = nullptr;  // global clock (set by Graph::evaluate)
    const Preferences*        prefs     = nullptr;   // app settings (set by Graph::evaluate)

    template <class T> const T& in(std::size_t i) const { return std::get<T>(inputs[i]); }
    template <class T> void out(std::size_t i, T v) { outputs[i] = Value(std::move(v)); }
};

class Node {
public:
    explicit Node(std::string name) : name_(std::move(name)) {}
    virtual ~Node() = default;

    const std::string& name() const { return name_; }
    int id() const { return id_; }

    const std::vector<Port>& inputs()  const { return inputs_; }
    const std::vector<Port>& outputs() const { return outputs_; }

    // Mutable access to an input's default value (edited by inline widgets).
    Value& inputDefault(std::size_t i) { return inputs_[i].defaultValue; }

    glm::vec2 pos{0.0f, 0.0f};

    // Compute outputs from resolved inputs. Called in topological order each frame.
    virtual void evaluate(EvalContext& ctx) = 0;

    // One-time GL setup (shaders, FBOs). Base does nothing (GL-free nodes).
    virtual void initGL() {}

    // Optional short status line shown under the node's name in the editor
    // (e.g. a loader's progress/result). Empty by default.
    virtual std::string statusLine() const { return std::string(); }

    // Persist/restore editable state that is NOT an input port. Default empty: every
    // node whose controls are all input ports needs nothing here. Only the Automation
    // node overrides these (its curves). The string is opaque to the project file.
    virtual std::string saveState() const { return std::string(); }
    virtual void        loadState(const std::string& /*s*/) {}

    // Optional button bank, rendered by the node editor as a row of buttons under the
    // node's name (GL-free: ints/strings only). Default = none. A node exposes preset/
    // mode buttons by overriding these; the editor calls onButtonPressed() on a click.
    virtual int         buttonCount() const { return 0; }       // 0 = no buttons
    virtual std::string buttonLabel(int /*i*/) const { return std::string(); }
    virtual int         buttonActive() const { return -1; }     // index drawn highlighted
    virtual int         buttonPending() const { return -1; }    // index drawn as "pending"
    virtual void        onButtonPressed(int /*i*/) {}           // a button was clicked

    // Optional tri-state grid, rendered by the node editor under the button bank (GL-free:
    // ints/strings only). Cell values are 0/1/2 (e.g. off/on/accent). Default = none. The
    // editor calls onGridCellPressed() on a click (the node decides how to cycle the value).
    virtual int         gridRows() const { return 0; }              // 0 = no grid
    virtual int         gridCols() const { return 0; }
    virtual int         gridCell(int /*r*/, int /*c*/) const { return 0; }   // 0/1/2
    virtual void        onGridCellPressed(int /*r*/, int /*c*/) {}
    virtual std::string gridRowLabel(int /*r*/) const { return std::string(); }

protected:
    void addInput(std::string n, PortType t, Value def) {
        inputs_.push_back({std::move(n), Direction::Input, t, std::move(def)});
    }
    // Overload carrying a numeric range for the inline Float slider.
    void addInput(std::string n, PortType t, Value def, float lo, float hi) {
        inputs_.push_back({std::move(n), Direction::Input, t, std::move(def), lo, hi});
    }
    void addOutput(std::string n, PortType t) {
        outputs_.push_back({std::move(n), Direction::Output, t, Value{}});
    }
    // A Float input rendered as a dropdown of `labels`; its value is the selected
    // index (0-based). Used for enum-like parameters (e.g. an LFO waveform).
    void addChoiceInput(std::string n, std::vector<std::string> labels, int def) {
        Port p;
        p.name = std::move(n);
        p.direction = Direction::Input;
        p.type = PortType::Float;
        p.defaultValue = Value((float)def);
        p.minVal = 0.0f;
        p.maxVal = labels.empty() ? 0.0f : (float)(labels.size() - 1);
        p.choices = std::move(labels);
        inputs_.push_back(std::move(p));
    }
    void addAssetInput(std::string n, AssetType type, std::string def = std::string()) {
        Port p;
        p.name         = std::move(n);
        p.direction    = Direction::Input;
        p.type         = PortType::String;
        p.defaultValue = Value(std::move(def));
        p.assetBacked  = true;
        p.assetType    = type;
        inputs_.push_back(std::move(p));
    }

private:
    friend class Graph;
    std::string       name_;
    int               id_ = -1;     // assigned by Graph::addNode
    std::vector<Port> inputs_;
    std::vector<Port> outputs_;
};

} // namespace oss
