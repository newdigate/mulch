#pragma once
#include <string>
#include <vector>

namespace oss {

// Native open-file dialog. `filters` are bare extensions ("wav", "mp3"); an empty list
// allows any file. Returns the chosen absolute path, or "" if the user cancels or on
// error. The native dialog library is confined to FileDialog.cpp.
std::string openFileDialog(const char* title, const std::vector<std::string>& filters);

} // namespace oss
