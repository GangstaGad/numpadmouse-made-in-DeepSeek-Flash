#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static BOOL CALLBACK MonProc(HMONITOR h, HDC dc, LPRECT r, LPARAM p)
{
    MONITORINFOEXW mi;
    UINT dx = 0, dy = 0;
    typedef BOOL (WINAPI *PFN)(HMONITOR, int, UINT *, UINT *);
    PFN f = (PFN)(void *)GetProcAddress(GetModuleHandleW(L"shcore.dll"), "GetDpiForMonitor");
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(h, (MONITORINFO *)&mi);
    if (f) f(h, 0, &dx, &dy);
    wprintf(L"mon %-30s rect=%ld,%ld..%ld,%ld  work=%ld,%ld..%ld,%ld  dpi=%ux%u primary=%d\n",
            mi.szDevice, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
            mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom, dx, dy,
            (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0);
    (void)dc; (void)r; (void)p;
    return TRUE;
}

int main(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *PFN_SPDAC)(HANDLE);
    typedef UINT (WINAPI *PFN_GDFS)(void);
    PFN_SPDAC a = (PFN_SPDAC)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    PFN_GDFS g = (PFN_GDFS)(void *)GetProcAddress(u, "GetDpiForSystem");
    BOOL r = a ? a((HANDLE)-4) : FALSE;
    POINT p;
    GetCursorPos(&p);
    printf("SetProcessDpiAwarenessContext(PMv2)=%d  GetDpiForSystem=%u\n", (int)r, g ? g() : 0);
    printf("virtual screen x=%d y=%d w=%d h=%d\n",
           GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
           GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    printf("primary %dx%d  cursor %ld,%ld\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), p.x, p.y);
    EnumDisplayMonitors(NULL, NULL, MonProc, 0);
    return 0;
}
