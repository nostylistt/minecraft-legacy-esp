// Minecraft Legacy ESP — minimal DLL injector.
//
// Usage:   legacy_esp_injector.exe [path\to\legacy_esp.dll]
//   - If no path is given, defaults to "legacy_esp.dll" next to the exe.
//   - Waits for a javaw.exe / java.exe process to appear, then LoadLibrary-injects into it.
//   - If a Minecraft process is already running, immediately injects into the newest one.
//
// Mode marker: writes %TEMP%\mle_hook_mode.txt before injection. Pass "direct" or "resolve"
//   as a second argument to override auto-detect (default: auto).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string>
#include <set>
#include <vector>

static void EnableDebugPriv() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &tok)) return;
    LUID luid;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(tok);
}

static std::vector<DWORD> FindJavaw() {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"javaw.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"java.exe")  == 0) {
                out.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

static bool Inject(DWORD pid, const std::wstring& dllPath) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) return false;
    const size_t bytes = (dllPath.length() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) { CloseHandle(proc); return false; }
    if (!WriteProcessMemory(proc, remotePath, dllPath.c_str(), bytes, nullptr)) {
        VirtualFreeEx(proc, remotePath, 0, MEM_RELEASE); CloseHandle(proc); return false;
    }
    HMODULE hK = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(hK, "LoadLibraryW");
    if (!loadLib) { VirtualFreeEx(proc, remotePath, 0, MEM_RELEASE); CloseHandle(proc); return false; }
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remotePath, 0, nullptr);
    if (!th) { VirtualFreeEx(proc, remotePath, 0, MEM_RELEASE); CloseHandle(proc); return false; }
    WaitForSingleObject(th, 15000);
    DWORD hMod = 0; GetExitCodeThread(th, &hMod);
    CloseHandle(th);
    VirtualFreeEx(proc, remotePath, 0, MEM_RELEASE);
    CloseHandle(proc);
    return hMod != 0;
}

static void WriteMode(const char* mode) {
    char tmp[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, tmp)) return;
    std::string path = std::string(tmp) + "mle_hook_mode.txt";
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(f, mode, (DWORD)strlen(mode), &wr, nullptr); CloseHandle(f);
}

static std::wstring ResolveDllPath(int argc, wchar_t** argv) {
    if (argc >= 2) return argv[1];
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring p = exePath;
    size_t slash = p.find_last_of(L'\\');
    if (slash != std::wstring::npos) p = p.substr(0, slash + 1);
    else p.clear();
    return p + L"legacy_esp.dll";
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleTitleW(L"Minecraft Legacy ESP — Injector");
    printf("=========================================\n");
    printf("  Minecraft Legacy ESP — injector\n");
    printf("=========================================\n");

    std::wstring dllPath = ResolveDllPath(argc, argv);
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[-] DLL not found: %ls\n", dllPath.c_str());
        printf("[-] Pass the DLL path as the first argument, or place it next to this exe.\n");
        getchar();
        return 1;
    }
    wprintf(L"[*] DLL: %ls\n", dllPath.c_str());

    const char* mode = "auto";
    if (argc >= 3) {
        wchar_t* m = argv[2];
        if (_wcsicmp(m, L"direct") == 0)  mode = "direct";
        else if (_wcsicmp(m, L"resolve") == 0) mode = "resolve";
    }
    if (strcmp(mode, "auto") != 0) {
        WriteMode(mode);
        printf("[*] Hook mode: %s (forced)\n", mode);
    } else {
        printf("[*] Hook mode: auto (DLL decides based on opengl32 state)\n");
    }

    EnableDebugPriv();

    // If Minecraft is already running, inject into the newest one and exit.
    std::vector<DWORD> running = FindJavaw();
    if (!running.empty()) {
        DWORD pid = running.back();
        printf("[*] Attaching to running java pid=%lu ...\n", pid);
        if (Inject(pid, dllPath)) {
            printf("[+] Injected. Press INSERT in-game to open the menu.\n");
        } else {
            printf("[-] Inject failed. Try running as administrator.\n");
        }
        printf("\nPress Enter to close.\n");
        getchar();
        return 0;
    }

    // Otherwise watch for a new java process.
    std::set<DWORD> before;
    for (DWORD p : FindJavaw()) before.insert(p);
    printf("[*] Waiting for Minecraft to start... (Ctrl+C to cancel)\n");
    std::set<DWORD> injected;
    for (;;) {
        for (DWORD p : FindJavaw()) {
            if (before.count(p) || injected.count(p)) continue;
            injected.insert(p);
            printf("[*] Minecraft detected (pid=%lu), injecting ...\n", p);
            for (int i = 0; i < 2000; ++i) {
                if (Inject(p, dllPath)) {
                    printf("[+] Injected. Press INSERT in-game to open the menu.\n");
                    printf("\nPress Enter to close.\n");
                    getchar();
                    return 0;
                }
                Sleep(5);
            }
            printf("[-] Inject failed after retries. Try running as administrator.\n");
            getchar();
            return 1;
        }
        Sleep(5);
    }
}
