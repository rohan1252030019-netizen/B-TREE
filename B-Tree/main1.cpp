#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "gui/window.h"

int WINAPI wWinMain(HINSTANCE hInstance,
                    HINSTANCE /*hPrevInstance*/,
                    LPWSTR    /*lpCmdLine*/,
                    int       nCmdShow)
{
    AppWindow app;
    return app.run(hInstance, nCmdShow);
}
