#pragma once
#ifndef DB_STORE_H
#define DB_STORE_H

#include "../models/file_record.h"
#include "sqlite3.h"
#include <string>
#include <vector>
#include <functional>

/*
 * DBStore
 * Thin RAII wrapper around the SQLite amalgamation.
 * All methods are synchronous and must be called from a single thread
 * (or externally synchronized).  The Index Engine is responsible for
 * thread safety above this layer.
 */
class DBStore {
public:
    DBStore();
    ~DBStore();

    // Open / create the database file.  Returns true on success.
    bool open(const std::wstring& dbPath);
    void close();
    bool isOpen() const { return db_ != nullptr; }

    // Ensure schema exists (CREATE TABLE IF NOT EXISTS).
    bool ensureSchema();

    // Load all rows. Used at startup to rebuild in-memory B+ Trees.
    bool loadAll(std::vector<FileRecord>& out);

    // CRUD
    bool insertRecord(FileRecord& rec);         // fills rec.id on success
    bool updateRecord(const FileRecord& rec);   // matched by path
    bool deleteRecord(const std::wstring& path);
    bool deleteRecordById(int64_t id);

    // Update path after rename / move
    bool updatePath(const std::wstring& oldPath, const std::wstring& newPath,
                    const std::wstring& newFilename, const std::wstring& newExtension);

    // Integrity: delete rows whose paths no longer exist on disk
    // Calls removedCb for each removed path so the caller can evict from B+ Trees.
    int pruneStaleRecords(std::function<void(const std::wstring&)> removedCb);

    // Helpers
    std::string lastError() const;

private:
    sqlite3*    db_ = nullptr;
    std::string lastError_;

    bool exec(const char* sql);
    static FileRecord rowToRecord(sqlite3_stmt* stmt);
};

#endif // DB_STORE_H
