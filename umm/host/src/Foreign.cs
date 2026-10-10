// ============================================================
//  Foreign.cs — 「原生 UnityModManager 共存适配器」
//
//  用户机器上可能已经装着原版 UMM（doorstop 启动时加载
//  <游戏>_Data\Managed\UnityModManager\UnityModManager.dll）。
//  过去我们不做检测，自己再扫一遍 Mods 目录把同样的 MOD 再加载
//  一次 —— 同一批静态单例被初始化两遍、Harmony 补丁打两遍，
//  两套 UMM 的 GUI 同时出现，设置（如 Overlayer 的 Lang=zh-CN）
//  被后加载的一方覆盖回默认值。
//
//  现在的策略：一旦在 AppDomain 里发现「不是我们这份」的
//  UnityModManager 程序集，加载器立即进入 passive（复用）模式：
//    · 不再扫描 / 加载任何 MOD，不再生成自己的 GUI；
//    · Bridge 的所有查询 / 操作通过反射转发给原版 UMM
//      （modEntries / Enabled / Active / ShowModSettings / Params），
//      工具侧的「MOD 管理器」页照常可用，但背后驱动的原版生态。
// ============================================================
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;

namespace AdofPerfectUmm
{
    internal static class Foreign
    {
        private static Assembly asm;
        private static object gate = new object();

        // ---- 反射成员缓存（原版 UMM 的类型在我们的编译期不存在）----
        private static PropertyInfo p_modEntries, p_modsPath, p_Params;
        private static FieldInfo f_modEntries, f_OnGUI;
        private static bool resolved;

        /// <summary>探测「别人的」UnityModManager 程序集（每次调用都重试，直到找到为止）。</summary>
        public static Assembly Detect()
        {
            if (asm != null) return asm;
            lock (gate)
            {
                if (asm != null) return asm;
                try
                {
                    Assembly own = typeof(Foreign).Assembly;
                    string ownLoc = SafeLoc(own);
                    Assembly[] all = AppDomain.CurrentDomain.GetAssemblies();
                    for (int i = 0; i < all.Length; i++)
                    {
                        Assembly a = all[i];
                        if (a == null || a == own) continue;
                        string name;
                        try { name = a.GetName().Name; } catch { continue; }
                        if (!string.Equals(name, "UnityModManager", StringComparison.OrdinalIgnoreCase)) continue;
                        string loc = SafeLoc(a);
                        // 同一路径（我们被 C++ 侧再次 open）不算外来；其余同名程序集都视为原版 UMM
                        if (!string.IsNullOrEmpty(loc) && !string.IsNullOrEmpty(ownLoc) &&
                            string.Equals(loc, ownLoc, StringComparison.OrdinalIgnoreCase)) continue;
                        asm = a;
                        resolved = false;
                        UnityModManagerNet.UnityModManager.Logger.Log(
                            "Detected a foreign UnityModManager ('" + loc + "'). Loader stays passive; mods are managed by it.");
                        return asm;
                    }
                }
                catch (Exception e)
                {
                    try { UnityModManagerNet.UnityModManager.Logger.LogException("Foreign/Detect", e); } catch { }
                }
            }
            return null;
        }

        public static bool Active { get { return Detect() != null; } }

        private static string SafeLoc(Assembly a)
        {
            try { return a != null ? a.Location : null; } catch { return null; }
        }

        // ---- 成员解析 ------------------------------------------------------------
        private static void Resolve()
        {
            if (resolved || asm == null) return;
            try
            {
                Type t = asm.GetType("UnityModManagerNet.UnityModManager", false);
                if (t == null) return;
                p_modEntries = t.GetProperty("modEntries", BindingFlags.Public | BindingFlags.Static);
                p_modsPath   = t.GetProperty("modsPath",   BindingFlags.Public | BindingFlags.Static);
                p_Params     = t.GetProperty("Params",     BindingFlags.Public | BindingFlags.Static);
                f_modEntries = t.GetField("modEntries",    BindingFlags.Public | BindingFlags.Static);
                Type entry = asm.GetType("UnityModManagerNet.UnityModManager+ModEntry", false)
                             ?? asm.GetType("UnityModManagerNet.ModEntry", false);
                if (entry != null) f_OnGUI = entry.GetField("OnGUI", BindingFlags.Public | BindingFlags.Instance);
                resolved = p_modEntries != null || f_modEntries != null;
            }
            catch { resolved = false; }
        }

