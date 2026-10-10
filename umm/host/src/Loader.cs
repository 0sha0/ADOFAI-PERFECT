// ============================================================
//  Loader.cs — 我们自己实现的 UMM 兼容加载器（4/4）
//
//  职责：
//    · Doorstop 入口（winhttp 代理在进程启动时调用 Doorstop.Entrypoint.Start）
//    · 找到游戏程序集后，用 Harmony 钩住 ADOStartup.Startup：
//        前缀 = 解析并加载全部 MOD（早于游戏启动逻辑，MOD 才能打补丁）
//        后缀 = 建出 UI 宿主 GameObject，开始逐帧驱动 MOD
//    · 扫描 MOD 目录、按 Requirements / LoadAfter 拓扑排序、读取 Params.xml
//    · 给工具的 C++ 侧提供 Bridge（查询状态 / 开关 / 打开设置）
// ============================================================
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using HarmonyLib;
using UnityModManagerNet;
using TinyJson;
using UnityEngine;

namespace UnityModManagerNet
{
    public partial class UnityModManager
    {
        private static readonly string ManagerVersionString = "0.32.5.0";

        // ------------------------------------------------------------
        //  初始化：读配置 / 定位 MOD 目录 / 挂程序集解析
        // ------------------------------------------------------------
        public static bool Initialize()
        {
            if (initialized) return true;
            initialized = true;

            Logger.Clear();
            Logger.Log("Initialize.");
            version = ParseVersion(ManagerVersionString);
            Logger.Log("Version: " + version + ".");

            try
            {
                unityVersion = ParseVersion(Application.unityVersion);
                Logger.Log("Unity Engine: " + unityVersion + ".");
            }
            catch { }

            Config = LoadConfig();
            Logger.Log("Game: " + Config.Name + ".");

            string path = ResolveModsPath();
            SetModsPath(path);
            Logger.Log("Mods path: " + path + ".");

            Params = Param.Load();
            Logger.Log("Params: " + Params.ModParams.Count + " entries.");

            AppDomain.CurrentDomain.AssemblyResolve += ResolveAssembly;
            return true;
        }

        private static GameInfo LoadConfig()
        {
            GameInfo info = new GameInfo();
            try
            {
                string dir = Path.GetDirectoryName(typeof(UnityModManager).Assembly.Location);
                string file = Path.Combine(dir, "AdofPerfectUmm.json");
                if (File.Exists(file))
                {
                    GameInfo loaded = File.ReadAllText(file).FromJson<GameInfo>();
                    if (loaded != null) info = loaded;
                }
            }
            catch (Exception e) { Logger.LogException("LoadConfig", e); }
            return info;
        }

        private static string ResolveModsPath()
        {
            string env = Environment.GetEnvironmentVariable("ADOFAI_PERFECT_MODS");
            if (!string.IsNullOrEmpty(env) && Directory.Exists(env)) return env;
            if (!string.IsNullOrEmpty(Config.ModsPath) && Directory.Exists(Config.ModsPath)) return Config.ModsPath;

            // <游戏根>\Mods
            string root = FindGameRoot();
            string guess = Path.Combine(root, Config.ModsDirectory);
            if (Directory.Exists(guess)) return guess;

            // Environment.CurrentDirectory\Mods
            string cwd = Path.Combine(Environment.CurrentDirectory, Config.ModsDirectory);
            if (Directory.Exists(cwd)) return cwd;

            try { Directory.CreateDirectory(guess); } catch { }
            return guess;
        }

        // 真正参与扫描的目录列表：主目录（配置/环境变量）优先，
        // 再补上「<游戏根>\Mods」（兼容原生 UMM 布局，避免替换加载器后原有 MOD 失效）。
        internal static readonly List<string> modsPaths = new List<string>();

        internal static void RebuildModsPaths()
        {
            modsPaths.Clear();
            AddModsPath(modsPath);
            try { AddModsPath(Path.Combine(FindGameRoot(), Config.ModsDirectory)); } catch { }
            if (modsPaths.Count == 0 && !string.IsNullOrEmpty(modsPath)) modsPaths.Add(modsPath);
        }

