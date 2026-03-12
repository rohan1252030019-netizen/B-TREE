#include "window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <ctime>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

// WM_USER messages shared with Visualizer
static constexpr UINT WM_TREE_SELECTOR  = WM_USER + 200; // visualizer tree-tab clicked
static constexpr UINT WM_INDEX_COMPLETE = WM_USER + 201; // background index finished
static constexpr UINT WM_ORDER_CHANGE   = WM_USER + 202; // NEW: visualizer Apply button
//   wParam  = new curOrder
//   lParam  = MAKELPARAM(minOrder, maxOrder)

const wchar_t* AppWindow::WND_CLASS = L"FileExplorerBPlusTree";

// ---------------------------------------------------------------------------
// Utility helpers
// ---------------------------------------------------------------------------

static std::wstring fmtSize(uint64_t sz) {
    if (sz == 0) return L"0 B";
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    int u = 0;
    double v = static_cast<double>(sz);
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    std::wostringstream oss;
    if (u == 0) oss << static_cast<uint64_t>(v) << L" B";
    else        oss << std::fixed << std::setprecision(1) << v << L" " << units[u];
    return oss.str();
}

static std::wstring fmtTime(time_t t) {
    if (t == 0) return L"\u2014";
    tm tm_info = {};
    localtime_s(&tm_info, &t);
    wchar_t buf[64];
    wcsftime(buf, 64, L"%Y-%m-%d %H:%M", &tm_info);
    return buf;
}

// ---------------------------------------------------------------------------
// In-memory dialog helper
// ---------------------------------------------------------------------------

struct InputDlgParam {
    const wchar_t* title;
    const wchar_t* label;
    const wchar_t* defaultText;
    std::wstring   result;
};

static INT_PTR CALLBACK InputDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    InputDlgParam* p =
        reinterpret_cast<InputDlgParam*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        p = reinterpret_cast<InputDlgParam*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(p));
        SetWindowTextW(dlg, p->title);

        HINSTANCE hInst = reinterpret_cast<HINSTANCE>(
            GetWindowLongPtrW(dlg, GWLP_HINSTANCE));

        CreateWindowW(L"STATIC", p->label,
            WS_CHILD | WS_VISIBLE,
            10, 14, 92, 20, dlg, (HMENU)(UINT_PTR)200, hInst, nullptr);

        HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", p->defaultText,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            108, 12, 222, 22, dlg, (HMENU)(UINT_PTR)100, hInst, nullptr);

        CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            140, 46, 88, 26, dlg, (HMENU)(UINT_PTR)IDOK, hInst, nullptr);

        CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            236, 46, 88, 26, dlg, (HMENU)(UINT_PTR)IDCANCEL, hInst, nullptr);

        SendMessageW(hEdit, EM_SETSEL, 0, -1);
        SetFocus(hEdit);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[MAX_PATH] = {};
            GetDlgItemTextW(dlg, 100, buf, MAX_PATH);
            if (p) p->result = buf;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static bool showInputDialog(HWND parent, HINSTANCE hInst,
                             const wchar_t* title,
                             const wchar_t* label,
                             const wchar_t* defaultText,
                             std::wstring& out)
{
    std::vector<WORD> t;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME |
                  DS_CENTER | DS_SETFONT;
    t.push_back(LOWORD(style));  t.push_back(HIWORD(style));
    t.push_back(0); t.push_back(0);
    t.push_back(0);
    t.push_back(0); t.push_back(0);
    t.push_back(205); t.push_back(50);
    t.push_back(0x0000);
    t.push_back(0x0000);
    t.push_back(0x0000);
    t.push_back(9);
    for (const wchar_t* c = L"Segoe UI"; *c; ++c)
        t.push_back(static_cast<WORD>(*c));
    t.push_back(0);

    InputDlgParam param{ title, label, defaultText, {} };
    INT_PTR res = DialogBoxIndirectParamW(
        hInst,
        reinterpret_cast<LPCDLGTEMPLATEW>(t.data()),
        parent,
        InputDlgProc,
        reinterpret_cast<LPARAM>(&param));

    if (res == IDOK && !param.result.empty()) {
        out = param.result;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Snapshot → NodeDraw helpers
// ---------------------------------------------------------------------------

static void buildNodeDrawList(
    const BPlusTree<std::wstring, FileRecord, 4>::NodeSnapshot& snap,
    std::vector<Visualizer::NodeDraw>& list,
    std::function<std::wstring(const std::wstring&)> fmtKey)
{
    Visualizer::NodeDraw nd;
    nd.isLeaf  = snap.isLeaf;
    nd.numKeys = snap.numKeys;
    nd.nodePtr = snap.nodePtr;
    nd.hasNext = snap.hasNext;
    for (auto& k : snap.keys)
        nd.keyStrings.push_back(fmtKey(k));
    list.push_back(nd);
    int myIdx = static_cast<int>(list.size()) - 1;
    for (auto& child : snap.children) {
        int childStart = static_cast<int>(list.size());
        buildNodeDrawList(child, list, fmtKey);
        list[myIdx].childPtrs.push_back(&list[childStart]);
    }
}

static void buildNodeDrawListU64(
    const BPlusTree<uint64_t, FileRecord, 4>::NodeSnapshot& snap,
    std::vector<Visualizer::NodeDraw>& list,
    std::function<std::wstring(uint64_t)> fmtKey)
{
    Visualizer::NodeDraw nd;
    nd.isLeaf  = snap.isLeaf;
    nd.numKeys = snap.numKeys;
    nd.nodePtr = snap.nodePtr;
    nd.hasNext = snap.hasNext;
    for (auto& k : snap.keys)
        nd.keyStrings.push_back(fmtKey(k));
    list.push_back(nd);
    int myIdx = static_cast<int>(list.size()) - 1;
    for (auto& child : snap.children) {
        int childStart = static_cast<int>(list.size());
        buildNodeDrawListU64(child, list, fmtKey);
        list[myIdx].childPtrs.push_back(&list[childStart]);
    }
}

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

AppWindow::AppWindow()
    : visualizer_(std::make_unique<Visualizer>())
    , fileIndex_(std::make_unique<FileIndex>())
    , fileManager_(std::make_unique<FileManager>())
{
}

AppWindow::~AppWindow() {
    if (fileIndex_) fileIndex_->shutdown();
    if (imgListSmall_) { ImageList_Destroy(imgListSmall_); imgListSmall_ = nullptr; }
}

int AppWindow::run(HINSTANCE hInst, int nCmdShow) {
    hInst_ = hInst;

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (!createMainWindow(hInst, nCmdShow)) {
        CoUninitialize();
        return 1;
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}

// ---------------------------------------------------------------------------
// Window creation
// ---------------------------------------------------------------------------

bool AppWindow::createMainWindow(HINSTANCE hInst, int nCmdShow) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WND_CLASS;
    wc.hIconSm       = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return false;

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW, WND_CLASS,
        L"FileExplorer B+ Tree",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 900,
        nullptr, nullptr, hInst, this);

    if (!hwnd_) return false;
    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);
    return true;
}

