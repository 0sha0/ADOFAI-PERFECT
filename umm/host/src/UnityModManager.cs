// ============================================================
//  UnityModManager.cs — 我们自己实现的 UMM 兼容加载器（1/4）
//
//  游戏里的 MOD 在编译期都引用了名为 UnityModManager 的程序集：
//  UnityModManagerNet.UnityModManager 及其嵌套类型 ModEntry / ModInfo /
//  ModSettings / Logger / Param / UI。下面按原版 UMM 的语义把这些成员
//  补齐（成员清单来自对 MOD 元数据引用表的逆向），使任何 UMM 规范的
//  MOD 都能绑定到本加载器；而“加载/排序/驱动/界面”全部由我们实现。
// ============================================================
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text.RegularExpressions;
using System.Xml.Serialization;
using UnityEngine;

namespace UnityModManagerNet
{
    public partial class UnityModManager
    {
        // ------------------------------------------------------------
        //  ModInfo —— Info.json 的映射
        // ------------------------------------------------------------
        public class ModInfo : IEquatable<ModInfo>
        {
            public string Id;
            public string DisplayName;
            public string Author;
            public string Version;
            public string ManagerVersion;
            public string GameVersion;
            public string[] Requirements;
            public string[] LoadAfter;
            public string AssemblyName;
            public string EntryMethod;
            public string HomePage;
            public string Repository;
            public string ContentType;
            [NonSerialized] public bool IsCheat = true;

            public static implicit operator bool(ModInfo exists) { return exists != null; }

            public bool Equals(ModInfo other) { return other != null && Id == other.Id; }
            public override bool Equals(object obj) { return Equals(obj as ModInfo); }
            public override int GetHashCode() { return Id != null ? Id.GetHashCode() : 0; }
        }

        public delegate void ToggleModsListen(ModEntry modEntry, bool result);

        // ------------------------------------------------------------
        //  Param —— Params.xml（启用状态）
        // ------------------------------------------------------------
        [XmlRoot("Param")]
        public sealed class Param
        {
            [Serializable]
            public class Mod
            {
                [XmlAttribute] public string Id;
                [XmlAttribute] public bool Enabled = true;
            }

            public float UIScale = 1f;

            public List<Mod> ModParams = new List<Mod>();

            private static readonly string filepath = Path.Combine(
                Path.GetDirectoryName(typeof(UnityModManager).Assembly.Location), "Params.xml");

            public static string FilePath { get { return filepath; } }

            public void Save()
            {
                try
                {
                    using (StreamWriter writer = new StreamWriter(filepath))
                        new XmlSerializer(typeof(Param)).Serialize(writer, this);
                }
                catch (Exception e)
                {
                    Logger.Error("Can't write file '" + filepath + "'.");
                    Logger.LogException(e);
                }
            }

            public static Param Load()
            {
                if (File.Exists(filepath))
                {
                    try
                    {
                        using (FileStream stream = File.OpenRead(filepath))
                            return new XmlSerializer(typeof(Param)).Deserialize(stream) as Param;
                    }
                    catch (Exception e)
                    {
                        Logger.Error("Can't read file '" + filepath + "'.");
                        Logger.LogException(e);
                    }
                }
                return new Param();
            }

            internal void ReadModParams()
            {
                foreach (Mod mod in ModParams)
                {
                    ModEntry entry = FindMod(mod.Id);
                    if (entry != null) entry.Enabled = mod.Enabled;
                }
            }
        }

        // ------------------------------------------------------------
        //  GameInfo —— 加载器配置（本实现用 AdofPerfectUmm.json）
        // ------------------------------------------------------------
        public class GameInfo
        {
            public string Name = "A Dance of Fire and Ice";
            public string Folder = "ADOFAI";
            public string ModsDirectory = "Mods";
            public string ModInfo = "Info.json";
            public string GameExe = "A Dance of Fire and Ice.exe";
            public string EntryPoint = "[UnityEngine.CoreModule.dll]UnityEngine.MonoBehaviour.cctor:Before";
            public string StartingPoint = "[Assembly-CSharp.dll]ADOStartup.Startup:Before";
            public string UIStartingPoint = "[Assembly-CSharp.dll]ADOStartup.Startup:After";
            public string MinimalManagerVersion = "0.22.14";
            public string ModsPath = string.Empty;
        }

