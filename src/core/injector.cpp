#include "injector.h"

#include <bcrypt.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "log.h"

#include "ghostframe/payload_ids.h"
#include "ghostframe/session_id.h"

#ifndef IMAGE_FILE_MACHINE_ARM64EC
#define IMAGE_FILE_MACHINE_ARM64EC 0xA641
#endif

namespace gf {

namespace {

using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);

std::wstring LocalAppDataDirectory() {
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) {
        result = raw;
        ::CoTaskMemFree(raw);
    }
    if (result.empty()) {
        wchar_t buffer[MAX_PATH] = {};
        if (::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH) != 0) {
            result = buffer;
        }
    }
    return result;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    if (::CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }
    if (::GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool Sha256(const void* data, size_t size, BYTE out[32]) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        const PUCHAR bytes = static_cast<PUCHAR>(const_cast<void*>(data));
        if (::BCryptHashData(hash, bytes, static_cast<ULONG>(size), 0) >= 0 &&
            ::BCryptFinishHash(hash, out, 32, 0) >= 0) {
            ok = true;
        }
        ::BCryptDestroyHash(hash);
    }
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

void DeletePayloadFile(const std::wstring& path) {
    // Payloads are marked read-only after verification, and DeleteFileW refuses
    // to unlink a read-only file.
    ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    ::DeleteFileW(path.c_str());
}

DWORD IntegrityOf(HANDLE process) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &token)) {
        return 0;
    }

    DWORD needed = 0;
    ::GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &needed);
    if (needed == 0) {
        ::CloseHandle(token);
        return 0;
    }

    std::vector<BYTE> buffer(needed);
    DWORD returned = 0;
    DWORD rid = 0;
    if (::GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), needed, &returned)) {
        auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
        if (label->Label.Sid != nullptr) {
            const DWORD count = *::GetSidSubAuthorityCount(label->Label.Sid);
            if (count > 0) {
                rid = *::GetSidSubAuthority(label->Label.Sid, count - 1);
            }
        }
    }
    ::CloseHandle(token);
    return rid;
}

Arch ArchitectureOf(HANDLE process) {
    static IsWow64Process2Fn query = reinterpret_cast<IsWow64Process2Fn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));

    if (query != nullptr) {
        USHORT processMachine = 0;
        USHORT nativeMachine = 0;
        if (query(process, &processMachine, &nativeMachine)) {
            if (processMachine == IMAGE_FILE_MACHINE_I386) {
                return Arch::X86;
            }
            if (processMachine == IMAGE_FILE_MACHINE_AMD64) {
                return Arch::X64;
            }
            if (processMachine == IMAGE_FILE_MACHINE_ARM64 ||
                processMachine == IMAGE_FILE_MACHINE_ARM64EC) {
                return Arch::Arm64;
            }
            if (processMachine == IMAGE_FILE_MACHINE_UNKNOWN) {
                switch (nativeMachine) {
                    case IMAGE_FILE_MACHINE_AMD64:
                        return Arch::X64;
                    case IMAGE_FILE_MACHINE_ARM64:
                    case IMAGE_FILE_MACHINE_ARM64EC:
                        return Arch::Arm64;
                    default:
                        return Arch::Other;
                }
            }
            return Arch::Other;
        }
    }

    BOOL wow64 = FALSE;
    if (::IsWow64Process(process, &wow64)) {
        return wow64 != FALSE ? Arch::X86 : Arch::X64;
    }
    return Arch::Other;
}

std::wstring HelperFailure(DWORD code) {
    if ((code & 0x80000000u) != 0) {
        return Win32Message(code & 0x7FFFFFFFu);
    }
    switch (code) {
        case 10:
            return L"invalid command line";
        case 11:
            return L"could not load the 32-bit agent";
        case 12:
            return L"agent export missing";
        case 13:
            return L"SetWindowsHookEx failed";
        case 14:
            return L"target did not answer";
        default:
            return FormatString(L"unknown error %u", code);
    }
}

}  // namespace

bool DescribeProcess(DWORD pid, ProcessInfo& info, std::wstring& error) {
    info = ProcessInfo{};
    info.pid = pid;

    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        const DWORD openError = ::GetLastError();
        error = L"cannot open target process: " + Win32Message(openError);
        return false;
    }

    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (::GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER value = {};
        value.LowPart = created.dwLowDateTime;
        value.HighPart = created.dwHighDateTime;
        info.createTime = value.QuadPart;
    }

    info.integrity = IntegrityOf(process);
    info.arch = ArchitectureOf(process);

    wchar_t image[1024] = {};
    DWORD size = _countof(image);
    if (::QueryFullProcessImageNameW(process, 0, image, &size)) {
        info.exePath.assign(image, size);
    }

    ::CloseHandle(process);
    return true;
}

