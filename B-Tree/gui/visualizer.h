#pragma once
// =============================================================================
// visualizer.h  –  B+ Tree Visualizer  (Win32, USF-style)
// NOTE: NodeDraw, ActiveTree, OrderConfig are nested inside Visualizer so
//       existing window.cpp code (Visualizer::NodeDraw etc.) compiles as-is.
// =============================================================================
#ifndef VISUALIZER_H
#define VISUALIZER_H

#include <windows.h>
#include <vector>
#include <set>
#include <string>
#include <functional>

class Visualizer {
public:
    // -----------------------------------------------------------------------
    // Public types  (kept as nested so window.cpp's Visualizer::NodeDraw works)
    // -----------------------------------------------------------------------
    struct NodeDraw {
        void*              nodePtr = nullptr;
        bool               isLeaf  = false;
        bool               hasNext = false;
        int                numKeys = 0;
        std::vector<std::wstring> keyStrings;
        std::vector<NodeDraw*>    childPtrs;   // non-owning
        RECT               rect    = {};       // set by layoutNodes()
    };

    enum class ActiveTree { Name = 0, Size = 1, Date = 2 };

    struct OrderConfig {
        int minOrder = 3;
        int maxOrder = 7;
        int curOrder = 4;
    };

    // -----------------------------------------------------------------------
    // Lifetime
    // -----------------------------------------------------------------------
    Visualizer();
    ~Visualizer();

    bool create(HWND parent, int x, int y, int w, int h, HINSTANCE hInst);
    void destroy();
    void resize(int x, int y, int w, int h);

    // -----------------------------------------------------------------------
    // Tree snapshot
    // -----------------------------------------------------------------------
    void refresh(const std::vector<NodeDraw>& nodes,
                 const std::vector<int>& rootIndices);

    // -----------------------------------------------------------------------
    // Animation triggers
    // -----------------------------------------------------------------------
    void notifySplit (void* nodePtr, int promotedKeyIdx = 0);
    void notifyMerge (void* leftPtr, void* rightPtr = nullptr);
    void notifySearch(const std::vector<void*>& traversed);
    void notifyInsert(void* nodePtr);

    // -----------------------------------------------------------------------
    // Tree selector
    // -----------------------------------------------------------------------
    void       setActiveTree(ActiveTree t);
    ActiveTree activeTree()  const { return activeTree_; }

    // -----------------------------------------------------------------------
    // Order config
    // -----------------------------------------------------------------------
    const OrderConfig& orderConfig() const     { return orderCfg_; }
    void  setOrderConfig(const OrderConfig& c) { orderCfg_ = c; }

    HWND hwnd() const { return hwnd_; }

private:
    // ---- Win32 -------------------------------------------------------
    HWND      hwnd_   = nullptr;
    HWND      parent_ = nullptr;
    HINSTANCE hInst_  = nullptr;

    HDC     offDC_  = nullptr;
    HBITMAP offBmp_ = nullptr;
    int     offW_   = 0, offH_ = 0;

    std::vector<NodeDraw> nodes_;
    std::vector<int>      rootIndices_;

    // ---- Layout constants -----------------------------------------------
    static constexpr int NODE_W      = 172;
    static constexpr int NODE_H      = 40;
    static constexpr int NODE_GAP_X  = 16;
    static constexpr int LEVEL_GAP_Y = 84;
    static constexpr int TOP_BAR_H   = 96;
    static constexpr int CANVAS_PAD  = 28;

    void layoutNodes();

    // ---- Highlights / animation -----------------------------------------
    void*  splitHighlight_  = nullptr;
    void*  mergeHighlight_  = nullptr;
    void*  insertHighlight_ = nullptr;
    std::set<void*> searchHighlight_;

    static constexpr UINT_PTR TIMER_ANIM   = 1;
    static constexpr int      ANIM_STEP_MS = 16;
    static constexpr int      ANIM_TICKS   = 48;
    int animTicks_ = 0;

    // ---- Pan / zoom -----------------------------------------------------
    int   panX_ = CANVAS_PAD, panY_ = 0;
    float zoom_ = 1.0f;
    bool  dragging_  = false;
    POINT dragStart_ = {};
    int   dragPanX_  = 0, dragPanY_ = 0;

    // ---- Top-bar hit rects (screen coords) ------------------------------
    ActiveTree activeTree_ = ActiveTree::Name;
    RECT btnName_ = {}, btnSize_ = {}, btnDate_ = {};

    // ---- Order panel ----------------------------------------------------
    OrderConfig orderCfg_;
    bool orderDirty_ = false;

    struct SpinCtrl {
        RECT minus = {}, plus = {}, label = {};
        int* value   = nullptr;
        int  minVal  = 3;
        int  maxVal  = 20;
        const wchar_t* caption = L"";
    };
    SpinCtrl spinMin_, spinMax_, spinCur_;
    RECT     btnApply_ = {};

    // ---- Drawing --------------------------------------------------------
    void ensureOffscreen(int w, int h);
    void onPaint();
    void paint(HDC dc, int W, int H);

    void drawBackground (HDC dc, int W, int H);
    void drawTopBar     (HDC dc, int W);
    void drawSelectors  (HDC dc, int cx, int y);
    void drawOrderPanel (HDC dc, int startX, int y, int availW);
    void drawArrows     (HDC dc);
    void drawNode       (HDC dc, const NodeDraw& n, float fade);
    void drawLegend     (HDC dc, int W, int H);
    void drawEmpty      (HDC dc, int W, int H);

    void     roundRect  (HDC dc, RECT r, int rx, int ry,
                         COLORREF fill, COLORREF brd, int pw = 2);
    void     drawTextEx (HDC dc, const wchar_t* s, RECT r, COLORREF col,
                         int sz, bool bold,
                         UINT fmt = DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    HFONT    makeFont   (int sz, bool bold);
    COLORREF lerpC      (COLORREF a, COLORREF b, float t);
    void     arrowHead  (HDC dc, int x, int y, double dx, double dy,
                         int sz, COLORREF col);

    // ---- Input ----------------------------------------------------------
    void onLButtonDown(int x, int y);
    void onLButtonUp  (int x, int y);
    void onMouseMove  (int x, int y);
    void onMouseWheel (int delta);
    void onTimer      ();
    void onSize       (int w, int h);

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
};

#endif // VISUALIZER_H