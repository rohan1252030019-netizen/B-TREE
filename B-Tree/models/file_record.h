#pragma once
#ifndef FILE_RECORD_H
#define FILE_RECORD_H

#include <string>
#include <ctime>
#include <cstdint>

// POD-style struct holding all metadata for one file or directory entry.
// This is the payload stored in every B+ Tree leaf node and every SQLite row.
struct FileRecord {
    int64_t     id;          // SQLite rowid (0 = not yet persisted)
    std::wstring filename;   // Base name without path (e.g. "report.txt")
    std::wstring extension;  // Lower-case extension without dot (e.g. "txt")
    uint64_t    size;        // File size in bytes (0 for directories)
    std::wstring path;       // Full absolute path (unique key in DB)
    time_t      created;     // Unix timestamp
    time_t      modified;    // Unix timestamp
    bool        isDirectory; // true if this entry is a folder

    FileRecord()
        : id(0), size(0), created(0), modified(0), isDirectory(false)
    {}

    FileRecord(const std::wstring& filename,
               const std::wstring& extension,
               uint64_t size,
               const std::wstring& path,
               time_t created,
               time_t modified,
               bool isDirectory = false)
        : id(0)
        , filename(filename)
        , extension(extension)
        , size(size)
        , path(path)
        , created(created)
        , modified(modified)
        , isDirectory(isDirectory)
    {}

    bool operator==(const FileRecord& o) const { return path == o.path; }
    bool operator!=(const FileRecord& o) const { return path != o.path; }
};

#endif // FILE_RECORD_H