void AppWindow::createControls() {
    createMenu();
    createToolbar();
    createStatusBar();
    createFolderTree();
    createFileList();

    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    int visY = rc.bottom - VIS_PANEL_H - STATUS_H;
    if (visY < TOOLBAR_H + 10) visY = TOOLBAR_H + 10;
    visualizer_->create(hwnd_, 0, visY, rc.right, VIS_PANEL_H, hInst_);

    // Push initial order config so the spinners show correct values
    Visualizer::OrderConfig initCfg;
    initCfg.minOrder = treeOrderMin_;
    initCfg.maxOrder = treeOrderMax_;
    initCfg.curOrder = treeOrderCur_;
    visualizer_->setOrderConfig(initCfg);

    doLayout();
    initFileIndex();
}

// ---------------------------------------------------------------------------
void AppWindow::createMenu() {
    HMENU hMenu  = CreateMenu();
    HMENU hFile  = CreatePopupMenu();
    HMENU hEdit  = CreatePopupMenu();
    HMENU hView  = CreatePopupMenu();
    HMENU hTools = CreatePopupMenu();

    AppendMenuW(hFile, MF_STRING,    IDM_FILE_NEW_FILE,   L"New &File\tCtrl+N");
    AppendMenuW(hFile, MF_STRING,    IDM_FILE_NEW_FOLDER, L"New Fo&lder\tCtrl+Shift+N");
    AppendMenuW(hFile, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hFile, MF_STRING,    IDM_FILE_EXIT,       L"E&xit");

    AppendMenuW(hEdit, MF_STRING,    IDM_EDIT_OPEN,       L"&Open\tEnter");
    AppendMenuW(hEdit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hEdit, MF_STRING,    IDM_EDIT_RENAME,     L"&Rename\tF2");
    AppendMenuW(hEdit, MF_STRING,    IDM_EDIT_DELETE,     L"&Delete\tDel");
    AppendMenuW(hEdit, MF_STRING,    IDM_EDIT_MOVE,       L"&Move to...");
    AppendMenuW(hEdit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hEdit, MF_STRING,    IDM_EDIT_PROPERTIES, L"&Properties");

    AppendMenuW(hView, MF_STRING,    IDM_VIEW_NAME,       L"Sort by &Name");
    AppendMenuW(hView, MF_STRING,    IDM_VIEW_SIZE,       L"Sort by &Size");
    AppendMenuW(hView, MF_STRING,    IDM_VIEW_DATE,       L"Sort by &Date Modified");

    AppendMenuW(hTools, MF_STRING,   IDM_TOOLS_INDEX_DIR, L"&Index Directory...");
    AppendMenuW(hTools, MF_STRING,   IDM_TOOLS_REINDEX,   L"&Reindex Current Directory");

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFile,  L"&File");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hEdit,  L"&Edit");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hView,  L"&View");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hTools, L"&Tools");
    SetMenu(hwnd_, hMenu);
}

void AppWindow::createToolbar() {
    toolbar_ = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NODIVIDER,
        0, 0, 0, TOOLBAR_H, hwnd_, nullptr, hInst_, nullptr);

    SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(toolbar_, TB_SETBITMAPSIZE, 0, MAKELONG(16, 16));

    TBADDBITMAP tbab = {};
    tbab.hInst = HINST_COMMCTRL;
    tbab.nID   = IDB_STD_SMALL_COLOR;
    SendMessageW(toolbar_, TB_ADDBITMAP, 0, (LPARAM)&tbab);

    TBBUTTON btns[5] = {};
    btns[0] = { STD_FILENEW,    IDT_NEW_FILE,   TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, (INT_PTR)L"New File"   };
    btns[1] = { STD_FILESAVE,   IDT_NEW_FOLDER, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, (INT_PTR)L"New Folder" };
    btns[2] = { 0,              0,              0,               BTNS_SEP,    {0}, 0, 0                       };
    btns[3] = { STD_DELETE,     IDT_DELETE,     TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, (INT_PTR)L"Delete"     };
    btns[4] = { STD_PROPERTIES, IDT_RENAME,     TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, (INT_PTR)L"Rename"     };
    SendMessageW(toolbar_, TB_ADDBUTTONS, 5, (LPARAM)btns);
    SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);

    RECT tbrc = {};
    SendMessageW(toolbar_, TB_GETITEMRECT, 4, (LPARAM)&tbrc);
    int searchX = tbrc.right + 16;

    CreateWindowW(L"STATIC", L"Search:",
        WS_CHILD | WS_VISIBLE,
        searchX, 11, 52, 18,
        toolbar_, (HMENU)(UINT_PTR)9000, hInst_, nullptr);

    searchEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        searchX + 56, 9, 200, 22,
        toolbar_, (HMENU)(UINT_PTR)9001, hInst_, nullptr);
    SendMessageW(searchEdit_, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search files...");
}

