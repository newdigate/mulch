#include "ui/FileDialog.h"
#include <nfd.h>
#include <cstddef>
#include <string>

namespace oss {

namespace {
std::string joinSpec(const std::vector<std::string>& filters) {
    std::string spec;
    for (std::size_t i = 0; i < filters.size(); ++i) { if (i) spec += ','; spec += filters[i]; }
    return spec;
}
// NFD wants null (not "") for "no default path".
const nfdchar_t* defPath(const std::string& p) { return p.empty() ? nullptr : p.c_str(); }
} // namespace

std::string openFileDialog(const char* /*title*/, const char* filterName,
                           const std::vector<std::string>& filters,
                           const std::string& defaultPath) {
    if (NFD_Init() != NFD_OKAY) return std::string();
    std::string spec = joinSpec(filters);
    nfdfilteritem_t item{ filterName, spec.c_str() };
    const nfdfilteritem_t* list = spec.empty() ? nullptr : &item;
    nfdfiltersize_t count = spec.empty() ? 0 : 1;

    nfdchar_t*  outPath = nullptr;
    nfdresult_t r = NFD_OpenDialog(&outPath, list, count, defPath(defaultPath));
    std::string result;
    if (r == NFD_OKAY && outPath) { result = outPath; NFD_FreePath(outPath); }
    NFD_Quit();
    return result;
}

std::string saveFileDialog(const char* /*title*/, const char* filterName,
                           const std::vector<std::string>& filters,
                           const std::string& defaultName,
                           const std::string& defaultPath) {
    if (NFD_Init() != NFD_OKAY) return std::string();
    std::string spec = joinSpec(filters);
    nfdfilteritem_t item{ filterName, spec.c_str() };
    const nfdfilteritem_t* list = spec.empty() ? nullptr : &item;
    nfdfiltersize_t count = spec.empty() ? 0 : 1;

    nfdchar_t*  outPath = nullptr;
    nfdresult_t r = NFD_SaveDialog(&outPath, list, count, defPath(defaultPath), defaultName.c_str());
    std::string result;
    if (r == NFD_OKAY && outPath) { result = outPath; NFD_FreePath(outPath); }
    NFD_Quit();
    return result;
}

std::vector<std::string> openMultipleFileDialog(const char* /*title*/, const char* filterName,
                                                const std::vector<std::string>& filters,
                                                const std::string& defaultPath) {
    std::vector<std::string> result;
    if (NFD_Init() != NFD_OKAY) return result;
    std::string spec = joinSpec(filters);
    nfdfilteritem_t item{ filterName, spec.c_str() };
    const nfdfilteritem_t* list = spec.empty() ? nullptr : &item;
    nfdfiltersize_t count = spec.empty() ? 0 : 1;

    const nfdpathset_t* paths = nullptr;
    if (NFD_OpenDialogMultiple(&paths, list, count, defPath(defaultPath)) == NFD_OKAY && paths) {
        nfdpathsetsize_t n = 0;
        if (NFD_PathSet_GetCount(paths, &n) == NFD_OKAY) {
            for (nfdpathsetsize_t i = 0; i < n; ++i) {
                nfdchar_t* p = nullptr;
                if (NFD_PathSet_GetPath(paths, i, &p) == NFD_OKAY && p) {
                    result.push_back(p);
                    NFD_PathSet_FreePath(p);
                }
            }
        }
        NFD_PathSet_Free(paths);
    }
    NFD_Quit();
    return result;
}

std::string pickFolderDialog(const char* /*title*/, const std::string& defaultPath) {
    if (NFD_Init() != NFD_OKAY) return std::string();
    nfdchar_t*  outPath = nullptr;
    nfdresult_t r = NFD_PickFolder(&outPath, defPath(defaultPath));
    std::string result;
    if (r == NFD_OKAY && outPath) { result = outPath; NFD_FreePath(outPath); }
    NFD_Quit();
    return result;
}

} // namespace oss
