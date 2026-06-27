#include "ui/FileDialog.h"
#include <nfd.h>
#include <cstddef>
#include <string>

namespace oss {

std::string openFileDialog(const char* /*title*/, const std::vector<std::string>& filters) {
    // NFD-extended has no title parameter (native dialogs use the OS default); `title`
    // stays in the signature for clarity and for a possible future backend.
    if (NFD_Init() != NFD_OKAY) return std::string();

    // One filter item built from the bare extensions, e.g. {"wav","mp3"} -> "wav,mp3".
    std::string spec;
    for (std::size_t i = 0; i < filters.size(); ++i) {
        if (i) spec += ',';
        spec += filters[i];
    }
    nfdfilteritem_t item{ "Media", spec.c_str() };
    const nfdfilteritem_t* list = spec.empty() ? nullptr : &item;
    nfdfiltersize_t count = spec.empty() ? 0 : 1;

    nfdchar_t*  outPath = nullptr;
    nfdresult_t r = NFD_OpenDialog(&outPath, list, count, nullptr);
    std::string result;
    if (r == NFD_OKAY && outPath) { result = outPath; NFD_FreePath(outPath); }
    NFD_Quit();
    return result;   // "" on cancel or error
}

} // namespace oss