        // ------------------------------------------------------------
        //  静态状态
        // ------------------------------------------------------------
        public static readonly List<ModEntry> modEntries = new List<ModEntry>();
        public static string modsPath { get; private set; }
        public static Version version { get; private set; }
        public static Version gameVersion { get; private set; }
        public static Version unityVersion { get; private set; }
        public static GameInfo Config { get; set; }
        public static Param Params { get; set; }

        internal static readonly Version VER_0 = new Version();
        internal static bool initialized;
        internal static bool started;
        internal static bool forbidDisableMods;

        public static event ToggleModsListen toggleModsListen;

        public static Version GetVersion() { return version; }
        internal static void SetVersion(Version v) { version = v; }
        internal static void SetModsPath(string path) { modsPath = path; RebuildModsPaths(); }
        internal static void SetGameVersion(Version v) { gameVersion = v; }

        public static Version ParseVersion(string str)
        {
            if (string.IsNullOrEmpty(str)) return new Version();
            string[] parts = str.Split('.');
            List<int> numbers = new List<int>();
            foreach (string part in parts)
            {
                Match match = Regex.Match(part, "\\d+");
                if (!match.Success) break;
                int n;
                if (!int.TryParse(match.Value, out n)) break;
                numbers.Add(n);
            }
            while (numbers.Count < 2) numbers.Add(0);
            if (numbers.Count > 4) numbers.RemoveRange(4, numbers.Count - 4);
            return new Version(numbers[0], numbers[1], numbers.Count > 2 ? numbers[2] : 0, numbers.Count > 3 ? numbers[3] : 0);
        }

        public static ModEntry FindMod(string id)
        {
            for (int i = 0; i < modEntries.Count; i++)
                if (modEntries[i].Info.Id == id) return modEntries[i];
            return null;
        }

        internal static bool TryGetEntryPoint(Assembly assembly, string entry, out Type type, out MethodInfo method)
        {
            type = null;
            method = null;
            if (string.IsNullOrEmpty(entry) || assembly == null) return false;

            string text = entry;
            int hash = text.IndexOf('#');
            if (hash != -1) text = text.Substring(0, hash);

            string className;
            string methodName;
            int sep = text.IndexOf("::", StringComparison.Ordinal);
            if (sep != -1)
            {
                className = text.Substring(0, sep);
                methodName = text.Substring(sep + 2);
            }
            else
            {
                int dot = text.LastIndexOf('.');
                if (dot == -1) return false;
                className = text.Substring(0, dot);
                methodName = text.Substring(dot + 1);
            }

            type = assembly.GetType(className) ?? assembly.GetType(className.Replace('.', '+'));
            if (type == null) return false;

            // 入口方法可能被重载：GetMethod 遇到重载会抛 AmbiguousMatchException，
            // 这里改成列出同名静态方法，优先挑 UMM 规范的两种签名：(ModEntry) 或 ()。
            const BindingFlags flags = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
            MethodInfo[] all;
            try { all = type.GetMethods(flags); } catch { return false; }
            MethodInfo first = null, byMod = null, byNone = null;
            for (int i = 0; i < all.Length; i++)
            {
                MethodInfo m = all[i];
                if (!string.Equals(m.Name, methodName, StringComparison.Ordinal)) continue;
                if (first == null) first = m;
                ParameterInfo[] ps = m.GetParameters();
                if (ps.Length == 0) { if (byNone == null) byNone = m; }
                else if (ps.Length == 1 && ps[0].ParameterType == typeof(ModEntry)) { if (byMod == null) byMod = m; }
            }
            method = byMod ?? byNone ?? first;
            return method != null;
        }

        internal static void OnModToggled(ModEntry entry, bool value)
        {
            ToggleModsListen handler = toggleModsListen;
            if (handler != null) handler(entry, value);
        }
    }
}

namespace UnityModManagerNet
{
    // 原版 UMM 的「允许热重载」标记。少数 MOD 的元数据里带这个特性，
    // 缺失时反射读取自定义特性会抛异常，这里补一个空实现。
    [AttributeUsage(AttributeTargets.Class)]
    public class EnableReloadingAttribute : Attribute { }
}