        private static void AddModsPath(string path)
        {
            if (string.IsNullOrEmpty(path) || !Directory.Exists(path)) return;
            foreach (string existing in modsPaths)
                if (string.Equals(existing, path, StringComparison.OrdinalIgnoreCase)) return;
            modsPaths.Add(path);
        }

        private static string FindGameRoot()
        {
            string dir = Path.GetDirectoryName(typeof(UnityModManager).Assembly.Location);
            DirectoryInfo info = new DirectoryInfo(dir);
            for (int i = 0; i < 8 && info != null; i++)
            {
                if (File.Exists(Path.Combine(info.FullName, Config.GameExe))) return info.FullName;
                if (Directory.Exists(Path.Combine(info.FullName, info.Name + "_Data"))) return info.FullName;
                info = info.Parent;
            }
            return Environment.CurrentDirectory;
        }

        private static Assembly ResolveAssembly(object sender, ResolveEventArgs args)
        {
            string name;
            try { name = new AssemblyName(args.Name).Name; }
            catch { return null; }

            if (name == "UnityModManager") return typeof(UnityModManager).Assembly;
            // 加载器自己的目录里带着 UMM 运行时依赖（0Harmony / dnlib …），
            // 先按文件名通用地找一遍，再退回游戏 Managed 目录。
            try
            {
                string own = Path.GetDirectoryName(typeof(UnityModManager).Assembly.Location);
                string probe = Path.Combine(own, name + ".dll");
                if (File.Exists(probe)) return Assembly.LoadFrom(probe);
                probe = Path.Combine(Path.Combine(own, ".."), name + ".dll");
                if (File.Exists(probe)) return Assembly.LoadFrom(probe);
            }
            catch { }
            if (name == "0Harmony" || name.StartsWith("0Harmony"))
            {
                string dir = Path.GetDirectoryName(typeof(UnityModManager).Assembly.Location);
                foreach (string candidate in new string[] { "0Harmony.dll", "0Harmony-1.2.dll", "0Harmony12.dll" })
                {
                    string file = Path.Combine(dir, candidate);
                    if (File.Exists(file)) { try { return Assembly.LoadFrom(file); } catch { } }
                }
                string managed = Path.Combine(dir, "..");
                string harmony = Path.Combine(managed, "0Harmony.dll");
                if (File.Exists(harmony)) { try { return Assembly.LoadFrom(harmony); } catch { } }
            }

            // 从各 MOD 目录（及其 lib 子目录）里兜底解析依赖
            try
            {
                for (int i = 0; i < modEntries.Count; i++)
                {
                    string dir = modEntries[i].Path;
                    string[] probes = new string[]
                    {
                        Path.Combine(dir, name + ".dll"),
                        Path.Combine(dir, "lib", name + ".dll"),
                    };
                    foreach (string probe in probes)
                        if (File.Exists(probe)) { try { return Assembly.LoadFrom(probe); } catch { } }
                }
            }
            catch { }
            return null;
        }

        // ------------------------------------------------------------
        //  扫描 + 排序 + 加载
        // ------------------------------------------------------------
        private static bool dirWarned;

