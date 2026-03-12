// =============================================================================
// visualizer.cpp  –  B+ Tree Visualizer  (Win32, USF-style)
// FIXES:
//   1. Crash fix: childPtrs rebuilt by index after nodes_ copy (no dangling ptrs)
//   2. Proper Reingold-Tilford style layout (no node overlap)
//   3. Correct leaf chain arrows
//   4. Zoom applied to canvas transform
// =============================================================================
#include "visualizer.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

// ---------------------------------------------------------------------------
// Colour palette
// ---------------------------------------------------------------------------
static constexpr COLORREF BG          = RGB( 15,  17,  26);
static constexpr COLORREF BG_BAR      = RGB( 20,  22,  34);
static constexpr COLORREF BG_BAR_LINE = RGB( 40,  46,  72);
static constexpr COLORREF DOT_COL     = RGB( 30,  35,  55);

static constexpr COLORREF NODE_FILL   = RGB( 28,  34,  58);
static constexpr COLORREF NODE_BRD    = RGB( 60,  80, 160);
static constexpr COLORREF LEAF_FILL   = RGB( 18,  42,  62);
static constexpr COLORREF LEAF_BRD    = RGB( 40, 130, 220);

static constexpr COLORREF TEXT_PRI    = RGB(200, 220, 255);
static constexpr COLORREF TEXT_DIM    = RGB( 80,  95, 140);
static constexpr COLORREF TEXT_ACCENT = RGB(100, 200, 255);
static constexpr COLORREF DIVIDER_COL = RGB( 50,  65, 110);

static constexpr COLORREF HL_SEARCH_F = RGB( 15,  55, 140);
static constexpr COLORREF HL_SEARCH_B = RGB( 40, 140, 255);
static constexpr COLORREF HL_SPLIT_F  = RGB(130,  55,   0);
static constexpr COLORREF HL_SPLIT_B  = RGB(255, 145,   0);
static constexpr COLORREF HL_MERGE_F  = RGB( 65,   0, 110);
static constexpr COLORREF HL_MERGE_B  = RGB(180,  50, 230);
static constexpr COLORREF HL_INSERT_F = RGB(  0,  80,  50);
static constexpr COLORREF HL_INSERT_B = RGB( 50, 220, 120);

static constexpr COLORREF ARR_CHILD   = RGB( 55,  75, 135);
static constexpr COLORREF ARR_LEAF    = RGB( 40, 190, 100);

static constexpr COLORREF BTN_ACT     = RGB( 40,  85, 195);
static constexpr COLORREF BTN_INACT   = RGB( 28,  34,  58);
static constexpr COLORREF BTN_BRD_ACT = RGB(100, 155, 255);
static constexpr COLORREF BTN_BRD_IN  = RGB( 55,  68, 110);

static constexpr COLORREF SPIN_FILL   = RGB( 22,  28,  50);
static constexpr COLORREF SPIN_BRD    = RGB( 55,  70, 120);
static constexpr COLORREF SPIN_BTN    = RGB( 32,  42,  80);

static constexpr COLORREF APPLY_CLEAN = RGB( 28,  80,  50);
static constexpr COLORREF APPLY_DIRTY = RGB(160,  90,   0);
static constexpr COLORREF APPLY_BRD_C = RGB( 50, 160,  90);
static constexpr COLORREF APPLY_BRD_D = RGB(255, 170,   0);

static const wchar_t* VIZ_CLASS = L"BPTreeViz_USF";

// ---------------------------------------------------------------------------
static inline bool hitTest(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------
Visualizer::Visualizer()  = default;
Visualizer::~Visualizer() { destroy(); }

bool Visualizer::create(HWND parent, int x, int y, int w, int h, HINSTANCE hInst) {
    parent_ = parent;
    hInst_  = hInst;

    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = VIZ_CLASS;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, VIZ_CLASS, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        x, y, w, h, parent, nullptr, hInst, this);
    if (!hwnd_) return false;

    SetTimer(hwnd_, TIMER_ANIM, ANIM_STEP_MS, nullptr);

    orderCfg_ = { 3, 7, 4 };

    spinMin_.value   = &orderCfg_.minOrder;
    spinMin_.minVal  = 3; spinMin_.maxVal = 10;
    spinMin_.caption = L"Min";

    spinMax_.value   = &orderCfg_.maxOrder;
    spinMax_.minVal  = 3; spinMax_.maxVal = 20;
    spinMax_.caption = L"Max";

    spinCur_.value   = &orderCfg_.curOrder;
    spinCur_.minVal  = 3; spinCur_.maxVal = 20;
    spinCur_.caption = L"Order m";

    return true;
}

