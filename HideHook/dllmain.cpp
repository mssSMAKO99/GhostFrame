#include "pch.h" // Удалите, если отключили предкомпилированные заголовки
#include <windows.h>

extern "C" __declspec(dllexport) LRESULT CALLBACK HookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        CWPSTRUCT* pMsg = reinterpret_cast<CWPSTRUCT*>(lParam);
        if (pMsg && pMsg->hwnd && pMsg->message == (WM_USER + 777)) {
            HWND hRoot = GetAncestor(pMsg->hwnd, GA_ROOT);
            if (hRoot) {
                DWORD targetAffinity = (pMsg->wParam == 1) ? 0x00000011 : 0x00000000;
                SetWindowDisplayAffinity(hRoot, targetAffinity);
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
