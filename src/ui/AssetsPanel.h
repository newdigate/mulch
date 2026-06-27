#pragma once

namespace oss {

class AssetLibrary;

// The "Assets" window: a tab bar (Audio / Video / MIDI / 3D); each tab is a table of that
// type's media files with an editable label + path (+ a native Browse button) and a remove
// button, plus an Add row. Edits mutate the library in place (project state — written to disk
// when the user saves the project, like the rest of the graph).
class AssetsPanel {
public:
    void draw(AssetLibrary& lib, bool* open);
};

} // namespace oss