        public static void Start()
        {
            if (started) return;

            RebuildModsPaths();
            if (modsPaths.Count == 0)
            {
                if (!dirWarned)
                {
                    dirWarned = true;
                    Logger.Error("Directory '" + modsPath + "' not exists.");
                }
                return;   // 目录还没就绪：不置 started，等 EnsureStarted 下一次重试
            }

            started = true;
            Logger.Log("Starting.");
            try
            {
                Dictionary<string, ModEntry> found = new Dictionary<string, ModEntry>();
                int seen = 0;
                for (int d = 0; d < modsPaths.Count; d++)
                {
                string scanDir = modsPaths[d];
                Logger.Log("Scanning '" + scanDir + "'.");
                foreach (string folder in Directory.GetDirectories(scanDir))
                {
                    string infoFile = Path.Combine(folder, Config.ModInfo);
                    if (!File.Exists(infoFile)) infoFile = Path.Combine(folder, Config.ModInfo.ToLower());
                    if (!File.Exists(infoFile))
                    {
                        Logger.Log("Skip '" + Path.GetFileName(folder) + "' (no " + Config.ModInfo + ").");
                        continue;
                    }
                    seen++;
                    try
                    {
                        ModInfo info = File.ReadAllText(infoFile).FromJson<ModInfo>();
                        if (info == null || string.IsNullOrEmpty(info.Id)) { Logger.Error("Id is null in '" + infoFile + "'."); continue; }
                        if (found.ContainsKey(info.Id)) { Logger.Error("Id '" + info.Id + "' already used."); continue; }
                        if (string.IsNullOrEmpty(info.AssemblyName) && File.Exists(Path.Combine(folder, info.Id + ".dll")))
                            info.AssemblyName = info.Id + ".dll";
                        if (string.IsNullOrEmpty(info.DisplayName)) info.DisplayName = info.Id;
                        found[info.Id] = new ModEntry(info, folder + Path.DirectorySeparatorChar);
                    }
                    catch (Exception e)
                    {
                        Logger.Error("Error parsing '" + infoFile + "'.");
                        Logger.LogException(e);
                    }
                }
                }

                Logger.Log("Parsed " + seen + " folder(s), " + found.Count + " mod(s).");

                List<ModEntry> sorted = TopoSort(found);
                modEntries.Clear();
                modEntries.AddRange(sorted);
                Params.ReadModParams();

                Logger.Log("Loading mods.");
                for (int i = 0; i < modEntries.Count; i++)
                {
                    ModEntry entry = modEntries[i];
                    if (!entry.Enabled) { entry.Logger.Log("To skip (disabled)."); continue; }
                    try { entry.Active = true; }
                    catch (Exception e) { entry.Logger.LogException(e); }
                }

                int ok = 0;
                for (int i = 0; i < modEntries.Count; i++) if (!modEntries[i].ErrorOnLoading) ok++;
                Logger.Log(("Finish. Successful loaded " + ok + "/" + modEntries.Count + " mods.").ToUpper());

                // MOD 都加载完了：把工具侧「还没落地」的设置补应用一次
                SettingsSync.RunPending();
            }
            catch (Exception e)
            {
                Logger.Error("Start failed.");
                Logger.LogException(e);
            }
        }

        internal static string[] LoadAfterOf(ModEntry entry)
        {
            List<string> list = new List<string>();
            list.AddRange(entry.LoadAfter);
            foreach (KeyValuePair<string, Version> requirement in entry.Requirements)
                if (!list.Contains(requirement.Key)) list.Add(requirement.Key);
            return list.ToArray();
        }

        private static List<ModEntry> TopoSort(Dictionary<string, ModEntry> mods)
        {
            List<ModEntry> result = new List<ModEntry>();
            List<ModEntry> pending = mods.Values.OrderBy(x => x.Info.Id, StringComparer.OrdinalIgnoreCase).ToList();
            int guard = 0;
            while (pending.Count > 0 && guard++ < 10000)
            {
                bool progress = false;
                for (int i = 0; i < pending.Count; i++)
                {
                    bool ready = true;
                    foreach (string dependency in LoadAfterOf(pending[i]))
                    {
                        if (dependency == pending[i].Info.Id) continue;
                        if (!mods.ContainsKey(dependency)) continue;
                        if (!result.Contains(mods[dependency])) { ready = false; break; }
                    }
                    if (!ready) continue;
                    result.Add(pending[i]);
                    pending.RemoveAt(i);
                    progress = true;
                    break;
                }
                if (!progress)
                {
                    Logger.Error("Circular dependency detected; appending remainder.");
                    result.AddRange(pending);
                    break;
                }
            }
            return result;
        }

