#include "ui/FileDialog.h"
#include <nfd.h>
#include <cstddef>
#include <string>

namespace oss {

namespace {
// Join bare extensions into NFD's comma-separated spec, e.g. {"wav","mp3"} -> "wav,mp3".
std::string joinSpec(const std::vector<std::string>& filters) {
    std::string spec;
    for (std::size_t i = 0; i < filters.size(); ++i) { if (i) spec += ','; spec += filters[i]; }
    return spec;
}
} // namespace

std::string openFileDialog(const char* /*title*/, const char* filterName,
                           const std::vector<std::string>& filters) {
    // NFD-extended has no title parameter (native dialogs use the OS default); `title`
    // stays in the signature for clarity.
    if (NFD_Init() != NFD_OKAY) return std::string();
    std::string spec = joinSpec(filters);
    nfdfilteritem_t item{ filterName, spec.c_str() };
    const nfdfilteritem_t* list = spec.empty() ? nullptr : &item;
    nfdfiltersize_t count = spec.empty() ? 0 : 1;

    nfdchar_t*  outPath = nullptr;
    nfdresult_t r = NFD_OpenDialog(&outPath, list, count, nullptr);
    std::string result;
    if (r == NFD_OKAY && outPath) { result = outPath; NFD_FreePath(outPath); }
    NFD_Quit();
    return result;
}

std::string saveFileDialog(const char* /*title*/, const char* filterName,
                           const std::vector<std::string>& filters,
                           const std::string& defaultName) {
    // `title` is unused (NFD has no title param) -- kept for signature symmetry; see openFileDialog.
    if (NFD_Init() != NFD_OKAY) return std::string();
    std::string spec = joinSpec(filters);
    nfdfilteritem_t item{ filterName, spec.c_str() };
    const nfdfilteritem_t* list = spec.empty() ? nullptr : &item;
    nfdfiltersize_t count = spec.empty() ? 0 : 1;

    nfdchar_t*  outPath = nullptr;
    // NFD_SaveDialog(outPath, filterList, count, defaultPath, defaultName)
    nfdresult_t r = NFD_SaveDialog(&outPath, list, count, nullptr, defaultName.c_str());
    std::string result;
    if (r == NFD_OKAY && outPath) { result = outPath; NFD_FreePath(outPath); }
    NFD_Quit();
    return result;
}

} // namespace oss
