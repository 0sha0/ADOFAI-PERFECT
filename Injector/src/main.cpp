// ============================================================
// Injector.exe — 将 ADOFAIPerfect.dll 注入 A Dance of Fire and Ice
//
// 用法:
//   Injector.exe                     # 自动等待并注入游戏（默认进程名/路径）
//   Injector.exe --dll <路径>        # 指定 DLL
//   Injector.exe --name <进程名>     # 指定目标进程名
//   Injector.exe --pid <进程ID>      # 直接指定 PID
// ============================================================
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>
#include <string>

static void SetUtf8Console()
{
    SetConsoleOutputCP(65001);
    SetConsoleTitleW(L"ADOFAI PERFECT Injector");
}

static DWORD FindProcessByName(const wchar_t* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, exeName) == 0)
            {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool ModuleLoadedInProcess(HANDLE hProc, const wchar_t* moduleName)
{
    HMODULE mods[1024] = {};
    DWORD needed = 0;
    if (!EnumProcessModulesEx(hProc, mods, sizeof(mods), &needed, LIST_MODULES_ALL))
        return false;
    int count = (int)(needed / sizeof(HMODULE));
    if (count > 1024) count = 1024;
    wchar_t name[MAX_PATH];
    for (int i = 0; i < count; i++)
    {
        if (GetModuleFileNameExW(hProc, mods[i], name, MAX_PATH))
        {
            const wchar_t* base = wcsrchr(name, L'\\');
            base = base ? base + 1 : name;
            if (_wcsicmp(base, moduleName) == 0)
                return true;
        }
    }
    return false;
}

static bool WaitForModule(HANDLE hProc, const wchar_t* module, int timeoutMs)
{
    int waited = 0;
    while (waited < timeoutMs)
    {
        if (ModuleLoadedInProcess(hProc, module))
            return true;
        Sleep(250);
        waited += 250;
    }
    return false;
}

static bool InjectDll(HANDLE hProc, const wchar_t* dllPath, std::string& err)
{
    size_t bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(hProc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        err = "VirtualAllocEx failed";
        return false;
    }
    if (!WriteProcessMemory(hProc, remote, dllPath, bytes, nullptr))
    {
        err = "WriteProcessMemory failed";
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        return false;
    }
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE loadLib =
        (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
    if (!loadLib)
    {
        err = "GetProcAddress(LoadLibraryW) failed";
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        return false;
    }
    HANDLE th = CreateRemoteThread(hProc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!th)
    {
        err = "CreateRemoteThread failed (被杀软拦截?)";
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        return false;
    }
    WaitForSingleObject(th, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(th, &exitCode);
    CloseHandle(th);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);

    if (!exitCode)
    {
        err = "LoadLibraryW 返回 0（DLL 加载失败，检查位数/依赖）";
        return false;
    }
    return true;
}

int wmain(int argc, wchar_t** argv)
{
    SetUtf8Console();

    wchar_t dllPath[MAX_PATH] = {};
    wchar_t procName[128] = L"A Dance of Fire and Ice.exe";
    DWORD pidArg = 0;

    // 默认 DLL = 本程序同目录下 ADOFAIPerfect.dll
    GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
    wchar_t* slash = wcsrchr(dllPath, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(dllPath, L"ADOFAIPerfect.dll");

    for (int i = 1; i < argc; i++)
    {
        if (!wcscmp(argv[i], L"--dll") && i + 1 < argc)
            wcscpy_s(dllPath, argv[++i]);
        else if (!wcscmp(argv[i], L"--name") && i + 1 < argc)
            wcscpy_s(procName, argv[++i]);
        else if (!wcscmp(argv[i], L"--pid") && i + 1 < argc)
            pidArg = (DWORD)_wtoi(argv[++i]);
    }

    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES)
    {
        wprintf(L"[!] 找不到 DLL: %ls\n", dllPath);
        return 1;
    }

    // 1. 找进程（最多等 10 分钟，支持先开注入器再开游戏）
    wprintf(L"[*] 等待游戏进程: %ls (最长 10 分钟)\n", procName);
    DWORD pid = pidArg;
    for (int waited = 0; !pid && waited < 600000; waited += 500)
    {
        pid = FindProcessByName(procName);
        if (!pid)
            Sleep(500);
    }
    if (!pid)
    {
        wprintf(L"[!] 未找到游戏进程，请先启动游戏。\n");
        return 1;
    }
    wprintf(L"[+] 找到游戏 PID=%lu\n", pid);

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                   PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
    if (!hProc)
    {
        wprintf(L"[!] OpenProcess 失败 (err=%lu)。请以相同或管理员权限运行。\n", GetLastError());
        return 1;
    }

    // 2. 等 Mono 运行时就绪（游戏初始化完毕）
    wprintf(L"[*] 等待 Mono 运行时加载...\n");
    if (!WaitForModule(hProc, L"mono-2.0-bdwgc.dll", 120000))
    {
        wprintf(L"[!] Mono 运行时未加载，游戏可能未完成启动。\n");
        CloseHandle(hProc);
        return 1;
    }
    wprintf(L"[+] Mono 运行时就绪\n");

    // 3. 注入
    wprintf(L"[*] 注入: %ls\n", dllPath);
    std::string err;
    if (!InjectDll(hProc, dllPath, err))
    {
        wprintf(L"[!] 注入失败: %hs\n", err.c_str());
        CloseHandle(hProc);
        return 1;
    }

    // 4. 校验
    if (WaitForModule(hProc, L"ADOFAIPerfect.dll", 5000))
        wprintf(L"[+] 注入成功！游戏中按 Insert 打开/隐藏菜单，End 卸载。\n");
    else
        wprintf(L"[!] DLL 已调用但未在模块列表中检测到（可能初始化失败，详见 %%LOCALAPPDATA%%\\ADOFvec\\adofai_cheat.log）\n");

    CloseHandle(hProc);
    return 0;
}