        /// <summary>原版 UMM 的 modEntries（IList）。未就绪返回 null。</summary>
        public static IList Entries()
        {
            Resolve();
            if (asm == null) return null;
            try
            {
                if (p_modEntries != null) return p_modEntries.GetValue(null, null) as IList;
                if (f_modEntries != null) return f_modEntries.GetValue(null) as IList;
            }
            catch (Exception e) { UnityModManagerNet.UnityModManager.Logger.LogException("Foreign/Entries", e); }
            return null;
        }

        /// <summary>按 Id 找原版 UMM 的 ModEntry（passive 模式的操作目标）。</summary>
        public static object Entry(string id)
        {
            return FindEntry(id);
        }

        private static object FindEntry(string id)
        {
            IList list = Entries();
            if (list == null || string.IsNullOrEmpty(id)) return null;
            foreach (object e in list)
            {
                if (e == null) continue;
                if (string.Equals(IdOf(e), id, StringComparison.Ordinal)) return e;
            }
            return null;
        }

        // ---- ModEntry 成员（按需反射，不缓存失败）---------------------------------
        private static object Member(object entry, string prop, string field)
        {
            if (entry == null) return null;
            try
            {
                PropertyInfo p = entry.GetType().GetProperty(prop, BindingFlags.Public | BindingFlags.Instance);
                if (p != null) return p.GetValue(entry, null);
                FieldInfo f = entry.GetType().GetField(field, BindingFlags.Public | BindingFlags.Instance);
                if (f != null) return f.GetValue(entry);
            }
            catch { }
            return null;
        }

        private static bool SetMember(object entry, string prop, string field, object value)
        {
            if (entry == null) return false;
            try
            {
                PropertyInfo p = entry.GetType().GetProperty(prop, BindingFlags.Public | BindingFlags.Instance);
                if (p != null && p.CanWrite) { p.SetValue(entry, value, null); return true; }
                FieldInfo f = entry.GetType().GetField(field, BindingFlags.Public | BindingFlags.Instance);
                if (f != null) { f.SetValue(entry, value); return true; }
            }
            catch { }
            return false;
        }

        public static string IdOf(object entry)
        {
            object info = Member(entry, "Info", "Info");
            string id = info != null ? Member(info, "Id", "Id") as string : null;
            if (!string.IsNullOrEmpty(id)) return id;
            return Member(entry, "Id", "Id") as string;
        }
        public static string NameOf(object entry)
        {
            object info = Member(entry, "Info", "Info");
            string dn = info != null ? Member(info, "DisplayName", "DisplayName") as string : null;
            return string.IsNullOrEmpty(dn) ? IdOf(entry) : dn;
        }
        public static string AuthorOf(object entry)    { object i = Member(entry, "Info", "Info"); return i != null ? (Member(i, "Author", "Author") as string ?? "") : ""; }
        public static string VersionOf(object entry)   { object i = Member(entry, "Info", "Info"); return i != null ? (Member(i, "Version", "Version") as string ?? "") : ""; }
        public static string AssemblyNameOf(object entry)
        {
            object i = Member(entry, "Info", "Info");
            return i != null ? (Member(i, "AssemblyName", "AssemblyName") as string ?? "") : "";
        }
        public static string PathOf(object entry)      { return Member(entry, "Path", "Path") as string; }
        public static bool   EnabledOf(object entry)   { return Equals(Member(entry, "Enabled", "Enabled"), true); }
        public static bool   ActiveOf(object entry)    { return Equals(Member(entry, "Active", "Active"), true); }
        public static bool   LoadedOf(object entry)    { return Equals(Member(entry, "Loaded", "Loaded"), true); }
        public static bool   ErrorOf(object entry)     { return Equals(Member(entry, "ErrorOnLoading", "ErrorOnLoading"), true); }
        public static bool   HasGui(object entry)
        {
            try { return f_OnGUI != null && f_OnGUI.GetValue(entry) != null; }
            catch { return false; }
        }

