#pragma once
#include <string>
#include <vector>
#include "core/Value.h"
#include "core/AssetLibrary.h"

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
    // A Float input flagged `integer` renders as a whole-number slider (SliderInt) over
    // [minVal, maxVal] instead of a float slider; the value is still stored as a float.
    // Set via Node::addIntInput (easier to set with the mouse than a fine float slider).
    bool      integer = false;
    // A String input marked asset-backed renders a library-picker dropdown (of this
    // AssetType) in the editor; picking copies the asset's path into this input.
    // Set via Node::addAssetInput. Defaults off, so ordinary String inputs are unaffected.
    bool      assetBacked = false;
    AssetType assetType   = AssetType::Audio;
};

} // namespace oss