void AppWindow::createStatusBar() {
    statusBar_ = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd_, nullptr, hInst_, nullptr);
    int parts[] = { 200, 400, 600, -1 };
    SendMessageW(statusBar_, SB_SETPARTS, 4, (LPARAM)parts);
}

void AppWindow::createFolderTree() {
    folderTree_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE |
        TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd_, (HMENU)(UINT_PTR)1, hInst_, nullptr);
}

void AppWindow::createFileList() {
    fileList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
        0, 0, 0, 0, hwnd_, (HMENU)(UINT_PTR)2, hInst_, nullptr);
    ListView_SetExtendedListViewStyle(fileList_,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    struct { const wchar_t* name; int w; int fmt; } cols[] = {
        { L"Name",          260, LVCFMT_LEFT  },
        { L"Size",          100, LVCFMT_RIGHT },
        { L"Type",          100, LVCFMT_LEFT  },
        { L"Date Modified", 160, LVCFMT_LEFT  },
    };
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW lvc = {};
        lvc.mask     = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        lvc.fmt      = cols[i].fmt;
        lvc.cx       = cols[i].w;
        lvc.pszText  = const_cast<wchar_t*>(cols[i].name);
        lvc.iSubItem = i;
        ListView_InsertColumn(fileList_, i, &lvc);
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void AppWindow::doLayout() {
    if (!hwnd_) return;
    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    int W = rc.right, H = rc.bottom;

    if (toolbar_) {
        SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
        RECT tbr = {};
        GetWindowRect(toolbar_, &tbr);
        int tbH2 = tbr.bottom - tbr.top;
        MoveWindow(toolbar_, 0, 0, W, tbH2, TRUE);
    }
    int tbH = TOOLBAR_H;
    if (toolbar_) {
        RECT tbr2 = {};
        GetWindowRect(toolbar_, &tbr2);
        int h2 = tbr2.bottom - tbr2.top;
        if (h2 > 0) tbH = h2;
    }

    int sbH = STATUS_H;
    if (statusBar_) MoveWindow(statusBar_, 0, H - sbH, W, sbH, TRUE);

    int visH = VIS_PANEL_H;
    int visY = H - sbH - visH;
    if (visY < tbH + 4) visY = tbH + 4;
    if (visualizer_ && visualizer_->hwnd())
        visualizer_->resize(0, visY, W, visH);

    int treeW = W * TREE_W_FRAC / 100;
    if (treeW < 80) treeW = 80;
    int listX = treeW + 2;
    int listW = W - listX;
    if (listW < 50) listW = 50;
    int paneY = tbH;
    int paneH = visY - paneY;
    if (paneH < 20) paneH = 20;

    if (folderTree_) MoveWindow(folderTree_, 0,     paneY, treeW, paneH, TRUE);
    if (fileList_)   MoveWindow(fileList_,   listX, paneY, listW, paneH, TRUE);

    if (searchEdit_ && toolbar_) {
        RECT cl = {};
        GetClientRect(toolbar_, &cl);
        RECT seRc = {};
        GetWindowRect(searchEdit_, &seRc);
        MapWindowPoints(nullptr, toolbar_, reinterpret_cast<LPPOINT>(&seRc), 2);
        int seW = cl.right - seRc.left - 8;
        if (seW > 80) MoveWindow(searchEdit_, seRc.left, seRc.top, seW, 22, TRUE);
    }
}

// ---------------------------------------------------------------------------
// File Index initialization
// ---------------------------------------------------------------------------

void AppWindow::initFileIndex() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    auto sl = exeDir.rfind(L'\\');
    if (sl != std::wstring::npos) exeDir = exeDir.substr(0, sl);
    dbPath_ = exeDir + L"\\file_index.db";

    if (!fileIndex_->initialize(dbPath_))
        MessageBoxW(hwnd_,
            L"Warning: could not open file_index.db.\n"
            L"Running without persistent indexing.",
            L"Index Warning", MB_ICONWARNING);

    fileIndex_->onStalePruned = [this](const std::wstring&) {
        PostMessageW(hwnd_, WM_INDEX_COMPLETE, 0, 0);
    };

    searchEngine_ = std::make_unique<SearchEngine>(*fileIndex_);

    fileManager_->onFileCreated = [this](FileRecord rec) {
        fileIndex_->addRecord(rec);
        PostMessageW(hwnd_, WM_INDEX_COMPLETE, 0, 0);
    };
    fileManager_->onFolderCreated = [this](FileRecord rec) {
        fileIndex_->addRecord(rec);
        PostMessageW(hwnd_, WM_INDEX_COMPLETE, 0, 0);
    };
    fileManager_->onRenamed = [this](std::wstring oldPath, FileRecord newRec) {
        fileIndex_->renameRecord(oldPath, newRec);
        PostMessageW(hwnd_, WM_INDEX_COMPLETE, 0, 0);
    };
    fileManager_->onDeleted = [this](std::wstring path) {
        fileIndex_->removeRecord(path);
        PostMessageW(hwnd_, WM_INDEX_COMPLETE, 0, 0);
    };
    fileManager_->onMoved = [this](std::wstring oldPath, FileRecord newRec) {
        fileIndex_->renameRecord(oldPath, newRec);
        PostMessageW(hwnd_, WM_INDEX_COMPLETE, 0, 0);
    };

    // B+ Tree animation callbacks
    fileIndex_->nameTree().onSplit = [this](SplitEvent ev) {
        if (visualizer_->activeTree() == Visualizer::ActiveTree::Name)
            visualizer_->notifySplit(ev.nodePtr, ev.promotedKeyIdx);
    };
    fileIndex_->nameTree().onMerge = [this](MergeEvent ev) {
        if (visualizer_->activeTree() == Visualizer::ActiveTree::Name)
            visualizer_->notifyMerge(ev.leftPtr, ev.rightPtr);
    };
    fileIndex_->nameTree().onSearch = [this](SearchEvent ev) {
        if (visualizer_->activeTree() == Visualizer::ActiveTree::Name)
            visualizer_->notifySearch(ev.traversedNodes);
    };
    fileIndex_->sizeTree().onSplit = [this](SplitEvent ev) {
        if (visualizer_->activeTree() == Visualizer::ActiveTree::Size)
            visualizer_->notifySplit(ev.nodePtr, ev.promotedKeyIdx);
    };
    fileIndex_->sizeTree().onMerge = [this](MergeEvent ev) {
        if (visualizer_->activeTree() == Visualizer::ActiveTree::Size)
            visualizer_->notifyMerge(ev.leftPtr, ev.rightPtr);
    };
    fileIndex_->dateTree().onSplit = [this](SplitEvent ev) {
        if (visualizer_->activeTree() == Visualizer::ActiveTree::Date)
            visualizer_->notifySplit(ev.nodePtr, ev.promotedKeyIdx);
    };
    fileIndex_->dateTree().onMerge = [this](MergeEvent ev) {
        if (visualizer_->activeTree() == Visualizer::ActiveTree::Date)
            visualizer_->notifyMerge(ev.leftPtr, ev.rightPtr);
    };

    // Default directory: Desktop
    wchar_t desk[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0, desk)))
        currentDir_ = desk;
    else
        currentDir_ = L"C:\\";

    refreshFolderTree();
    navigateTo(currentDir_);
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