        // ---- 操作 ----------------------------------------------------------------
        public static bool SetEnabled(string id, bool on)
        {
            object e = FindEntry(id);
            if (e == null) return false;
            SetMember(e, "Enabled", "Enabled", on);
            SetMember(e, "Active", "Active", on);     // 原版 Active setter 会自行 Load/OnToggle
            SaveTheirParams();
            return true;
        }

        /// <summary>原版 GUI 的「显示该 MOD 设置页」开关（0.32 的 ShowModSettings 属性）。</summary>
        public static bool SetShowSettings(string id, bool on)
        {
            object e = FindEntry(id);
            if (e == null) return false;
            if (!SetMember(e, "ShowModSettings", "ShowModSettings", on)) return false;
            return true;
        }

        public static bool IsShowSettings(string id)
        {
            object e = FindEntry(id);
            return e != null && Equals(Member(e, "ShowModSettings", "ShowModSettings"), true);
        }

        public static bool ShowSettingsOf(object entry)
        {
            return entry != null && Equals(Member(entry, "ShowModSettings", "ShowModSettings"), true);
        }

        public static string ModsPath()
        {
            Resolve();
            try { return p_modsPath != null ? (p_modsPath.GetValue(null, null) as string) : null; }
            catch { return null; }
        }

        /// <summary>原版 UMM 的安装目录（它的 UnityModManager.dll 所在目录）。</summary>
        public static string Dir()
        {
            string loc = SafeLoc(asm);
            return string.IsNullOrEmpty(loc) ? null : Path.GetDirectoryName(loc);
        }

        /// <summary>把启用状态写回原版 UMM 的 Params.xml（Best effort）。</summary>
        public static void SaveTheirParams()
        {
            Resolve();
            try
            {
                object param = p_Params != null ? p_Params.GetValue(null, null) : null;
                if (param == null) return;
                MethodInfo save = param.GetType().GetMethod("Save", BindingFlags.Public | BindingFlags.Instance, null, Type.EmptyTypes, null);
                if (save != null) save.Invoke(param, null);
            }
            catch { }
        }

        /// <summary>原版加载器日志：优先内存 History，兜底读它的 Log.txt 尾部。</summary>
        public static string LogTail(int maxLines)
        {
            try
            {
                Resolve();
                Type t = asm != null ? asm.GetType("UnityModManagerNet.UnityModManager+Logger", false)
                                     ?? asm.GetType("UnityModManagerNet.Logger", false) : null;
                if (t != null)
                {
                    PropertyInfo h = t.GetProperty("History", BindingFlags.Public | BindingFlags.Static);
                    if (h != null)
                    {
                        string[] lines = h.GetValue(null, null) as string[];
                        if (lines != null && lines.Length > 0)
                        {
                            int from = lines.Length > maxLines ? lines.Length - maxLines : 0;
                            var sb = new System.Text.StringBuilder();
                            for (int i = from; i < lines.Length; i++) sb.AppendLine(lines[i]);
                            return sb.ToString();
                        }
                    }
                }
                string dir = Dir();
                if (!string.IsNullOrEmpty(dir))
                {
                    string file = Path.Combine(dir, "Log.txt");
                    if (File.Exists(file))
                    {
                        string[] all = File.ReadAllLines(file);
                        int from = all.Length > maxLines ? all.Length - maxLines : 0;
                        var sb2 = new System.Text.StringBuilder();
                        for (int i = from; i < all.Length; i++) sb2.AppendLine(all[i]);
                        return sb2.ToString();
                    }
                }
            }
            catch { }
            return string.Empty;
        }
    }
}
