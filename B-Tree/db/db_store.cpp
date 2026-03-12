#include "db_store.h"
#include <windows.h>
#include <shlwapi.h>
#include <stdexcept>
#include <cstring>

// Helper: convert wstring to UTF-8
static std::string wToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

// Helper: convert UTF-8 to wstring
static std::wstring utf8ToW(const char* s) {
    if (!s || !*s) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &ws[0], len);
    return ws;
}

DBStore::DBStore() : db_(nullptr) {}

DBStore::~DBStore() {
    close();
}

bool DBStore::open(const std::wstring& dbPath) {
    close();
    std::string path = wToUtf8(dbPath);
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    // Enable WAL mode for better concurrency
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    return true;
}

void DBStore::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool DBStore::ensureSchema() {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS file_index ("
        "  id        INTEGER PRIMARY KEY,"
        "  filename  TEXT    NOT NULL,"
        "  extension TEXT,"
        "  size      INTEGER,"
        "  path      TEXT    NOT NULL UNIQUE,"
        "  created   INTEGER,"
        "  modified  INTEGER,"
        "  is_dir    INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_filename  ON file_index(filename);"
        "CREATE INDEX IF NOT EXISTS idx_extension ON file_index(extension);"
        "CREATE INDEX IF NOT EXISTS idx_size      ON file_index(size);";
    return exec(sql);
}

bool DBStore::loadAll(std::vector<FileRecord>& out) {
    out.clear();
    const char* sql = "SELECT id,filename,extension,size,path,created,modified,is_dir "
                      "FROM file_index ORDER BY filename;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(rowToRecord(stmt));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool DBStore::insertRecord(FileRecord& rec) {
    const char* sql =
        "INSERT OR REPLACE INTO file_index(filename,extension,size,path,created,modified,is_dir)"
        " VALUES(?,?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    std::string fn  = wToUtf8(rec.filename);
    std::string ext = wToUtf8(rec.extension);
    std::string p   = wToUtf8(rec.path);
    sqlite3_bind_text(stmt, 1, fn.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(rec.size));
    sqlite3_bind_text(stmt, 4, p.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(rec.created));
    sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(rec.modified));
    sqlite3_bind_int(stmt,  7, rec.isDirectory ? 1 : 0);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    rec.id = sqlite3_last_insert_rowid(db_);
    return true;
}

bool DBStore::updateRecord(const FileRecord& rec) {
    const char* sql =
        "UPDATE file_index SET filename=?,extension=?,size=?,created=?,modified=?,is_dir=?"
        " WHERE path=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    std::string fn  = wToUtf8(rec.filename);
    std::string ext = wToUtf8(rec.extension);
    std::string p   = wToUtf8(rec.path);
    sqlite3_bind_text(stmt, 1, fn.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(rec.size));
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(rec.created));
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(rec.modified));
    sqlite3_bind_int(stmt,  6, rec.isDirectory ? 1 : 0);
    sqlite3_bind_text(stmt, 7, p.c_str(),   -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool DBStore::deleteRecord(const std::wstring& path) {
    std::string p = wToUtf8(path);
    const char* sql = "DELETE FROM file_index WHERE path=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(stmt, 1, p.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool DBStore::deleteRecordById(int64_t id) {
    const char* sql = "DELETE FROM file_index WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(id));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool DBStore::updatePath(const std::wstring& oldPath, const std::wstring& newPath,
                         const std::wstring& newFilename, const std::wstring& newExtension)
{
    const char* sql =
        "UPDATE file_index SET path=?,filename=?,extension=? WHERE path=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    std::string np  = wToUtf8(newPath);
    std::string nfn = wToUtf8(newFilename);
    std::string nex = wToUtf8(newExtension);
    std::string op  = wToUtf8(oldPath);
    sqlite3_bind_text(stmt, 1, np.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, nfn.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, nex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, op.c_str(),  -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

int DBStore::pruneStaleRecords(std::function<void(const std::wstring&)> removedCb) {
    // Load all paths
    const char* sql = "SELECT path FROM file_index;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;

    std::vector<std::wstring> stale;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::wstring path = utf8ToW(p);
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES)
            stale.push_back(path);
    }
    sqlite3_finalize(stmt);

    int count = 0;
    for (auto& p : stale) {
        deleteRecord(p);
        if (removedCb) removedCb(p);
        ++count;
    }
    return count;
}

std::string DBStore::lastError() const { return lastError_; }

bool DBStore::exec(const char* sql) {
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        if (errmsg) {
            lastError_ = errmsg;
            sqlite3_free(errmsg);
        }
        return false;
    }
    return true;
}

FileRecord DBStore::rowToRecord(sqlite3_stmt* stmt) {
    FileRecord r;
    r.id          = sqlite3_column_int64(stmt, 0);
    const char* fn  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const char* ext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    r.size        = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
    const char* p   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    r.created     = static_cast<time_t>(sqlite3_column_int64(stmt, 5));
    r.modified    = static_cast<time_t>(sqlite3_column_int64(stmt, 6));
    r.isDirectory = sqlite3_column_int(stmt, 7) != 0;
    r.filename    = utf8ToW(fn  ? fn  : "");
    r.extension   = utf8ToW(ext ? ext : "");
    r.path        = utf8ToW(p   ? p   : "");
    return r;
}