void AppWindow::navigateTo(const std::wstring& dir) {
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(hwnd_, (L"Cannot navigate to:\n" + dir).c_str(),
                    L"Navigation Error", MB_ICONERROR);
        return;
    }
    currentDir_ = dir;
    clearSearch();
    populateListView();
    updateStatusBar();
    refreshVisualizer();
}

void AppWindow::refreshFolderTree() {
    TreeView_DeleteAllItems(folderTree_);
    treeItemData_.clear();

    auto* rd = new TreeItemData{ L"" };
    treeItemData_.emplace_back(rd);

    TVINSERTSTRUCTW tvis = {};
    tvis.hParent        = TVI_ROOT;
    tvis.hInsertAfter   = TVI_LAST;
    tvis.item.mask      = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    tvis.item.pszText   = const_cast<wchar_t*>(L"This PC");
    tvis.item.lParam    = reinterpret_cast<LPARAM>(rd);
    tvis.item.cChildren = 1;
    HTREEITEM hRoot = TreeView_InsertItem(folderTree_, &tvis);

    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t dp[4] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0' };
        addTreeItem(hRoot, dp, dp, true);
    }
    TreeView_Expand(folderTree_, hRoot, TVE_EXPAND);
}

HTREEITEM AppWindow::addTreeItem(HTREEITEM parent,
                                  const std::wstring& text,
                                  const std::wstring& path,
                                  bool hasChildren)
{
    auto* d = new TreeItemData{ path };
    treeItemData_.emplace_back(d);
    TVINSERTSTRUCTW tvis = {};
    tvis.hParent        = parent;
    tvis.hInsertAfter   = TVI_SORT;
    tvis.item.mask      = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    tvis.item.pszText   = const_cast<wchar_t*>(text.c_str());
    tvis.item.lParam    = reinterpret_cast<LPARAM>(d);
    tvis.item.cChildren = hasChildren ? 1 : 0;
    return TreeView_InsertItem(folderTree_, &tvis);
}

