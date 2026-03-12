#include "file_index.h"
#include <algorithm>
#include <cctype>
#include <cwctype>

// ---------------------------------------------------------------------------
FileIndex::FileIndex()
    : nameTree_(std::make_unique<NameTree>())
    , sizeTree_(std::make_unique<SizeTree>())
    , dateTree_(std::make_unique<DateTree>())
    , stopIntegrity_(false)
{
    InitializeCriticalSection(&dbLock_);
}

FileIndex::~FileIndex() {
    shutdown();
    DeleteCriticalSection(&dbLock_);
}

bool FileIndex::initialize(const std::wstring& dbPath) {
    EnterCriticalSection(&dbLock_);
    bool ok = db_.open(dbPath) && db_.ensureSchema();
    if (!ok) {
        LeaveCriticalSection(&dbLock_);
        return false;
    }

    // Load all records into memory trees
    std::vector<FileRecord> records;
    db_.loadAll(records);
    LeaveCriticalSection(&dbLock_);

    for (auto& rec : records)
        insertIntoTrees(rec);

    // Spawn background integrity sweep
    stopIntegrity_ = false;
    integrityThread_ = CreateThread(nullptr, 0, integrityThreadProc, this, 0, nullptr);

    return true;
}

void FileIndex::shutdown() {
    stopIntegrity_ = true;
    if (integrityThread_) {
        WaitForSingleObject(integrityThread_, 5000);
        CloseHandle(integrityThread_);
        integrityThread_ = nullptr;
    }
    EnterCriticalSection(&dbLock_);
    db_.close();
    LeaveCriticalSection(&dbLock_);
}

// ---------------------------------------------------------------------------
// Mutations

bool FileIndex::addRecord(FileRecord& rec) {
    // Write to DB first
    EnterCriticalSection(&dbLock_);
    bool ok = db_.insertRecord(rec); // fills rec.id
    LeaveCriticalSection(&dbLock_);
    if (!ok) return false;

    insertIntoTrees(rec);
    return true;
}

bool FileIndex::removeRecord(const std::wstring& path) {
    // Find record via prefix search (need the full rec for tree removal keys)
    std::wstring lcPath = lowerCase(path);
    auto results = nameTree_->traverseAscending();
    FileRecord target;
    bool found = false;
    for (auto& r : results) {
        if (lowerCase(r.path) == lcPath) {
            target = r;
            found  = true;
            break;
        }
    }
    if (!found) {
        // Try exact case
        for (auto& r : results) {
            if (r.path == path) {
                target = r;
                found  = true;
                break;
            }
        }
    }
    if (!found) return false;

    removeFromTrees(target);

    EnterCriticalSection(&dbLock_);
    db_.deleteRecord(path);
    LeaveCriticalSection(&dbLock_);

    return true;
}

bool FileIndex::updateRecord(const FileRecord& oldRec, FileRecord& newRec) {
    removeFromTrees(oldRec);
    insertIntoTrees(newRec);

    EnterCriticalSection(&dbLock_);
    bool ok = db_.updateRecord(newRec);
    LeaveCriticalSection(&dbLock_);
    return ok;
}

bool FileIndex::renameRecord(const std::wstring& oldPath, FileRecord& newRec) {
    // Find old record
    std::vector<FileRecord> all = nameTree_->traverseAscending();
    FileRecord oldRec;
    bool found = false;
    for (auto& r : all) {
        if (r.path == oldPath) {
            oldRec = r;
            found  = true;
            break;
        }
    }
    if (!found) return false;

    removeFromTrees(oldRec);
    insertIntoTrees(newRec);

    EnterCriticalSection(&dbLock_);
    bool ok = db_.updatePath(oldPath, newRec.path, newRec.filename, newRec.extension);
    LeaveCriticalSection(&dbLock_);
    return ok;
}

// ---------------------------------------------------------------------------
// Read API

std::vector<FileRecord> FileIndex::getAllSortedByName(bool ascending) const {
    return ascending ? nameTree_->traverseAscending()
                     : nameTree_->traverseDescending();
}

std::vector<FileRecord> FileIndex::getAllSortedBySize(bool ascending) const {
    return ascending ? sizeTree_->traverseAscending()
                     : sizeTree_->traverseDescending();
}

std::vector<FileRecord> FileIndex::getAllSortedByDate(bool ascending) const {
    return ascending ? dateTree_->traverseAscending()
                     : dateTree_->traverseDescending();
}

