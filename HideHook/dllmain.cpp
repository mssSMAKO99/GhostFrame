#include "pch.h" // Удалите, если отключили предкомпилированные заголовки
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#ifndef WDA_NONE
#define WDA_NONE               0x00000000
#endif
#ifndef WDA_MONITOR
#define WDA_MONITOR            0x00000001
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

typedef BOOL(WINAPI* fnSetWDA)(HWND, DWORD);

static fnSetWDA   g_RealOrig = nullptr;
static fnSetWDA   g_Trampoline = nullptr;
static bool       g_HookInstalled = false;
static HWND       g_ProtectedHwnd = nullptr;

static BOOL CallOriginalSetWDA(HWND hwnd, DWORD affinity) {
    if (!g_Trampoline) return ::SetWindowDisplayAffinity(hwnd, affinity);
    return g_Trampoline(hwnd, affinity);
}

// ---------- хук ----------
static BOOL WINAPI Hook_SetWDA(HWND hwnd, DWORD affinity) {
    // Сравниваем корневые предки: ловит дочерние/вспомогательные окна
    bool sameRoot = false;
    if (hwnd && g_ProtectedHwnd) {
        HWND r1 = GetAncestor(hwnd, GA_ROOT);
        HWND r2 = GetAncestor(g_ProtectedHwnd, GA_ROOT);
        sameRoot = (r1 && r2 && r1 == r2);
    }

    if (sameRoot && affinity != WDA_EXCLUDEFROMCAPTURE) {
        OutputDebugStringW(L"[GhostFrame] Blocked affinity change on protected root\n");
        return TRUE;
    }
    if (affinity == WDA_MONITOR) {
        OutputDebugStringW(L"[GhostFrame] Blocked global WDA_MONITOR\n");
        return TRUE;
    }
    return g_Trampoline(hwnd, affinity);
}

// ---------- дизассемблер длин ----------
static bool IsModRMOpcode(uint8_t op) {
    switch (op) {
    case 0x01: case 0x03: case 0x09: case 0x0B:
    case 0x11: case 0x13: case 0x19: case 0x1B:
    case 0x21: case 0x23: case 0x29: case 0x2B:
    case 0x31: case 0x33: case 0x39: case 0x3B:
    case 0x84: case 0x85: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D:
    case 0xFF:
        return true;
    }
    return false;
}

static const uint8_t* ParseModRM(const uint8_t* p) {
    uint8_t modrm = *p++;
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm = modrm & 7;
    if (mod != 3) {
        if (rm == 4) {
            uint8_t sib = *p++;
            if (mod == 0 && (sib & 7) == 5) p += 4;
        }
        if (mod == 0 && rm == 5) p += 4;
        else if (mod == 1) p += 1;
        else if (mod == 2) p += 4;
    }
    return p;
}

static size_t InstrLen(const uint8_t* code) {
    const uint8_t* p = code;
    bool rexW = false;
    while (true) {
        uint8_t b = *p;
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
            b == 0x64 || b == 0x65) {
            p++;
        }
        else if (b >= 0x40 && b <= 0x4F) {
            if (b & 0x08) rexW = true;
            p++;
        }
        else break;
    }
    uint8_t op = *p++;
    if (op == 0x0F) {
        uint8_t op2 = *p++;
        switch (op2) {
        case 0x05: case 0x0B: case 0x34: case 0x35:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x06: case 0x07: case 0x08: case 0x09:
        case 0xA2: case 0xAA:
            return p - code;
        case 0x1E: case 0x1F:
        case 0xB6: case 0xB7: case 0xBE: case 0xBF:
            p = ParseModRM(p);
            return p - code;
        default:
            return 0;
        }
    }
    if (op >= 0x50 && op <= 0x5F) return p - code;
    if (op == 0x90 || op == 0xC3 || op == 0xCC || op == 0xC9) return p - code;
    if (op == 0xE8 || op == 0xE9 || op == 0xEB) return 0;
    if (op >= 0x70 && op <= 0x7F) return 0;
    if (IsModRMOpcode(op)) { p = ParseModRM(p); return p - code; }
    if (op == 0x83) { p++; p += 1; return p - code; }
    if (op == 0x81 || op == 0xC7) { p = ParseModRM(p); p += 4; return p - code; }
    if (op >= 0xB8 && op <= 0xBF) { p += rexW ? 8 : 4; return p - code; }
    return 0;
}