        // ------------------------------------------------------------
        //  UI 宿主 GameObject
        // ------------------------------------------------------------
        internal static bool uiPending;

        public static bool RunUI()
        {
            if (UI.Instance != null) return true;
            try
            {
                GameObject go = new GameObject("AdofPerfectUMM");
                go.hideFlags = HideFlags.HideAndDontSave;
                UnityEngine.Object.DontDestroyOnLoad(go);
                go.AddComponent<UI>();
                Logger.Log("UI spawned.");
                uiPending = false;
                return true;
            }
            catch (Exception e)
            {
                uiPending = true;
                Logger.Log("UI spawn deferred: " + e.Message);
                return false;
            }
        }
    }
}

namespace Doorstop
{
    public class Entrypoint
    {
        public static void Start()
        {
            AdofPerfectUmm.Loader.Boot();
        }
    }
}

namespace AdofPerfectUmm
{
    // ============================================================
    //  Loader —— 引导流程
    // ============================================================
    public static class Loader
    {
        private static readonly object gate = new object();
        private static bool booted;
        private static bool unityReady;
        private static bool pendingLogged;

        // 触发启动的游戏程序集（与原版 UMM 的判断一致）。
        private static readonly string[] TriggerAssemblies = { "Assembly-CSharp", "GH.Runtime", "AtomGame", "Game" };

        public static void Boot()
        {
            lock (gate)
            {
                if (booted) return;
                booted = true;
            }
            try
            {
                // ★ 门卫（doorstop）阶段绝对不要调用任何 Unity API。
                //   这时 Unity 里带 [ThreadSafe] 的内部调用还不能解析：
                //     · Debug.Log              → DebugLogHandler.Internal_Log_Injected
                //     · 任何返回 string 的 API → OutStringMarshaller + BindingsAllocator.Free
                //   一旦在这里调用失败，Mono 会把失败的包装器永久缓存，之后整个进程
                //   的 Scene.name / SystemInfo / Debug.Log 都会一直抛 MissingMethodException，
                //   表现就是 MOD「显示加载成功」但一跑起来就集体报错。
                //   所以这里只订阅纯 BCL 的 AssemblyLoad，等游戏程序集加载后再碰 Unity。
                AppDomain.CurrentDomain.AssemblyLoad += OnAssemblyLoad;
                UnityModManager.Logger.Log("Doorstop entry: waiting for the game assembly.");
            }
            catch (Exception e)
            {
                UnityModManager.Logger.Error("Boot failed.");
                UnityModManager.Logger.LogException(e);
            }
        }

        internal static Assembly FindAssembly(string name)
        {
            if (string.IsNullOrEmpty(name)) return null;
            foreach (Assembly asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                try { if (asm.GetName().Name == name) return asm; } catch { }
            }
            return null;
        }

        private static void OnAssemblyLoad(object sender, AssemblyLoadEventArgs args)
        {
            if (args == null || args.LoadedAssembly == null) return;
            string name;
            try { name = args.LoadedAssembly.GetName().Name; }
            catch { return; }
            if (!IsTriggerAssembly(name)) return;
            MarkUnityReady("Game assembly '" + name + "' loaded; mods will start on the next frame.");
        }

        private static bool IsTriggerAssembly(string name)
        {
            if (string.IsNullOrEmpty(name)) return false;
            for (int i = 0; i < TriggerAssemblies.Length; i++)
                if (string.Equals(TriggerAssemblies[i], name, StringComparison.Ordinal)) return true;
            return false;
        }

        private static Assembly FindTriggerAssembly()
        {
            for (int i = 0; i < TriggerAssemblies.Length; i++)
            {
                Assembly asm = FindAssembly(TriggerAssemblies[i]);
                if (asm != null) return asm;
            }
            return null;
        }

        internal static Assembly GameAssembly;