void Visualizer::destroy() {
    if (!hwnd_) return;
    KillTimer(hwnd_, TIMER_ANIM);
    if (offDC_)  { DeleteDC(offDC_);      offDC_  = nullptr; }
    if (offBmp_) { DeleteObject(offBmp_); offBmp_ = nullptr; }
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void Visualizer::resize(int x, int y, int w, int h) {
    if (hwnd_) SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER);
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------
void Visualizer::notifySplit(void* p, int) {
    splitHighlight_  = p;
    mergeHighlight_  = nullptr;
    insertHighlight_ = nullptr;
    animTicks_       = ANIM_TICKS;
}
void Visualizer::notifyMerge(void* p, void*) {
    mergeHighlight_  = p;
    splitHighlight_  = nullptr;
    insertHighlight_ = nullptr;
    animTicks_       = ANIM_TICKS;
}
void Visualizer::notifySearch(const std::vector<void*>& v) {
    searchHighlight_.clear();
    for (auto* p : v) searchHighlight_.insert(p);
    animTicks_ = ANIM_TICKS;
}
void Visualizer::notifyInsert(void* p) {
    insertHighlight_ = p;
    animTicks_       = ANIM_TICKS;
}

void Visualizer::setActiveTree(ActiveTree t) {
    if (activeTree_ == t) return;
    activeTree_ = t;
    searchHighlight_.clear();
    splitHighlight_ = mergeHighlight_ = insertHighlight_ = nullptr;
}

// ---------------------------------------------------------------------------
// Snapshot + layout
// ---------------------------------------------------------------------------
void Visualizer::refresh(const std::vector<NodeDraw>& nodes,
                         const std::vector<int>& rootIndices)
{
    // -----------------------------------------------------------------------
    // CRASH FIX: childPtrs in the incoming nodes point into the CALLER's
    // vector.  After we copy into nodes_, those pointers dangle.
    // Solution: record child relationships by index, copy nodes (clearing
    // childPtrs), then re-link using our own vector's addresses.
    // -----------------------------------------------------------------------
    int N = (int)nodes.size();

    // Build index map: original pointer → position in nodes[]
    std::map<const NodeDraw*, int> ptrToIdx;
    for (int i = 0; i < N; ++i)
        ptrToIdx[&nodes[i]] = i;

    // Record children as index lists
    std::vector<std::vector<int>> childIdx(N);
    for (int i = 0; i < N; ++i)
        for (auto* cp : nodes[i].childPtrs) {
            auto it = ptrToIdx.find(cp);
            if (it != ptrToIdx.end())
                childIdx[i].push_back(it->second);
        }

    // Copy nodes (childPtrs will be cleared, then rebuilt)
    nodes_ = nodes;
    for (auto& nd : nodes_) nd.childPtrs.clear();

    // Re-link childPtrs using our own vector
    for (int i = 0; i < N; ++i)
        for (int ci : childIdx[i])
            nodes_[i].childPtrs.push_back(&nodes_[ci]);

    rootIndices_ = rootIndices;
    layoutNodes();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Layout  –  proper subtree-width algorithm (no overlaps)
// ---------------------------------------------------------------------------
void Visualizer::layoutNodes() {
    if (nodes_.empty()) return;

    // --- Step 1: compute subtree width for each node (post-order) ----------
    // We work on indices, not pointers.
    int N = (int)nodes_.size();
    std::vector<int> subtreeW(N, 0);

    // We need a post-order traversal.  Build it via DFS from roots.
    std::vector<int> order;
    std::vector<bool> visited(N, false);

    std::function<void(int)> dfs = [&](int i) {
        if (i < 0 || i >= N || visited[i]) return;
        visited[i] = true;
        for (auto* cp : nodes_[i].childPtrs) {
            int ci = (int)(cp - nodes_.data());
            dfs(ci);
        }
        order.push_back(i);
    };
    for (int r : rootIndices_) dfs(r);

    // Post-order: leaves first
    for (int i : order) {
        if (nodes_[i].childPtrs.empty()) {
            subtreeW[i] = NODE_W;
        } else {
            int total = 0;
            int nch   = (int)nodes_[i].childPtrs.size();
            for (auto* cp : nodes_[i].childPtrs) {
                int ci = (int)(cp - nodes_.data());
                total += subtreeW[ci];
            }
            total += NODE_GAP_X * (nch - 1);
            subtreeW[i] = std::max(total, NODE_W);
        }
    }

    // --- Step 2: assign x,y by DFS (top-down) ------------------------------
    std::function<void(int, int, int)> place = [&](int i, int leftX, int depth) {
        int y = CANVAS_PAD + depth * (NODE_H + LEVEL_GAP_Y);

        if (nodes_[i].childPtrs.empty()) {
            int x = leftX;
            nodes_[i].rect = { x, y, x + NODE_W, y + NODE_H };
        } else {
            // Place children left-to-right
            int cx = leftX;
            for (auto* cp : nodes_[i].childPtrs) {
                int ci = (int)(cp - nodes_.data());
                place(ci, cx, depth + 1);
                cx += subtreeW[ci] + NODE_GAP_X;
            }
            // Centre this node over its children
            int firstCi = (int)(nodes_[i].childPtrs.front() - nodes_.data());
            int lastCi  = (int)(nodes_[i].childPtrs.back()  - nodes_.data());
            int childLeft  = nodes_[firstCi].rect.left;
            int childRight = nodes_[lastCi].rect.right;
            int midX = (childLeft + childRight) / 2 - NODE_W / 2;
            nodes_[i].rect = { midX, y, midX + NODE_W, y + NODE_H };
        }
    };

    int startX = CANVAS_PAD;
    for (int r : rootIndices_) {
        place(r, startX, 0);
        startX += subtreeW[r] + NODE_GAP_X * 2;
    }
}

// ---------------------------------------------------------------------------
// Offscreen buffer
// ---------------------------------------------------------------------------
void Visualizer::ensureOffscreen(int w, int h) {
    if (w == offW_ && h == offH_ && offDC_) return;
    if (offBmp_) DeleteObject(offBmp_);
    if (offDC_)  DeleteDC(offDC_);
    HDC hdc = GetDC(hwnd_);
    offDC_  = CreateCompatibleDC(hdc);
    offBmp_ = CreateCompatibleBitmap(hdc, w, h);
    SelectObject(offDC_, offBmp_);
    ReleaseDC(hwnd_, hdc);
    offW_ = w; offH_ = h;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------
void Visualizer::onPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc; GetClientRect(hwnd_, &rc);
    ensureOffscreen(rc.right, rc.bottom);
    paint(offDC_, rc.right, rc.bottom);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, offDC_, 0, 0, SRCCOPY);
    EndPaint(hwnd_, &ps);
}

void Visualizer::paint(HDC dc, int W, int H) {
    drawBackground(dc, W, H);
    drawTopBar(dc, W);

    if (nodes_.empty()) {
        drawEmpty(dc, W, H);
    } else {
        // Apply pan + zoom via world transform
        XFORM xf = {};
        xf.eM11 = zoom_; xf.eM22 = zoom_;
        xf.eDx  = (float)panX_;
        xf.eDy  = (float)(panY_ + TOP_BAR_H);
        int oldMode = SetGraphicsMode(dc, GM_ADVANCED);
        SetWorldTransform(dc, &xf);

        drawArrows(dc);
        float fade = (animTicks_ > 0) ? (float)animTicks_ / ANIM_TICKS : 0.f;
        for (auto& n : nodes_) drawNode(dc, n, fade);

        // Reset transform
        XFORM identity = { 1,0,0,1,0,0 };
        SetWorldTransform(dc, &identity);
        SetGraphicsMode(dc, oldMode);
    }

    drawLegend(dc, W, H);
}

// ---------------------------------------------------------------------------
// Background with dot grid
// ---------------------------------------------------------------------------
void Visualizer::drawBackground(HDC dc, int W, int H) {
    HBRUSH bg = CreateSolidBrush(BG);
    RECT all  = { 0, 0, W, H };
    FillRect(dc, &all, bg);
    DeleteObject(bg);

    const int GRID = 28;
    for (int y = TOP_BAR_H + GRID; y < H; y += GRID)
        for (int x = GRID; x < W; x += GRID)
            SetPixel(dc, x, y, DOT_COL);
}

// ---------------------------------------------------------------------------
// Top bar
// ---------------------------------------------------------------------------
void Visualizer::drawTopBar(HDC dc, int W) {
    HBRUSH br = CreateSolidBrush(BG_BAR);
    RECT bar  = { 0, 0, W, TOP_BAR_H };
    FillRect(dc, &bar, br);
    DeleteObject(br);

    HPEN lp = CreatePen(PS_SOLID, 1, BG_BAR_LINE);
    SelectObject(dc, lp);
    MoveToEx(dc, 0, TOP_BAR_H - 1, nullptr);
    LineTo  (dc, W, TOP_BAR_H - 1);
    DeleteObject(lp);

    int row1Y = 5, row1H = 34;
    drawTextEx(dc, L"B+ Tree Visualizer",
               { 10, row1Y, 210, row1Y + row1H },
               TEXT_ACCENT, 13, true, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    drawSelectors(dc, W / 2, row1Y + (row1H - 28) / 2);

    int row2Y = row1Y + row1H + 3;
    drawOrderPanel(dc, 10, row2Y, W - 20);
}

// ---------------------------------------------------------------------------
void Visualizer::drawSelectors(HDC dc, int cx, int y) {
    static const wchar_t* LABELS[] = { L"Name Tree", L"Size Tree", L"Data Tree" };
    const int BW = 98, BH = 28, GAP = 6;
    int startX = cx - (3 * BW + 2 * GAP) / 2;

    HFONT hf = makeFont(12, true);
    HFONT of = (HFONT)SelectObject(dc, hf);
    SetBkMode(dc, TRANSPARENT);

    RECT* rects[3] = { &btnName_, &btnSize_, &btnDate_ };
    for (int i = 0; i < 3; ++i) {
        RECT r = { startX + i*(BW+GAP), y,
                   startX + i*(BW+GAP) + BW, y + BH };
        *rects[i] = r;
        bool active = (static_cast<int>(activeTree_) == i);
        roundRect(dc, r, 7, 7,
                  active ? BTN_ACT  : BTN_INACT,
                  active ? BTN_BRD_ACT : BTN_BRD_IN, 2);
        SetTextColor(dc, active ? RGB(235,245,255) : TEXT_DIM);
        DrawTextW(dc, LABELS[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, of); DeleteObject(hf);
}

// ---------------------------------------------------------------------------
// Order panel
// ---------------------------------------------------------------------------
void Visualizer::drawOrderPanel(HDC dc, int startX, int y, int availW) {
    const int LBL_W = 52, BTN_W = 22, VAL_W = 30, GAP = 8, H = 26;
    const int APPLY_W = 68;

    int x = startX;

    auto placeSpin = [&](SpinCtrl& s) {
        s.label = { x, y, x + LBL_W, y + H };
        x += LBL_W + 2;
        s.minus = { x, y, x + BTN_W, y + H };
        x += BTN_W;
        s.plus  = { x + VAL_W, y, x + VAL_W + BTN_W, y + H };
        x      += VAL_W + BTN_W + GAP;
    };
    placeSpin(spinMin_);
    placeSpin(spinMax_);
    placeSpin(spinCur_);

    int ax    = startX + availW - APPLY_W;
    btnApply_ = { ax, y, ax + APPLY_W, y + H };

    HFONT hfB = makeFont(11, true);
    HFONT hfN = makeFont(11, false);
    SetBkMode(dc, TRANSPARENT);

    auto drawOneSpin = [&](SpinCtrl& s) {
        HFONT of = (HFONT)SelectObject(dc, hfB);
        SetTextColor(dc, TEXT_DIM);
        DrawTextW(dc, s.caption, -1, &s.label,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        roundRect(dc, s.minus, 4, 4, SPIN_BTN, SPIN_BRD, 1);
        SetTextColor(dc, TEXT_PRI);
        DrawTextW(dc, L"\u2212", -1, &s.minus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT vr = { s.minus.right, y, s.plus.left, y + H };
        roundRect(dc, vr, 3, 3, SPIN_FILL, SPIN_BRD, 1);
        wchar_t buf[8]; _itow_s(*s.value, buf, 8, 10);
        SetTextColor(dc, TEXT_ACCENT);
        DrawTextW(dc, buf, -1, &vr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        roundRect(dc, s.plus, 4, 4, SPIN_BTN, SPIN_BRD, 1);
        SetTextColor(dc, TEXT_PRI);
        DrawTextW(dc, L"+", -1, &s.plus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(dc, of);
    };

    drawOneSpin(spinMin_);
    drawOneSpin(spinMax_);
    drawOneSpin(spinCur_);

    COLORREF af = orderDirty_ ? APPLY_DIRTY : APPLY_CLEAN;
    COLORREF ab = orderDirty_ ? APPLY_BRD_D : APPLY_BRD_C;
    roundRect(dc, btnApply_, 6, 6, af, ab, 2);
    HFONT of3 = (HFONT)SelectObject(dc, hfB);
    SetTextColor(dc, orderDirty_ ? RGB(255,220,100) : RGB(100,220,140));
    DrawTextW(dc, L"Apply", -1, &btnApply_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of3);

    DeleteObject(hfB);
    DeleteObject(hfN);
}

// ---------------------------------------------------------------------------
// Arrows  (drawn in canvas/world space)
// ---------------------------------------------------------------------------
void Visualizer::drawArrows(HDC dc) {
    // Parent → child lines
    HPEN cp = CreatePen(PS_SOLID, 2, ARR_CHILD);
    SelectObject(dc, cp);
    for (auto& n : nodes_) {
        if (n.isLeaf) continue;
        for (auto* ch : n.childPtrs) {
            if (!ch) continue;
            int px = n.rect.left  + NODE_W / 2;
            int py = n.rect.bottom;
            int cx = ch->rect.left + NODE_W / 2;
            int cy = ch->rect.top;
            MoveToEx(dc, px, py, nullptr);
            LineTo  (dc, cx, cy);
            arrowHead(dc, cx, cy, (double)(cx - px), (double)(cy - py), 8, ARR_CHILD);
        }
    }
    DeleteObject(cp);

    // Leaf chain arrows (horizontal, dashed green)
    // Collect leaves sorted by x position
    std::vector<int> leafIdx;
    for (int i = 0; i < (int)nodes_.size(); ++i)
        if (nodes_[i].isLeaf) leafIdx.push_back(i);

    std::sort(leafIdx.begin(), leafIdx.end(), [&](int a, int b) {
        return nodes_[a].rect.left < nodes_[b].rect.left;
    });

    HPEN lp = CreatePen(PS_DOT, 1, ARR_LEAF);
    SelectObject(dc, lp);
    SetBkMode(dc, TRANSPARENT);

    for (int k = 0; k + 1 < (int)leafIdx.size(); ++k) {
        int i = leafIdx[k];
        if (!nodes_[i].hasNext) continue;
        int j = leafIdx[k + 1];

        int y1 = nodes_[i].rect.top + NODE_H / 2;
        int x1 = nodes_[i].rect.right;
        int y2 = nodes_[j].rect.top + NODE_H / 2;
        int x2 = nodes_[j].rect.left;

        MoveToEx(dc, x1 + 2, y1, nullptr);
        LineTo  (dc, x2 - 2, y2);
        arrowHead(dc, x2, y2, (double)(x2 - x1), 0.0, 7, ARR_LEAF);
    }
    DeleteObject(lp);
}

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------
void Visualizer::drawNode(HDC dc, const NodeDraw& n, float fade) {
    const RECT& r = n.rect;
    COLORREF baseFill = n.isLeaf ? LEAF_FILL : NODE_FILL;
    COLORREF baseBrd  = n.isLeaf ? LEAF_BRD  : NODE_BRD;
    COLORREF fill = baseFill, brd = baseBrd;

    if (fade > 0.f) {
        if      (splitHighlight_  == n.nodePtr)     { fill = lerpC(baseFill, HL_SPLIT_F,  fade); brd = lerpC(baseBrd, HL_SPLIT_B,  fade); }
        else if (mergeHighlight_  == n.nodePtr)     { fill = lerpC(baseFill, HL_MERGE_F,  fade); brd = lerpC(baseBrd, HL_MERGE_B,  fade); }
        else if (insertHighlight_ == n.nodePtr)     { fill = lerpC(baseFill, HL_INSERT_F, fade); brd = lerpC(baseBrd, HL_INSERT_B, fade); }
        else if (searchHighlight_.count(n.nodePtr)) { fill = lerpC(baseFill, HL_SEARCH_F, fade); brd = lerpC(baseBrd, HL_SEARCH_B, fade); }
    }

    roundRect(dc, r, 9, 9, fill, brd, 2);

    // Glow ring
    bool isHL = (fade > 0.3f) &&
        (splitHighlight_ == n.nodePtr || mergeHighlight_ == n.nodePtr ||
         insertHighlight_ == n.nodePtr || searchHighlight_.count(n.nodePtr));
    if (isHL) {
        RECT gr = { r.left-3, r.top-3, r.right+3, r.bottom+3 };
        HPEN   gp = CreatePen(PS_SOLID, 2, lerpC(BG, brd, fade * 0.6f));
        HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
        HPEN   op = (HPEN)SelectObject(dc, gp);
        HBRUSH ob = (HBRUSH)SelectObject(dc, nb);
        RoundRect(dc, gr.left, gr.top, gr.right, gr.bottom, 13, 13);
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(gp);
    }

    // Keys
    HFONT hf = makeFont(11, false);
    HFONT of = (HFONT)SelectObject(dc, hf);
    SetBkMode(dc, TRANSPARENT);

    if (n.numKeys > 0) {
        int sliceW = NODE_W / n.numKeys;

        HPEN dp  = CreatePen(PS_SOLID, 1, DIVIDER_COL);
        HPEN op2 = (HPEN)SelectObject(dc, dp);
        for (int i = 1; i < n.numKeys; ++i) {
            int x = r.left + i * sliceW;
            MoveToEx(dc, x, r.top + 5,    nullptr);
            LineTo  (dc, x, r.bottom - 5);
        }
        SelectObject(dc, op2); DeleteObject(dp);

        SetTextColor(dc, TEXT_PRI);
        for (int i = 0; i < n.numKeys && i < (int)n.keyStrings.size(); ++i) {
            RECT kr = { r.left + i * sliceW,     r.top,
                        r.left + (i+1) * sliceW, r.bottom };
            DrawTextW(dc, n.keyStrings[i].c_str(), -1, &kr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    } else {
        SetTextColor(dc, TEXT_DIM);
        RECT tr = r;
        DrawTextW(dc, L"(empty)", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(dc, of); DeleteObject(hf);

    // Type badge (L / I)
    {
        HFONT bf  = makeFont(9, true);
        HFONT ob2 = (HFONT)SelectObject(dc, bf);
        SetTextColor(dc, n.isLeaf ? lerpC(LEAF_BRD, HL_INSERT_B, 0.5f) : TEXT_DIM);
        RECT tag = { r.right - 16, r.top + 2, r.right - 2, r.top + 14 };
        DrawTextW(dc, n.isLeaf ? L"L" : L"I", -1, &tag,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, ob2); DeleteObject(bf);
    }
}

// ---------------------------------------------------------------------------
// Empty state
// ---------------------------------------------------------------------------
void Visualizer::drawEmpty(HDC dc, int W, int H) {
    int cx = W / 2, cy = (TOP_BAR_H + H) / 2;
    HPEN p  = CreatePen(PS_SOLID, 1, RGB(35, 42, 70));
    HPEN op = (HPEN)SelectObject(dc, p);
    MoveToEx(dc, cx,    cy-40, nullptr); LineTo(dc, cx,    cy+10);
    MoveToEx(dc, cx,    cy-20, nullptr); LineTo(dc, cx-50, cy+10);
    MoveToEx(dc, cx,    cy-20, nullptr); LineTo(dc, cx+50, cy+10);
    MoveToEx(dc, cx-50, cy+10, nullptr); LineTo(dc, cx-80, cy+40);
    MoveToEx(dc, cx-50, cy+10, nullptr); LineTo(dc, cx-20, cy+40);
    MoveToEx(dc, cx+50, cy+10, nullptr); LineTo(dc, cx+20, cy+40);
    MoveToEx(dc, cx+50, cy+10, nullptr); LineTo(dc, cx+80, cy+40);
    SelectObject(dc, op); DeleteObject(p);

    HFONT hf = makeFont(14, false);
    HFONT of = (HFONT)SelectObject(dc, hf);
    SetTextColor(dc, TEXT_DIM);
    SetBkMode(dc, TRANSPARENT);
    RECT tr = { 0, cy + 55, W, cy + 80 };
    DrawTextW(dc, L"Tree is empty \u2014 add files to begin", -1, &tr,
              DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, of); DeleteObject(hf);
}

// ---------------------------------------------------------------------------
// Legend
// ---------------------------------------------------------------------------
void Visualizer::drawLegend(HDC dc, int /*W*/, int H) {
    struct E { COLORREF f, b; const wchar_t* lbl; };
    static const E entries[] = {
        { NODE_FILL,   NODE_BRD,    L"Internal" },
        { LEAF_FILL,   LEAF_BRD,    L"Leaf"     },
        { HL_SEARCH_F, HL_SEARCH_B, L"Search"   },
        { HL_INSERT_F, HL_INSERT_B, L"Insert"   },
        { HL_SPLIT_F,  HL_SPLIT_B,  L"Split"    },
        { HL_MERGE_F,  HL_MERGE_B,  L"Merge"    },
    };
    const int COUNT = (int)(sizeof(entries)/sizeof(entries[0]));

    HFONT hf = makeFont(10, false);
    HFONT of = (HFONT)SelectObject(dc, hf);
    SetBkMode(dc, TRANSPARENT);

    int bx = 6, by = H - 4 - COUNT * 17;
    for (int i = 0; i < COUNT; ++i) {
        RECT sw = { bx, by+2, bx+12, by+13 };
        roundRect(dc, sw, 3, 3, entries[i].f, entries[i].b, 1);
        SetTextColor(dc, TEXT_DIM);
        RECT tr = { bx+16, by, bx+100, by+16 };
        DrawTextW(dc, entries[i].lbl, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        by += 17;
    }
    SelectObject(dc, of); DeleteObject(hf);
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
void Visualizer::roundRect(HDC dc, RECT r, int rx, int ry,
                            COLORREF fill, COLORREF brd, int pw)
{
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pn = CreatePen(PS_SOLID, pw, brd);
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    HPEN   op = (HPEN)  SelectObject(dc, pn);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rx, ry);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pn);
}

void Visualizer::drawTextEx(HDC dc, const wchar_t* s, RECT r,
                             COLORREF col, int sz, bool bold, UINT fmt)
{
    HFONT hf = makeFont(sz, bold);
    HFONT of = (HFONT)SelectObject(dc, hf);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, fmt);
    SelectObject(dc, of); DeleteObject(hf);
}

HFONT Visualizer::makeFont(int sz, bool bold) {
    return CreateFontW(sz, 0, 0, 0,
                       bold ? FW_BOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

COLORREF Visualizer::lerpC(COLORREF a, COLORREF b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    return RGB(
        (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
        (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
        (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t)
    );
}

void Visualizer::arrowHead(HDC dc, int x, int y,
                            double dx, double dy, int sz, COLORREF col)
{
    double len = sqrt(dx*dx + dy*dy);
    if (len < 1.0) return;
    double ux = dx/len, uy = dy/len;
    double lx = -uy*sz*0.5, ly = ux*sz*0.5;
    POINT head[3] = {
        { x, y },
        { (int)(x - ux*sz + lx), (int)(y - uy*sz + ly) },
        { (int)(x - ux*sz - lx), (int)(y - uy*sz - ly) }
    };
    HBRUSH br = CreateSolidBrush(col);
    HPEN   pn = CreatePen(PS_SOLID, 1, col);
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    HPEN   op = (HPEN)  SelectObject(dc, pn);
    Polygon(dc, head, 3);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pn);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
void Visualizer::onMouseWheel(int delta) {
    float f = (delta > 0) ? 1.12f : (1.f/1.12f);
    zoom_ = std::clamp(zoom_ * f, 0.15f, 5.0f);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Visualizer::onLButtonDown(int x, int y) {
    if (hitTest(btnName_, x, y)) {
        setActiveTree(ActiveTree::Name);
        PostMessage(parent_, WM_USER+200, 0, 0);
        InvalidateRect(hwnd_, nullptr, FALSE); return;
    }
    if (hitTest(btnSize_, x, y)) {
        setActiveTree(ActiveTree::Size);
        PostMessage(parent_, WM_USER+200, 1, 0);
        InvalidateRect(hwnd_, nullptr, FALSE); return;
    }
    if (hitTest(btnDate_, x, y)) {
        setActiveTree(ActiveTree::Date);
        PostMessage(parent_, WM_USER+200, 2, 0);
        InvalidateRect(hwnd_, nullptr, FALSE); return;
    }

    auto handleSpin = [&](SpinCtrl& s) -> bool {
        if (hitTest(s.minus, x, y)) {
            if (*s.value > s.minVal) { --(*s.value); orderDirty_ = true; }
            InvalidateRect(hwnd_, nullptr, FALSE); return true;
        }
        if (hitTest(s.plus, x, y)) {
            if (*s.value < s.maxVal) { ++(*s.value); orderDirty_ = true; }
            InvalidateRect(hwnd_, nullptr, FALSE); return true;
        }
        return false;
    };
    if (handleSpin(spinMin_)) return;
    if (handleSpin(spinMax_)) return;
    if (handleSpin(spinCur_)) return;

    if (hitTest(btnApply_, x, y)) {
        orderCfg_.curOrder = std::clamp(orderCfg_.curOrder,
                                        orderCfg_.minOrder,
                                        orderCfg_.maxOrder);
        orderDirty_ = false;
        PostMessage(parent_, WM_USER+202,
                    (WPARAM)orderCfg_.curOrder,
                    MAKELPARAM(orderCfg_.minOrder, orderCfg_.maxOrder));
        InvalidateRect(hwnd_, nullptr, FALSE); return;
    }

    if (y > TOP_BAR_H) {
        dragging_  = true;
        dragStart_ = { x, y };
        dragPanX_  = panX_;
        dragPanY_  = panY_;
        SetCapture(hwnd_);
    }
}

void Visualizer::onLButtonUp(int, int) {
    dragging_ = false;
    ReleaseCapture();
}

void Visualizer::onMouseMove(int x, int y) {
    if (!dragging_) return;
    panX_ = dragPanX_ + (x - dragStart_.x);
    panY_ = dragPanY_ + (y - dragStart_.y);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Visualizer::onTimer() {
    if (animTicks_ <= 0) return;
    --animTicks_;
    if (animTicks_ == 0) {
        searchHighlight_.clear();
        splitHighlight_ = mergeHighlight_ = insertHighlight_ = nullptr;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Visualizer::onSize(int, int) {
    if (offDC_)  { DeleteDC(offDC_);      offDC_  = nullptr; }
    if (offBmp_) { DeleteObject(offBmp_); offBmp_ = nullptr; }
    offW_ = offH_ = 0;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK Visualizer::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Visualizer* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<Visualizer*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<Visualizer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_PAINT:       self->onPaint();                                return 0;
    case WM_ERASEBKGND:                                                  return 1;
    case WM_SIZE:        self->onSize(LOWORD(lp), HIWORD(lp));           return 0;
    case WM_MOUSEWHEEL:  self->onMouseWheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
    case WM_LBUTTONDOWN: self->onLButtonDown(LOWORD(lp), HIWORD(lp));    return 0;
    case WM_LBUTTONUP:   self->onLButtonUp  (LOWORD(lp), HIWORD(lp));    return 0;
    case WM_MOUSEMOVE:   self->onMouseMove  (LOWORD(lp), HIWORD(lp));    return 0;
    case WM_TIMER:       if (wp == TIMER_ANIM) self->onTimer();           return 0;
    case WM_DESTROY:                                                      return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}