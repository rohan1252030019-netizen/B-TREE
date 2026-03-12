#pragma once
#ifndef FILE_INDEX_H
#define FILE_INDEX_H

#include "../bplustree/bplustree.h"
#include "../models/file_record.h"
#include "../db/db_store.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

/*
 * FileIndex
 * Maintains three B+ Trees over the same set of FileRecord objects:
 *   - nameTree_:  keyed by lowercase filename
 *   - sizeTree_:  keyed by file size (uint64_t)
 *   - dateTree_:  keyed by modified timestamp (time_t → uint64_t)
 *
 * Every mutation is written to both the in-memory trees AND the SQLite DB.
 *
 * The class is thread-safe at the B+ Tree level (each tree has its own SRWLOCK).
 * SQLite writes are serialized by a separate critical section.
 */
class FileIndex {
public:
    using NameTree = BPlusTree<std::wstring, FileRecord, 4>;
    using SizeTree = BPlusTree<uint64_t,     FileRecord, 4>;
    using DateTree = BPlusTree<uint64_t,     FileRecord, 4>;

    FileIndex();
    ~FileIndex();

    // Initialize: open DB, ensure schema, load all records, spawn integrity thread.
    // dbPath: full path to file_index.db.
    // Returns true on success.
    bool initialize(const std::wstring& dbPath);
    void shutdown();

    // -----------------------------------------------------------------------
    // Mutation API (writes to trees + DB atomically)
    bool addRecord(FileRecord& rec);
    bool removeRecord(const std::wstring& path);
    bool updateRecord(const FileRecord& oldRec, FileRecord& newRec);
    bool renameRecord(const std::wstring& oldPath, FileRecord& newRec);

    // -----------------------------------------------------------------------
    // Read API
    std::vector<FileRecord> getAllSortedByName(bool ascending = true) const;
    std::vector<FileRecord> getAllSortedBySize(bool ascending = true) const;
    std::vector<FileRecord> getAllSortedByDate(bool ascending = true) const;

    std::vector<FileRecord> getByDirectory(const std::wstring& dirPath) const;

    // Search delegates to SearchEngine; exposed here for convenience
    std::vector<FileRecord> prefixSearch(const std::wstring& prefix) const;
    std::vector<FileRecord> exactSearch(const std::wstring& filename) const;
    std::vector<FileRecord> sizeRange(uint64_t lo, uint64_t hi) const;
    std::vector<FileRecord> dateRange(time_t lo, time_t hi) const;
    std::vector<FileRecord> extensionSearch(const std::wstring& ext) const;

    // -----------------------------------------------------------------------
    // Tree access for visualizer
    NameTree& nameTree() { return *nameTree_; }
    SizeTree& sizeTree() { return *sizeTree_; }
    DateTree& dateTree() { return *dateTree_; }

    // -----------------------------------------------------------------------
    // Statistics
    size_t totalRecords() const;
    int    treeHeight(int treeIdx) const; // 0=name, 1=size, 2=date

    // Callback fired when integrity sweep removes a stale entry
    std::function<void(const std::wstring& removedPath)> onStalePruned;

private:
    std::unique_ptr<NameTree> nameTree_;
    std::unique_ptr<SizeTree> sizeTree_;
    std::unique_ptr<DateTree> dateTree_;
    DBStore                   db_;
    CRITICAL_SECTION          dbLock_;

    // Background integrity thread
    HANDLE integrityThread_ = nullptr;
    volatile bool stopIntegrity_ = false;
    static DWORD WINAPI integrityThreadProc(LPVOID param);
    void runIntegritySweep();

    // Internal helpers
    void insertIntoTrees(const FileRecord& rec);
    void removeFromTrees(const FileRecord& rec);

    static std::wstring lowerCase(const std::wstring& s);
    static uint64_t dateKey(time_t t) { return static_cast<uint64_t>(t); }
};

#endif // FILE_INDEX_H