bool Injector::Initialize(std::wstring& error) {
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    // Preferred location: %LOCALAPPDATA%\GhostFrame (per user, inherited ACLs).
    // Fall back to %TEMP% for locked down or redirected profiles.
    std::wstring root;

    wchar_t overrideRoot[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"GF_PAYLOAD_ROOT", overrideRoot, MAX_PATH) > 0) {
        root = overrideRoot;
        if (!EnsureDirectory(root)) {
            error = L"GF_PAYLOAD_ROOT is not usable: " + root;
            return false;
        }
    }

    const std::wstring localAppData = LocalAppDataDirectory();
    if (root.empty() && !localAppData.empty()) {
        root = localAppData + L"\\GhostFrame";
        if (!EnsureDirectory(root)) {
            LogLine(L"cannot use %s: %s", root.c_str(), Win32Message(::GetLastError()).c_str());
            root.clear();
        }
    }
    if (root.empty()) {
        wchar_t temp[MAX_PATH] = {};
        if (::GetTempPathW(MAX_PATH, temp) != 0) {
            root = std::wstring(temp) + L"GhostFrame";
            if (!EnsureDirectory(root)) {
                root.clear();
            }
        }
    }
    if (root.empty()) {
        error = L"cannot create a writable payload folder (LOCALAPPDATA and TEMP both failed)";
        return false;
    }

    baseDir_ = root;
    SweepStalePayloads();

    wchar_t suffix[48] = {};
    swprintf_s(suffix, L"\\%u_%08X", ::GetCurrentProcessId(), NewSessionId());
    dir_ = baseDir_ + suffix;

    if (!::CreateDirectoryW(dir_.c_str(), nullptr)) {
        error = L"cannot create the payload folder " + dir_ + L": " +
                Win32Message(::GetLastError());
        return false;
    }

    if (!Extract(IDR_AGENT_X64, L"GhostFrameAgent64.dll", error) ||
        !Extract(IDR_AGENT_X86, L"GhostFrameAgent32.dll", error) ||
        !Extract(IDR_HELPER_X86, L"GhostFrameHelper32.exe", error)) {
        return false;
    }

    message_ = ::RegisterWindowMessageW(kAttachMessageName);
    if (message_ == 0) {
        error = L"RegisterWindowMessage failed: " + Win32Message(::GetLastError());
        return false;
    }

    const std::wstring agentPath = dir_ + L"\\GhostFrameAgent64.dll";
    agent64_ = ::LoadLibraryExW(agentPath.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (agent64_ == nullptr) {
        error = L"cannot load the 64-bit agent: " + Win32Message(::GetLastError());
        return false;
    }

    hook64_ = reinterpret_cast<HOOKPROC>(::GetProcAddress(agent64_, "GfAgentHookProc"));
    if (hook64_ == nullptr) {
        error = L"the 64-bit agent does not export GfAgentHookProc";
        return false;
    }

    LogLine(L"payload folder: %s", dir_.c_str());
    return true;
}

void Injector::Shutdown() {
    ReleasePendingHook();
    if (agent64_ != nullptr) {
        ::FreeLibrary(agent64_);
        agent64_ = nullptr;
        hook64_ = nullptr;
    }
    if (dir_.empty()) {
        return;
    }

    // Best effort: agents that are still alive keep their copy mapped, in which
    // case the folder is removed by the sweep on the next start.
    DeletePayloadFile(dir_ + L"\\GhostFrameAgent64.dll");
    DeletePayloadFile(dir_ + L"\\GhostFrameAgent32.dll");
    DeletePayloadFile(dir_ + L"\\GhostFrameHelper32.exe");
    if (::RemoveDirectoryW(dir_.c_str())) {
        LogLine(L"payload folder removed");
    } else {
        LogLine(L"payload folder kept (still in use): %s", Win32Message(::GetLastError()).c_str());
    }
    dir_.clear();
}

