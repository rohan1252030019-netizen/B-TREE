#pragma once
#ifndef SEARCH_ENGINE_H
#define SEARCH_ENGINE_H

#include "file_index.h"
#include "../models/file_record.h"
#include <string>
#include <vector>

/*
 * SearchEngine
 * Thin facade over the B+ Tree query APIs.  All search operations
 * return sorted result vectors.  The GUI calls this layer exclusively;
 * it never touches the trees directly.
 */
class SearchEngine {
public:
    explicit SearchEngine(FileIndex& index);

    // Exact filename match (case-insensitive), O(log n)
    std::vector<FileRecord> exactSearch(const std::wstring& filename) const;

    // Prefix match on filename (case-insensitive), uses leaf scan
    std::vector<FileRecord> prefixSearch(const std::wstring& prefix) const;

    // Filter by extension (case-insensitive), linear scan of name tree
    std::vector<FileRecord> extensionFilter(const std::wstring& ext) const;

    // Size range query via size B+ Tree
    std::vector<FileRecord> sizeRange(uint64_t minBytes, uint64_t maxBytes) const;

    // Date-modified range query via date B+ Tree
    std::vector<FileRecord> dateRange(time_t from, time_t to) const;

    // Combined: prefix + optional extension filter
    std::vector<FileRecord> combinedSearch(const std::wstring& prefix,
                                           const std::wstring& extFilter) const;

private:
    FileIndex& index_;
};

#endif // SEARCH_ENGINE_H
