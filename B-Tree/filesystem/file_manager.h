#pragma once
#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../models/file_record.h"
#include <functional>
#include <string>
#include <vector>

/*
 * FileManager
 * All methods call real Windows APIs to perform filesystem operations.
 * On success the caller receives a populated FileRecord; the Application
 * Controller then feeds it to the FileIndex.
 */
class FileManager {
public:
    // Callback fired after a successful operation so the controller can
    // update the index.
    std::function<void(FileRecord)>                       onFileCreated;
    std::function<void(FileRecord)>                       onFolderCreated;
    std::function<void(std::wstring /*old*/, FileRecord)> onRenamed;
    std::function<void(std::wstring /*path*/)>            onDeleted;
    std::function<void(std::wstring /*old*/, FileRecord)> onMoved;

    // Create an empty file at parentDir\filename.
    // Returns true and fills `out` with the new record.
    bool createFile(const std::wstring& parentDir,
                    const std::wstring& filename,
                    FileRecord& out);

    // Create a directory at parentDir\dirname.
    bool createFolder(const std::wstring& parentDir,
                      const std::wstring& dirname,
                      FileRecord& out);

    // Rename file/folder at `oldPath` to `newName` (base name only).
    bool rename(const std::wstring& oldPath,
                const std::wstring& newName,
                FileRecord& out);

    // Delete file/folder (send to Recycle Bin if `recycle` == true).
    bool deleteItem(const std::wstring& path, bool recycle = true);

    // Move file to destDir.
    bool move(const std::wstring& srcPath,
              const std::wstring& destDir,
              FileRecord& out);

    // Open the file/folder with the system default application.
    bool openWithShell(const std::wstring& path, HWND hwnd);

    // Fill a FileRecord from the file's current metadata.
    static bool statRecord(const std::wstring& path, FileRecord& out);

    // Enumerate direct children of a directory.
    static std::vector<FileRecord> listDirectory(const std::wstring& dir);

    // Helpers
    static std::wstring extensionOf(const std::wstring& filename);
    static std::wstring basenameOf(const std::wstring& path);
    static std::wstring parentOf(const std::wstring& path);
};

#endif // FILE_MANAGER_H