void Injector::SweepStalePayloads() {
    const std::wstring pattern = baseDir_ + L"\\*";
    WIN32_FIND_DATAW entry = {};
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        if (entry.cFileName[0] == L'.') {
            continue;
        }

        const DWORD pid = static_cast<DWORD>(wcstoul(entry.cFileName, nullptr, 10));
        if (pid == 0 || pid == ::GetCurrentProcessId()) {
            continue;
        }

        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process != nullptr) {
            ::CloseHandle(process);
            continue;
        }
        // Only a pid that cannot exist any more is stale.  ACCESS_DENIED means
        // the process is alive but out of reach (e.g. elevated) and its payload
        // must be left alone.
        if (::GetLastError() != ERROR_INVALID_PARAMETER) {
            continue;
        }

        const std::wstring folder = baseDir_ + L"\\" + entry.cFileName;
        WIN32_FIND_DATAW inner = {};
        HANDLE innerFind = ::FindFirstFileW((folder + L"\\*").c_str(), &inner);
        if (innerFind != INVALID_HANDLE_VALUE) {
            do {
                if ((inner.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    const std::wstring innerPath = folder + L"\\" + inner.cFileName;
                    // Payloads are read-only; DeleteFileW would fail on them.
                    ::SetFileAttributesW(innerPath.c_str(), FILE_ATTRIBUTE_NORMAL);
                    ::DeleteFileW(innerPath.c_str());
                }
            } while (::FindNextFileW(innerFind, &inner));
            ::FindClose(innerFind);
        }
        ::RemoveDirectoryW(folder.c_str());
    } while (::FindNextFileW(find, &entry));

    ::FindClose(find);
}

bool Injector::Extract(int resourceId, const wchar_t* fileName, std::wstring& error) {
    HRSRC resource = ::FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (resource == nullptr) {
        error = FormatString(L"embedded payload %s is missing", fileName);
        return false;
    }

    const DWORD size = ::SizeofResource(nullptr, resource);
    HGLOBAL loaded = ::LoadResource(nullptr, resource);
    const void* bytes = loaded != nullptr ? ::LockResource(loaded) : nullptr;
    if (bytes == nullptr || size == 0) {
        error = FormatString(L"cannot read embedded payload %s", fileName);
        return false;
    }

    BYTE expected[32] = {};
    if (!Sha256(bytes, size, expected)) {
        error = FormatString(L"cannot hash embedded payload %s", fileName);
        return false;
    }

    const std::wstring path = dir_ + L"\\" + fileName;
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"cannot write " + path + L": " + Win32Message(::GetLastError());
        return false;
    }

    DWORD written = 0;
    const BOOL ok = ::WriteFile(file, bytes, size, &written, nullptr);
    ::CloseHandle(file);

    if (!ok || written != size) {
        error = L"cannot write " + path + L": " + Win32Message(::GetLastError());
        ::DeleteFileW(path.c_str());
        return false;
    }

    // Read the copy back and compare its size and SHA-256 with the embedded
    // resource.  This catches truncated writes and accidental corruption.
    bool verified = false;
    HANDLE check = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (check != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER actualSize = {};
        if (::GetFileSizeEx(check, &actualSize) && actualSize.QuadPart == size) {
            std::vector<BYTE> readback(size);
            DWORD total = 0;
            bool readOk = true;
            while (total < size) {
                DWORD chunk = 0;
                if (!::ReadFile(check, readback.data() + total, size - total, &chunk, nullptr) ||
                    chunk == 0) {
                    readOk = false;
                    break;
                }
                total += chunk;
            }
            BYTE actual[32] = {};
            verified = readOk && total == size && Sha256(readback.data(), size, actual) &&
                       ::memcmp(expected, actual, sizeof(expected)) == 0;
        }
        ::CloseHandle(check);
    }

    if (!verified) {
        error = FormatString(L"embedded payload %s failed the integrity check", fileName);
        ::DeleteFileW(path.c_str());
        return false;
    }

    // The payload folder is user-writable, so another process running as the
    // same user could still swap the file between extraction and injection
    // (TOCTOU).  Read-only plus verification protects against accidents, not
    // against a hostile same-user process; the account itself is the boundary.
    if (!::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY)) {
        error = L"cannot mark " + path + L" read-only: " + Win32Message(::GetLastError());
        ::DeleteFileW(path.c_str());
        return false;
    }
    return true;
}

