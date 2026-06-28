#pragma once
#include <string>
#include <vector>

namespace oss {

// Native open-file dialog. `filterName` labels the filter shown in the dialog (e.g. "Project");
// `filters` are bare extensions ("oss"); an empty list allows any file. Returns the chosen
// path, or "" if the user cancels or on error. The native dialog library is confined to the .cpp.
// `defaultPath` (a directory) seeds where the dialog opens; "" uses the OS default.
std::string openFileDialog(const char* title, const char* filterName,
                           const std::vector<std::string>& filters,
                           const std::string& defaultPath = "");

std::string saveFileDialog(const char* title, const char* filterName,
                           const std::vector<std::string>& filters,
                           const std::string& defaultName,
                           const std::string& defaultPath = "");

std::vector<std::string> openMultipleFileDialog(const char* title, const char* filterName,
                                                const std::vector<std::string>& filters,
                                                const std::string& defaultPath = "");

// Native "choose a folder" dialog. `defaultPath` seeds the starting directory. Returns the chosen
// directory, or "" on cancel/error.
std::string pickFolderDialog(const char* title, const std::string& defaultPath = "");

} // namespace oss
