/* ============================================================================
   NumpadMouse — управление мышью с цифрового блока клавиатуры.
   Только Win32 API, статический CRT, без .NET / VC++ runtime / внешних DLL.

   Win+Alt+K  — включить / выключить режим
   Win+Alt+Q  — выход

   В режиме:
     ↑ / ↓ / ← / →   — курсор вверх / вниз / влево / вправо
     7                — левая кнопка мыши
     9                — правая кнопка мыши
     5                — средняя кнопка
     0 / .            — колесо вниз / вверх
   (стрелки нумпада 8/2/4/6 и 7/9/5/0/. работают так же; NumLock не важен)
   Клавиши в режиме съедаются — цифры не печатаются.
   ========================================================================== */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <string.h>
#include <stdarg.h>

#define APP_NAME   L"NumpadMouse"

#define WM_TRAY    (WM_APP + 1)
#define WM_BEEP    (WM_APP + 2)
#define ID_TRAY    1
#define IDT_MOVE   1

#define MENU_TOGGLE    1001
#define MENU_AUTOSTART 1002
#define MENU_EXIT      1003
#define MENU_HINT      1004

#define TICK_MS 12

enum {
    B_UP = 0, B_DOWN, B_LEFT, B_RIGHT,
    B_BTN_L, B_BTN_R, B_BTN_M,
    B_WH_UP, B_WH_DOWN,
    B_COUNT
};
#define BIT(b)   (1u << (b))
#define MOVEBITS (BIT(B_UP) | BIT(B_DOWN) | BIT(B_LEFT) | BIT(B_RIGHT) | BIT(B_WH_UP) | BIT(B_WH_DOWN))

static const int kSteps[] = { 2, 3, 4, 5, 7, 9, 12, 16, 21, 27 };
static const int kStepsN   = (int)(sizeof(kSteps) / sizeof(kSteps[0]));

static HWND      g_hwnd;
static HHOOK     g_hook;
static HMENU     g_menu;
static HINSTANCE g_inst;
static BOOL      g_mode;
static BOOL      g_timerOn;
static BOOL      g_kDown;
static DWORD     g_held;
static DWORD     g_vk[B_COUNT];
static ULONGLONG g_since[B_COUNT];
static ULONGLONG g_downAt[B_COUNT];
static ULONGLONG g_wheelAt;
static HICON     g_iconOn, g_iconOff;
static UINT      g_wmTaskbarCreated;

/* ------------------------------------------------------------------ utils */

static HANDLE g_logFile;
static BOOL   g_dbg;

static void Log(const WCHAR *fmt, ...)
{
    WCHAR buf[512], line[560];
    va_list ap;
    DWORD w = 0;
    if (!g_dbg || g_logFile == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    wvsprintfW(buf, fmt, ap);
    va_end(ap);
    wsprintfW(line, L"%lu %s\r\n", (unsigned long)GetTickCount64(), buf);
    WriteFile(g_logFile, line, (DWORD)(lstrlenW(line) * sizeof(WCHAR)), &w, NULL);
}

static void LogOpen(void)
{
    WCHAR path[MAX_PATH];
    g_dbg = GetEnvironmentVariableW(L"NUMPADMOUSE_DEBUG", NULL, 0) > 0;
    g_logFile = INVALID_HANDLE_VALUE;
    if (!g_dbg) return;
    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"numpadmouse.log");
    g_logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void GetSelfPath(WCHAR *buf, DWORD cch)
{
    if (!GetModuleFileNameW(NULL, buf, cch)) buf[0] = 0;
}

static void GetInstallExe(WCHAR *out, DWORD cch)
{
    WCHAR dir[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH * 2);
    if (!n) n = GetEnvironmentVariableW(L"USERPROFILE", dir, MAX_PATH * 2);
    if (!n) { lstrcpynW(dir, L"C:\\ProgramData", MAX_PATH * 2); }
    lstrcpynW(out, dir, (int)cch);
    lstrcatW(out, L"\\NumpadMouse\\NumpadMouse.exe");
}

static void DirOf(WCHAR *path)
{
    WCHAR *p = wcsrchr(path, L'\\');
    if (p) *p = 0;
}

static BOOL IsElevated(void)
{
    HANDLE tok;
    TOKEN_ELEVATION te;
    DWORD cb = 0;
    BOOL r = FALSE;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return FALSE;
    if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &cb))
        r = (te.TokenIsElevated != 0);
    CloseHandle(tok);
    return r;
}

