// ============================================================
// GameDir.cpp — 游戏目录解析/持久化（从原 MOD 加载器中独立出来）
// ============================================================
#include "GameDir.h"
#include "Lang.h"
#include "Log.h"
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <cstring>

#pragma comment(lib, "shell32.lib")

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
        ShellExecuteA(nullptr, "open", Get(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}