// ============================================================
// GameDir.cpp — 游戏目录解析/持久化（从原 MOD 加载器中独立出来）
// ============================================================
#include "GameDir.h"
#include "Lang.h"
#include "Log.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <string>
#include <mutex>
#include <cstring>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace GameDir
{
    namespace
    {
        char g_dir[MAX_PATH] = { 0 };
        bool g_init = false;

        std::string DirOfA(const char* file)
        {
            std::string s = file ? file : "";
            size_t k = s.find_last_of("\\/");
            return (k == std::string::npos) ? s : s.substr(0, k);
        }
        std::string TrimSlash(std::string d)
        {
            while (!d.empty() && (d.back() == '\\' || d.back() == '/'))
                d.pop_back();
            return d;
        }
        bool DirExists(const std::string& d)
        {
            DWORD a = GetFileAttributesA(d.c_str());
            return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }
        void Resolve()
        {
            g_init = true;
            char p[MAX_PATH] = { 0 };
            GetModuleFileNameA(nullptr, p, MAX_PATH);
            std::string d = DirOfA(p);

            char saved[MAX_PATH] = { 0 };
            I18N::Prefs::GetStr("game_dir", saved, sizeof(saved), "");
            if (saved[0])
            {
                if (Valid(saved))
                    d = TrimSlash(saved);
                else
                    Log::Printf("[dir] saved game dir invalid, fallback: %s", saved);
            }
            strncpy_s(g_dir, d.c_str(), _TRUNCATE);
            Log::Printf("[dir] game dir = %s", g_dir);
        }
    }

    bool Valid(const char* dir)
    {
        if (!dir || !dir[0] || !DirExists(dir))
            return false;
        std::string d = TrimSlash(dir);
        if (GetFileAttributesA((d + "\\A Dance of Fire and Ice_Data").c_str()) != INVALID_FILE_ATTRIBUTES)
            return true;
        if (GetFileAttributesA((d + "\\A Dance of Fire and Ice.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
            return true;
        // 兼容改名的整合包：存在 *(_Data) 目录即可
        std::string pat = d + "\\*_Data";
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(pat.c_str(), &fd);
        bool ok = (h != INVALID_HANDLE_VALUE);
        if (ok) FindClose(h);
        return ok;
    }

    const char* Get()
    {
        if (!g_init) Resolve();
        return g_dir;
    }

    void Set(const char* dir)
    {
        if (!g_init) Resolve();
        std::string d = TrimSlash(dir ? dir : "");
        if (!d.empty() && Valid(d.c_str()))
        {
            strncpy_s(g_dir, d.c_str(), _TRUNCATE);
            I18N::Prefs::SetStr("game_dir", d.c_str());
            Log::Printf("[dir] game dir set to %s", g_dir);
        }
        else
        {
            char p[MAX_PATH] = { 0 };
            GetModuleFileNameA(nullptr, p, MAX_PATH);
            strncpy_s(g_dir, DirOfA(p).c_str(), _TRUNCATE);
            I18N::Prefs::SetStr("game_dir", "");
            Log::Printf("[dir] game dir reset to %s", g_dir);
        }
        I18N::Prefs::Save();
    }

    void OpenFolder()
    {
        ShellAsync::Open(Get());
    }
}

// ============================================================
// ShellAsync — 见 GameDir.h 顶部说明（禁止在 Present 渲染线程里阻塞）
// ============================================================
namespace ShellAsync
{
    namespace
    {
        std::mutex  g_mx;
        std::string g_folder;                  // 已就绪的选目录结果（空 = 用户取消）
        bool        g_folderReady = false;
        std::string g_file;                    // 已就绪的选文件结果
        bool        g_fileReady = false;
        volatile LONG g_dlgOpen = 0;           // 是否有对话框正在等用户

        // 对话框 owner：Unity 主窗口（找不到就无 owner，仍可用）
        HWND OwnerWindow()
        {
            HWND h = FindWindowW(L"UnityWndClass", nullptr);
            return h ? h : nullptr;
        }

        DWORD WINAPI OpenThread(LPVOID param)
        {
            std::string* t = (std::string*)param;
            if (t)
            {
                ShellExecuteA(nullptr, "open", t->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                delete t;
            }
            return 0;
        }

        DWORD WINAPI FolderThread(LPVOID param)
        {
            std::string title;
            if (param) { title = *(std::string*)param; delete (std::string*)param; }
            std::string picked;
            // 新式浏览对话框（BIF_NEWDIALOGSTYLE）要求 STA：工作线程刚起来，套间是干净的。
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const bool comOk = SUCCEEDED(hr);
            wchar_t wtitle[160] = L"Select folder";
            if (!title.empty())
                MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wtitle, 160);
            BROWSEINFOW bi = {};
            bi.hwndOwner = OwnerWindow();
            bi.lpszTitle = wtitle;
            bi.ulFlags = BIF_RETURNONLYFSDIRS | (comOk ? (BIF_NEWDIALOGSTYLE | BIF_USENEWUI) : 0);
            LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
            if (idl)
            {
                wchar_t wpath[MAX_PATH * 2] = {};
                if (SHGetPathFromIDListW(idl, wpath))
                {
                    char u8[MAX_PATH * 2] = {};
                    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, u8, sizeof(u8), nullptr, nullptr);
                    picked = u8;
                }
                CoTaskMemFree(idl);
            }
            if (comOk) CoUninitialize();
            {
                std::lock_guard<std::mutex> lk(g_mx);
                g_folder = picked;
                g_folderReady = true;
            }
            InterlockedExchange(&g_dlgOpen, 0);
            return 0;
        }

        struct FileJob
        {
            std::wstring filter;   // 双 NUL 结尾的过滤器串（含末尾两个 NUL）
            std::wstring title;
        };

        // OPENFILENAME 的过滤器是「双 NUL 结尾」的多段串，例如
        //   L"Malody Skin (*.msp)\0*.msp\0All files\0*.*\0"
        // 用 std::wstring 的 const wchar_t* 赋值会在第一个 NUL 处截断（只剩一段），
        // 且末尾没有双 NUL —— GetOpenFileNameW 会一路读出界。按双 NUL 语义手工拷贝。
        std::wstring DupFilter(const wchar_t* f)
        {
            std::wstring s;
            if (!f) return s;
            for (;;)
            {
                const wchar_t* e = f;
                while (*e) e++;
                s.append(f, (size_t)(e - f) + 1);        // 连段尾 NUL 一起拷
                if (!e[1]) { s.push_back(L'\0'); break; } // 双 NUL = 结束
                f = e + 1;
            }
            return s;
        }

        DWORD WINAPI FileThread(LPVOID param)
        {
            FileJob* job = (FileJob*)param;
            std::string picked;
            if (job)
            {
                wchar_t file[MAX_PATH * 2] = {};
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = OwnerWindow();
                ofn.lpstrFilter = job->filter.empty() ? nullptr : job->filter.c_str();
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH * 2;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
                ofn.lpstrTitle = job->title.empty() ? nullptr : job->title.c_str();
                if (GetOpenFileNameW(&ofn))
                {
                    char u8[MAX_PATH * 2] = {};
                    WideCharToMultiByte(CP_UTF8, 0, file, -1, u8, sizeof(u8), nullptr, nullptr);
                    picked = u8;
                }
                delete job;
            }
            {
                std::lock_guard<std::mutex> lk(g_mx);
                g_file = picked;
                g_fileReady = true;
            }
            InterlockedExchange(&g_dlgOpen, 0);
            return 0;
        }
    }

    void Open(const char* utf8Target)
    {
        if (!utf8Target || !utf8Target[0]) return;
        std::string* t = new std::string(utf8Target);
        HANDLE h = CreateThread(nullptr, 0, OpenThread, t, 0, nullptr);
        if (h) CloseHandle(h);
        else delete t;
    }

    bool Busy() { return InterlockedCompareExchange(&g_dlgOpen, 1, 1) != 0; }

    bool PickFolder(const char* titleUtf8)
    {
        if (InterlockedExchange(&g_dlgOpen, 1)) return false;   // 已有一个在等
        std::string* t = new std::string(titleUtf8 ? titleUtf8 : "");
        HANDLE h = CreateThread(nullptr, 0, FolderThread, t, 0, nullptr);
        if (h) CloseHandle(h);
        else { delete t; InterlockedExchange(&g_dlgOpen, 0); return false; }
        return true;
    }

    bool TakeFolder(char* outUtf8, int n)
    {
        if (!outUtf8 || n <= 0) return false;
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_folderReady) return false;
        outUtf8[0] = 0;
        if (!g_folder.empty()) snprintf(outUtf8, (size_t)n, "%s", g_folder.c_str());
        g_folder.clear();
        g_folderReady = false;
        return outUtf8[0] != 0;                                 // 取消 = false（调用方忽略即可）
    }

    bool PickFile(const wchar_t* filter, const wchar_t* title)
    {
        if (InterlockedExchange(&g_dlgOpen, 1)) return false;
        FileJob* job = new FileJob();
        if (filter) job->filter = DupFilter(filter);
        if (title) job->title = title;
        HANDLE h = CreateThread(nullptr, 0, FileThread, job, 0, nullptr);
        if (h) CloseHandle(h);
        else { delete job; InterlockedExchange(&g_dlgOpen, 0); return false; }
        return true;
    }

    bool TakeFile(char* outUtf8, int n)
    {
        if (!outUtf8 || n <= 0) return false;
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_fileReady) return false;
        outUtf8[0] = 0;
        if (!g_file.empty()) snprintf(outUtf8, (size_t)n, "%s", g_file.c_str());
        g_file.clear();
        g_fileReady = false;
        return outUtf8[0] != 0;
    }
}