// ---------- разрешение forwarder'а (x86/x64 различаются семантикой disp32) ----------
static uint8_t* FollowForwarder(uint8_t* target) {
    if (target[0] != 0xFF || target[1] != 0x25) return target;
    int32_t disp = *(int32_t*)(target + 2);

    uint8_t** ptrSlot = nullptr;
#ifdef _WIN64
    // jmp qword ptr [rip+disp32]
    ptrSlot = (uint8_t**)(target + 6 + disp);
#else
    // jmp dword ptr [abs32]
    ptrSlot = (uint8_t**)disp;
#endif

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(ptrSlot, &mbi, sizeof(mbi))) return target;
    if (mbi.State != MEM_COMMIT) return target;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return target;

    uint8_t* real = *ptrSlot;
    if (!real || real == target) return target;
    return real;
}

static bool InstallHook() {
    if (g_HookInstalled) return true;

    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) return false;

    g_RealOrig = (fnSetWDA)GetProcAddress(hUser32, "SetWindowDisplayAffinity");
    if (!g_RealOrig) return false;

    uint8_t* hookTarget = (uint8_t*)g_RealOrig;

    for (int i = 0; i < 4; i++) {
        uint8_t* next = FollowForwarder(hookTarget);
        if (next == hookTarget) break;
        wchar_t buf[128];
        swprintf_s(buf, L"[GhostFrame] Forwarder L%d: %p -> %p\n", i + 1, hookTarget, next);
        OutputDebugStringW(buf);
        hookTarget = next;
    }

    {
        HMODULE hMod = nullptr;
        if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)hookTarget, &hMod)) {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(hMod, path, MAX_PATH);
            wchar_t* name = wcsrchr(path, L'\\');
            if (name) name++; else name = path;
            wchar_t buf[256];
            swprintf_s(buf, L"[GhostFrame] Real function in: %s @ %p\n", name, hookTarget);
            OutputDebugStringW(buf);
        }
    }

    uint8_t* target = hookTarget;

    {
        wchar_t buf[200];
        swprintf_s(buf, L"[GhostFrame] real prologue: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            target[0], target[1], target[2], target[3], target[4], target[5], target[6], target[7],
            target[8], target[9], target[10], target[11], target[12], target[13], target[14], target[15]);
        OutputDebugStringW(buf);
    }

    size_t saved = 0;
    while (saved < 5) {
        size_t len = InstrLen(target + saved);
        if (len == 0) {
            wchar_t buf[64];
            swprintf_s(buf, L"[GhostFrame] Disasm failed at offset %zu\n", saved);
            OutputDebugStringW(buf);
            return false;
        }
        saved += len;
        if (saved > 20) { OutputDebugStringW(L"[GhostFrame] Boundary too far\n"); return false; }
    }

    size_t trampSize = saved + 32;
    void* trampMem = VirtualAlloc(nullptr, trampSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampMem) return false;

    uint8_t* tramp = (uint8_t*)trampMem;
    memcpy(tramp, target, saved);

#ifdef _WIN64
    tramp[saved + 0] = 0xFF;
    tramp[saved + 1] = 0x25;
    tramp[saved + 2] = 0x00; tramp[saved + 3] = 0x00;
    tramp[saved + 4] = 0x00; tramp[saved + 5] = 0x00;
    uint64_t backAddr = (uint64_t)(target + saved);
    memcpy(tramp + saved + 6, &backAddr, 8);
#else
    tramp[saved + 0] = 0xE9;
    int32_t relBack = (int32_t)((intptr_t)(target + saved) - (intptr_t)(tramp + saved + 5));
    memcpy(tramp + saved + 1, &relBack, 4);