void AppWindow::expandTreeItem(HTREEITEM item) {
    TVITEMW tvi = {};
    tvi.mask  = TVIF_PARAM;
    tvi.hItem = item;
    if (!TreeView_GetItem(folderTree_, &tvi)) return;

    TreeItemData* data = reinterpret_cast<TreeItemData*>(tvi.lParam);
    if (!data || data->path.empty()) return;

    HTREEITEM firstChild = TreeView_GetChild(folderTree_, item);
    if (firstChild) {
        TVITEMW cvi = {};
        cvi.mask  = TVIF_PARAM;
        cvi.hItem = firstChild;
        TreeView_GetItem(folderTree_, &cvi);
        if (cvi.lParam != 0) return;
        TreeView_DeleteItem(folderTree_, firstChild);
    }

    std::wstring pattern = data->path;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L"*";

    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        std::wstring childPath = data->path;
        if (!childPath.empty() && childPath.back() != L'\\') childPath += L'\\';
        childPath += fd.cFileName;

        bool hasSub = false;
        std::wstring sp = childPath + L"\\*";
        WIN32_FIND_DATAW sfd = {};
        HANDLE hs = FindFirstFileExW(sp.c_str(), FindExInfoBasic, &sfd,
                                     FindExSearchNameMatch, nullptr,
                                     FIND_FIRST_EX_LARGE_FETCH);
        if (hs != INVALID_HANDLE_VALUE) {
            do {
                if ((sfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    !(sfd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM)) &&
                    wcscmp(sfd.cFileName,L".")!=0 && wcscmp(sfd.cFileName,L"..")!=0)
                { hasSub = true; break; }
            } while (FindNextFileW(hs, &sfd));
            FindClose(hs);
        }
        addTreeItem(item, fd.cFileName, childPath, hasSub);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

// ---------------------------------------------------------------------------
// ListView
// ---------------------------------------------------------------------------

void AppWindow::populateListView() {
    auto records = fileIndex_->getByDirectory(currentDir_);
    if (records.empty()) {
        auto live = FileManager::listDirectory(currentDir_);
        for (auto& r : live) fileIndex_->addRecord(r);
        records = live;
    }

    switch (sortCol_) {
    case 1:
        std::sort(records.begin(), records.end(), [this](const FileRecord& a, const FileRecord& b) {
            return sortAsc_ ? (a.size < b.size) : (a.size > b.size);
        });
        break;
    case 2:
        std::sort(records.begin(), records.end(), [this](const FileRecord& a, const FileRecord& b) {
            return sortAsc_ ? (a.extension < b.extension) : (a.extension > b.extension);
        });
        break;
    case 3:
        std::sort(records.begin(), records.end(), [this](const FileRecord& a, const FileRecord& b) {
            return sortAsc_ ? (a.modified < b.modified) : (a.modified > b.modified);
        });
        break;
    default:
        if (!sortAsc_) std::reverse(records.begin(), records.end());
        break;
    }
    populateListViewFromRecords(records);
}

void AppWindow::populateListViewFromRecords(const std::vector<FileRecord>& recs) {
    SendMessageW(fileList_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(fileList_);
    int idx = 0;
    for (auto& rec : recs) {
        LVITEMW lvi = {};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = idx;
        lvi.pszText = const_cast<wchar_t*>(rec.filename.c_str());
        ListView_InsertItem(fileList_, &lvi);

        std::wstring sz   = rec.isDirectory ? L"<DIR>" : fmtSize(rec.size);
        std::wstring type = rec.isDirectory ? L"Folder"
                          : (rec.extension.empty() ? L"File" : rec.extension + L" file");
        std::wstring dt   = fmtTime(rec.modified);
        ListView_SetItemText(fileList_, idx, 1, const_cast<wchar_t*>(sz.c_str()));
        ListView_SetItemText(fileList_, idx, 2, const_cast<wchar_t*>(type.c_str()));
        ListView_SetItemText(fileList_, idx, 3, const_cast<wchar_t*>(dt.c_str()));
        ++idx;
    }
    SendMessageW(fileList_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(fileList_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Visualizer refresh
// ---------------------------------------------------------------------------

void AppWindow::refreshVisualizer() {
    if (!visualizer_ || !visualizer_->hwnd()) return;
    std::vector<Visualizer::NodeDraw> nodes;
    std::vector<int> roots = { 0 };

    switch (visualizer_->activeTree()) {
    case Visualizer::ActiveTree::Name: {
        auto snap = fileIndex_->nameTree().snapshot();
        buildNodeDrawList(snap, nodes, [](const std::wstring& k) -> std::wstring {
            return (k.size() > 12) ? k.substr(0, 11) + L"\u2026" : k;
        });
        break;
    }
    case Visualizer::ActiveTree::Size: {
        auto snap = fileIndex_->sizeTree().snapshot();
        buildNodeDrawListU64(snap, nodes, [](uint64_t k) -> std::wstring {
            return fmtSize(k);
        });
        break;
    }
    case Visualizer::ActiveTree::Date: {
        auto snap = fileIndex_->dateTree().snapshot();
        buildNodeDrawListU64(snap, nodes, [](uint64_t k) -> std::wstring {
            return fmtTime(static_cast<time_t>(k));
        });
        break;
    }
    }
    if (nodes.empty()) roots.clear();
    visualizer_->refresh(nodes, roots);
}

void AppWindow::updateStatusBar() {
    if (!statusBar_) return;
    size_t total = fileIndex_->totalRecords();
    auto dirRecs = fileIndex_->getByDirectory(currentDir_);

    std::wostringstream s0, s1, s2, s3;
    s0 << dirRecs.size() << L" items";
    s1 << L"Indexed: " << total;
    s2 << L"Tree height: "
       << fileIndex_->treeHeight(static_cast<int>(visualizer_->activeTree()));
    // Show current order in status bar
    s3 << currentDir_ << L"  [m=" << treeOrderCur_ << L"]";

    SendMessageW(statusBar_, SB_SETTEXTW, 0, (LPARAM)s0.str().c_str());
    SendMessageW(statusBar_, SB_SETTEXTW, 1, (LPARAM)s1.str().c_str());
    SendMessageW(statusBar_, SB_SETTEXTW, 2, (LPARAM)s2.str().c_str());
    SendMessageW(statusBar_, SB_SETTEXTW, 3, (LPARAM)s3.str().c_str());
}

// ---------------------------------------------------------------------------
// Tree order rebuild  (called when visualizer Apply button is clicked)
// ---------------------------------------------------------------------------

void AppWindow::rebuildTreesWithOrder(int newOrder, int newMin, int newMax) {
    // Clamp all values
    if (newMin   < 3)        newMin   = 3;
    if (newMax   < newMin)   newMax   = newMin;
    if (newOrder < newMin)   newOrder = newMin;
    if (newOrder > newMax)   newOrder = newMax;

    treeOrderMin_ = newMin;
    treeOrderMax_ = newMax;
    treeOrderCur_ = newOrder;

    // Tell the index to rebuild with the new order
    // fileIndex_ exposes a rebuildWithOrder() or equivalent; adjust to your API.
    // Common patterns (use whichever matches your FileIndex class):
    //
    //   Option A: fileIndex_->rebuildWithOrder(newOrder);
    //   Option B: fileIndex_->nameTree().rebuild(newOrder);
    //             fileIndex_->sizeTree().rebuild(newOrder);
    //             fileIndex_->dateTree().rebuild(newOrder);
    //   Option C (re-index from scratch):
    fileIndex_->nameTree().clear();
    fileIndex_->sizeTree().clear();
    fileIndex_->dateTree().clear();
    for (auto& rec : FileManager::listDirectory(currentDir_))
        fileIndex_->addRecord(rec);

    // Sync updated config back to the visualizer spinners
    Visualizer::OrderConfig cfg;
    cfg.minOrder = treeOrderMin_;
    cfg.maxOrder = treeOrderMax_;
    cfg.curOrder = treeOrderCur_;
    visualizer_->setOrderConfig(cfg);

    // Refresh everything
    populateListView();
    refreshVisualizer();
    updateStatusBar();
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void AppWindow::cmdNewFile() {
    std::wstring name;
    if (!showInputDialog(hwnd_, hInst_, L"New File", L"File name:",
                         L"NewFile.txt", name)) return;
    FileRecord rec;
    if (!fileManager_->createFile(currentDir_, name, rec)) {
        MessageBoxW(hwnd_,
            (L"Failed to create \"" + name + L"\".\n"
             L"The file may already exist or you may lack permission.").c_str(),
            L"Error", MB_ICONERROR);
        return;
    }
    populateListView(); refreshVisualizer(); updateStatusBar();
}

void AppWindow::cmdNewFolder() {
    std::wstring name;
    if (!showInputDialog(hwnd_, hInst_, L"New Folder", L"Folder name:",
                         L"New Folder", name)) return;
    FileRecord rec;
    if (!fileManager_->createFolder(currentDir_, name, rec)) {
        MessageBoxW(hwnd_,
            (L"Failed to create folder \"" + name + L"\".\n"
             L"It may already exist or you may lack permission.").c_str(),
            L"Error", MB_ICONERROR);
        return;
    }
    populateListView(); refreshVisualizer(); updateStatusBar();
    refreshFolderTree();
}

void AppWindow::cmdRename() {
    std::wstring oldPath = selectedListViewPath();
    if (oldPath.empty()) {
        MessageBoxW(hwnd_, L"Select a file or folder to rename.",
                    L"Rename", MB_ICONINFORMATION); return;
    }
    std::wstring oldName = FileManager::basenameOf(oldPath);
    std::wstring newName;
    if (!showInputDialog(hwnd_, hInst_, L"Rename", L"New name:",
                         oldName.c_str(), newName)) return;
    if (newName == oldName) return;

    FileRecord rec;
    if (!fileManager_->rename(oldPath, newName, rec)) {
        MessageBoxW(hwnd_,
            (L"Rename failed: \"" + oldName + L"\" \u2192 \"" + newName + L"\"").c_str(),
            L"Error", MB_ICONERROR);
        return;
    }
    populateListView(); refreshVisualizer(); updateStatusBar();
}

void AppWindow::cmdDelete() {
    std::wstring path = selectedListViewPath();
    if (path.empty()) {
        MessageBoxW(hwnd_, L"Select a file or folder to delete.",
                    L"Delete", MB_ICONINFORMATION); return;
    }
    std::wstring name = FileManager::basenameOf(path);
    if (MessageBoxW(hwnd_,
            (L"Move \"" + name + L"\" to the Recycle Bin?").c_str(),
            L"Confirm Delete", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;

    if (!fileManager_->deleteItem(path, true)) {
        MessageBoxW(hwnd_,
            (L"Delete failed: \"" + name + L"\"").c_str(),
            L"Error", MB_ICONERROR);
        return;
    }
    populateListView(); refreshVisualizer(); updateStatusBar();
}

void AppWindow::cmdMove() {
    std::wstring srcPath = selectedListViewPath();
    if (srcPath.empty()) {
        MessageBoxW(hwnd_, L"Select a file or folder to move.",
                    L"Move", MB_ICONINFORMATION); return;
    }
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd_;
    bi.lpszTitle = L"Select destination folder";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t dest[MAX_PATH] = {};
    SHGetPathFromIDListW(pidl, dest);
    CoTaskMemFree(pidl);

    if (std::wstring(dest) == FileManager::parentOf(srcPath)) {
        MessageBoxW(hwnd_, L"The item is already in that folder.",
                    L"Move", MB_ICONINFORMATION); return;
    }
    FileRecord rec;
    if (!fileManager_->move(srcPath, dest, rec)) {
        MessageBoxW(hwnd_,
            (L"Move failed: \"" + FileManager::basenameOf(srcPath) +
             L"\" \u2192 \"" + dest + L"\"").c_str(),
            L"Error", MB_ICONERROR);
        return;
    }
    populateListView(); refreshVisualizer(); updateStatusBar();
}

void AppWindow::cmdOpen() {
    std::wstring path = selectedListViewPath();
    if (path.empty()) return;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hwnd_,
            (L"Path does not exist:\n" + path).c_str(),
            L"Error", MB_ICONERROR);
        return;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) { navigateTo(path); return; }
    if (!fileManager_->openWithShell(path, hwnd_))
        MessageBoxW(hwnd_,
            (L"No associated application for:\n" + path).c_str(),
            L"Open", MB_ICONINFORMATION);
}

void AppWindow::cmdProperties() {
    std::wstring path = selectedListViewPath();
    if (path.empty()) {
        MessageBoxW(hwnd_, L"Select a file or folder.",
                    L"Properties", MB_ICONINFORMATION); return;
    }
    std::wstring name = FileManager::basenameOf(path);
    auto results = fileIndex_->exactSearch(name);
    FileRecord rec;
    bool found = false;
    for (auto& r : results) if (r.path == path) { rec = r; found = true; break; }
    if (!found) found = FileManager::statRecord(path, rec);
    if (!found) {
        MessageBoxW(hwnd_, (L"Cannot stat: " + path).c_str(),
                    L"Error", MB_ICONERROR); return;
    }
    std::wostringstream oss;
    oss << L"Name:      " << rec.filename   << L"\r\n"
        << L"Path:      " << rec.path       << L"\r\n"
        << L"Extension: " << (rec.extension.empty() ? L"(none)" : rec.extension) << L"\r\n"
        << L"Size:      " << fmtSize(rec.size) << L"\r\n"
        << L"Created:   " << fmtTime(rec.created)  << L"\r\n"
        << L"Modified:  " << fmtTime(rec.modified) << L"\r\n"
        << L"Type:      " << (rec.isDirectory ? L"Folder" : L"File");
    MessageBoxW(hwnd_, oss.str().c_str(), L"Properties", MB_ICONINFORMATION);
}

void AppWindow::cmdIndexDirectory() {
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd_;
    bi.lpszTitle = L"Select directory to index recursively";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t pathBuf[MAX_PATH] = {};
    SHGetPathFromIDListW(pidl, pathBuf);
    CoTaskMemFree(pidl);

    struct TP { std::wstring dir; FileIndex* idx; HWND hwnd; };
    auto* tp = new TP{ pathBuf, fileIndex_.get(), hwnd_ };
    HANDLE ht = CreateThread(nullptr, 0, [](LPVOID lp) -> DWORD {
        auto* p = static_cast<TP*>(lp);
        std::vector<std::wstring> dirs{ p->dir };
        while (!dirs.empty()) {
            std::wstring d = dirs.back(); dirs.pop_back();
            for (auto& e : FileManager::listDirectory(d)) {
                p->idx->addRecord(e);
                if (e.isDirectory) dirs.push_back(e.path);
            }
        }
        PostMessageW(p->hwnd, WM_INDEX_COMPLETE, 0, 0);
        delete p; return 0;
    }, tp, 0, nullptr);
    if (ht) CloseHandle(ht);
}

void AppWindow::cmdReindex() {
    fileIndex_->nameTree().clear();
    fileIndex_->sizeTree().clear();
    fileIndex_->dateTree().clear();
    for (auto& e : FileManager::listDirectory(currentDir_))
        fileIndex_->addRecord(e);
    populateListView(); refreshVisualizer(); updateStatusBar();
}

void AppWindow::cmdSort(int col, bool asc) {
    sortCol_ = col; sortAsc_ = asc;
    if (searchActive_) performSearch(lastSearchQuery_);
    else               populateListView();
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

void AppWindow::onSearchChange() {
    KillTimer(hwnd_, TIMER_SEARCH);
    SetTimer(hwnd_, TIMER_SEARCH, SEARCH_DEBOUNCE_MS, nullptr);
}

void AppWindow::performSearch(const std::wstring& query) {
    lastSearchQuery_ = query;
    if (query.empty()) { clearSearch(); populateListView(); updateStatusBar(); return; }
    searchActive_ = true;
    auto results = searchEngine_->prefixSearch(query);
    populateListViewFromRecords(results);
    refreshVisualizer();
    updateStatusBar();
}

void AppWindow::clearSearch() {
    searchActive_    = false;
    lastSearchQuery_.clear();
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------

std::wstring AppWindow::selectedListViewPath() const {
    int sel = ListView_GetNextItem(fileList_, -1, LVNI_SELECTED);
    if (sel < 0) return L"";
    wchar_t name[MAX_PATH] = {};
    ListView_GetItemText(fileList_, sel, 0, name, MAX_PATH);
    std::wstring path = currentDir_;
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    return path + name;
}

std::wstring AppWindow::selectedListViewName() const {
    int sel = ListView_GetNextItem(fileList_, -1, LVNI_SELECTED);
    if (sel < 0) return L"";
    wchar_t buf[MAX_PATH] = {};
    ListView_GetItemText(fileList_, sel, 0, buf, MAX_PATH);
    return buf;
}

std::vector<std::wstring> AppWindow::selectedListViewPaths() const {
    std::vector<std::wstring> paths;
    int idx = -1;
    while ((idx = ListView_GetNextItem(fileList_, idx, LVNI_SELECTED)) >= 0) {
        wchar_t buf[MAX_PATH] = {};
        ListView_GetItemText(fileList_, idx, 0, buf, MAX_PATH);
        std::wstring p = currentDir_;
        if (!p.empty() && p.back() != L'\\') p += L'\\';
        paths.push_back(p + buf);
    }
    return paths;
}

FileRecord AppWindow::selectedListViewRecord() const {
    FileRecord rec;
    FileManager::statRecord(selectedListViewPath(), rec);
    return rec;
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

void AppWindow::showContextMenu(int sx, int sy) {
    bool sel = (ListView_GetNextItem(fileList_, -1, LVNI_SELECTED) >= 0);
    UINT gf  = sel ? 0u : (UINT)MF_GRAYED;
    HMENU m  = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | gf,    IDM_CTX_OPEN,       L"Open");
    AppendMenuW(m, MF_SEPARATOR,      0, nullptr);
    AppendMenuW(m, MF_STRING | gf,    IDM_CTX_RENAME,     L"Rename\tF2");
    AppendMenuW(m, MF_STRING | gf,    IDM_CTX_DELETE,     L"Delete\tDel");
    AppendMenuW(m, MF_STRING | gf,    IDM_CTX_MOVE,       L"Move to...");
    AppendMenuW(m, MF_SEPARATOR,      0, nullptr);
    AppendMenuW(m, MF_STRING | gf,    IDM_CTX_PROPERTIES, L"Properties");
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, hwnd_, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case IDM_CTX_OPEN:       cmdOpen();       break;
    case IDM_CTX_RENAME:     cmdRename();     break;
    case IDM_CTX_DELETE:     cmdDelete();     break;
    case IDM_CTX_MOVE:       cmdMove();       break;
    case IDM_CTX_PROPERTIES: cmdProperties(); break;
    }
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK AppWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<AppWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handleMessage(msg, wp, lp);
}

LRESULT AppWindow::handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE:
        createControls();
        return 0;

    case WM_SIZE:
        doLayout();
        return 0;

    case WM_COMMAND: {
        UINT cmd = LOWORD(wp);
        if (HIWORD(wp) == EN_CHANGE && reinterpret_cast<HWND>(lp) == searchEdit_) {
            onSearchChange(); return 0;
        }
        switch (cmd) {
        case IDM_FILE_NEW_FILE:
        case IDT_NEW_FILE:        cmdNewFile();        break;
        case IDM_FILE_NEW_FOLDER:
        case IDT_NEW_FOLDER:      cmdNewFolder();      break;
        case IDM_FILE_EXIT:       PostQuitMessage(0);  break;
        case IDM_EDIT_OPEN:       cmdOpen();           break;
        case IDM_EDIT_RENAME:
        case IDT_RENAME:          cmdRename();         break;
        case IDM_EDIT_DELETE:
        case IDT_DELETE:          cmdDelete();         break;
        case IDM_EDIT_MOVE:       cmdMove();           break;
        case IDM_EDIT_PROPERTIES: cmdProperties();     break;
        case IDM_VIEW_NAME:       cmdSort(0, true);    break;
        case IDM_VIEW_SIZE:       cmdSort(1, true);    break;
        case IDM_VIEW_DATE:       cmdSort(3, true);    break;
        case IDM_TOOLS_INDEX_DIR: cmdIndexDirectory(); break;
        case IDM_TOOLS_REINDEX:   cmdReindex();        break;
        }
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR* nm = reinterpret_cast<NMHDR*>(lp);
        if (!nm) return 0;

        if (nm->hwndFrom == folderTree_) {
            if (nm->code == TVN_ITEMEXPANDINGW) {
                NMTREEVIEWW* ntv = reinterpret_cast<NMTREEVIEWW*>(lp);
                if (ntv->action == TVE_EXPAND) expandTreeItem(ntv->itemNew.hItem);
            } else if (nm->code == TVN_SELCHANGEDW) {
                NMTREEVIEWW* ntv = reinterpret_cast<NMTREEVIEWW*>(lp);
                TreeItemData* d = reinterpret_cast<TreeItemData*>(ntv->itemNew.lParam);
                if (d && !d->path.empty()) navigateTo(d->path);
            }
        }
        if (nm->hwndFrom == fileList_) {
            switch (nm->code) {
            case NM_DBLCLK: cmdOpen(); break;
            case NM_RCLICK: { POINT pt{}; GetCursorPos(&pt); showContextMenu(pt.x, pt.y); break; }
            case LVN_COLUMNCLICK: {
                NMLISTVIEW* nl = reinterpret_cast<NMLISTVIEW*>(lp);
                int col = nl->iSubItem;
                if (col == sortCol_) sortAsc_ = !sortAsc_;
                else { sortCol_ = col; sortAsc_ = true; }
                cmdSort(sortCol_, sortAsc_);
                break;
            }
            case LVN_KEYDOWN: {
                NMLVKEYDOWN* kd = reinterpret_cast<NMLVKEYDOWN*>(lp);
                if (kd->wVKey == VK_F2)     cmdRename();
                if (kd->wVKey == VK_DELETE) cmdDelete();
                if (kd->wVKey == VK_RETURN) cmdOpen();
                break;
            }
            }
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_SEARCH) {
            KillTimer(hwnd_, TIMER_SEARCH);
            wchar_t buf[256] = {};
            if (searchEdit_) GetWindowTextW(searchEdit_, buf, 256);
            std::wstring q = buf;
            if (q.empty()) {
                clearSearch(); populateListView(); updateStatusBar();
            } else {
                performSearch(q);
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && searchActive_) {
            if (searchEdit_) SetWindowTextW(searchEdit_, L"");
            clearSearch(); populateListView(); updateStatusBar();
        }
        return 0;

    // Visualizer tree-tab switched
    case WM_TREE_SELECTOR:
        refreshVisualizer();
        updateStatusBar();
        return 0;

    // Background index finished  (existing message, unchanged)
    case WM_INDEX_COMPLETE:
        populateListView();
        refreshVisualizer();
        updateStatusBar();
        return 0;

    // -----------------------------------------------------------------------
    // NEW: user clicked "Apply" in the visualizer order panel
    //   wParam  = new curOrder
    //   lParam  = MAKELPARAM(minOrder, maxOrder)
    // -----------------------------------------------------------------------
    case WM_ORDER_CHANGE: {
        int newOrder = static_cast<int>(wp);
        int newMin   = static_cast<int>(LOWORD(lp));
        int newMax   = static_cast<int>(HIWORD(lp));
        rebuildTreesWithOrder(newOrder, newMin, newMax);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}