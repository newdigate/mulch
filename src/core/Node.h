#pragma once
#include <string>
#include <vector>
#include <cstddef>
#include <glm/vec2.hpp>
#include "core/Port.h"
#include "core/Value.h"

namespace oss {

class Graph;

// Per-frame, fully-resolved evaluation context handed to a node.
struct EvalContext {
    const std::vector<Value>& inputs;   // one resolved value per input port
    std::vector<Value>&       outputs;  // node writes one value per output port
    float                     dt;        // seconds since previous frame

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

private:
    friend class Graph;
    std::string       name_;
    int               id_ = -1;     // assigned by Graph::addNode
    std::vector<Port> inputs_;
    std::vector<Port> outputs_;
};

} // namespace oss