bool Injector::Inject(HWND target, DWORD launcherPid, uint32_t sessionId,
                      InjectionStrategy strategy, std::wstring& error) {
    if (target == nullptr || !::IsWindow(target)) {
        error = L"the target window is gone";
        return false;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(target, &pid);

    ProcessInfo info;
    if (!DescribeProcess(pid, info, error)) {
        return false;
    }

    if (info.integrity != 0) {
        const DWORD current = IntegrityOf(::GetCurrentProcess());
        if (current != 0 && info.integrity > current) {
            error = L"the target runs at a higher integrity level — restart GhostFrame as administrator";
            return false;
        }
    }

    LogLine(L"injecting into pid %u (arch %d, integrity 0x%X), self integrity 0x%X", pid,
            static_cast<int>(info.arch), info.integrity,
            IntegrityOf(::GetCurrentProcess()));

    switch (info.arch) {
        case Arch::X86:
            return InjectHelper(target, launcherPid, sessionId, error);
        case Arch::X64:
            return InjectHook(target, launcherPid, sessionId, strategy, error);
        case Arch::Arm64:
            error = L"ARM64 targets are not supported";
            return false;
        case Arch::Other:
        default:
            error = L"cannot determine the target architecture";
            return false;
    }
}

void Injector::ReleasePendingHook() {
    if (pendingHook_ != nullptr) {
        ::UnhookWindowsHookEx(pendingHook_);
        pendingHook_ = nullptr;
    }
}

bool Injector::InjectHook(HWND target, DWORD launcherPid, uint32_t sessionId,
                          InjectionStrategy strategy, std::wstring& error) {
    const DWORD threadId = ::GetWindowThreadProcessId(target, nullptr);
    if (threadId == 0) {
        error = L"cannot find the target thread";
        return false;
    }

    const int hookType = strategy == InjectionStrategy::CallWndProc ? WH_CALLWNDPROC : WH_GETMESSAGE;
    HHOOK hook = ::SetWindowsHookExW(hookType, hook64_, agent64_, threadId);
    if (hook == nullptr) {
        error = L"SetWindowsHookEx failed: " + Win32Message(::GetLastError());
        return false;
    }

    if (strategy == InjectionStrategy::CallWndProc) {
        DWORD_PTR result = 0;
        const LRESULT sent =
            ::SendMessageTimeoutW(target, message_, static_cast<WPARAM>(launcherPid),
                                  static_cast<LPARAM>(sessionId), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                  1000, &result);
        const DWORD sendError = ::GetLastError();
        ::UnhookWindowsHookEx(hook);

        if (sent == 0) {
            error = L"the target did not answer: " +
                    Win32Message(sendError != 0 ? sendError : ERROR_TIMEOUT);
            return false;
        }
        return true;
    }

    // WH_GETMESSAGE is asynchronous: post the bootstrap message and keep the
    // hook until the caller has seen the agent attach.
    if (!::PostMessageW(target, message_, static_cast<WPARAM>(launcherPid),
                        static_cast<LPARAM>(sessionId))) {
        const DWORD postError = ::GetLastError();
        ::UnhookWindowsHookEx(hook);
        error = L"PostMessage failed: " + Win32Message(postError);
        return false;
    }

    pendingHook_ = hook;
    return true;
}

bool Injector::InjectHelper(HWND target, DWORD launcherPid, uint32_t sessionId,
                            std::wstring& error) {
    const std::wstring helper = dir_ + L"\\GhostFrameHelper32.exe";
    const std::wstring agent = dir_ + L"\\GhostFrameAgent32.dll";

    wchar_t commandLine[1024] = {};
    const int length = swprintf_s(
        commandLine, L"\"%s\" %llu %u %u \"%s\"", helper.c_str(),
        static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(target)), launcherPid,
        sessionId, agent.c_str());
    if (length <= 0) {
        error = L"cannot build the helper command line";
        return false;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};

    if (!::CreateProcessW(helper.c_str(), commandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, dir_.c_str(), &startup, &process)) {
        error = L"cannot start the 32-bit helper: " + Win32Message(::GetLastError());
        return false;
    }

    const DWORD wait = ::WaitForSingleObject(process.hProcess, 3000);
    DWORD code = 0;
    ::GetExitCodeProcess(process.hProcess, &code);

    if (wait != WAIT_OBJECT_0) {
        ::TerminateProcess(process.hProcess, 1);
        code = 14;
    }

    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);

    if (code != 0) {
        error = L"the 32-bit helper failed: " + HelperFailure(code);
        return false;
    }
    return true;
}

}  // namespace gf
