#pragma once
#ifndef WINDOW_H
#define WINDOW_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>

#include "visualizer.h"
#include "../filesystem/file_manager.h"
#include "../index/file_index.h"
#include "../index/search_engine.h"

#include <memory>
#include <string>
#include <vector>

/*
 * AppWindow
 * The single top-level Win32 window that hosts:
 *   - Menu bar
 *   - Toolbar (New File / New Folder / Rename / Delete / Search)
 *   - Splitter: left = FolderTreeView, right = FileListView
 *   - B+ Tree Visualizer panel (docked at bottom)
 *   - Status bar
 *
 * It owns the FileIndex, FileManager, SearchEngine and Visualizer.
 * All user actions flow through AppWindow → FileManager → FileIndex → Visualizer.
 */
class AppWindow {
public:
    AppWindow();
    ~AppWindow();

    // Entry point: create window and run message loop.
    int run(HINSTANCE hInst, int nCmdShow);

private:
    HWND        hwnd_       = nullptr;
    HWND        toolbar_    = nullptr;
    HWND        statusBar_  = nullptr;
    HWND        folderTree_ = nullptr;
    HWND        fileList_   = nullptr;
    HWND        searchEdit_ = nullptr; // search box embedded in toolbar
    HINSTANCE   hInst_      = nullptr;
    HIMAGELIST  imgListSmall_ = nullptr;

    std::unique_ptr<Visualizer>    visualizer_;
    std::unique_ptr<FileIndex>     fileIndex_;
    std::unique_ptr<FileManager>   fileManager_;
    std::unique_ptr<SearchEngine>  searchEngine_;

    // Current directory shown in ListView
    std::wstring currentDir_;

    // Search debounce timer ID
    static constexpr UINT TIMER_SEARCH = 2001;
    static constexpr int  SEARCH_DEBOUNCE_MS = 300;
    bool searchActive_ = false;
    std::wstring lastSearchQuery_;

    // Context menu item IDs
    enum MenuCmd : UINT {
        IDM_FILE_NEW_FILE   = 1001,
        IDM_FILE_NEW_FOLDER = 1002,
        IDM_FILE_EXIT       = 1003,
        IDM_EDIT_RENAME     = 1004,
        IDM_EDIT_DELETE     = 1005,
        IDM_EDIT_MOVE       = 1006,
        IDM_EDIT_PROPERTIES = 1007,
        IDM_EDIT_OPEN       = 1008,
        IDM_VIEW_NAME       = 1010,
        IDM_VIEW_SIZE       = 1011,
        IDM_VIEW_DATE       = 1012,
        IDM_TOOLS_INDEX_DIR = 1020,
        IDM_TOOLS_REINDEX   = 1021,
        // Toolbar buttons
        IDT_NEW_FILE   = 2001,
        IDT_NEW_FOLDER = 2002,
        IDT_RENAME     = 2003,
        IDT_DELETE     = 2004,
        // Context menu copies
        IDM_CTX_OPEN       = 3001,
        IDM_CTX_RENAME     = 3002,
        IDM_CTX_DELETE     = 3003,
        IDM_CTX_MOVE       = 3004,
        IDM_CTX_PROPERTIES = 3005,
    };

    // Layout constants
    static constexpr int TOOLBAR_H     = 40;
    static constexpr int STATUS_H      = 24;
    static constexpr int VIS_PANEL_H   = 260;
    static constexpr int TREE_W_FRAC   = 25; // 25% of client width

    // Window class name
    static const wchar_t* WND_CLASS;

    // WinProc (static dispatch)
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp);

    // Initialization
    bool createMainWindow(HINSTANCE hInst, int nCmdShow);
    void createControls();
    void createMenu();
    void createToolbar();
    void createStatusBar();
    void createFolderTree();
    void createFileList();
    void initFileIndex();

    // Layout
    void doLayout();

    // Navigation
    void navigateTo(const std::wstring& dir);
    void populateFolderTree(HTREEITEM parent, const std::wstring& dir, int depth);
    void refreshFolderTree();
    void populateListView();
    void populateListViewFromRecords(const std::vector<FileRecord>& recs);
    void refreshVisualizer();
    void updateStatusBar();

    // Commands
    void cmdNewFile();
    void cmdNewFolder();
    void cmdRename();
    void cmdDelete();
    void cmdMove();
    void cmdOpen();
    void cmdProperties();
    void cmdIndexDirectory();
    void cmdReindex();
    void cmdSort(int col, bool ascending);

    // Search
    void onSearchChange();
    void performSearch(const std::wstring& query);
    void clearSearch();

    // Helpers
    std::wstring selectedListViewPath() const;
    std::wstring selectedListViewName() const;
    std::vector<std::wstring> selectedListViewPaths() const;
    FileRecord selectedListViewRecord() const;

    // Sort state
    int  sortCol_ = 0;
    bool sortAsc_ = true;

    // B+ Tree order configuration
    int treeOrderMin_ = 3;
    int treeOrderMax_ = 7;
    int treeOrderCur_ = 4;

    // Tree order rebuild (called when visualizer Apply button is clicked)
    void rebuildTreesWithOrder(int newOrder, int newMin, int newMax);

    // Folder tree item ↔ path mapping
    struct TreeItemData {
        std::wstring path;
    };
    std::vector<std::unique_ptr<TreeItemData>> treeItemData_;

    HTREEITEM addTreeItem(HTREEITEM parent, const std::wstring& text,
                         const std::wstring& path, bool hasChildren);
    void expandTreeItem(HTREEITEM item);

    // Context menu
    void showContextMenu(int screenX, int screenY);

    // DB path
    std::wstring dbPath_;
};

#endif // WINDOW_H