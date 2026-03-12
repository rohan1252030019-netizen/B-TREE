// =============================================================================
// INTEGRATION PATCH  –  add this to your existing main.cpp / MainWndProc
// =============================================================================
//
// The new visualizer sends TWO messages to the parent window:
//
//  WM_USER + 200  (already handled)  →  tree selector changed
//                 wParam = 0/1/2     →  Name / Size / Date
//
//  WM_USER + 201  (NEW)              →  user clicked "Apply" in order panel
//                 wParam  = new curOrder  (e.g. 5)
//                 lParam  = MAKELPARAM(minOrder, maxOrder)  (e.g. 3, 7)
//
// In your MainWndProc WM_COMMAND / message handler, add:
// =============================================================================

/*

case WM_USER + 200:
    RefreshVisualizer(hwnd, g_vis);
    return 0;

// ---- NEW: handle order change from the visualizer's Apply button ----
case WM_USER + 201: {
    int newOrder = (int)wp;
    int newMin   = LOWORD(lp);
    int newMax   = HIWORD(lp);

    // Clamp just in case
    if (newOrder < 3)  newOrder = 3;
    if (newMin   < 3)  newMin   = 3;
    if (newMax   < newMin) newMax = newMin;
    newOrder = std::clamp(newOrder, newMin, newMax);

    g_order = newOrder;

    // Rebuild all three trees with the new order
    g_index.rebuild(g_order);

    // Update the order edit box in the left panel (if you have one)
    SetDlgItemInt(hwnd, ID_EDIT_ORDER, g_order, FALSE);

    // Sync the OrderConfig back to the visualizer
    OrderConfig cfg;
    cfg.minOrder = newMin;
    cfg.maxOrder = newMax;
    cfg.curOrder = newOrder;
    g_vis->setOrderConfig(cfg);

    // Append to output log
    HWND out = GetDlgItem(hwnd, ID_OUTPUT);
    if (out) {
        std::wstring msg = L"Order rebuilt: m=" + std::to_wstring(g_order)
                         + L"  [min=" + std::to_wstring(newMin)
                         + L", max=" + std::to_wstring(newMax) + L"]\r\n";
        int len = GetWindowTextLengthW(out);
        SendMessageW(out, EM_SETSEL, len, len);
        SendMessageW(out, EM_REPLACESEL, 0, (LPARAM)msg.c_str());
    }

    RefreshVisualizer(hwnd, g_vis);
    return 0;
}

*/

// =============================================================================
// ALSO:  After creating the Visualizer in WM_CREATE, set the initial config:
// =============================================================================
/*

    g_vis = new Visualizer();
    g_vis->create(hwnd, 280, 0, 980, 780, hInst);

    // Push initial order config so the spinners show the right values
    OrderConfig initCfg;
    initCfg.minOrder = 3;
    initCfg.maxOrder = 7;
    initCfg.curOrder = g_order;   // e.g. 4
    g_vis->setOrderConfig(initCfg);

*/

// =============================================================================
// SPIN CONTROL BOUNDS (optional, tune as needed):
//   spinMin_  → min 3, max 10    (minimum B+ order the user can pick)
//   spinMax_  → min 3, max 20    (maximum B+ order the user can pick)
//   spinCur_  → min 3, max 20    (current order – clamped to [min,max] on Apply)
// These are set inside Visualizer::create() and can be changed via setOrderConfig().
// =============================================================================