#endif

    g_Trampoline = (fnSetWDA)tramp;

    intptr_t delta = (intptr_t)&Hook_SetWDA - (intptr_t)(target + 5);
    void* stubMem = nullptr;

    if (delta < INT32_MIN || delta > INT32_MAX) {
        intptr_t tgt = (intptr_t)target;
        bool found = false;
        for (int i = 1; i <= 256 && !found; i++) {
            for (int dir = -1; dir <= 1; dir += 2) {
                intptr_t hint = tgt + dir * (intptr_t)i * 0x10000;
                if (hint < 0x10000) continue;
                void* m = VirtualAlloc((void*)hint, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (m) {
                    intptr_t d = (intptr_t)m - (intptr_t)(target + 5);
                    if (d >= INT32_MIN && d <= INT32_MAX) { stubMem = m; found = true; break; }
                    VirtualFree(m, 0, MEM_RELEASE);
                }
            }
        }
        if (!stubMem) {
            OutputDebugStringW(L"[GhostFrame] Failed to allocate stub near target\n");
            VirtualFree(trampMem, 0, MEM_RELEASE);
            g_Trampoline = nullptr;
            return false;
        }
        uint8_t* stub = (uint8_t*)stubMem;
        stub[0] = 0xFF; stub[1] = 0x25;
        stub[2] = 0x00; stub[3] = 0x00; stub[4] = 0x00; stub[5] = 0x00;
#ifdef _WIN64
        uint64_t hookAddr = (uint64_t)&Hook_SetWDA;
        memcpy(stub + 6, &hookAddr, 8);
#else
        uint32_t hookAddr = (uint32_t)(uintptr_t)&Hook_SetWDA;
        memcpy(stub + 2, &hookAddr, 4);  // FF 25 abs32 → jmp dword ptr [abs32], no need for +6
#endif
        delta = (intptr_t)stub - (intptr_t)(target + 5);
        OutputDebugStringW(L"[GhostFrame] Using near stub\n");
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
        if (stubMem) VirtualFree(stubMem, 0, MEM_RELEASE);
        VirtualFree(trampMem, 0, MEM_RELEASE);
        g_Trampoline = nullptr;
        return false;
    }
    target[0] = 0xE9;
    int32_t rel = (int32_t)delta;
    memcpy(target + 1, &rel, 4);
    VirtualProtect(target, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, 5);

    g_HookInstalled = true;
    OutputDebugStringW(L"[GhostFrame] Hook installed on REAL function (5-byte E9)\n");
    return true;
}

// ---------- точка входа из лаунчера ----------
extern "C" __declspec(dllexport) LRESULT CALLBACK HookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        CWPSTRUCT* pMsg = reinterpret_cast<CWPSTRUCT*>(lParam);
        if (pMsg && pMsg->hwnd && pMsg->message == (WM_USER + 777)) {
            HWND hRoot = GetAncestor(pMsg->hwnd, GA_ROOT);
            if (hRoot) {
                HMODULE hSelf = nullptr;
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    (LPCWSTR)&HookProc, &hSelf);
                if (pMsg->wParam == 1) {
                    g_ProtectedHwnd = hRoot;
                    InstallHook();
                    CallOriginalSetWDA(hRoot, WDA_EXCLUDEFROMCAPTURE);
                }
                else {
                    CallOriginalSetWDA(hRoot, WDA_NONE);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

extern "C" __declspec(dllexport) BOOL IsHookInstalled() {
    return g_HookInstalled ? TRUE : FALSE;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hModule);
    return TRUE;
}

#ifdef _WIN64
#pragma comment(linker, "/EXPORT:HookProc")
#pragma comment(linker, "/EXPORT:IsHookInstalled")
#else
#pragma comment(linker, "/EXPORT:HookProc=_HookProc@12")
#pragma comment(linker, "/EXPORT:IsHookInstalled=_IsHookInstalled")
#endif