std::vector<FileRecord> FileIndex::getByDirectory(const std::wstring& dirPath) const {
    // Walk name tree and filter by parent path
    std::vector<FileRecord> all = nameTree_->traverseAscending();
    std::vector<FileRecord> result;
    std::wstring lcDir = lowerCase(dirPath);
    // Normalise trailing backslash
    if (!lcDir.empty() && lcDir.back() != L'\\') lcDir += L'\\';

    for (auto& r : all) {
        std::wstring lcPath = lowerCase(r.path);
        // Direct children only: path starts with dir and has no further backslash
        if (lcPath.size() > lcDir.size() &&
            lcPath.substr(0, lcDir.size()) == lcDir)
        {
            std::wstring rest = lcPath.substr(lcDir.size());
            // No backslash in remainder → direct child
            if (rest.find(L'\\') == std::wstring::npos)
                result.push_back(r);
        }
    }
    return result;
}

std::vector<FileRecord> FileIndex::prefixSearch(const std::wstring& prefix) const {
    std::wstring lc = lowerCase(prefix);
    return nameTree_->prefixSearch(lc);
}

std::vector<FileRecord> FileIndex::exactSearch(const std::wstring& filename) const {
    // Range query covers all tiebreaks for this key
    std::wstring lc = lowerCase(filename);
    return nameTree_->rangeQuery(lc, lc);
}

std::vector<FileRecord> FileIndex::sizeRange(uint64_t lo, uint64_t hi) const {
    return sizeTree_->rangeQuery(lo, hi);
}

std::vector<FileRecord> FileIndex::dateRange(time_t lo, time_t hi) const {
    return dateTree_->rangeQuery(dateKey(lo), dateKey(hi));
}

std::vector<FileRecord> FileIndex::extensionSearch(const std::wstring& ext) const {
    // Walk name tree filtering by extension (secondary tree not separately maintained
    // but extension field is indexed in SQLite for disk queries; in-memory we filter)
    std::wstring lcExt = lowerCase(ext);
    std::vector<FileRecord> all = nameTree_->traverseAscending();
    std::vector<FileRecord> result;
    for (auto& r : all) {
        if (lowerCase(r.extension) == lcExt)
            result.push_back(r);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Stats

size_t FileIndex::totalRecords() const {
    return nameTree_->size();
}

int FileIndex::treeHeight(int treeIdx) const {
    switch (treeIdx) {
    case 0: return nameTree_->height();
    case 1: return sizeTree_->height();
    case 2: return dateTree_->height();
    default: return 0;
    }
}

// ---------------------------------------------------------------------------
// Internal

void FileIndex::insertIntoTrees(const FileRecord& rec) {
    std::wstring lcName = lowerCase(rec.filename);
    nameTree_->insert(lcName,        rec.path, rec);
    sizeTree_->insert(rec.size,      rec.path, rec);
    dateTree_->insert(dateKey(rec.modified), rec.path, rec);
}

void FileIndex::removeFromTrees(const FileRecord& rec) {
    std::wstring lcName = lowerCase(rec.filename);
    nameTree_->remove(lcName,        rec.path);
    sizeTree_->remove(rec.size,      rec.path);
    dateTree_->remove(dateKey(rec.modified), rec.path);
}

std::wstring FileIndex::lowerCase(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = static_cast<wchar_t>(towlower(c));
    return r;
}

// ---------------------------------------------------------------------------
// Background integrity thread

DWORD WINAPI FileIndex::integrityThreadProc(LPVOID param) {
    FileIndex* self = static_cast<FileIndex*>(param);
    // Small initial delay to let the main window open first
    Sleep(2000);
    if (!self->stopIntegrity_)
        self->runIntegritySweep();
    return 0;
}

void FileIndex::runIntegritySweep() {
    EnterCriticalSection(&dbLock_);
    db_.pruneStaleRecords([this](const std::wstring& stalePath) {
        // Remove from in-memory trees too
        // We can't call removeFromTrees directly (need the full record for keys)
        // so we iterate; performance is acceptable since this is a background op
        std::vector<FileRecord> all = nameTree_->traverseAscending();
        for (auto& r : all) {
            if (r.path == stalePath) {
                removeFromTrees(r);
                break;
            }
        }
        if (onStalePruned) onStalePruned(stalePath);
    });
    LeaveCriticalSection(&dbLock_);
}
