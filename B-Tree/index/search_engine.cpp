#include "search_engine.h"
#include <algorithm>
#include <cwctype>

static std::wstring toLower(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = static_cast<wchar_t>(towlower(c));
    return r;
}

SearchEngine::SearchEngine(FileIndex& index)
    : index_(index)
{}

std::vector<FileRecord> SearchEngine::exactSearch(const std::wstring& filename) const {
    return index_.exactSearch(filename);
}

std::vector<FileRecord> SearchEngine::prefixSearch(const std::wstring& prefix) const {
    if (prefix.empty()) return {};
    return index_.prefixSearch(prefix);
}

std::vector<FileRecord> SearchEngine::extensionFilter(const std::wstring& ext) const {
    if (ext.empty()) return {};
    return index_.extensionSearch(ext);
}

std::vector<FileRecord> SearchEngine::sizeRange(uint64_t minBytes, uint64_t maxBytes) const {
    return index_.sizeRange(minBytes, maxBytes);
}

std::vector<FileRecord> SearchEngine::dateRange(time_t from, time_t to) const {
    return index_.dateRange(from, to);
}

std::vector<FileRecord> SearchEngine::combinedSearch(const std::wstring& prefix,
                                                      const std::wstring& extFilter) const
{
    auto results = prefixSearch(prefix);
    if (!extFilter.empty()) {
        std::wstring lcExt = toLower(extFilter);
        // Remove leading dot if user typed ".txt"
        if (!lcExt.empty() && lcExt[0] == L'.') lcExt = lcExt.substr(1);
        results.erase(
            std::remove_if(results.begin(), results.end(),
                [&lcExt](const FileRecord& r) {
                    std::wstring re = r.extension;
                    for (auto& c : re) c = static_cast<wchar_t>(towlower(c));
                    return re != lcExt;
                }),
            results.end());
    }
    return results;
}
