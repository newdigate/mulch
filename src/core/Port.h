#pragma once
#include <string>
#include <vector>
#include "core/Value.h"

namespace oss {

enum class Direction { Input, Output };

struct Port {
    std::string name;
    Direction  direction;
    PortType   type;
    Value      defaultValue;   // used for an unconnected input; drives inline widgets
    float      minVal = 0.0f;  // range for an inline Float slider (ignored otherwise)
    float      maxVal = 1.0f;
    // Optional dropdown labels: a Float input with a non-empty list renders as a
    // combo whose value is the selected index (used for enum-like parameters).
    std::vector<std::string> choices;
};

} // namespace oss