static BOOL GetUserSidString(WCHAR *out, DWORD cch)
{
    HANDLE tok;
    BOOL ok = FALSE;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return FALSE;
    DWORD need = 0;
    GetTokenInformation(tok, TokenUser, NULL, 0, &need);
    if (need) {
        TOKEN_USER *tu = (TOKEN_USER *)HeapAlloc(GetProcessHeap(), 0, need);
        if (tu) {
            if (GetTokenInformation(tok, TokenUser, tu, need, &need)) {
                LPWSTR s = NULL;
                if (ConvertSidToStringSidW(tu->User.Sid, &s)) {
                    lstrcpynW(out, s, (int)cch);
                    LocalFree(s);
                    ok = TRUE;
                }
            }
            HeapFree(GetProcessHeap(), 0, tu);
        }
    }
    CloseHandle(tok);
    return ok;
}

/* runs a command line hidden, waits, returns TRUE on exit code 0 */
static BOOL RunHidden(const WCHAR *cmdline)
{
    WCHAR buf[2048];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    lstrcpynW(buf, cmdline, 2048);
    if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return FALSE;
    WaitForSingleObject(pi.hProcess, 20000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

/* -------------------------------------------------------------- autostart */

#define RUNKEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static BOOL SetRunValue(const WCHAR *exe)
{
    HKEY k;
    WCHAR v[MAX_PATH + 8];
    LONG r;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return FALSE;
    wsprintfW(v, L"\"%s\"", exe);
    r = RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE *)v, (DWORD)((lstrlenW(v) + 1) * sizeof(WCHAR)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static void RemoveRunValue(void)
{
    RegDeleteKeyValueW(HKEY_CURRENT_USER, RUNKEY, APP_NAME);
}

static BOOL HasRunValue(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, RUNKEY, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static BOOL TaskExists(void)
{
    return RunHidden(L"schtasks.exe /Query /TN \"NumpadMouse\"");
}

static BOOL DeleteTask(void)
{
    return RunHidden(L"schtasks.exe /Delete /F /TN \"NumpadMouse\"");
}

static BOOL CreateLogonTask(const WCHAR *exe)
{
    WCHAR sid[184], xml[8192], tmp[MAX_PATH], cmd[2048];
    HANDLE f;
    DWORD w = 0;
    BOOL ok;

    if (!GetUserSidString(sid, 184)) return FALSE;

    lstrcpyW(xml, L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L" <RegistrationInfo><Description>NumpadMouse</Description></RegistrationInfo>\r\n"
        L" <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>");
    lstrcatW(xml, sid);
    lstrcatW(xml, L"</UserId></LogonTrigger></Triggers>\r\n"
        L" <Principals><Principal id=\"Author\"><UserId>");
    lstrcatW(xml, sid);
    lstrcatW(xml, L"</UserId><LogonType>InteractiveToken</LogonType>"
        L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L" <Settings>\r\n"
        L"  <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
        L"  <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"  <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"  <AllowHardTerminate>true</AllowHardTerminate>\r\n"
        L"  <StartWhenAvailable>true</StartWhenAvailable>\r\n"
        L"  <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n"
        L"  <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>\r\n"
        L"  <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"  <Enabled>true</Enabled>\r\n"
        L"  <Hidden>false</Hidden>\r\n"
        L"  <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
        L"  <WakeToRun>false</WakeToRun>\r\n"
        L"  <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
        L"  <Priority>7</Priority>\r\n"
        L" </Settings>\r\n"
        L" <Actions Context=\"Author\"><Exec><Command>");
    lstrcatW(xml, exe);
    lstrcatW(xml, L"</Command></Exec></Actions>\r\n"
        L"</Task>\r\n");

    GetTempPathW(MAX_PATH, tmp);
    lstrcatW(tmp, L"numpadmouse_task.xml");
    f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    WriteFile(f, "\xFF\xFE", 2, &w, NULL);
    WriteFile(f, xml, (DWORD)(lstrlenW(xml) * sizeof(WCHAR)), &w, NULL);
    CloseHandle(f);

    lstrcpyW(cmd, L"schtasks.exe /Create /F /TN \"NumpadMouse\" /XML \"");
    lstrcatW(cmd, tmp);
    lstrcatW(cmd, L"\"");
    ok = RunHidden(cmd);
    DeleteFileW(tmp);
    return ok;
}

static BOOL AutostartOn(void)
{
    return TaskExists() || HasRunValue();
}

static BOOL InstallSelf(void)
{
    WCHAR self[MAX_PATH], dst[MAX_PATH], dir[MAX_PATH];
    GetSelfPath(self, MAX_PATH);
    GetInstallExe(dst, MAX_PATH);

    if (lstrcmpiW(self, dst) != 0) {
        lstrcpynW(dir, dst, MAX_PATH);
        DirOf(dir);
        CreateDirectoryW(dir, NULL);
        /* если файл занят работающей копией — не страшно, Run/задача всё равно настроятся */
        CopyFileW(self, dst, FALSE);
    }

    if (TaskExists()) {
        RemoveRunValue();
    } else if (!(IsElevated() && CreateLogonTask(dst))) {
        SetRunValue(dst);
    } else {
        RemoveRunValue();
    }
    return TRUE;
}

static void UninstallSelf(void)
{
    WCHAR self[MAX_PATH], dst[MAX_PATH], dir[MAX_PATH], cmd[MAX_PATH * 3];
    GetSelfPath(self, MAX_PATH);
    GetInstallExe(dst, MAX_PATH);
    RemoveRunValue();
    DeleteTask();
    lstrcpynW(dir, dst, MAX_PATH);
    DirOf(dir);
    if (lstrcmpiW(self, dst) != 0) {
        DeleteFileW(dst);
        RemoveDirectoryW(dir);
    } else {
        wsprintfW(cmd, L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"%s\" & rmdir \"%s\"", dst, dir);
        RunHidden(cmd);
    }
}

/* ------------------------------------------------------------------ mouse */

static void MoveBy(int dx, int dy)
{
    POINT p;
    INPUT in;
    int vx, vy, vw, vh, nx, ny;

    if (!GetCursorPos(&p)) return;
    vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw < 2) vw = 2;
    if (vh < 2) vh = 2;

    nx = p.x + dx;
    ny = p.y + dy;
    if (nx < vx) nx = vx;
    if (nx > vx + vw - 1) nx = vx + vw - 1;
    if (ny < vy) ny = vy;
    if (ny > vy + vh - 1) ny = vy + vh - 1;

    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)(((LONGLONG)(nx - vx) * 65535) / (vw - 1));
    in.mi.dy = (LONG)(((LONGLONG)(ny - vy) * 65535) / (vh - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(in));
}

static void SendButton(int b, BOOL down)
{
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    if (b == B_BTN_L)      in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN   : MOUSEEVENTF_LEFTUP;
    else if (b == B_BTN_R) in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;
    else                   in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    SendInput(1, &in, sizeof(in));
}

static void SendWheel(int dir)
{
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)(dir * WHEEL_DELTA);
    SendInput(1, &in, sizeof(in));
}

static void UpdateTimer(void)
{
    BOOL need = (g_held & MOVEBITS) != 0;
    if (need && !g_timerOn) {
        g_timerOn = SetTimer(g_hwnd, IDT_MOVE, TICK_MS, NULL) != 0;
    } else if (!need && g_timerOn) {
        KillTimer(g_hwnd, IDT_MOVE);
        g_timerOn = FALSE;
    }
}

static void PressBit(int b, DWORD vk)
{
    if (g_held & BIT(b)) return;
    g_held |= BIT(b);
    g_vk[b] = vk;
    g_since[b] = GetTickCount64();
    g_downAt[b] = g_since[b];
    if (b == B_BTN_L || b == B_BTN_R || b == B_BTN_M) {
        SendButton(b, TRUE);
    } else if (b == B_WH_UP || b == B_WH_DOWN) {
        SendWheel(b == B_WH_UP ? +1 : -1);
        g_wheelAt = g_since[b];
    }
    UpdateTimer();
}

static void ReleaseBit(int b)
{
    if (!(g_held & BIT(b))) return;
    g_held &= ~BIT(b);
    if (b == B_BTN_L || b == B_BTN_R || b == B_BTN_M) SendButton(b, FALSE);
    UpdateTimer();
}

static void ReleaseAll(void)
{
    int i;
    for (i = 0; i < B_COUNT; i++) ReleaseBit(i);
}

static int StepFor(ULONGLONG heldMs)
{
    int i = (int)(heldMs / 170);
    if (i >= kStepsN) i = kStepsN - 1;
    return kSteps[i];
}

static void OnTick(void)
{
    ULONGLONG now;
    BOOL r, l, u, d;
    int dx = 0, dy = 0, i;

    if (!g_mode) { ReleaseAll(); return; }

    now = GetTickCount64();

    /* страховка от «залипших» клавиш: физическое удержание клавиши даёт
       автоповтор (WM_KEYDOWN каждые 30-50 мс), поэтому долгая тишина =
       потерянный keyup (secure desktop, смена раскладки, выдернутый хук). */
    for (i = 0; i < B_COUNT; i++) {
        if (g_held & BIT(i)) {
            if (now - g_downAt[i] > 1500) ReleaseBit(i);
        }
    }

    r = (g_held & BIT(B_RIGHT)) != 0;
    l = (g_held & BIT(B_LEFT)) != 0;
    u = (g_held & BIT(B_UP)) != 0;
    d = (g_held & BIT(B_DOWN)) != 0;

    if (r && !l)      dx =  StepFor(now - g_since[B_RIGHT]);
    else if (l && !r) dx = -StepFor(now - g_since[B_LEFT]);
    if (d && !u)      dy =  StepFor(now - g_since[B_DOWN]);
    else if (u && !d) dy = -StepFor(now - g_since[B_UP]);

    if (dx && dy) {                       /* диагональ — не быстрее прямой */
        dx = dx * 7 / 10;
        dy = dy * 7 / 10;
        if (!dx) dx = r ? 1 : -1;
        if (!dy) dy = d ? 1 : -1;
    }
    if (dx || dy) { Log(L"tick dx=%d dy=%d held=%08X", dx, dy, (unsigned)g_held); MoveBy(dx, dy); }

    if ((g_held & BIT(B_WH_UP)) && now - g_wheelAt >= 55)   { SendWheel(+1); g_wheelAt = now; }
    if ((g_held & BIT(B_WH_DOWN)) && now - g_wheelAt >= 55) { SendWheel(-1); g_wheelAt = now; }
}

/* ------------------------------------------------------- keyboard mapping */

static DWORD MapKey(DWORD vk, BOOL ext)
{
    if (ext) {
        /* обычные стрелки (блок навигации) */
        switch (vk) {
        case VK_UP:    return BIT(B_UP);
        case VK_DOWN:  return BIT(B_DOWN);
        case VK_LEFT:  return BIT(B_LEFT);
        case VK_RIGHT: return BIT(B_RIGHT);
        default:       return 0;
        }
    }
    /* цифровой блок (в т.ч. при выключенном NumLock) */
    switch (vk) {
    case VK_NUMPAD8: case VK_UP:    return BIT(B_UP);
    case VK_NUMPAD2: case VK_DOWN:  return BIT(B_DOWN);
    case VK_NUMPAD4: case VK_LEFT:  return BIT(B_LEFT);
    case VK_NUMPAD6: case VK_RIGHT: return BIT(B_RIGHT);
    case VK_NUMPAD7: case VK_HOME:  return BIT(B_BTN_L);
    case VK_NUMPAD9: case VK_PRIOR: return BIT(B_BTN_R);
    case VK_NUMPAD5: case VK_CLEAR: return BIT(B_BTN_M);
    case VK_NUMPAD1: case VK_END:   return BIT(B_LEFT) | BIT(B_DOWN);
    case VK_NUMPAD3: case VK_NEXT:  return BIT(B_RIGHT) | BIT(B_DOWN);
    case VK_NUMPAD0: case VK_INSERT: return BIT(B_WH_DOWN);
    case VK_DECIMAL: case VK_DELETE: return BIT(B_WH_UP);
    case VK_SUBTRACT: return BIT(B_WH_DOWN);
    case VK_ADD:      return BIT(B_WH_UP);
    default: return 0;
    }
}

static BOOL WinDown(void)
{
    return (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
}
static BOOL AltDown(void)
{
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

/* ------------------------------------------------------------------- tray */

static HICON MakeIcon(BOOL on)
{
    const int S = 32;
    BITMAPV5HEADER bh;
    ICONINFO ii;
    HBITMAP hb, hm;
    HDC dc, mdc;
    HGDIOBJ old;
    RECT r;
    DWORD *px;
    void *bits = NULL;
    int x, y, i;

    ZeroMemory(&bh, sizeof(bh));
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = S;
    bh.bV5Height = -S;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;

    dc = CreateCompatibleDC(NULL);
    hb = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, &bits, NULL, 0);
    DeleteDC(dc);
    if (!hb || !bits) return LoadIconW(NULL, IDI_APPLICATION);
    px = (DWORD *)bits;

    {
        DWORD fill  = on ? 0xFF35C24A : 0xFF7A7A7A;
        DWORD edge  = on ? 0xFF1D7A2C : 0xFF4E4E4E;
        DWORD glyph = 0xFFF2F2F2;
        for (y = 0; y < S; y++) {
            for (x = 0; x < S; x++) {
                int in = 0, bord = 0, corner = 0;
                if (x >= 4 && x <= 27 && y >= 4 && y <= 27) {
                    in = 1;
                    /* скругление углов */
                    if (x < 10 && y < 10) corner = 1;
                    else if (x > 21 && y < 10) corner = 2;
                    else if (x < 10 && y > 21) corner = 3;
                    else if (x > 21 && y > 21) corner = 4;
                    if (corner) {
                        int cx = (corner == 2 || corner == 4) ? 21 : 10;
                        int cy = (corner == 3 || corner == 4) ? 21 : 10;
                        int dxs = x - cx, dys = y - cy;
                        if (dxs * dxs + dys * dys > 30) in = 0;
                    }
                    if (in && (x <= 5 || x >= 26 || y <= 5 || y >= 26)) bord = 1;
                }
                if (!in) { px[y * S + x] = 0x00000000; continue; }
                /* плюс-прицел в центре */
                if ((x >= 15 && x <= 16 && y >= 9 && y <= 22) ||
                    (y >= 15 && y <= 16 && x >= 9 && x <= 22)) {
                    px[y * S + x] = glyph;
                } else {
                    px[y * S + x] = bord ? edge : fill;
                }
            }
        }
        (void)i;
    }

    hm = CreateBitmap(S, S, 1, 1, NULL);
    mdc = CreateCompatibleDC(NULL);
    old = SelectObject(mdc, hm);
    r.left = 0; r.top = 0; r.right = S; r.bottom = S;
    FillRect(mdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SelectObject(mdc, old);
    DeleteDC(mdc);

    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmMask = hm;
    ii.hbmColor = hb;
    {
        HICON ic = CreateIconIndirect(&ii);
        DeleteObject(hb);
        DeleteObject(hm);
        return ic ? ic : LoadIconW(NULL, IDI_APPLICATION);
    }
}

static void TrayIconAddOrUpdate(BOOL add)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAY;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_mode ? g_iconOn : g_iconOff;
    lstrcpynW(nid.szTip, g_mode ? L"NumpadMouse: ВКЛ (Win+Alt+K)" : L"NumpadMouse: выкл (Win+Alt+K)", 128);
    if (!add) nid.uFlags |= NIF_TIP;
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &nid);
}

static void TrayBalloon(void)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAY;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_NOSOUND | NIIF_INFO;
    lstrcpynW(nid.szInfoTitle, L"NumpadMouse", 64);
    lstrcpynW(nid.szInfo, g_mode ? L"Режим мыши включён" : L"Режим мыши выключен", 256);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* ------------------------------------------------------------------- mode */

static void ToggleMode(void)
{
    g_mode = !g_mode;
    Log(L"TOGGLE mode=%d", (int)g_mode);
    if (!g_mode) ReleaseAll();
    TrayIconAddOrUpdate(FALSE);
    TrayBalloon();
    if (g_hwnd) PostMessageW(g_hwnd, WM_BEEP, (WPARAM)g_mode, 0);
}

static void ShowMenu(void)
{
    POINT pt;
    int cmd;

    if (!g_menu) {
        g_menu = CreatePopupMenu();
        AppendMenuW(g_menu, MF_STRING, MENU_TOGGLE, L"Режим мыши  (Win+Alt+K)");
        AppendMenuW(g_menu, MF_STRING, MENU_AUTOSTART, L"Запускать при входе в систему");
        AppendMenuW(g_menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(g_menu, MF_STRING, MENU_EXIT, L"Выход  (Win+Alt+Q)");
    }
    CheckMenuItem(g_menu, MENU_TOGGLE, MF_BYCOMMAND | (g_mode ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu, MENU_AUTOSTART, MF_BYCOMMAND | (AutostartOn() ? MF_CHECKED : MF_UNCHECKED));

    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    cmd = (int)TrackPopupMenu(g_menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                              pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);

    switch (cmd) {
    case MENU_TOGGLE: ToggleMode(); break;
    case MENU_AUTOSTART:
        if (AutostartOn()) { RemoveRunValue(); DeleteTask(); }
        else InstallSelf();
        TrayIconAddOrUpdate(FALSE);
        break;
    case MENU_EXIT: DestroyWindow(g_hwnd); break;
    default: break;
    }
}

/* ------------------------------------------------------------------- hook */

static LRESULT CALLBACK KeyProc(int code, WPARAM wp, LPARAM lp)
{
    KBDLLHOOKSTRUCT *k;
    BOOL down, ext;

    if (code != HC_ACTION) return CallNextHookEx(g_hook, code, wp, lp);
    k = (KBDLLHOOKSTRUCT *)lp;
    down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
    ext = (k->flags & LLKHF_EXTENDED) != 0;

    if (k->vkCode == 'K') {
        if (down) {
            if (!g_kDown && WinDown() && AltDown()) {
                g_kDown = TRUE;
                ToggleMode();
                return 1;
            }
        } else {
            g_kDown = FALSE;
        }
    } else if (k->vkCode == 'Q' && down) {
        if (WinDown() && AltDown()) {
            PostMessageW(g_hwnd, WM_COMMAND, MENU_EXIT, 0);
            return 1;
        }
    }

    if (g_mode) {
        DWORD m = MapKey(k->vkCode, ext);
        Log(L"key vk=%04X ext=%d down=%d map=%08X", (unsigned)k->vkCode, (int)ext, (int)down, (unsigned)m);
        if (m) {
            int i;
            if (down) {
                for (i = 0; i < B_COUNT; i++) {
                    if (m & BIT(i)) {
                        PressBit(i, k->vkCode);
                        g_downAt[i] = GetTickCount64();   /* автоповтор = клавиша реально зажата */
                    }
                }
            } else {
                for (i = 0; i < B_COUNT; i++) if (m & BIT(i)) ReleaseBit(i);
            }
            return 1;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

/* ------------------------------------------------------------------- main */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) ToggleMode();
        else if (lp == WM_RBUTTONUP) ShowMenu();
        return 0;

    case WM_BEEP:
        Beep(wp ? 1100 : 620, 45);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case MENU_EXIT: DestroyWindow(hwnd); return 0;
        case MENU_TOGGLE: ToggleMode(); return 0;
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_MOVE) { OnTick(); return 0; }
        break;

    case WM_ENDSESSION:
    case WM_QUERYENDSESSION:
        ReleaseAll();
        break;

    case WM_DESTROY: {
        NOTIFYICONDATAW nid;
        ReleaseAll();
        if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = ID_TRAY;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    }

    if (msg == g_wmTaskbarCreated && g_wmTaskbarCreated) {
        TrayIconAddOrUpdate(TRUE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnableDpiAwareness(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef BOOL (WINAPI *PFN_SPDAC)(HANDLE);
        typedef BOOL (WINAPI *PFN_SPDA)(void);
        PFN_SPDAC a = (PFN_SPDAC)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        PFN_SPDA  b = (PFN_SPDA)(void *)GetProcAddress(u, "SetProcessDPIAware");
        if (a && a((HANDLE)-4)) return;
        if (b) b();
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int show)
{
    HANDLE mutex;
    WNDCLASSEXW wc;
    MSG msg;

    (void)hPrev; (void)show;
    g_inst = hInst;

    EnableDpiAwareness();
    LogOpen();
    Log(L"start elevated=%d", (int)IsElevated());

    if (cmdLine && wcsstr(cmdLine, L"--uninstall")) {
        UninstallSelf();
        return 0;
    }

    /* автозапуск настраивается всегда, даже если копия уже работает:
       так «Запуск от имени администратора» переводит автозапуск на задачу
       с повышенными правами, не мешая текущему экземпляру */
    if (!(cmdLine && wcsstr(cmdLine, L"--noinstall"))) {
        InstallSelf();
    }

    if (cmdLine && wcsstr(cmdLine, L"--install")) return 0;

    mutex = CreateMutexW(NULL, FALSE, L"Local\\NumpadMouse.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    g_iconOn = MakeIcon(TRUE);
    g_iconOff = MakeIcon(FALSE);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"NumpadMouseWnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"NumpadMouseWnd", APP_NAME, WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    TrayIconAddOrUpdate(TRUE);

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyProc, hInst, 0);
    if (!g_hook) {
        MessageBoxW(NULL, L"Не удалось установить глобальный хук клавиатуры.", APP_NAME, MB_ICONERROR);
        return 1;
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook) UnhookWindowsHookEx(g_hook);
    if (mutex) CloseHandle(mutex);
    return 0;
}
