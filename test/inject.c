/* test harness v3: считает только *инжектированные* события (WH_MOUSE_LL) и
   инжектит только те клавиши, которые режим съедает, — в чужие поля ввода
   ничего не улетает. Кнопки мыши проверяются только с аргументом "clicks".

   inject.exe          — движение/колесо/автоповтор (клавиши глотаются)
   inject.exe clicks   — плюс клики левой/правой/средней кнопкой            */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static volatile LONG g_moves, g_dx, g_dy;
static volatile LONG g_ld, g_lu, g_rd, g_ru, g_md, g_mu, g_wh;
static POINT g_prev;
static volatile LONG g_havePrev;

static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
        if (m->flags & LLMHF_INJECTED) {
            if (wp == WM_MOUSEMOVE) {
                if (g_havePrev) { g_dx += m->pt.x - g_prev.x; g_dy += m->pt.y - g_prev.y; g_moves++; }
                g_prev = m->pt;
                g_havePrev = 1;
            }
            if (wp == WM_LBUTTONDOWN) g_ld++;
            if (wp == WM_LBUTTONUP) g_lu++;
            if (wp == WM_RBUTTONDOWN) g_rd++;
            if (wp == WM_RBUTTONUP) g_ru++;
            if (wp == WM_MBUTTONDOWN) g_md++;
            if (wp == WM_MBUTTONUP) g_mu++;
            if (wp == WM_MOUSEWHEEL) g_wh++;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static DWORD WINAPI HookThread(LPVOID p)
{
    MSG msg;
    (void)p;
    SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(NULL), 0);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

static void KeyEx(DWORD vk, BOOL down, BOOL ext)
{
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &in, sizeof(in));
}

static void Tap(DWORD vk) { KeyEx(vk, TRUE, FALSE); Sleep(40); KeyEx(vk, FALSE, FALSE); Sleep(40); }
static void Hold(DWORD vk, int ms, BOOL ext)
{
    KeyEx(vk, TRUE, ext); Sleep(ms); KeyEx(vk, FALSE, ext); Sleep(80);
}

static void Reset(void)
{
    g_moves = g_dx = g_dy = 0;
    g_ld = g_lu = g_rd = g_ru = g_md = g_mu = g_wh = 0;
    g_havePrev = 0;
    SetCursorPos(GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2);
    Sleep(60);
    g_moves = g_dx = g_dy = 0;
    g_havePrev = 0;
}

static void Hotkey(void)
{
    KeyEx(VK_LWIN, TRUE, TRUE); KeyEx(VK_MENU, TRUE, FALSE); Sleep(30);
    Tap('K');
    KeyEx(VK_MENU, FALSE, FALSE); KeyEx(VK_LWIN, FALSE, TRUE);
    Sleep(250);
}

static int g_fail;

static void Check(const char *what, int cond)
{
    printf("%-38s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) g_fail++;
}

int main(int argc, char **argv)
{
    int clicks = (argc > 1 && argv[1] && argv[1][0] == 'c');
    HANDLE th = CreateThread(NULL, 0, HookThread, NULL, 0, NULL);
    Sleep(300);

    Hotkey();                              /* режим ВКЛ */

    /* обычные стрелки */
    Reset(); Hold(VK_UP, 300, TRUE);
    printf("[arrow up] moves=%ld dy=%ld\n", g_moves, g_dy);
    Check("arrow up -> mouse up", g_dy <= -30 && g_moves >= 10);

    Reset(); Hold(VK_DOWN, 300, TRUE);
    Check("arrow down -> mouse down", g_dy >= 30 && g_moves >= 10);

    Reset(); Hold(VK_RIGHT, 300, TRUE);
    Check("arrow right -> mouse right", g_dx >= 30 && g_moves >= 10);

    Reset(); Hold(VK_LEFT, 300, TRUE);
    Check("arrow left -> mouse left", g_dx <= -30 && g_moves >= 10);

    /* стрелки нумпада */
    Reset(); Hold(VK_NUMPAD8, 300, FALSE);
    Check("numpad8 -> mouse up", g_dy <= -30 && g_moves >= 10);

    Reset(); Hold(VK_NUMPAD3, 300, FALSE);
    Check("numpad3 -> down-right", g_dx >= 20 && g_dy >= 20);

    /* ускорение */
    Reset(); Hold(VK_RIGHT, 1200, TRUE);
    Check("acceleration on long hold", g_dx >= 300);

    /* автоповтор (физическое удержание) */
    Reset();
    {
        DWORD t0 = GetTickCount();
        LONG at1400 = 0, at2500 = 0;
        KeyEx(VK_RIGHT, TRUE, TRUE);
        for (;;) {
            DWORD dt = GetTickCount() - t0;
            if (dt >= 1400 && !at1400) at1400 = g_dx;
            if (dt >= 2500) { at2500 = g_dx; break; }
            Sleep(30);
            KeyEx(VK_RIGHT, TRUE, TRUE);
        }
        KeyEx(VK_RIGHT, FALSE, TRUE);
        Sleep(80);
        Check("hold + autorepeat keeps moving", at2500 - at1400 > 50);
    }

    /* потерянный keyup -> авто-отпускание */
    Reset();
    {
        LONG a, b;
        KeyEx(VK_RIGHT, TRUE, TRUE);
        Sleep(2000); a = g_dx;
        Sleep(700);  b = g_dx;
        KeyEx(VK_RIGHT, FALSE, TRUE);
        Sleep(80);
        Check("stuck key auto-released", b == a && a > 0);
    }

    /* колесо */
    Reset(); Hold(VK_NUMPAD0, 200, FALSE);
    Check("numpad0 -> wheel", g_wh >= 1);

    /* кнопки — только по требованию (реально кликают по рабочим окнам) */
    if (clicks) {
        Reset(); Tap(VK_NUMPAD7); Sleep(120);
        Check("numpad7 -> left button", g_ld == 1 && g_lu == 1);
        Reset(); Tap(VK_NUMPAD9); Sleep(120);
        Check("numpad9 -> right button", g_rd == 1 && g_ru == 1);
        Reset(); Tap(VK_NUMPAD5); Sleep(120);
        Check("numpad5 -> middle button", g_md == 1 && g_mu == 1);
    }

    Hotkey();                              /* режим ВЫКЛ */

    printf("\nRESULT: %s (%d failed)\n", g_fail ? "FAIL" : "ALL PASS", g_fail);
    (void)th;
    return g_fail ? 1 : 0;
}
