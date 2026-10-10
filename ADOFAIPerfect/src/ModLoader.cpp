// ============================================================
// ModLoader.cpp — 自研 UMM 兼容加载器的 C++ 侧桥接（实现）
//   · 安装加载器文件到游戏目录
//   · 在游戏主线程上调用 AdofPerfectUmm.Bridge
//   · 渲染线程只读缓存，绝不直接碰 Mono
// ============================================================
#include "ModLoader.h"
#include "GameDir.h"
#include "GameBridge.h"
#include "MonoApi.h"
#include "Lang.h"
#include "Log.h"

#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

namespace ModLoader
{
    namespace
    {
        // ---------------- 基础文件工具 ----------------
        bool FileExists(const std::string& p)
        {
            DWORD a = GetFileAttributesA(p.c_str());
            return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        }
        bool DirExists(const std::string& p)
        {
            DWORD a = GetFileAttributesA(p.c_str());
            return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }
        std::string JoinPath(const std::string& a, const std::string& b)
        {
            if (a.empty()) return b;
            if (b.empty()) return a;
            char last = a[a.size() - 1];
            if (last == '\\' || last == '/') return a + b;
            return a + "\\" + b;
        }
        long long FileSize(const std::string& p)
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &fad)) return -1;
            return ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        }
        std::string ReadAll(const std::string& path)
        {
            HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return std::string();
            LARGE_INTEGER sz{};
            GetFileSizeEx(h, &sz);
            std::string out;
            out.resize((size_t)sz.QuadPart);
            DWORD got = 0;
            if (!out.empty()) ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
            out.resize(got);
            CloseHandle(h);
            return out;
        }
        bool WriteAll(const std::string& path, const std::string& data)
        {
            HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return false;
            DWORD wrote = 0;
            bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr) && wrote == data.size());
            CloseHandle(h);
            return ok;
        }
        bool CopyFileTo(const std::string& src, const std::string& dst)
        {
            if (!FileExists(src)) return false;
            if (FileExists(dst) && FileSize(src) == FileSize(dst)) return true;
            return CopyFileA(src.c_str(), dst.c_str(), FALSE) != 0;
        }
        std::string DllDir()
        {
            static std::string s;
            static bool once = false;
            if (!once)
            {
                once = true;
                HMODULE self = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)(const void*)&DllDir, &self) && self)
                {
                    char p[MAX_PATH * 2] = { 0 };
                    DWORD n = GetModuleFileNameA(self, p, (DWORD)sizeof(p));
                    if (n > 0 && n < sizeof(p))
                    {
                        std::string full(p, n);
                        size_t k = full.find_last_of("\\/");
                        s = (k == std::string::npos) ? std::string(".\\") : full.substr(0, k + 1);
                    }
                }
                if (s.empty()) s = ".\\";
            }
            return s;
        }
        std::string FindDataDir(const std::string& gameDir)
        {
            std::string pat = gameDir + "\\*_Data";
            WIN32_FIND_DATAA fd{};
            HANDLE h = FindFirstFileA(pat.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return std::string();
            std::string found = fd.cFileName;
            FindClose(h);
            return found;
        }
        std::string ManagedDir()
        {
            std::string g = GameDir::Get();
            if (g.empty() || !DirExists(g)) return std::string();
            std::string data = FindDataDir(g);
            if (data.empty()) return std::string();
            return JoinPath(JoinPath(g, data), "Managed");
        }
        std::string JsonEscape(const std::string& s)
        {
            std::string o;
            for (char c : s)
            {
                if (c == '\\' || c == '"') { o += '\\'; o += c; }
                else o += c;
            }
            return o;
        }

        // ---------------- 结果缓存 ----------------
        std::mutex   g_resMx;
        std::string  g_state = "[]";
        std::string  g_log;
        std::string  g_err;
        std::atomic<bool> g_ready{ false };

        // 「设置是否真的应用进 MOD」的异步回报（apply 命令用）
        std::mutex   g_applyMx;
        std::string  g_applyId;
        std::string  g_applyRes;
        bool         g_applyPending = false;

        std::mutex              g_jobMx;
        std::vector<std::pair<std::string, std::string>> g_jobs;

        std::string  g_modsDir;
        bool         g_modsDirLoaded = false;
        DWORD        g_lastTick = 0;
        DWORD        g_lastInstallTry = 0;

        void SetResultState(const std::string& json)
        {
            std::lock_guard<std::mutex> lk(g_resMx);
            g_state = json;
            g_ready = (json.size() > 2 && json[0] == '[');
        }
        void SetResultLog(const std::string& text)
        {
            std::lock_guard<std::mutex> lk(g_resMx);
            g_log = text;
        }
        void SetResultError(const std::string& text)
        {
            std::lock_guard<std::mutex> lk(g_resMx);
            g_err = text;
        }

        // ---------------- Mono 桥接（仅主线程）----------------
        MonoClass* g_bridge = nullptr;

        bool ResolveBridge()
        {
            if (g_bridge) return true;
            if (!MonoApi::Ready()) return false;
            using namespace MonoApi;

            // 已加载的程序集里找我们的 Bridge（门卫/注入两条路都可能命中）。
            // 注意：重名的「原生 UnityModManager」里没有 Bridge，所以必须按类判定，
            // 不能只看 mono_image_loaded 是否返回非空。
            if (mono_image_loaded && mono_class_from_name)
            {
                MonoImage* img = mono_image_loaded("UnityModManager");
                if (img)
                {
                    MonoClass* c = mono_class_from_name(img, "AdofPerfectUmm", "Bridge");
                    if (c)
                    {
                        g_bridge = c;
                        if (mono_class_init) mono_class_init(c);
                        return true;
                    }
                }
            }

            // 还没加载：按文件路径把我们的加载器装进 AppDomain
            {
                std::string dll = JoinPath(LoaderDirPath(), "UnityModManager.dll");
                if (!FileExists(dll)) return false;
                MonoDomain* dom = mono_domain_get ? mono_domain_get() : nullptr;
                if (!dom || !mono_domain_assembly_open) return false;
                MonoAssembly* as = mono_domain_assembly_open(dom, dll.c_str());
                if (!as || !mono_assembly_get_image) return false;
                MonoImage* img = mono_assembly_get_image(as);
                if (!img) return false;
                MonoClass* c = mono_class_from_name(img, "AdofPerfectUmm", "Bridge");
                if (!c) return false;
                g_bridge = c;
                if (mono_class_init) mono_class_init(c);
                return true;
            }
        }

        std::string CallBridge(const char* method, int argc, const char* a0, const char* a1)
        {
            using namespace MonoApi;
            if (!ResolveBridge()) return "err:no-bridge";
            MonoDomain* dom = mono_domain_get ? mono_domain_get() : nullptr;
            if (!dom) return std::string();
            MonoMethod* m = mono_class_get_method_from_name(g_bridge, method, argc);
            if (!m) return "err:no-method";
            MonoString* s0 = mono_string_new(dom, a0 ? a0 : "");
            MonoString* s1 = mono_string_new(dom, a1 ? a1 : "");
            void* args[2] = { s0, s1 };
            MonoObject* exc = nullptr;
            MonoObject* res = mono_runtime_invoke(m, nullptr, args, &exc);
            if (exc)
            {
                char buf[128];
                sprintf_s(buf, "err:exception in %s", method);
                return std::string(buf);
            }
            if (!res) return std::string();
            char* u = mono_string_to_utf8((MonoString*)res);
            std::string out = u ? u : "";
            if (u && mono_free) mono_free(u);
            return out;
        }

        void ExecuteJob(const std::string& cmd, const std::string& arg)
        {
            if (!MonoApi::Ready()) { SetResultError("err:mono-not-ready"); return; }
            if (cmd == "start")
            {
                std::string r = CallBridge("StartWith", 1, arg.c_str(), nullptr);
                SetResultError(r == "ok" ? std::string() : r);
            }
            else if (cmd == "state")
            {
                std::string r = CallBridge("Invoke", 2, "state", "");
                // 调试留档：加载器回报的原始状态（便于比对 MOD Id / 排查“未识别”）
                {
                    std::string ld = LoaderDirPath();
                    if (!ld.empty()) WriteAll(JoinPath(ld, "State.json"), r);
                }
                if (!r.empty() && r[0] == '[') { SetResultState(r); SetResultError(std::string()); }
                else { SetResultState("[]"); SetResultError(r.empty() ? "err:no-state" : r); }
            }
            else if (cmd == "log")
            {
                std::string r = CallBridge("Invoke", 2, "log", "");
                SetResultLog(r);
            }
            else if (cmd == "apply")
            {
                // 工具侧保存设置文件之后调用：把新的设置值热应用进正在运行的 MOD
                std::string r = CallBridge("Invoke", 2, "apply", arg.c_str());
                {
                    std::lock_guard<std::mutex> lk(g_applyMx);
                    g_applyId = arg;
                    g_applyRes = r;
                    g_applyPending = true;
                }
                if (g_applyRes.compare(0, 4, "err:") == 0) SetResultError(r);
            }
            else
            {
                std::string r = CallBridge("Invoke", 2, cmd.c_str(), arg.c_str());
                if (!r.empty() && r.compare(0, 4, "err:") == 0) SetResultError(r);
            }
        }

        void RunJobs(void*)
        {
            for (;;)
            {
                std::pair<std::string, std::string> job;
                {
                    std::lock_guard<std::mutex> lk(g_jobMx);
                    if (g_jobs.empty()) break;
                    job = g_jobs.front();
                    g_jobs.erase(g_jobs.begin());
                }
                ExecuteJob(job.first, job.second);
            }
        }
    } // namespace

    // ============================================================
    //  安装 / 配置
    // ============================================================
    std::string LoaderDirPath()
    {
        std::string m = ManagedDir();
        // 用我们自己的目录，避免和「原生 UnityModManager」的 DLL 抢同一个文件
        // （那个文件在游戏运行时被内存映射住，无法覆写）。
        return m.empty() ? std::string() : JoinPath(m, "AdofPerfectUmm");
    }

    std::string ModsDir()
    {
        if (!g_modsDirLoaded)
        {
            g_modsDirLoaded = true;
            char b[MAX_PATH * 2] = { 0 };
            I18N::Prefs::GetStr("mod.dir", b, sizeof(b), "");
            if (b[0] && DirExists(b)) g_modsDir = b;
            else
            {
                std::string g = GameDir::Get();
                g_modsDir = DirExists(JoinPath(g, "Mods")) ? JoinPath(g, "Mods") : JoinPath(g, "Mods");
            }
            std::string ld = LoaderDirPath();
            if (!ld.empty())
            {
                std::string cfg = ReadAll(JoinPath(ld, "AdofPerfectUmm.json"));
                if (!cfg.empty() && g_modsDir.empty()) g_modsDir = JoinPath(GameDir::Get(), "Mods");
            }
        }
        return g_modsDir;
    }

    void SetModsDir(const std::string& dir)
    {
        g_modsDirLoaded = true;
        g_modsDir = dir;
        std::string ld = LoaderDirPath();
        if (ld.empty()) return;
        CreateDirectoryA(ld.c_str(), nullptr);
        std::string json;
        json += "{\r\n";
        json += "  \"Name\": \"A Dance of Fire and Ice\",\r\n";
        json += "  \"GameExe\": \"A Dance of Fire and Ice.exe\",\r\n";
        json += "  \"ModsDirectory\": \"Mods\",\r\n";
        json += "  \"ModInfo\": \"Info.json\",\r\n";
        json += "  \"ModsPath\": \"" + JsonEscape(dir) + "\",\r\n";
        json += "  \"StartingPoint\": \"[Assembly-CSharp.dll]ADOStartup.Startup:Before\",\r\n";
        json += "  \"UIStartingPoint\": \"[Assembly-CSharp.dll]ADOStartup.Startup:After\",\r\n";
        json += "  \"MinimalManagerVersion\": \"0.22.14\"\r\n";
        json += "}\r\n";
        WriteAll(JoinPath(ld, "AdofPerfectUmm.json"), json);
    }

    bool Installed()
    {
        std::string src = JoinPath(DllDir() + "umm", "UnityModManager.dll");
        std::string ld = LoaderDirPath();
        if (ld.empty() || !FileExists(src)) return false;
        std::string dst = JoinPath(ld, "UnityModManager.dll");
        if (!FileExists(dst)) return false;
        if (FileSize(dst) != FileSize(src)) return false;
        if (!FileExists(JoinPath(ld, "AdofPerfectUmm.json"))) return false;
        // 启动钩子（doorstop）也必须指向我们的加载器，否则下次启动根本不会加载它
        std::string g = GameDir::Get();
        if (g.empty()) return false;
        std::string ini = ReadAll(JoinPath(g, "doorstop_config.ini"));
        if (ini.find(dst) == std::string::npos) return false;
        return true;
    }

    // UnityDoorstop：把「游戏启动时加载哪个程序集」指向我们的加载器。
    // 原配置只备份一次，用户可随时还原。
    static bool WriteDoorstopConfig()
    {
        std::string g = GameDir::Get();
        if (g.empty()) return false;
        std::string srcDir = DllDir() + "umm";
        std::string winhttp = JoinPath(srcDir, "winhttp_x64.dll");
        std::string dstWinhttp = JoinPath(g, "winhttp.dll");
        if (!FileExists(dstWinhttp) && FileExists(winhttp))
            CopyFileTo(winhttp, dstWinhttp);

        std::string ini = JoinPath(g, "doorstop_config.ini");
        if (FileExists(ini) && !FileExists(ini + ".adofperfect-backup"))
            CopyFileA(ini.c_str(), (ini + ".adofperfect-backup").c_str(), FALSE);

        std::string target = JoinPath(LoaderDirPath(), "UnityModManager.dll");
        std::string doc;
        doc += "[General]\r\n";
        doc += "enabled = true\r\n";
        doc += "target_assembly = " + target + "\r\n";
        return WriteAll(ini, doc);
    }

    bool Install(std::string* message)
    {
        std::string g = GameDir::Get();
        std::string managed = ManagedDir();
        std::string ld = LoaderDirPath();
        if (g.empty() || managed.empty() || ld.empty())
        {
            if (message) *message = "game folder not set";
            return false;
        }

        std::string srcDir = DllDir() + "umm";
        std::string srcDll = JoinPath(srcDir, "UnityModManager.dll");
        std::string srcHarmony = JoinPath(srcDir, "0Harmony.dll");
        if (!FileExists(srcDll))
        {
            if (message) *message = "loader not built: " + srcDll;
            return false;
        }

        CreateDirectoryA(ld.c_str(), nullptr);

        bool ok = true;
        ok = CopyFileTo(srcDll, JoinPath(ld, "UnityModManager.dll")) && ok;
        if (FileExists(srcHarmony))
        {
            CopyFileTo(srcHarmony, JoinPath(ld, "0Harmony.dll"));
            // 游戏 Managed 根下若没有 0Harmony，也补一份（MOD 依赖它）
            if (!FileExists(JoinPath(managed, "0Harmony.dll")))
                CopyFileTo(srcHarmony, JoinPath(managed, "0Harmony.dll"));
        }
        // dnlib：JALib 等 MOD 会通过 AssemblyResolve 找它（原版 UMM 目录里也带）
        std::string srcDnlib = JoinPath(srcDir, "dnlib.dll");
        if (FileExists(srcDnlib)) CopyFileTo(srcDnlib, JoinPath(ld, "dnlib.dll"));

        // MOD 目录配置（供「直接启动游戏」时的 doorstop 路径使用）
        SetModsDir(ModsDir());

        // UnityDoorstop：让下次启动游戏时直接加载我们的加载器
        // （这样 MOD 能在游戏启动前打补丁；注入器路径只是兜底）
        WriteDoorstopConfig();

        if (!ok && message) *message = "copy failed (game running? close it and retry)";
        else if (message) *message = "installed to " + ld;
        return ok;
    }

    // ============================================================
    //  每帧驱动
    // ============================================================
    void Tick()
    {
        DWORD now = GetTickCount();
        if (now - g_lastTick < 700) return;
        g_lastTick = now;

        if (!Ready() && now - g_lastInstallTry > 5000)
        {
            g_lastInstallTry = now;
            if (!Installed()) Install(nullptr);
        }

        std::string dir = ModsDir();
        {
            std::lock_guard<std::mutex> lk(g_jobMx);
            if (g_jobs.size() > 256) return;   // 主线程长时间没跑：别无限堆积
            g_jobs.push_back({ "start", dir });
            g_jobs.push_back({ "state", "" });
        }
        // 不依赖 g_queued：GameBridge::PostTask 内部已按位去重；
        // 早期用 g_queued 时，若首次投递发生在“主线程投递器就绪前”，
        // 标志会永久卡在 true，任务再也不执行（表现为 MOD 一直不加载）。
        GameBridge::QueueMainThreadWork(&RunJobs, nullptr);
    }

    bool Ready()
    {
        return g_ready.load();
    }
    std::string StateJson()
    {
        std::lock_guard<std::mutex> lk(g_resMx);
        return g_state;
    }
    std::string LogTail()
    {
        std::lock_guard<std::mutex> lk(g_resMx);
        return g_log;
    }
    std::string LastError()
    {
        std::lock_guard<std::mutex> lk(g_resMx);
        return g_err;
    }

    // ============================================================
    //  操作
    // ============================================================
    void QueueCommand(const std::string& cmd, const std::string& arg)
    {
        {
            std::lock_guard<std::mutex> lk(g_jobMx);
            if (g_jobs.size() > 256) return;
            g_jobs.push_back({ cmd, arg });
        }
        GameBridge::QueueMainThreadWork(&RunJobs, nullptr);
    }

    void SetEnabled(const std::string& id, bool on) { QueueCommand(on ? "enable" : "disable", id); }
    void OpenSettings(const std::string& id) { QueueCommand("open", id); }
    void CloseSettings(const std::string& id) { QueueCommand("close", id); }
    void CloseAllWindows() { QueueCommand("closeAll", ""); }
    void Reload() { QueueCommand("reload", ""); QueueCommand("log", ""); }

    // 保存设置文件之后调用：让 MOD 立刻用上新值。
    // toggle=true 时还会把 MOD 关掉/打开一次（个别 MOD 只在 OnToggle 里应用设置）。
    void ApplySettings(const std::string& id, bool toggle)
    {
        if (id.empty()) return;
        QueueCommand("apply", toggle ? (id + "|1") : id);
    }

    bool TakeApplyResult(std::string& id, std::string& res)
    {
        std::lock_guard<std::mutex> lk(g_applyMx);
        if (!g_applyPending) return false;
        g_applyPending = false;
        id = g_applyId;
        res = g_applyRes;
        return true;
    }

    // 只在意变化：工具每帧都可能调用，避免把无意义的任务堆进队列
    void SetTheme(const std::string& hexRgb)
    {
        static std::string last;
        if (hexRgb.empty() || hexRgb == last) return;
        last = hexRgb;
        QueueCommand("theme", hexRgb);
    }
}
