#pragma once
#include <string>
#include <vector>

namespace oss {

// Native open-file dialog. `filterName` labels the filter shown in the dialog (e.g. "Project");
// `filters` are bare extensions ("oss"); an empty list allows any file. Returns the chosen
// path, or "" if the user cancels or on error. The native dialog library is confined to the .cpp.
std::string openFileDialog(const char* title, const char* filterName,
                           const std::vector<std::string>& filters);

// Native save-file dialog. `defaultName` seeds the filename field (e.g. "project.oss").
// Same return contract as openFileDialog.
std::string saveFileDialog(const char* title, const char* filterName,
                           const std::vector<std::string>& filters,
                           const std::string& defaultName);

// Native multi-select open dialog. Returns every chosen path, or an empty vector if the
// user cancels or on error. Same `filterName`/`filters` contract as openFileDialog.
std::vector<std::string> openMultipleFileDialog(const char* title, const char* filterName,
                                                const std::vector<std::string>& filters);

} // namespace oss