        // 游戏程序集就绪 —— 到这一刻 Unity 的脚本运行时（含 [ThreadSafe] 内部调用）
        // 才真正可用，从这里开始才可以放心调用 Unity API / 启动 MOD。
        private static void MarkUnityReady(string reason)
        {
            Unhook();
            unityReady = true;
            if (!pendingLogged)
            {
                pendingLogged = true;
                UnityModManager.Logger.Log(reason);
            }
            // ★ 千万不要在这里直接加载 MOD：AssemblyLoad 事件是在 Mono 的程序集
            //   加载流程里回调的，此时再调 Assembly.LoadFrom 会把 Unity 原生层弄崩
            //   （已实测：加载到 AdofaiTweaks 时 Crash!!!）。只装帧回调，
            //   下一帧 Application.onBeforeRender（正常主线程上下文）里再启动。
            try { TryHookFrame(); }
            catch (Exception e) { UnityModManager.Logger.LogException("TryHookFrame", e); }
        }

        private static void Unhook()
        {
            try { AppDomain.CurrentDomain.AssemblyLoad -= OnAssemblyLoad; } catch { }
        }

        // ---- 逐帧驱动 -----------------------------------------------------
        // 为什么不用 Harmony 钩 ADOStartup.Startup：
        //   门卫（doorstop）阶段程序集太早被加载，此时 Harmony 的共享状态
        //   （HarmonySharedState，靠 Cecil 动态建模块 + 反射写静态字段）还没法
        //   建立，lock(state) 直接 ArgumentNullException —— 表现为“钩子失败、
        //   MOD 一个都不加载”。改挂 UnityEngine.Application.onBeforeRender：
        //   引擎就绪后每帧回调一次（主线程），在这里启动 MOD / 建 UI 最稳。
        private static bool frameHooked;
        private static bool frameHookLogged;

        internal static void TryHookFrame()
        {
            if (frameHooked) return;
            try
            {
                Type app = FindAssembly("UnityEngine.CoreModule") != null
                               ? FindAssembly("UnityEngine.CoreModule").GetType("UnityEngine.Application")
                               : null;
                if (app == null) return;
                MethodInfo cb = typeof(Loader).GetMethod("OnBeforeRender", BindingFlags.Static | BindingFlags.NonPublic);
                if (cb == null) return;
                EventInfo ev = app.GetEvent("onBeforeRender", BindingFlags.Public | BindingFlags.Static);
                if (ev != null)
                {
                    ev.AddEventHandler(null, Delegate.CreateDelegate(ev.EventHandlerType, cb));
                }
                else
                {
                    // 某些 Unity 版本里它是 delegate 字段而不是 event
                    FieldInfo fi = app.GetField("onBeforeRender", BindingFlags.Public | BindingFlags.Static);
                    if (fi == null) return;
                    Delegate d = Delegate.CreateDelegate(fi.FieldType, cb);
                    fi.SetValue(null, Delegate.Combine(fi.GetValue(null) as Delegate, d));
                }
                frameHooked = true;
                Unhook();
                UnityModManager.Logger.Log("Frame hook installed (Application.onBeforeRender).");
            }
            catch (Exception e)
            {
                if (!frameHookLogged)
                {
                    frameHookLogged = true;
                    UnityModManager.Logger.Log("Frame hook not ready: " + e.Message);
                }
            }
        }

        private static void OnBeforeRender()
        {
            try { EnsureStarted(); } catch { }
        }

        internal static void TryStart()
        {
            try { UnityModManager.Start(); }
            catch (Exception e) { UnityModManager.Logger.LogException("Start", e); }
        }

        internal static void TryRunUI()
        {
            try { UnityModManager.RunUI(); }
            catch (Exception e) { UnityModManager.Logger.LogException("RunUI", e); }
        }

