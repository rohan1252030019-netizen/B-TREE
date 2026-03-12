#include "file_manager.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cwctype>
#include <string>

// ---------------------------------------------------------------------------
// Static helpers

std::wstring FileManager::extensionOf(const std::wstring& filename) {
    auto pos = filename.rfind(L'.');
    if (pos == std::wstring::npos || pos == 0) return L"";
    std::wstring ext = filename.substr(pos + 1);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext;
}

std::wstring FileManager::basenameOf(const std::wstring& path) {
    auto p1 = path.rfind(L'\\');
    auto p2 = path.rfind(L'/');
    size_t pos = std::wstring::npos;
    if (p1 != std::wstring::npos) pos = p1;
    if (p2 != std::wstring::npos) pos = (pos == std::wstring::npos) ? p2 : std::max(pos, p2);
    if (pos == std::wstring::npos) return path;
    return path.substr(pos + 1);
}

std::wstring FileManager::parentOf(const std::wstring& path) {
    std::wstring p = path;
    // Strip trailing backslash
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    auto pos = p.rfind(L'\\');
    if (pos == std::wstring::npos) return L"";
    return p.substr(0, pos);
}

static time_t fromFileTime(const FILETIME& ft) {
    ULONGLONG ull = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    if (ull < 116444736000000000ULL) return 0;
    return static_cast<time_t>((ull - 116444736000000000ULL) / 10000000ULL);
}

bool FileManager::statRecord(const std::wstring& path, FileRecord& out) {
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        return false;

    out.path        = path;
    out.filename    = basenameOf(path);
    out.isDirectory = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out.extension   = out.isDirectory ? L"" : extensionOf(out.filename);
    out.size        = out.isDirectory ? 0 :
                      (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
    out.created     = fromFileTime(attr.ftCreationTime);
    out.modified    = fromFileTime(attr.ftLastWriteTime);
    return true;
}

// ---------------------------------------------------------------------------

bool FileManager::createFile(const std::wstring& parentDir,
                             const std::wstring& filename,
                             FileRecord& out)
{
    std::wstring fullPath = parentDir;
    if (!fullPath.empty() && fullPath.back() != L'\\') fullPath += L'\\';
    fullPath += filename;

    HANDLE hFile = CreateFileW(
        fullPath.c_str(),
        GENERIC_WRITE,
        0, nullptr,
        CREATE_NEW,        // Fail if already exists
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE) return false;
    CloseHandle(hFile);

    if (!statRecord(fullPath, out)) return false;
    if (onFileCreated) onFileCreated(out);
    return true;
}

bool FileManager::createFolder(const std::wstring& parentDir,
                               const std::wstring& dirname,
                               FileRecord& out)
{
    std::wstring fullPath = parentDir;
    if (!fullPath.empty() && fullPath.back() != L'\\') fullPath += L'\\';
    fullPath += dirname;

    if (!CreateDirectoryW(fullPath.c_str(), nullptr)) return false;
    if (!statRecord(fullPath, out)) return false;
    out.isDirectory = true;
    if (onFolderCreated) onFolderCreated(out);
    return true;
}

bool FileManager::rename(const std::wstring& oldPath,
                         const std::wstring& newName,
                         FileRecord& out)
{
    std::wstring parent  = parentOf(oldPath);
    std::wstring newPath = parent;
    if (!newPath.empty() && newPath.back() != L'\\') newPath += L'\\';
    newPath += newName;

    // MoveFileEx for atomic rename
    if (!MoveFileExW(oldPath.c_str(), newPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;

    if (!statRecord(newPath, out)) return false;
    if (onRenamed) onRenamed(oldPath, out);
    return true;
}

bool FileManager::deleteItem(const std::wstring& path, bool recycle) {
    if (recycle) {
        // Build double-null terminated string required by SHFileOperation
        std::vector<wchar_t> buf(path.size() + 2, L'\0');
        std::copy(path.begin(), path.end(), buf.begin());

        SHFILEOPSTRUCTW shfo = {};
        shfo.wFunc  = FO_DELETE;
        shfo.pFrom  = buf.data();
        shfo.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
        if (SHFileOperationW(&shfo) != 0) return false;
    } else {
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) return false;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            if (!RemoveDirectoryW(path.c_str())) return false;
        } else {
            if (!DeleteFileW(path.c_str())) return false;
        }
    }
    if (onDeleted) onDeleted(path);
    return true;
}

bool FileManager::move(const std::wstring& srcPath,
                       const std::wstring& destDir,
                       FileRecord& out)
{
    std::wstring name     = basenameOf(srcPath);
    std::wstring destPath = destDir;
    if (!destPath.empty() && destPath.back() != L'\\') destPath += L'\\';
    destPath += name;

    if (!MoveFileExW(srcPath.c_str(), destPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED))
        return false;

    if (!statRecord(destPath, out)) return false;
    if (onMoved) onMoved(srcPath, out);
    return true;
}

bool FileManager::openWithShell(const std::wstring& path, HWND hwnd) {
    HINSTANCE result = ShellExecuteW(hwnd, L"open", path.c_str(),
                                     nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

std::vector<FileRecord> FileManager::listDirectory(const std::wstring& dir) {
    std::vector<FileRecord> result;
    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L"*";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return result;

    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;

        FileRecord rec;
        rec.filename    = name;
        rec.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        rec.extension   = rec.isDirectory ? L"" : extensionOf(name);
        rec.size        = rec.isDirectory ? 0 :
                          (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        rec.created     = fromFileTime(fd.ftCreationTime);
        rec.modified    = fromFileTime(fd.ftLastWriteTime);

        std::wstring fullPath = dir;
        if (!fullPath.empty() && fullPath.back() != L'\\') fullPath += L'\\';
        fullPath += name;
        rec.path = fullPath;

        result.push_back(rec);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return result;
}
