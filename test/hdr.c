/* hdr.exe — показать/переключить HDR (advanced color) для активных дисплеев.
   hdr.exe          — только показать состояние
   hdr.exe off|on   — выключить / включить HDR                                  */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>

static void friendly(LUID adapter, UINT32 id, wchar_t *out, size_t cch)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
    ZeroMemory(&tn, sizeof(tn));
    tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tn.header.size = sizeof(tn);
    tn.header.adapterId = adapter;
    tn.header.id = id;
    if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS)
        wcsncpy(out, tn.monitorFriendlyDeviceName, cch);
    else
        wcscpy(out, L"?");
    out[cch - 1] = 0;
}

int wmain(int argc, wchar_t **argv)
{
    UINT32 np = 0, nm = 0, i;
    DISPLAYCONFIG_PATH_INFO *paths;
    DISPLAYCONFIG_MODE_INFO *modes;
    int want = -1;      /* -1 показать, 0 выключить, 1 включить */

    if (argc > 1) {
        if (!_wcsicmp(argv[1], L"off")) want = 0;
        else if (!_wcsicmp(argv[1], L"on")) want = 1;
    }

    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) {
        printf("GetDisplayConfigBufferSizes failed\n");
        return 1;
    }
    paths = (DISPLAYCONFIG_PATH_INFO *)calloc(np, sizeof(*paths));
    modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nm, sizeof(*modes));
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) != ERROR_SUCCESS) {
        printf("QueryDisplayConfig failed\n");
        return 1;
    }

    for (i = 0; i < np; i++) {
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO g;
        wchar_t name[64];
        LONG rc;

        friendly(paths[i].targetInfo.adapterId, paths[i].targetInfo.id, name, 64);

        ZeroMemory(&g, sizeof(g));
        g.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        g.header.size = sizeof(g);
        g.header.adapterId = paths[i].targetInfo.adapterId;
        g.header.id = paths[i].targetInfo.id;
        rc = DisplayConfigGetDeviceInfo(&g.header);

        wprintf(L"[%u] %s  supported=%u enabled=%u forceDisabled=%u rc=%ld\n", i, name,
                rc == ERROR_SUCCESS ? g.advancedColorSupported : 0,
                rc == ERROR_SUCCESS ? g.advancedColorEnabled : 0,
                rc == ERROR_SUCCESS ? g.advancedColorForceDisabled : 0, (long)rc);

        if (want >= 0 && rc == ERROR_SUCCESS && (int)g.advancedColorEnabled != want) {
            DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE s;
            LONG sr;
            ZeroMemory(&s, sizeof(s));
            s.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
            s.header.size = sizeof(s);
            s.header.adapterId = paths[i].targetInfo.adapterId;
            s.header.id = paths[i].targetInfo.id;
            s.enableAdvancedColor = (UINT32)want;
            sr = DisplayConfigSetDeviceInfo(&s.header);
            wprintf(L"    set HDR=%d -> %s (rc=%ld)\n", want, sr == ERROR_SUCCESS ? "OK" : "failed", (long)sr);
        }
    }
    free(paths);
    free(modes);
    return 0;
}