        // ---- 给 C++ 工具用的幂等入口（若 doorstop 未安装则由此兜底） ----
        public static void EnsureStarted()
        {
            Boot();
            if (!unityReady)
            {
                // 兜底：doorstop 挂得太晚时 AssemblyLoad 事件可能已经错过，
                // 用纯 BCL 反射在已加载程序集里找一遍。走到这里说明调用方
                // （帧回调 / 工具主线程）本身就是安全上下文，可以立即启动。
                if (FindTriggerAssembly() != null) MarkUnityReady("Game assembly already loaded; starting mods.");
                if (!unityReady) return;   // Unity 未就绪前绝不碰 Unity API
            }
            UnityModManager.Logger.UnityLogEnabled = true;
            if (!UnityModManager.initialized)
            {
                try { UnityModManager.Initialize(); }
                catch (Exception e) { UnityModManager.Logger.LogException("Initialize", e); }
            }
            TryHookFrame();
            if (!UnityModManager.started) TryStart();
            if (UnityModManager.UI.Instance == null) TryRunUI();
        }
    }

    // ============================================================
    //  Bridge —— C++ 侧通过 Mono 调用本类来查询/操作加载器
    // ============================================================
    public static class Bridge
    {
        public const string Protocol = "adofperfect-umm/1";

        public static string Ping()
        {
            try { Loader.EnsureStarted(); } catch { }
            return Protocol + " " + (UnityModManager.GetVersion() != null ? UnityModManager.GetVersion().ToString() : "?");
        }

        public static void EnsureStarted()
        {
            try { Loader.EnsureStarted(); } catch (Exception e) { UnityModManager.Logger.LogException("EnsureStarted", e); }
        }

        private class ModState
        {
            public string id;
            public string name;
            public string author;
            public string version;
            public bool enabled;
            public bool active;
            public bool loaded;
            public bool error;
            public bool gui;
            public bool open;
            public string assembly;
        }

        public static string State()
        {
            try
            {
                List<ModState> list = new List<ModState>();
                for (int i = 0; i < UnityModManager.modEntries.Count; i++)
                {
                    var entry = UnityModManager.modEntries[i];
                    ModState state = new ModState();
                    state.id = entry.Info.Id;
                    state.name = string.IsNullOrEmpty(entry.Info.DisplayName) ? entry.Info.Id : entry.Info.DisplayName;
                    state.author = entry.Info.Author;
                    state.version = entry.Info.Version;
                    state.enabled = entry.Enabled;
                    state.active = entry.Active;
                    state.loaded = entry.Loaded;
                    state.error = entry.ErrorOnLoading;
                    state.gui = entry.OnGUI != null;
                    state.open = UnityModManager.UI.IsOpen(entry);
                    state.assembly = entry.Info.AssemblyName;
                    list.Add(state);
                }
                return list.ToJson();
            }
            catch (Exception e)
            {
                UnityModManager.Logger.LogException("State", e);
                return "[]";
            }
        }

        public static string ModsPath()
        {
            return UnityModManager.modsPath ?? string.Empty;
        }

        // 工具侧主入口：把当前 MOD 目录交给加载器，并保证它在主线程上跑起来
        public static string StartWith(string modsDir)
        {
            try
            {
                Loader.Boot();
                if (!string.IsNullOrEmpty(modsDir) && Directory.Exists(modsDir)) UnityModManager.SetModsPath(modsDir);
                Loader.EnsureStarted();
                return UnityModManager.started ? "ok" : "err:start-failed";
            }
            catch (Exception e)
            {
                UnityModManager.Logger.LogException("Bridge.StartWith", e);
                return "err:" + e.Message;
            }
        }

        public static string Log(int maxLines)
        {
            try
            {
                string[] lines = UnityModManager.Logger.History;
                int from = lines.Length > maxLines ? lines.Length - maxLines : 0;
                StringBuilder sb = new StringBuilder();
                for (int i = from; i < lines.Length; i++) sb.AppendLine(lines[i]);
                return sb.ToString();
            }
            catch { return string.Empty; }
        }

