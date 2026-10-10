// ============================================================
//  Foreign.cs — 反射适配官方 UnityModManager（Sidecar 版）
//
//  与 umm/host/src/Foreign.cs 同源，但这里没有我们自己的
//  UnityModManager 实现：所有对官方 UMM 的访问都走反射，
//  日志走 SideLogger。Sidecar 永远是「复用模式」—— 它存在的
//  唯一目的就是让工具的 MOD 管理器页能驱动官方内核加载的 MOD。
// ============================================================
using System;
using System.Collections;
using System.IO;
using System.Reflection;

namespace AdofPerfectUmm
{
    internal static class Foreign
    {
        private static Assembly asm;
        private static readonly object gate = new object();

        private static PropertyInfo p_modEntries, p_modsPath, p_Params;
        private static FieldInfo f_modEntries, f_OnGUI;
        private static bool resolved;

        public static Assembly Detect()
        {
            if (asm != null) return asm;
            lock (gate)
            {
                if (asm != null) return asm;
                try
                {
                    Assembly[] all = AppDomain.CurrentDomain.GetAssemblies();
                    for (int i = 0; i < all.Length; i++)
                    {
                        Assembly a = all[i];
                        if (a == null) continue;
                        string name;
                        try { name = a.GetName().Name; } catch { continue; }
                        if (!string.Equals(name, "UnityModManager", StringComparison.OrdinalIgnoreCase)) continue;
                        // Sidecar 自己不叫这个名字；任何叫 UnityModManager 的程序集
                        // 都是官方内核（doorstop / 旧自研加载器均可能）
                        asm = a;
                        resolved = false;
                        SideLogger.Log("Bridged to UnityModManager '" + SafeLoc(a) + "'.");
                        return asm;
                    }
                }
                catch (Exception e) { SideLogger.LogException("Detect", e); }
            }
            return null;
        }

        public static bool Active { get { return Detect() != null; } }

        private static string SafeLoc(Assembly a)
        {
            try { return a != null ? a.Location : null; } catch { return null; }
        }

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

        public static IList Entries()
        {
            Resolve();
            if (asm == null) return null;
            try
            {
                if (p_modEntries != null) return p_modEntries.GetValue(null, null) as IList;
                if (f_modEntries != null) return f_modEntries.GetValue(null) as IList;
            }
            catch (Exception e) { SideLogger.LogException("Entries", e); }
            return null;
        }

        public static object Entry(string id)
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
        public static bool ShowSettingsOf(object entry)
        {
            return entry != null && Equals(Member(entry, "ShowModSettings", "ShowModSettings"), true);
        }

        public static bool SetEnabled(string id, bool on)
        {
            object e = Entry(id);
            if (e == null) return false;
            SetMember(e, "Enabled", "Enabled", on);
            SetMember(e, "Active", "Active", on);
            SaveTheirParams();
            return true;
        }

        public static bool SetShowSettings(string id, bool on)
        {
            object e = Entry(id);
            if (e == null) return false;
            return SetMember(e, "ShowModSettings", "ShowModSettings", on);
        }

        public static string ModsPath()
        {
            Resolve();
            try { return p_modsPath != null ? (p_modsPath.GetValue(null, null) as string) : null; }
            catch { return null; }
        }

        public static string Dir()
        {
            string loc = SafeLoc(asm);
            return string.IsNullOrEmpty(loc) ? null : Path.GetDirectoryName(loc);
        }

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