        public static string Invoke(string command, string argument)
        {
            try
            {
                if (string.IsNullOrEmpty(command)) return "err:no-command";
                switch (command)
                {
                    case "start": return StartWith(argument);
                    case "state": return State();
                    case "log": return Log(80);
                    case "modsPath": return ModsPath();
                    case "reload": return Reload();
                    case "enable": return SetEnabled(argument, true);
                    case "disable": return SetEnabled(argument, false);
                    case "open": return Open(argument);
                    case "close": return Close(argument);
                    case "closeAll": UnityModManager.UI.CloseAll(); return "ok";
                    case "theme":
                    {
                        // 工具侧把当前皮肤强调色推过来：让游戏内的 MOD 窗口同色
                        float r, g, b;
                        if (TryParseHexColor(argument, out r, out g, out b))
                            UnityModManager.UI.SetAccent(r, g, b);
                        return "ok";
                    }
                    case "hasGui":
                    {
                        var entry = UnityModManager.FindMod(argument);
                        return entry != null && entry.OnGUI != null ? "1" : "0";
                    }
                    case "apply":
                    {
                        // 工具侧保存设置文件后调用：把新值热应用进正在运行的 MOD。
                        // 参数：<ModId> 或 <ModId>|1（1 = 再关掉/打开一次 MOD，
                        // 让只在 OnToggle 里应用设置的 MOD 也生效）。
                        string mid = argument ?? string.Empty;
                        bool toggle = false;
                        int bar = mid.IndexOf('|');
                        if (bar >= 0)
                        {
                            toggle = mid.Substring(bar + 1).Trim() == "1";
                            mid = mid.Substring(0, bar);
                        }
                        return UnityModManager.SettingsSync.Apply(mid, toggle);
                    }
                    default: return "err:unknown-command";
                }
            }
            catch (Exception e)
            {
                UnityModManager.Logger.LogException("Bridge.Invoke(" + command + ")", e);
                return "err:" + e.Message;
            }
        }

        // "#RRGGBB" / "RRGGBB" → 0..1 的三通道
        private static bool TryParseHexColor(string s, out float r, out float g, out float b)
        {
            r = g = b = 1f;
            if (string.IsNullOrEmpty(s)) return false;
            s = s.Trim();
            if (s[0] == '#') s = s.Substring(1);
            if (s.Length != 6) return false;
            int v;
            if (!int.TryParse(s, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out v)) return false;
            r = ((v >> 16) & 0xFF) / 255f;
            g = ((v >> 8) & 0xFF) / 255f;
            b = (v & 0xFF) / 255f;
            return true;
        }

        private static string Reload()
        {
            Loader.EnsureStarted();
            return "ok";
        }

        private static string SetEnabled(string id, bool value)
        {
            var entry = UnityModManager.FindMod(id);
            if (entry == null) return "err:not-found";
            entry.Enabled = value;
            if (value)
            {
                entry.Active = true;
                if (!entry.Active) return "err:toggle-failed";
            }
            else
            {
                if (UnityModManager.UI.IsOpen(entry)) UnityModManager.UI.Close(entry);
                entry.Active = false;
            }
            SaveParams();
            return "ok";
        }

        private static string Open(string id)
        {
            var entry = UnityModManager.FindMod(id);
            if (entry == null) return "err:not-found";
            if (entry.OnGUI == null) return "err:no-gui";
            UnityModManager.UI.Open(entry);
            return "ok";
        }

        private static string Close(string id)
        {
            var entry = UnityModManager.FindMod(id);
            if (entry == null) return "err:not-found";
            UnityModManager.UI.Close(entry);
            return "ok";
        }

        // 把当前启用状态写回 Params.xml
        public static string SaveParams()
        {
            try
            {
                var param = new UnityModManager.Param();
                for (int i = 0; i < UnityModManager.modEntries.Count; i++)
                {
                    var entry = UnityModManager.modEntries[i];
                    param.ModParams.Add(new UnityModManager.Param.Mod { Id = entry.Info.Id, Enabled = entry.Enabled });
                }
                param.Save();
                return "ok";
            }
            catch (Exception e)
            {
                UnityModManager.Logger.LogException("SaveParams", e);
                return "err:" + e.Message;
            }
        }
    }
}








